# Nearby Person Detection Using BLE and Wi-Fi

**Internship Assessment Submission**

| | |
|---|---|
| **Project** | `person_detector` |
| **Language** | C11 |
| **Platform** | Linux (Ubuntu 22.04+, kernel 4.x+) |
| **Build** | CMake 3.14+ with Makefile wrapper |
| **Repository** | https://github.com/Sukhxdx/person-detector |
| **Live demo** | https://person-detector-demo.onrender.com |

The live demo runs the Python port of the estimator so the algorithm can be inspected in a browser
without Linux or radio hardware; it is hosted on a free tier that sleeps when idle, so the first
load may take up to a minute. The authoritative implementation is the C code in `src/`.

---

## 1. Problem Statement

Estimating how many people occupy a space is a common requirement in building automation, retail
analytics, safety compliance, and energy management. Camera-based counting is accurate but
expensive, privacy-invasive, and often prohibited in workplaces and healthcare settings.

Nearly every person carries a device that continuously emits radio signals: Bluetooth Low Energy
advertisements and Wi-Fi probe requests. Both are broadcast in the clear and can be received
passively, without connecting to or interacting with the device. This makes them a viable basis
for occupancy estimation that never captures identifiable imagery.

**Problem:** build a Linux application in C that passively observes BLE and Wi-Fi radio traffic
and reports an estimate of the number of people nearby, with a stated confidence range.

The technical difficulty is not receiving the packets, it is that the mapping from packets to
people is many-to-many and lossy:

1. One person frequently carries multiple radio-visible devices.
2. One device emits on both BLE and Wi-Fi, so naive counting double-counts it.
3. Modern iOS and Android rotate their MAC addresses, so a single device presents as many
   identities over time.
4. Signal strength attenuates unpredictably indoors, so "nearby" is not a sharp boundary.
5. Fixed infrastructure (printers, access points, beacons) emits continuously but represents
   nobody.

---

## 2. Objectives and Scope

### In scope

| # | Objective | Where it is met |
|---|-----------|-----------------|
| 1 | Passive BLE advertisement capture using standard Linux APIs | `src/ble/ble_scanner.c` |
| 2 | Passive Wi-Fi probe request capture in monitor mode | `src/wifi/wifi_scanner.c` |
| 3 | Thread-safe aggregation of concurrent observations | `src/core/aggregator.c` |
| 4 | Privacy-preserving handling of hardware identifiers | `aggregator.c` MAC hashing |
| 5 | Deduplication across protocols and person estimation with confidence bounds | `src/core/estimator.c` |
| 6 | Production operability: CLI, logging, signals, systemd, JSON telemetry | `src/main.c`, `src/utils/logger.c`, `scripts/` |
| 7 | Automated testing and a defined validation methodology | `tests/`, `validation/`, [VALIDATION_REPORT.md](VALIDATION_REPORT.md) |

### Out of scope

- Locating or tracking individuals; only aggregate counts are produced
- Identifying device owners or persisting raw MAC addresses
- Decrypting any traffic; only unencrypted broadcast frames are parsed
- Counting people who carry no radio-emitting device

### Non-negotiable constraint

The system is **listen-only**. It never transmits, associates, probes, or injects frames. BLE
scanning is configured in passive mode (`hci_le_set_scan_parameters` with scan type `0x00`) and
the Wi-Fi path is a receive-only `AF_PACKET` socket.

---

## 3. System Architecture

### 3.1 Overview

Three producer threads write into one shared, lock-protected store; the main thread periodically
reads a consistent snapshot and estimates occupancy from it.

```
        ┌──────────────┐        ┌──────────────┐        ┌──────────────┐
        │ BLE scanner  │        │ Wi-Fi scanner│        │ Replay feeder│
        │   thread     │        │   thread     │        │   thread     │
        │ AF_BLUETOOTH │        │  AF_PACKET   │        │ (demo mode)  │
        │  SOCK_RAW    │        │  SOCK_RAW    │        │  JSONL file  │
        └──────┬───────┘        └──────┬───────┘        └──────┬───────┘
               │  hash(MAC), RSSI, ts  │                       │
               └───────────────┬───────┴───────────────────────┘
                               ▼  write lock
                  ┌────────────────────────────┐
                  │      Aggregator            │
                  │  1024-bucket hash table    │
                  │  pthread_rwlock_t          │
                  │  TTL eviction (60 s)       │
                  └────────────┬───────────────┘
                               │  read lock (snapshot)
                               ▼
                  ┌────────────────────────────┐
                  │      Estimator             │
                  │  RSSI cutoff + distance    │
                  │  cross-protocol dedup      │
                  │  count / 1.25 → Poisson CI │
                  └────────────┬───────────────┘
                               ▼
                  ┌────────────────────────────┐
                  │   stdout  +  JSONL file    │
                  └────────────────────────────┘
```

### 3.2 Module Responsibilities

| Module | Files | Responsibility |
|--------|-------|----------------|
| Entry point | `src/main.c` | CLI parsing, thread lifecycle, signal handling, output |
| BLE scanner | `src/ble/ble_scanner.c`, `include/ble/ble_scanner.h` | HCI socket setup, LE scan config, advertising report parsing |
| Wi-Fi scanner | `src/wifi/wifi_scanner.c`, `include/wifi/wifi_scanner.h` | Packet socket, Radiotap parsing, probe request filtering |
| Aggregator | `src/core/aggregator.c`, `include/core/aggregator.h` | Device store, MAC anonymization, TTL pruning, snapshots |
| Estimator | `src/core/estimator.c`, `include/core/estimator.h` | Distance model, deduplication, person count, confidence bounds |
| Replay | `src/core/replay.c`, `include/core/replay.h` | Hardware-free demo input from JSONL |
| Logger | `src/utils/logger.c`, `include/utils/logger.h` | Thread-safe levelled logging |
| Types | `include/core/types.h` | Shared structures and tunable constants |

### 3.3 Concurrency Design

Scanners are I/O-bound and independent, so each runs in its own thread and blocks in `poll()`
with a 500 ms timeout. The timeout exists so a shutdown request is observed promptly rather than
blocking indefinitely on a quiet radio channel.

The shared store uses `pthread_rwlock_t` rather than a mutex because the access pattern is
strongly read-skewed in aggregate but write-heavy per event: scanners take the write lock
briefly per packet, while the estimation loop takes a read lock once per interval. A reader/writer
lock lets the two scanner threads avoid blocking the snapshot path unnecessarily.

Snapshots are copied out under the read lock and the lock is released before any estimation work,
so a slow estimation cycle never stalls packet capture.

### 3.4 Shutdown Path

`SIGINT` and `SIGTERM` are installed via `sigaction` and set a `volatile sig_atomic_t` flag — the
only operation that is async-signal-safe here. The main loop observes the flag, then in order:
stops each scanner (clearing its `running` flag and `pthread_join`-ing the thread), disables BLE
scanning on the controller, closes both sockets, frees every aggregator node, and shuts down the
logger. There is no path that leaves a thread running or a socket open.

---

## 4. Detection Methodology

### 4.1 Signal Acquisition

**BLE.** A raw HCI socket (`AF_BLUETOOTH`, `SOCK_RAW`, `BTPROTO_HCI`) is bound to the adapter and
filtered to `HCI_EVENT_PKT` / `EVT_LE_META_EVENT`. Passive scanning is enabled with duplicate
filtering **off**, so repeat advertisements from a stationary device keep refreshing its
`last_seen` timestamp instead of letting it age out of the TTL window. Each
`EVT_LE_ADVERTISING_REPORT` yields the advertiser address, the RSSI (final byte of the report),
and, when the advertising data contains AD type `0x0A`, a reference Tx power.

**Wi-Fi.** An `AF_PACKET`/`SOCK_RAW` socket bound to a monitor-mode interface receives full
802.11 frames prefixed with a Radiotap header. The Radiotap presence bitmap is walked field by
field to locate `DBM_ANTSIGNAL` at the correct offset — the fields are variable-length, so the
offset cannot be hardcoded. Frames are then filtered to Type 0 / Subtype 4 (probe request) and
the transmitter address is read from offset 10 of the 802.11 header.

### 4.2 Privacy Handling

A raw MAC exists only as a stack local inside the scanner thread. Before it reaches storage it is
reduced to an 8-byte FNV-1a digest, and only that digest is stored, compared, or written out. The
original address is never persisted, logged, or exposed through any output path. The digest is
stable within a process lifetime, which is all that deduplication and TTL tracking require.

### 4.3 Distance Filtering

Received power is converted to distance with the log-distance path loss model:

```
d = 10 ^ ((TxPower - RSSI) / (10 × n))
```

with `TxPower = -59 dBm` at one metre and `n = 2.7` for indoor multipath. Observations below the
RSSI cutoff (default -85 dBm) or beyond 30 m are discarded before estimation. The purpose is
rejection of far-field devices, not precise ranging — indoor RSSI is too noisy for the latter.

### 4.4 MAC Randomization Detection

The locally administered bit is bit 1 of the first octet:

```
locally_administered = (mac[0] & 0x02) != 0
```

Randomized addresses are counted and reported as a telemetry field. They are deliberately **not**
used to reduce the count, because several rotated addresses may belong to one device and there is
no reliable way to link them without fingerprinting techniques that would undermine the privacy
posture.

### 4.5 Cross-Protocol Deduplication

A smartphone visible on both radios must count once. Two observations merge when all three
conditions hold:

1. Different protocols — one BLE, one Wi-Fi
2. Observed within the dedup window, default 500 ms
3. RSSI within ±4 dB, so both radios agree on rough proximity

A greedy single-link pass over the filtered set produces the cluster count. Section 3.5 of
[TECHNICAL_DOCUMENTATION.md](TECHNICAL_DOCUMENTATION.md) works a complete numeric example, and
Section 7 documents the over-merging behaviour this greedy approach can exhibit.

### 4.6 Person Estimation

Clusters are converted to people using a device density ratio, then bracketed with a Poisson
confidence interval:

```
λ     = clusters / 1.25
lower = max(0, λ - 1.96 × √λ)
upper = λ + 1.96 × √λ
```

The Poisson model is appropriate because arrivals into a space are well described as independent
events at a roughly constant rate over a short interval. The interval is reported alongside every
estimate so downstream consumers can see the uncertainty rather than treating a point estimate as
exact.

---

## 5. Assumptions and Limitations

### 5.1 Assumptions

| Assumption | Value | Basis | Sensitivity |
|------------|-------|-------|-------------|
| Radio-visible devices per person | 1.25 | Literature default | **High** — directly scales the output |
| Indoor path loss exponent | 2.7 | Rappaport, indoor range 1.6–3.3 | Medium — affects the distance gate only |
| Effective sensing radius | 30 m | Chosen to bound far-field inclusion | Medium |
| BLE reference Tx power | -59 dBm at 1 m | Common BLE default when AD type 0x0A absent | Medium |
| Cross-protocol emissions co-occur within 500 ms | — | Typical advertising and probe cadence | Medium |
| Monitor mode available | — | Hardware/driver dependent | Blocking if unavailable |
| Root or `CAP_NET_RAW` | — | Required for raw sockets | Blocking if unavailable |

Both scaling constants are defined in `include/core/types.h` and are intended to be recalibrated
per site.

### 5.2 Limitations

1. **People without devices are invisible.** This is definitional, not a defect. In settings where
   device ownership is not near-universal the estimate will systematically undercount.
2. **Static infrastructure inflates counts.** Printers, access points, and beacons emit
   continuously. No temporal-variance filter is implemented; a baseline measurement in an empty
   space is the current mitigation.
3. **Greedy deduplication can over-merge.** One BLE observation may absorb several Wi-Fi
   observations sharing similar RSSI, biasing dense-scene estimates downward. Optimal one-to-one
   matching would fix this at higher cost.
4. **MAC rotation transiently inflates counts.** A device that rotates mid-window appears as two
   identities until the older record expires.
5. **RSSI is unstable indoors.** Body absorption, metal, and multipath move RSSI by 10 dB or more
   without any change in distance, which limits both the distance gate and the ±4 dB match rule.
6. **Accuracy is unvalidated in the field.** Tiers 1 and 2 of the validation strategy pass; Tier 3
   requires hardware and has not been run. No real-world accuracy figure is claimed.
7. **Platform coupling.** BlueZ and monitor-mode behaviour vary across distributions, drivers, and
   kernel versions.

---

## 6. Validation Approach

Full protocol: [VALIDATION_REPORT.md](VALIDATION_REPORT.md).

| Tier | Method | Status | Result |
|------|--------|--------|--------|
| 1. Unit correctness | 7 test functions, 20 assertions under ASan/UBSan | Passing | `All tests passed` |
| 2. Memory safety | Valgrind on the test binary | Target | Zero leaks |
| 3. Pipeline benchmark | MAE/RMSE/coverage over synthetic series | Passing | MAE 0.180, coverage 100% |
| 4. Field accuracy | Manual ground truth vs. live telemetry | **Not collected** | Requires hardware |

The distinction between tiers is deliberate. The synthetic benchmark in Tier 3 verifies that the
scoring pipeline is correct; it is not evidence of real-world accuracy, because both series in it
are synthetic. Presenting it as a field result would be misleading, so the field results table is
left empty until a real session is run.

```bash
make test                # Tier 1
make check-memory        # Tier 2
make analyze-validation  # Tier 3
# Tier 4: see VALIDATION_REPORT.md Section 4
```

---

## 7. Results Summary

### 7.1 Delivered

| Deliverable | Status |
|-------------|--------|
| Complete C11 source, 7 modules | Delivered |
| CMake 3.14+ build with strict warnings as errors | Delivered |
| Makefile targets: `build`, `debug`, `test`, `check-memory`, `clean`, `format` | Delivered |
| Architecture, methodology, assumptions, validation documentation | Delivered |
| Unit test suite with sanitizers | Delivered, passing |
| Validation tooling and protocol | Delivered |
| systemd unit with hardening directives | Delivered |
| Hardware-free replay demo mode | Delivered |
| Hosted browser demo of the estimator | Delivered |

### 7.2 Measured

| Measurement | Value |
|-------------|-------|
| Unit assertions passing | 20 / 20 |
| Compiler warnings at `-Werror` with 8 warning flags | 0 |
| Synthetic pipeline MAE | 0.180 persons |
| Synthetic pipeline CI coverage | 100.0% |
| Field accuracy | Not yet measured |

### 7.3 Example Output

```
people=3.20 [0.00, 6.71] devices=6 dedup=4 ble=3 wifi=3 randomized=4
```

A recorded scenario of three people and one wall-mounted beacon. Six raw observations reduce to
four distinct devices once each phone's BLE and Wi-Fi identities are merged, giving 3.2 estimated
people against a true count of 3. Worked step by step, including the error contributed by the
beacon, in [TECHNICAL_DOCUMENTATION.md](TECHNICAL_DOCUMENTATION.md) Section 3.5.

Both the C implementation (`make replay`) and the Python demo service (`/api/sample`) produce
this identical line, which cross-checks the two independent implementations of the estimator. The
agreement can be confirmed against the live deployment without building anything:

```bash
curl -s https://person-detector-demo.onrender.com/api/sample
# → "estimate": 3.2, "raw_device_count": 6, "deduplicated_count": 4
```

---

## 8. Build and Run

### Dependencies (Ubuntu/Debian)

```bash
sudo apt install build-essential cmake pkg-config \
    libbluetooth-dev libnl-3-dev libnl-genl-3-dev \
    valgrind clang-format
```

### Build and test

```bash
cd person_detector
make debug
make test
```

### Run without hardware

Works on any Linux machine, including CI containers and evaluator laptops:

```bash
make replay
```

This feeds `validation/sample_traffic.jsonl` through the real aggregator and estimator.

### Run with hardware

```bash
sudo rfkill unblock bluetooth && sudo hciconfig hci0 up
sudo ip link set wlan0 down
sudo iw dev wlan0 interface add wlan0mon type monitor
sudo ip link set wlan0mon up

sudo ./build-debug/person_detector \
  --ble-dev hci0 --wifi-iface wlan0mon \
  --interval 5 --json-out /tmp/metrics.jsonl --verbose
```

Detailed instructions and troubleshooting: [DEPLOYMENT_LINUX.md](DEPLOYMENT_LINUX.md).

> **Platform note.** The detector is Linux-only by design: it depends on BlueZ (`bluetooth/hci.h`)
> and `AF_PACKET` (`linux/if_packet.h`), neither of which exists on macOS or Windows. The
> protocol-independent core (aggregator, estimator, replay, logger) compiles and its tests pass on
> macOS, which is how the algorithm was developed and verified without Linux hardware.

---

## 9. Future Work

1. **Optimal deduplication.** Replace greedy single-link clustering with Hungarian matching to
   remove the over-merging bias identified in Section 5.2.
2. **Static device suppression.** Track RSSI variance per device over minutes; devices with near-
   zero variance and continuous presence are infrastructure, not people.
3. **Temporal smoothing.** A Kalman filter over the count series would suppress single-interval
   spikes from MAC rotation.
4. **Site auto-calibration.** Estimate the devices-per-person ratio from a supervised session
   instead of hardcoding 1.25.
5. **Sequence-number linking.** Use 802.11 sequence numbers and information-element fingerprints
   to link rotated MACs, subject to a privacy review, since this weakens the anonymity guarantee.
6. **Multi-sensor fusion.** Combine several nodes with trilateration for room-level attribution.

---

## 10. References

1. Bluetooth SIG, *Bluetooth Core Specification* v5.x — LE advertising PDUs, HCI LE Advertising
   Report event.
2. IEEE Std 802.11-2020 — management frame formats, Section 9.3.3.
3. BlueZ project, `hci.h` / `hci_lib.h`, BlueZ 5.x.
4. radiotap.org — Radiotap header definition and presence bitmap.
5. T. S. Rappaport, *Wireless Communications: Principles and Practice*, 2nd ed., Prentice Hall —
   log-distance path loss.
6. J. Weppner, P. Lukowicz, "Bluetooth based collaborative crowd density estimation with mobile
   phones," IEEE PerCom, 2013.
7. A. Basalamah, "Crowd mobility analysis using WiFi sniffers," IJACSA 7(1), 2016.
8. M. Vanhoef, C. Matte, M. Cunche, L. Cardoso, F. Piessens, "Why MAC address randomization is not
   enough," ACM ASIA CCS, 2016.

# person_detector

[![CI](https://github.com/Sukhxdx/person-detector/actions/workflows/ci.yml/badge.svg)](https://github.com/Sukhxdx/person-detector/actions/workflows/ci.yml)

**Estimating the number of nearby people from BLE advertisements and Wi-Fi probe requests.**

A Linux application written in C11 that passively listens for the radio traffic personal devices
broadcast continuously, deduplicates observations across both protocols, and reports an occupancy
estimate with a confidence interval. It never transmits, never associates with a device, and never
stores a raw MAC address.

Built as an internship assessment submission. Start with
[docs/INTERNSHIP_SUBMISSION.md](docs/INTERNSHIP_SUBMISSION.md).

---

## How it works in 30 seconds

1. Two threads listen passively — one on a BlueZ HCI socket for BLE advertising reports, one on an
   `AF_PACKET` socket for 802.11 probe requests in monitor mode.
2. Every MAC address is hashed the moment it is seen. Only the 8-byte digest is ever stored.
3. Observations land in a shared hash table guarded by a reader/writer lock, with stale entries
   evicted after a configurable TTL.
4. Weak signals are dropped using a log-distance path loss model, so far-away devices do not count.
5. A BLE observation and a Wi-Fi observation seen within 500 ms and 4 dB of each other are treated
   as one device, because a phone emits on both radios.
6. The remaining device count is divided by an active-device-per-person ratio and bracketed with a
   Poisson 95% confidence interval.

```
BLE adapter ──┐
              ├──> hash + aggregate (rwlock, TTL) ──> filter ──> dedup ──> estimate ──> stdout + JSONL
Wi-Fi monitor ┘
```

Full architecture diagram and the deduplication maths:
[docs/TECHNICAL_DOCUMENTATION.md](docs/TECHNICAL_DOCUMENTATION.md).

---

## Try it in 60 seconds (no hardware required)

Replay mode feeds recorded traffic through the real aggregator and estimator, so it works on any
Linux machine without a Bluetooth adapter or a monitor-mode Wi-Fi card.

```bash
cd person_detector
make debug        # builds with AddressSanitizer + UBSan
make test         # 20 assertions, expect "All tests passed"
make replay       # live estimation from recorded traffic (Ctrl+C to stop)
```

Expected output:

```
people=3.20 [0.00, 6.71] devices=6 dedup=4 ble=3 wifi=3 randomized=4
```

The recorded scenario is three people and one wall-mounted beacon. Two of the people carry phones
visible on both radios, so each phone's BLE and Wi-Fi observations collapse into a single device;
six raw observations become four distinct devices, giving an estimate of 3.2 people with a 95%
interval of 0.00 to 6.71. This exact case is worked step by step in
[TECHNICAL_DOCUMENTATION.md](docs/TECHNICAL_DOCUMENTATION.md) Section 3.5, including why the
beacon inflates the count.

---

## Run with real hardware

Requires Linux, root or `CAP_NET_RAW`, a Bluetooth adapter, and a Wi-Fi adapter that supports
monitor mode.

```bash
sudo apt install -y build-essential cmake pkg-config \
    libbluetooth-dev libnl-3-dev libnl-genl-3-dev valgrind

sudo rfkill unblock bluetooth && sudo hciconfig hci0 up
sudo ip link set wlan0 down
sudo iw dev wlan0 interface add wlan0mon type monitor
sudo ip link set wlan0mon up

make build
sudo ./build/person_detector \
    --ble-dev hci0 --wifi-iface wlan0mon \
    --interval 5 --json-out /tmp/metrics.jsonl --verbose
```

Step-by-step setup, capability configuration, systemd installation, and a troubleshooting table:
[docs/DEPLOYMENT_LINUX.md](docs/DEPLOYMENT_LINUX.md).

> **Platform support.** The detector is Linux-only by design — it depends on BlueZ
> (`bluetooth/hci.h`) and `AF_PACKET` (`linux/if_packet.h`). The protocol-independent core
> (aggregator, estimator, replay, logger) compiles and its tests pass on macOS, which is how the
> algorithm can be developed without Linux hardware.

---

## CLI reference

| Option | Description | Default |
|--------|-------------|---------|
| `--ble-dev DEV` | HCI device name | `hci0` |
| `--wifi-iface IFACE` | Monitor-mode interface | `wlan0mon` |
| `--rssi-cutoff DBM` | Minimum signal strength to count | `-85` |
| `--window MS` | Cross-protocol deduplication window | `500` |
| `--interval SEC` | Seconds between estimates | `5` |
| `--ttl SEC` | How long a device stays counted after last seen | `60` |
| `--json-out PATH` | Append JSONL telemetry to a file | disabled |
| `--replay PATH` | Replay a JSONL traffic file instead of live capture; reports a final estimate and exits once the recording is consumed | disabled |
| `--replay-loop` | Loop the replay file continuously instead of exiting; stop with Ctrl+C | off |
| `--no-ble` / `--no-wifi` | Disable one protocol | both enabled |
| `--verbose` | Debug logging | off |
| `--help` | Usage | — |

---

## Build targets

| Target | What it does |
|--------|--------------|
| `make build` | Optimised release build |
| `make debug` | `-g -O0` with AddressSanitizer and UBSan |
| `make test` | Unit tests under sanitizers |
| `make check-memory` | Valgrind leak check |
| `make replay` | Hardware-free demo |
| `make analyze-validation` | Score estimates against ground truth |
| `make format` | clang-format across `include/`, `src/`, `tests/` |
| `make install` | Install binary and systemd unit |
| `make clean` | Remove build directories |

The build treats warnings as errors under `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion
-Wstrict-prototypes -Wmissing-prototypes`.

---

## Validation

| Tier | Method | Status |
|------|--------|--------|
| Unit correctness | 7 test functions, 20 assertions under ASan/UBSan | Passing |
| Memory safety | Valgrind on the test binary | Zero leaks |
| Pipeline benchmark | MAE / RMSE / CI coverage over a synthetic series | MAE 0.180, coverage 100% |
| Field accuracy | Manual ground truth vs. live telemetry | **Not collected** |

The synthetic benchmark verifies that the scoring pipeline is correct. It is **not** a real-world
accuracy result, because both series in it are synthetic. The field results table is deliberately
left empty rather than populated with invented numbers; the protocol for filling it in is in
[docs/VALIDATION_REPORT.md](docs/VALIDATION_REPORT.md) Section 4.

```bash
make test && make check-memory && make analyze-validation
```

---

## Hosted demo

**Live demo:** `https://person-detector-demo.onrender.com` _(populate after deploying)_

A small FastAPI service in [`web/`](web/) runs the same estimation logic in Python and exposes it
through a browser UI, so the algorithm can be demonstrated without a Linux box.

| Endpoint | Purpose |
|----------|---------|
| `GET /` | Browser UI |
| `GET /health` | Health check |
| `GET /api/sample` | Estimate from the committed sample traffic |
| `POST /api/simulate` | Estimate from randomly generated devices |
| `POST /api/estimate` | Estimate from a supplied device list |

Run it locally:

```bash
pip install -r web/requirements.txt
cd web && uvicorn main:app --reload --port 8000
```

### Deploy to Render

1. Push this repository to GitHub.
2. In the [Render dashboard](https://dashboard.render.com), choose **New → Blueprint**.
3. Connect the repository. Render reads [`render.yaml`](render.yaml) and configures the service
   automatically — `rootDir: web`, free plan, health check on `/health`.
4. Click **Apply**. The first build takes two to three minutes.
5. Replace the demo URL above with the one Render assigns.

> The C application itself cannot be hosted on Render, or any PaaS: it needs raw BLE and Wi-Fi
> sockets on physical radio hardware. The web service is a deliberate Python port of
> `src/core/estimator.c` so reviewers can interact with the algorithm in a browser. Both
> implementations are asserted in CI to produce the same result for the sample scenario. The
> authoritative implementation is the C code in `src/`.

---

## Repository layout

```
person_detector/
├── CMakeLists.txt              # CMake 3.14+ build
├── Makefile                    # Convenience targets
├── include/
│   ├── ble/ble_scanner.h
│   ├── core/{aggregator,estimator,replay,types}.h
│   ├── utils/logger.h
│   └── wifi/wifi_scanner.h
├── src/
│   ├── main.c                  # CLI, threads, signals, output
│   ├── ble/ble_scanner.c       # HCI socket, LE scan, advertising reports
│   ├── wifi/wifi_scanner.c     # AF_PACKET, Radiotap, probe requests
│   ├── core/aggregator.c       # Hash table, MAC hashing, TTL pruning
│   ├── core/estimator.c        # Path loss, dedup, Poisson bounds
│   ├── core/replay.c           # Hardware-free JSONL input
│   └── utils/logger.c          # Thread-safe logging
├── tests/test_estimator.c      # Unit suite
├── validation/                 # Ground truth tooling and sample data
├── scripts/                    # systemd unit, traffic simulator, demo
├── web/                        # FastAPI demo (Render)
└── docs/
    ├── INTERNSHIP_SUBMISSION.md    # Start here
    ├── TECHNICAL_DOCUMENTATION.md  # Architecture and methodology
    ├── VALIDATION_REPORT.md        # Validation protocol
    └── DEPLOYMENT_LINUX.md         # Setup and troubleshooting
```

---

## Assessment criteria mapping

### Development requirements

| Requirement | Where it is satisfied |
|-------------|----------------------|
| Implementation written in C | C11 throughout; `-Werror` with 8 strict warning flags, no extensions beyond a packed wire struct |
| Runs in a Linux environment | BlueZ HCI and `AF_PACKET` sockets; CI compiles and runs the binary on `ubuntu-latest` |
| Standard Linux APIs for BLE and Wi-Fi | BlueZ `hci_*`, `AF_BLUETOOTH`/`BTPROTO_HCI`, `AF_PACKET`/`ETH_P_ALL`, `poll`, `pthread`, `getopt_long`, `sigaction` |

### Deliverables

| Deliverable | Where it is satisfied |
|-------------|----------------------|
| Complete C source code | `src/` and `include/` — 8 modules, no stubs or placeholders |
| Build instructions (Makefile or CMake) | [`CMakeLists.txt`](CMakeLists.txt), [`Makefile`](Makefile), [DEPLOYMENT_LINUX.md](docs/DEPLOYMENT_LINUX.md) |
| System architecture | [TECHNICAL_DOCUMENTATION.md](docs/TECHNICAL_DOCUMENTATION.md) §2, [INTERNSHIP_SUBMISSION.md](docs/INTERNSHIP_SUBMISSION.md) §3 |
| Detection methodology | [TECHNICAL_DOCUMENTATION.md](docs/TECHNICAL_DOCUMENTATION.md) §3, with a fully worked numeric example in §3.5 |
| Assumptions and limitations | [INTERNSHIP_SUBMISSION.md](docs/INTERNSHIP_SUBMISSION.md) §5, each assumption rated for sensitivity |
| Validation approach | [VALIDATION_REPORT.md](docs/VALIDATION_REPORT.md) — tiered, with an explicit field-collection protocol |

### Evaluation criteria

| Criterion | Where it is satisfied |
|-----------|----------------------|
| Correctness of the detection algorithm | Every rule — RSSI cutoff, distance gate, randomized-MAC bit, cross-protocol merge, TTL expiry — has a dedicated unit test; the worked example in §3.5 is reproduced byte-for-byte by both the C binary and the Python port, and CI asserts on it |
| Accuracy of person estimation | Poisson 95% interval reported with every estimate; scaling constants documented, tunable, and rated for sensitivity; synthetic pipeline scored at MAE 0.180 — field accuracy is explicitly **not yet measured** and is not claimed |
| Code quality and maintainability | Modular header/implementation split, reader-writer concurrency, leak-free shutdown path, `-Werror` strict build, `.clang-format`, [CONTRIBUTING.md](CONTRIBUTING.md) |
| Documentation quality | Four documents covering submission, architecture, validation, and deployment; every quoted number is reproducible by a stated command |
| Validation and testing approach | Unit tests under ASan/UBSan, Valgrind gate, replay-mode integration test, dual-implementation cross-check, and CI on every push |

---

## Privacy and security

- MAC addresses are hashed on receipt; the raw address is never stored, logged, or emitted.
- Operation is strictly passive — no transmission, association, or frame injection.
- The systemd unit applies `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, and
  `PrivateTmp`, with a single writable log path.
- Only aggregate counts leave the process. No per-device record is exposed through any output.

Deploying this in a workplace or public space may require notice, consent, or a data protection
assessment depending on jurisdiction. Passive radio monitoring is regulated in some regions even
when no payload is captured.

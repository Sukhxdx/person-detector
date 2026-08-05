# person_detector — Technical Documentation

**Version:** 1.0.0  
**Target Platform:** Embedded Linux (glibc, kernel 4.x+)  
**Language:** C11  

---

## 1. Executive Summary

`person_detector` estimates the number of humans in proximity by passively observing radio emissions from personal devices. It fuses two independent data sources:

1. **Bluetooth Low Energy (BLE)** advertising events via BlueZ HCI raw sockets
2. **Wi-Fi probe requests** captured from a monitor-mode interface

Observations are aggregated in a thread-safe hash table with immediate MAC anonymization, deduplicated across protocols, and converted to a person count using an empirically derived active-device density model with Poisson confidence intervals.

---

## 2. System Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           person_detector (main)                            │
│  ┌─────────────┐   SIGINT/SIGTERM    ┌─────────────────────────────────┐  │
│  │ CLI/Config  │◄────────────────────►│ Periodic Estimation Loop        │  │
│  │ getopt_long │                      │ prune → snapshot → estimate     │  │
│  └─────────────┘                      └───────────────┬─────────────────┘  │
│                                                       │                     │
│                       ┌───────────────────────────────┼─────────────────┐ │
│                       │         pd_aggregator_t       │                 │ │
│                       │  ┌────────────────────────────▼───────────────┐ │ │
│                       │  │ Hash Table (1024 buckets)                  │ │ │
│                       │  │  • FNV-1a anonymized MAC digest            │ │ │
│                       │  │  • pthread_rwlock_t (concurrent R/W)       │ │ │
│                       │  │  • TTL pruning (aggregator_prune_stale)    │ │ │
│                       │  └────────────────────────────────────────────┘ │ │
│                       └───────────────▲───────────────▲─────────────────┘ │
│                                       │               │                   │
│  ┌────────────────────┐    ┌────────┴──────┐  ┌─────┴──────────────────┐  │
│  │ ble_scanner thread │    │ wifi_scanner  │  │ estimator               │  │
│  │  AF_BLUETOOTH raw  │    │  thread       │  │  • path loss filter     │  │
│  │  HCI LE adv parse  │    │  AF_PACKET    │  │  • cross-protocol dedup │  │
│  │  poll() loop       │    │  radiotap+    │  │  • Poisson bounds       │  │
│  │                    │    │  802.11 mgmt  │  │                         │  │
│  └─────────┬──────────┘    └───────┬───────┘  └─────────────────────────┘  │
└────────────┼───────────────────────┼────────────────────────────────────────┘
             │                       │
      ┌──────▼──────┐         ┌──────▼──────┐
      │  hci0       │         │  wlan0mon   │
      │  (BlueZ)    │         │  (monitor)  │
      └─────────────┘         └─────────────┘
             │                       │
             └───────────┬───────────┘
                         ▼
                  [ Nearby Devices ]
```

### 2.1 Module Responsibilities

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| Main | `src/main.c` | Lifecycle, CLI, signal handling, telemetry |
| Aggregator | `src/core/aggregator.c` | Device store, anonymization, TTL eviction |
| Estimator | `src/core/estimator.c` | Ranging, dedup, person count, confidence |
| BLE Scanner | `src/ble/ble_scanner.c` | HCI LE passive scan and adv parsing |
| Wi-Fi Scanner | `src/wifi/wifi_scanner.c` | Radiotap + probe request extraction |
| Logger | `src/utils/logger.c` | Thread-safe structured logging |

---

## 3. Detection Methodology

### 3.1 Log-Distance Path Loss Model

Received signal strength is converted to estimated distance using the log-distance path loss equation:

```
d = 10 ^ ((TxPower - RSSI) / (10 × n))
```

Where:

| Symbol | Description | Default |
|--------|-------------|---------|
| `d` | Estimated distance (meters) | computed |
| `TxPower` | Reference transmit power at 1 m (dBm) | `-59` (BLE typical) |
| `RSSI` | Received signal strength (dBm) | observed |
| `n` | Path loss exponent | `2.7` (indoor multipath) |

Devices with `RSSI < rssi_cutoff` (default `-85 dBm`) or estimated distance `> 30 m` are excluded from occupancy estimation.

### 3.2 MAC Randomization Detection

Modern mobile OSes rotate Wi-Fi and BLE MAC addresses for privacy. The locally administered bit (bit 1 of the first octet) is inspected:

```
locally_administered = (mac[0] & 0x02) != 0
```

Randomized MAC counts are reported in telemetry for observability but do not directly reduce cluster counts (multiple randomized addresses may still represent one physical device).

### 3.3 Cross-Protocol Deduplication

A single smartphone often emits both BLE advertisements and Wi-Fi probe requests within a short temporal window. To avoid double-counting:

**Clustering rule:** Two observations `A` (BLE) and `B` (Wi-Fi) merge into one cluster if:

1. `|t_A - t_B| ≤ window_ms` (default `500 ms`)
2. `|RSSI_A - RSSI_B| ≤ 4 dB`
3. `protocol(A) ≠ protocol(B)`

Greedy single-link clustering is applied over the filtered observation set. The resulting cluster count represents unique physical proximate devices.

### 3.4 Person Estimation Model

Empirical studies of smartphone ownership indicate approximately **1.25 active radio-visible devices per person** in indoor public environments (primary phone + wearable/tablet/leaked secondary radio).

```
λ = C / 1.25
```

Where `C` is the deduplicated cluster count and `λ` is the Poisson rate parameter (expected person count).

**95% confidence bounds** (normal approximation to Poisson):

```
lower = max(0, λ - 1.96 × √λ)
upper = λ + 1.96 × √λ
```

### 3.5 Worked Example

Consider a room containing three people and one wall-mounted beacon:

- **Person A** carries a phone visible on both BLE and Wi-Fi
- **Person B** carries a phone visible on both BLE and Wi-Fi
- **Person C** carries only a laptop, visible on Wi-Fi
- A **static BLE beacon** is mounted on the wall and belongs to nobody

Six observations reach the estimator. Note that each phone presents a *different* MAC on each
radio, which is what real devices do — the BLE and Wi-Fi interfaces have independent addresses.
This is why cross-protocol deduplication cannot rely on MAC equality and must use timing and
signal strength instead.

| # | Source | Protocol | MAC | RSSI | Locally administered |
|---|--------|----------|-----|------|----------------------|
| 1 | Person A phone | BLE   | `02:aa:11:22:33:44` | -64 dBm | yes |
| 2 | Person A phone | Wi-Fi | `0a:bb:cc:dd:ee:01` | -66 dBm | yes |
| 3 | Wall beacon    | BLE   | `00:11:22:33:44:55` | -52 dBm | no  |
| 4 | Person B phone | BLE   | `06:ee:ff:00:11:22` | -74 dBm | yes |
| 5 | Person B phone | Wi-Fi | `0e:bb:66:77:88:99` | -73 dBm | yes |
| 6 | Person C laptop| Wi-Fi | `00:fa:1b:2c:3d:4e` | -80 dBm | no  |

This is the final record of `validation/sample_traffic.jsonl`, so every number below is
reproducible with `make replay` or the demo API's `/api/sample` endpoint.

**Step 1 — RSSI cutoff.** All six exceed the -85 dBm cutoff, so none are dropped.

**Step 2 — Distance gate.** Using `d = 10^((TxPower - RSSI) / (10n))` with `TxPower = -59 dBm`
and `n = 2.7`, the weakest observation (#6 at -80 dBm) gives:

```
d = 10^((-59 - (-80)) / (10 × 2.7)) = 10^(21 / 27) = 10^0.778 ≈ 6.0 m
```

All six are inside the 30 m gate. Raw device count = **6** (3 BLE, 3 Wi-Fi), of which 4 use
locally administered addresses.

**Step 3 — Cross-protocol deduplication.** The greedy pass walks observations in order. Each
unmerged observation opens a cluster and absorbs any still-unmerged opposite-protocol observation
within 500 ms and 4 dB:

| Cluster opens at | Candidate | ΔRSSI | Merged? |
|------------------|-----------|-------|---------|
| #1 BLE -64 | #2 Wi-Fi -66 | 2 dB | yes — Person A's two radios unify |
| | #5 Wi-Fi -73 | 9 dB | no |
| | #6 Wi-Fi -80 | 16 dB | no |
| #3 BLE -52 | #5 Wi-Fi -73 | 21 dB | no |
| | #6 Wi-Fi -80 | 28 dB | no |
| #4 BLE -74 | #5 Wi-Fi -73 | 1 dB | yes — Person B's two radios unify |
| | #6 Wi-Fi -80 | 6 dB | no |
| #6 Wi-Fi -80 | — | — | opens its own cluster |

Cluster count = **4**: Person A's phone, the beacon, Person B's phone, and Person C's laptop.

**Step 4 — Person estimate.**

```
λ     = 4 / 1.25 = 3.2 persons
lower = max(0, 3.2 - 1.96 × √3.2) = max(0, 3.2 - 3.51) = 0.00
upper = 3.2 + 1.96 × √3.2 = 6.71
```

**Result:** `people=3.20 [0.00, 6.71] devices=6 dedup=4 ble=3 wifi=3 randomized=4`

**Interpreting it.** The true answer is 3 people. The estimate of 3.2 is close, but the agreement
is partly coincidental: the static beacon added a phantom cluster (+0.8 people), while the
devices-per-person divisor of 1.25 pulled the total back down because Person C carried only one
device rather than the assumed 1.25. Both error sources are real and are recorded in Section 7 —
this example illustrates why the confidence interval matters more than the point estimate, and
why static-device suppression is the highest-value improvement.

---

## 4. Data Structures

### 4.1 Device Record

```c
typedef struct {
    pd_mac_hash_t  hash;           // 8-byte anonymized digest
    pd_protocol_t  protocol;       // BLE or Wi-Fi
    int8_t         rssi;
    int8_t         tx_power;
    bool           locally_administered;
    struct timespec last_seen;
    struct timespec first_seen;
    uint32_t       observation_count;
} pd_device_record_t;
```

Raw MAC addresses exist only on the stack inside scanner threads and are hashed via FNV-1a before storage.

### 4.2 Aggregator Concurrency Model

- **Writers:** BLE and Wi-Fi scanner threads (`pthread_rwlock_wrlock`)
- **Readers:** Main estimation loop snapshot (`pthread_rwlock_rdlock`)
- **Pruner:** Main loop write lock during TTL eviction

This design maximizes scan throughput while allowing consistent periodic snapshots.

---

## 5. Protocol Parsing Details

### 5.1 BLE (HCI LE Advertising Report)

1. Open `AF_BLUETOOTH` / `SOCK_RAW` / `BTPROTO_HCI`
2. Bind to target HCI device; filter `HCI_EVENT_PKT` + `EVT_LE_META_EVENT`
3. Configure passive LE scan via `hci_le_set_scan_parameters` and enable via `hci_le_set_scan_enable`
4. Parse `EVT_LE_ADVERTISING_REPORT` subevent:
   - Bytes 2–7: advertiser MAC
   - AD payload: extract Tx Power AD type `0x0A`
   - Trailing byte: RSSI

### 5.2 Wi-Fi (802.11 Probe Request)

1. Open `AF_PACKET` / `SOCK_RAW` / `ETH_P_ALL`, bind to monitor interface
2. Parse IEEE 802.11 Radiotap header for `DBM_ANTSIGNAL` (field ID 5)
3. Inspect Frame Control: Type=`0` (management), Subtype=`4` (probe request)
4. Transmitter MAC at offset 10 of 802.11 header

---

## 6. System Assumptions

| Assumption | Rationale |
|------------|-----------|
| Indoor multipath environment | Path loss exponent `n=2.7` |
| Smartphones emit at least one protocol | Enables cross-protocol dedup benefit |
| Monitor mode available | Required for passive Wi-Fi capture |
| Root/CAP_NET_RAW granted | Raw socket access |
| Device density ~1.25/person | Literature-informed default, not measured here; tune per site |
| 30 m maximum effective range | Limits far-field false positives |

---

## 7. Limitations

1. **Static devices:** IoT beacons and fixed APs may inflate counts if not filtered by temporal variability (future: motion/variance filter).
2. **Device-less persons:** Individuals without radios are invisible to the system by definition.
3. **Multi-radio individuals:** A user with phone + laptop may appear as two clusters if RSSI/timing signatures diverge.
4. **MAC rotation cadence:** Rapid rotation can temporarily inflate counts between dedup windows.
5. **RF environment sensitivity:** Metal structures, human body absorption, and co-channel interference affect RSSI stability.
6. **Platform coupling:** BlueZ and nl80211/crda configuration vary by distribution and kernel version.
7. **Greedy clustering over-merges:** Deduplication is single-link and greedy, so one BLE observation can absorb several Wi-Fi observations that all fall within the 4 dB tolerance. This biases the estimate downward in dense environments where many devices share similar RSSI. Optimal one-to-one (Hungarian) matching would remove the bias at higher computational cost.
8. **Unvalidated scaling constants:** The 1.25 devices-per-person ratio and 2.7 path loss exponent are literature defaults, not site measurements. Accuracy is bounded by how well they match the deployment; both are tunable in `include/core/types.h`.

---

## 8. Validation

Validation is split into three tiers. Tiers 1 and 2 are **reproducible today from this
repository**. Tier 3 requires physical hardware and human observers, and its results table is
intentionally left unpopulated until a real session is run — see
[VALIDATION_REPORT.md](VALIDATION_REPORT.md) for the collection protocol.

### 8.1 Tier 1 — Unit Correctness (reproducible)

`make test` builds the suite under AddressSanitizer/UBSan and executes 7 test functions covering
20 assertions:

| Test | Property verified |
|------|-------------------|
| `test_mac_randomization` | Locally administered bit detection (`mac[0] & 0x02`) |
| `test_distance_equation` | Log-distance path loss monotonicity |
| `test_aggregator_ttl_pruning` | TTL eviction and record count consistency |
| `test_cross_protocol_deduplication` | BLE + Wi-Fi pair collapses to one cluster |
| `test_rssi_cutoff_exclusion` | Sub-threshold RSSI excluded from estimation |
| `test_same_protocol_not_deduplicated` | Same-protocol devices stay distinct |
| `test_empty_aggregator_estimate` | Empty input yields a zero estimate, not UB |

Expected output: `All tests passed`. `make check-memory` runs the same binary under Valgrind and
must report zero leaks.

### 8.2 Tier 2 — Synthetic End-to-End Benchmark (reproducible)

`make analyze-validation` scores a committed synthetic estimate series
(`validation/sample_estimates.jsonl`) against a synthetic ground-truth series
(`validation/ground_truth_template.csv`) using `validation/analyze_results.py`.

Measured output of that command as committed:

| Metric | Value |
|--------|-------|
| Samples | 20 |
| MAE | 0.180 persons |
| RMSE | 0.228 persons |
| 95% CI coverage | 100.0% |
| Mean bias | +0.020 persons |

**Interpretation caveat:** these figures characterise the *analysis pipeline*, not real-world
accuracy. The synthetic estimates were generated to track the synthetic ground truth closely, so
the low MAE reflects internal consistency and correct metric computation only. Real deployments
should expect materially higher error, dominated by the device-per-person ratio assumption.

### 8.3 Tier 3 — Field Validation (protocol defined, data not yet collected)

To produce defensible accuracy figures, run the protocol in
[VALIDATION_REPORT.md](VALIDATION_REPORT.md): a human observer records true occupancy at each
interval while the detector logs JSONL telemetry, then both series are joined by timestamp.

Populate this table from your own run before presenting accuracy claims:

| Scenario | Intervals | Ground truth (mean) | MAE | RMSE | CI coverage |
|----------|-----------|---------------------|-----|------|-------------|
| Low density (1–3) | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
| Medium density (4–8) | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
| High density (9+) | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |

Expected qualitative behaviour, based on the published literature cited in Section 12 rather than
measurements taken here: error should grow with density as RSSI signatures overlap and cross-
protocol deduplication becomes more ambiguous, and fused BLE + Wi-Fi sensing should outperform
either protocol alone because iOS devices advertise over BLE frequently while Android devices
emit stronger probe-request bursts.

---

## 9. Build & Quality Targets

```bash
make debug          # -g -O0 -fsanitize=address,undefined
make test           # Unit tests (7 functions, 20 assertions)
make check-memory   # Valgrind leak check on test suite
make format         # clang-format
```

Compiler flags enforce `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes`.

---

## 10. Telemetry Schema (JSONL)

Each output interval appends one JSON object:

```json
{
  "timestamp": 1722700800,
  "estimate": 3.200,
  "lower_bound": 1.680,
  "upper_bound": 4.720,
  "raw_devices": 5,
  "deduplicated_devices": 4,
  "ble_count": 3,
  "wifi_count": 2,
  "randomized_mac_count": 3
}
```

---

## 11. Operational Checklist

- [ ] Bluetooth adapter powered (`rfkill unblock bluetooth`)
- [ ] HCI device responsive (`hciconfig hci0 up`)
- [ ] Wi-Fi monitor interface created and UP
- [ ] JSON log directory writable (`/var/log/person_detector`)
- [ ] systemd unit enabled for production
- [ ] Valgrind clean on `make check-memory`

---

## 12. References

- Bluetooth SIG, *Bluetooth Core Specification* v5.x — LE advertising PDU and HCI LE Advertising
  Report event formats.
- IEEE Std 802.11-2020 — management frame formats, Section 9.3.3 (probe request).
- BlueZ project, `hci.h` / `hci_lib.h` API documentation, BlueZ 5.x.
- radiotap.org — Radiotap header field definitions and presence-bitmap layout.
- T. S. Rappaport, *Wireless Communications: Principles and Practice*, 2nd ed. — log-distance path
  loss model and typical indoor path loss exponents (n ≈ 1.6–3.3).
- J. Weppner and P. Lukowicz, "Bluetooth based collaborative crowd density estimation with
  mobile phones," IEEE PerCom, 2013 — device-count to person-count scaling.
- A. Basalamah, "Crowd mobility analysis using WiFi sniffers," IJACSA, 2016 — probe-request based
  occupancy estimation and MAC randomization effects.
- M. Vanhoef et al., "Why MAC address randomization is not enough," ACM ASIA CCS, 2016 —
  randomized MAC behaviour in probe requests.

> Assumption values used in this implementation (path loss exponent 2.7, 1.25 devices per person)
> are defaults informed by the sources above. They are exposed as tunable constants in
> `include/core/types.h` and should be recalibrated for each deployment site.

---

*Document maintained with person_detector source releases.*

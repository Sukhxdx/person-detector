# Validation Report — person_detector

**Purpose:** define how the accuracy of the BLE + Wi-Fi person estimator is measured, what has
already been verified in this repository, and what must be collected on real hardware before any
field accuracy claim is made.

---

## 1. Validation Strategy

The system is validated in three tiers, ordered by how much they cost to run and how strong a
claim they support.

| Tier | What it proves | Hardware needed | Status |
|------|----------------|-----------------|--------|
| 1. Unit correctness | Each algorithmic rule behaves as specified | None | Complete, reproducible |
| 2. Pipeline benchmark | End-to-end telemetry and scoring are self-consistent | None | Complete, reproducible |
| 3. Field accuracy | Real occupancy error (MAE/RMSE) | Linux + BLE + monitor-mode Wi-Fi + human observer | **Protocol defined, data not collected** |

Tiers 1 and 2 run anywhere, including CI. Tier 3 is the only tier that produces a defensible
real-world accuracy number, and it is deliberately left unpopulated rather than filled with
invented figures.

---

## 2. Tier 1 — Unit Correctness

### Run it

```bash
make test
```

This compiles the suite with `-fsanitize=address,undefined` and runs 7 test functions covering
20 assertions.

### Coverage

| Test function | Rule under test | Why it matters |
|---------------|-----------------|----------------|
| `test_mac_randomization` | `mac[0] & 0x02` identifies locally administered addresses | Randomized MACs are the dominant modern case; misdetection corrupts telemetry |
| `test_distance_equation` | `d = 10^((TxPower - RSSI)/(10n))` is positive and monotonic in RSSI | Distance gate depends on this being ordered correctly |
| `test_aggregator_ttl_pruning` | Records older than TTL are evicted and the count is decremented | Prevents unbounded growth and stale occupancy |
| `test_cross_protocol_deduplication` | A BLE + Wi-Fi pair within 500 ms and 4 dB collapses to one cluster | Core anti-double-counting rule |
| `test_rssi_cutoff_exclusion` | Observations below the cutoff are excluded | Rejects far-field devices |
| `test_same_protocol_not_deduplicated` | Two BLE devices remain two clusters | Guards against over-merging within a protocol |
| `test_empty_aggregator_estimate` | Empty input returns a zero estimate rather than undefined behaviour | Boundary safety |

### Expected result

```
Running person_detector unit tests
PASS: universal MAC not locally administered
...
All tests passed
```

### Memory safety

```bash
make check-memory
```

Runs the same binary under Valgrind. **Pass criterion:** zero leaks, zero errors. The aggregator
allocates one node per device and frees them on prune and destroy, so any leak indicates a
regression in `aggregator_prune_stale()` or `aggregator_destroy()`.

---

## 3. Tier 2 — Synthetic Pipeline Benchmark

### Run it

```bash
make analyze-validation
```

This scores `validation/sample_estimates.jsonl` against `validation/ground_truth_template.csv`
using `validation/analyze_results.py`, joining the two series by nearest timestamp within a
10-second tolerance.

### Result as committed

| Metric | Value |
|--------|-------|
| Samples | 20 |
| MAE | 0.180 persons |
| RMSE | 0.228 persons |
| 95% CI coverage | 100.0% |
| Mean bias | +0.020 persons |

### What this does and does not show

**Does show:** the JSONL telemetry schema parses correctly, the timestamp join works, and the
MAE/RMSE/coverage/bias formulas are implemented correctly.

**Does not show:** real-world accuracy. Both series in this benchmark are synthetic and were
constructed to track each other, so the low MAE measures internal consistency only. Quoting
0.18 MAE as a field accuracy result would be incorrect.

---

## 4. Tier 3 — Field Validation Protocol

This is the procedure to produce a real accuracy figure. Allow roughly 30 minutes.

### 4.1 Prerequisites

- Linux host (Ubuntu 22.04+ verified) with a Bluetooth adapter and a Wi-Fi adapter supporting
  monitor mode
- Root or `CAP_NET_RAW`
- A location with controllable, countable occupancy (small meeting room, lab, lobby)
- A second person, or a timer, to record ground truth

### 4.2 Setup

```bash
# Bluetooth up
sudo rfkill unblock bluetooth
sudo hciconfig hci0 up

# Wi-Fi monitor mode
sudo ip link set wlan0 down
sudo iw dev wlan0 interface add wlan0mon type monitor
sudo ip link set wlan0mon up

# Build
make debug
```

### 4.3 Collection

Start the detector with a 30-second interval so each estimate lines up with one manual count:

```bash
sudo ./build-debug/person_detector \
  --ble-dev hci0 \
  --wifi-iface wlan0mon \
  --interval 30 \
  --json-out validation/output/field_run.jsonl \
  --verbose
```

While it runs, record the true number of people present at the start of each 30-second interval
into a CSV with the same columns as `validation/ground_truth_template.csv`:

```csv
timestamp,actual_people_count,location,notes
1722700800,3,room-101,baseline
1722700830,4,room-101,one entered
```

Use `date +%s` to get the Unix timestamp at each mark.

**Minimum sample size:** 20 intervals (10 minutes at 30 s). For a density breakdown, collect at
least 10 intervals in each band: low (1–3 people), medium (4–8), high (9+).

### 4.4 Scoring

```bash
python3 validation/analyze_results.py \
  --estimates validation/output/field_run.jsonl \
  --ground-truth validation/output/field_ground_truth.csv \
  --output validation/output/field_results.md
```

The script prints MAE, RMSE, 95% CI coverage, and mean bias, and writes a per-interval table.

### 4.5 Results Table (populate from your run)

| Scenario | Intervals | Ground truth (mean) | MAE | RMSE | CI coverage | Mean bias |
|----------|-----------|---------------------|-----|------|-------------|-----------|
| Low density (1–3) | | | | | | |
| Medium density (4–8) | | | | | | |
| High density (9+) | | | | | | |
| **Overall** | | | | | | |

---

## 5. Metric Definitions

For `n` matched intervals with estimate `ŷᵢ` and ground truth `yᵢ`:

```
MAE   = (1/n) Σ |ŷᵢ - yᵢ|
RMSE  = sqrt( (1/n) Σ (ŷᵢ - yᵢ)² )
Bias  = (1/n) Σ (ŷᵢ - yᵢ)
Cov   = fraction of intervals where lowerᵢ ≤ yᵢ ≤ upperᵢ
```

**How to read them.** MAE is the headline accuracy number in units of people. RMSE exceeding MAE
substantially indicates a few large errors rather than uniform drift. Bias separates systematic
over- or under-counting (which is fixable by tuning the devices-per-person ratio) from random
error (which is not). Coverage should approach 95%; materially lower means the Poisson interval
is too narrow for the real noise.

---

## 6. Threats to Validity

| Threat | Effect on results | Mitigation |
|--------|-------------------|------------|
| Static infrastructure devices (printers, APs, beacons) | Inflates counts with a constant offset | Record a baseline in an empty room and subtract |
| Observer error in dense scenes | Corrupts ground truth itself | Cap validation at counts a single observer can track reliably; use video for high density |
| Devices in pockets or bags | Attenuates RSSI, may fall below cutoff | Report the cutoff used; repeat with -90 dBm to quantify sensitivity |
| People carrying zero or multiple devices | Breaks the 1.25 ratio | Record the true device-per-person ratio for the cohort and recalibrate |
| MAC rotation during a session | Transiently inflates counts | Keep intervals short relative to rotation period; report TTL used |
| Adjacent-room leakage | Counts people who are not in the space | Note wall construction; compare against a shielded baseline |
| Single-site testing | Results do not generalise | State the site explicitly; do not extrapolate |

---

## 7. Reproducibility Checklist

- [ ] `make test` passes with 20/20 assertions
- [ ] `make check-memory` reports zero leaks
- [ ] `make analyze-validation` reproduces the Tier 2 table in Section 3
- [ ] Field run captured with ≥ 20 matched intervals
- [ ] Ground truth CSV committed alongside the estimates JSONL
- [ ] Section 4.5 table populated with real numbers
- [ ] Configuration recorded: RSSI cutoff, TTL, interval, dedup window, path loss exponent

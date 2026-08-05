# Contributing

## Before you start

```bash
make debug && make test && make check-memory
```

All three must pass before and after your change.

## C coding standards

**Language and build.** C11, no compiler extensions beyond `__attribute__((packed))` where a
wire format requires it. The build runs with warnings as errors:

```
-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion
-Wstrict-prototypes -Wmissing-prototypes
```

Never weaken a warning flag to make a build pass. Fix the code.

**Formatting.** 4-space indent, no tabs, 100-column limit, Linux brace style. Run `make format`
before committing; `.clang-format` is authoritative.

**Naming.**

| Kind | Convention | Example |
|------|-----------|---------|
| Public functions | `module_verb_noun` | `aggregator_prune_stale` |
| Public types | `pd_*_t` or `module_t` | `pd_device_record_t`, `ble_scanner_t` |
| Constants and macros | `PD_UPPER_SNAKE` | `PD_DEFAULT_TTL_SEC` |
| Static functions | lower_snake, no prefix | `parse_radiotap_rssi` |
| Struct fields | lower_snake | `last_seen` |

**Headers.** Every non-static function needs a prototype in the matching header — `-Wmissing-
prototypes` enforces this. Use include guards of the form `PERSON_DETECTOR_<NAME>_H`. Headers
include only what they need for their own declarations.

**Error handling.** Return `int` with `0` for success and `-1` for failure, or `NULL` for
allocation failures. Check every allocation. Log at `LOG_ERROR` before returning a failure that
ends an operation, and free everything already acquired on the error path. Do not fail silently
on the main path.

**Integer types.** Be explicit. `-Wconversion` and `-Wsign-compare` are on, so cast deliberately
rather than relying on implicit promotion, and cast to `size_t` when comparing against a size or
length. If you find yourself adding a cast to silence a warning, first confirm the underlying
types are actually right.

**Concurrency.** Anything reachable from more than one thread goes through the aggregator's
`pthread_rwlock_t`. Take the narrowest lock for the shortest time; never call into estimation or
I/O while holding a lock. Cross-thread flags are `volatile bool`, and signal handlers touch only
`volatile sig_atomic_t`.

**Memory.** Every allocation has exactly one owner and one free. `make check-memory` must report
zero leaks — this is a hard gate, not a guideline.

**Comments.** Explain why, not what. A comment should capture a constraint, a wire-format rule, or
a non-obvious trade-off. Do not narrate the code.

```c
/* Duplicate filtering stays off so repeat advertisements keep refreshing
   last_seen; otherwise still-present devices would age out via TTL. */
if (hci_le_set_scan_enable(dd, 0x01, 0x00, 1000) < 0) {
```

## Tests

Add a test to `tests/test_estimator.c` for any change to detection or aggregation logic. Each
test is a `static void test_*(void)` using `test_assert_true`, registered in `main`. Cover the
boundary, not just the happy path — the RSSI cutoff, TTL expiry, and empty-input cases all exist
because boundaries are where this code breaks.

## Python

`validation/` and `web/` follow PEP 8 with type hints and 100-column lines. `web/estimator.py` is
a deliberate port of `src/core/estimator.c`: if you change the algorithm in one, change it in the
other, and confirm both still produce the documented sample output.

```
people=3.20 [0.00, 6.71] devices=6 dedup=4 ble=3 wifi=3 randomized=4
```

## Documentation

Update the docs in the same commit as the behaviour change. Numbers quoted in documentation must
come from a command a reader can run. Never present simulated results as measured field data —
see `docs/VALIDATION_REPORT.md` for how the tiers are separated.

## Commits

Write a subject line in the imperative mood explaining why the change was needed. CI must be
green before merge.

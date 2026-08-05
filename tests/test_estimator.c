#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "core/aggregator.h"
#include "core/estimator.h"
#include "core/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

static void test_assert_true(const char *name, int condition)
{
    if (!condition) {
        (void)fprintf(stderr, "FAIL: %s\n", name);
        g_failures++;
    } else {
        (void)printf("PASS: %s\n", name);
    }
}

static void test_mac_randomization(void)
{
    pd_mac_t universal = { .bytes = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 } };
    pd_mac_t randomized = { .bytes = { 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE } };

    test_assert_true("universal MAC not locally administered",
                     !estimator_is_locally_administered(&universal));
    test_assert_true("randomized MAC locally administered",
                     estimator_is_locally_administered(&randomized));
}

static void test_distance_equation(void)
{
    double d = estimator_distance_m(-65, -59, PD_PATH_LOSS_EXPONENT);
    test_assert_true("distance positive for typical RSSI", d > 0.0 && d < 100.0);

    double weak = estimator_distance_m(-95, -59, PD_PATH_LOSS_EXPONENT);
    test_assert_true("weak RSSI yields larger distance", weak > d);
}

static void test_aggregator_ttl_pruning(void)
{
    pd_aggregator_t *agg = aggregator_create(1U);
    test_assert_true("aggregator created", agg != NULL);
    if (agg == NULL) {
        return;
    }

    pd_mac_t mac = { .bytes = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 } };
    test_assert_true("aggregator update succeeds",
                     aggregator_update(agg, &mac, PD_PROTO_BLE, -60, -59) == 0);

    test_assert_true("single active device", aggregator_device_count(agg) == 1U);

    sleep(2U);

    size_t pruned = aggregator_prune_stale(agg);
    test_assert_true("stale record pruned", pruned == 1U);
    test_assert_true("no devices remain", aggregator_device_count(agg) == 0U);

    aggregator_destroy(agg);
}

static void test_cross_protocol_deduplication(void)
{
    pd_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rssi_cutoff = PD_DEFAULT_RSSI_CUTOFF;
    cfg.dedup_window_ms = PD_DEFAULT_WINDOW_MS;

    struct timespec now;
    (void)clock_gettime(CLOCK_REALTIME, &now);

    pd_device_record_t records[2];
    memset(records, 0, sizeof(records));

    records[0].protocol = PD_PROTO_BLE;
    records[0].rssi = -62;
    records[0].tx_power = -59;
    records[0].last_seen = now;
    records[0].first_seen = now;

    records[1].protocol = PD_PROTO_WIFI;
    records[1].rssi = -64;
    records[1].tx_power = -59;
    records[1].last_seen = now;
    records[1].first_seen = now;

    pd_estimate_result_t result;
    test_assert_true("estimator compute succeeds",
                     estimator_compute(records, 2U, &cfg, &result) == 0);
    test_assert_true("cross-protocol pair deduplicated to one cluster",
                     result.deduplicated_count == 1U);
    test_assert_true("person estimate near one device/person ratio",
                     result.estimate >= 0.5 && result.estimate <= 1.5);
}

static void test_rssi_cutoff_exclusion(void)
{
    pd_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rssi_cutoff = PD_DEFAULT_RSSI_CUTOFF;
    cfg.dedup_window_ms = PD_DEFAULT_WINDOW_MS;

    struct timespec now;
    (void)clock_gettime(CLOCK_REALTIME, &now);

    pd_device_record_t records[1];
    memset(records, 0, sizeof(records));
    records[0].protocol = PD_PROTO_BLE;
    records[0].rssi = -90;
    records[0].tx_power = -59;
    records[0].last_seen = now;

    pd_estimate_result_t result;
    test_assert_true("weak RSSI excluded",
                     estimator_compute(records, 1U, &cfg, &result) == 0);
    test_assert_true("zero raw devices after cutoff", result.raw_device_count == 0U);
    test_assert_true("zero estimate for empty set", result.estimate == 0.0);
}

static void test_same_protocol_not_deduplicated(void)
{
    pd_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rssi_cutoff = PD_DEFAULT_RSSI_CUTOFF;
    cfg.dedup_window_ms = PD_DEFAULT_WINDOW_MS;

    struct timespec now;
    (void)clock_gettime(CLOCK_REALTIME, &now);

    pd_device_record_t records[2];
    memset(records, 0, sizeof(records));

    records[0].protocol = PD_PROTO_BLE;
    records[0].rssi = -62;
    records[0].tx_power = -59;
    records[0].last_seen = now;

    records[1].protocol = PD_PROTO_BLE;
    records[1].rssi = -63;
    records[1].tx_power = -59;
    records[1].last_seen = now;

    pd_estimate_result_t result;
    test_assert_true("same-protocol compute succeeds",
                     estimator_compute(records, 2U, &cfg, &result) == 0);
    test_assert_true("same-protocol devices remain separate",
                     result.deduplicated_count == 2U);
}

static void test_empty_aggregator_estimate(void)
{
    pd_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rssi_cutoff = PD_DEFAULT_RSSI_CUTOFF;
    cfg.dedup_window_ms = PD_DEFAULT_WINDOW_MS;

    pd_estimate_result_t result;
    test_assert_true("empty set compute succeeds",
                     estimator_compute(NULL, 0U, &cfg, &result) == 0);
    test_assert_true("empty estimate is zero", result.estimate == 0.0);
    test_assert_true("empty dedup count is zero", result.deduplicated_count == 0U);
}

int main(void)
{
    (void)printf("Running person_detector unit tests\n");

    test_mac_randomization();
    test_distance_equation();
    test_aggregator_ttl_pruning();
    test_cross_protocol_deduplication();
    test_rssi_cutoff_exclusion();
    test_same_protocol_not_deduplicated();
    test_empty_aggregator_estimate();

    if (g_failures > 0) {
        (void)fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    (void)printf("All tests passed\n");
    return EXIT_SUCCESS;
}

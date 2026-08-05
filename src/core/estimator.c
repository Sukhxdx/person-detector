#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "core/estimator.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool estimator_is_locally_administered(const pd_mac_t *mac)
{
    if (mac == NULL) {
        return false;
    }
    return (mac->bytes[0] & 0x02U) != 0U;
}

double estimator_distance_m(int8_t rssi, int8_t tx_power, double path_loss_n)
{
    if (path_loss_n <= 0.0) {
        return 0.0;
    }
    double exponent = ((double)tx_power - (double)rssi) / (10.0 * path_loss_n);
    return pow(10.0, exponent);
}

static int64_t timespec_diff_ms(const struct timespec *a, const struct timespec *b)
{
    int64_t sec = (int64_t)a->tv_sec - (int64_t)b->tv_sec;
    int64_t nsec = (int64_t)a->tv_nsec - (int64_t)b->tv_nsec;
    return sec * 1000LL + nsec / 1000000LL;
}

static bool records_within_window(const pd_device_record_t *a,
                                  const pd_device_record_t *b,
                                  uint32_t window_ms)
{
    int64_t diff = timespec_diff_ms(&a->last_seen, &b->last_seen);
    if (diff < 0) {
        diff = -diff;
    }
    return (uint64_t)diff <= (uint64_t)window_ms;
}

static bool rssi_signatures_match(int8_t a, int8_t b)
{
    int diff = (int)a - (int)b;
    if (diff < 0) {
        diff = -diff;
    }
    return diff <= PD_RSSI_MATCH_TOLERANCE;
}

static double poisson_lower_bound(double lambda)
{
    if (lambda <= 0.0) {
        return 0.0;
    }
    double bound = lambda - 1.96 * sqrt(lambda);
    return (bound < 0.0) ? 0.0 : bound;
}

static double poisson_upper_bound(double lambda)
{
    if (lambda <= 0.0) {
        return 0.0;
    }
    return lambda + 1.96 * sqrt(lambda);
}

int estimator_compute(const pd_device_record_t *records,
                      size_t count,
                      const pd_config_t *cfg,
                      pd_estimate_result_t *result)
{
    if (cfg == NULL || result == NULL) {
        return -1;
    }

    memset(result, 0, sizeof(*result));
    result->timestamp = time(NULL);

    if (records == NULL || count == 0U) {
        return 0;
    }

    bool *merged = calloc(count, sizeof(bool));
    bool *active = calloc(count, sizeof(bool));
    if (merged == NULL || active == NULL) {
        free(merged);
        free(active);
        return -1;
    }

    size_t valid_count = 0U;
    for (size_t i = 0; i < count; i++) {
        if (records[i].rssi < cfg->rssi_cutoff) {
            continue;
        }
        double dist = estimator_distance_m(records[i].rssi,
                                           records[i].tx_power,
                                           PD_PATH_LOSS_EXPONENT);
        if (dist > 30.0) {
            continue;
        }
        active[i] = true;
        valid_count++;
        if (records[i].protocol == PD_PROTO_BLE) {
            result->ble_count++;
        } else {
            result->wifi_count++;
        }
        if (records[i].locally_administered) {
            result->randomized_mac_count++;
        }
    }

    result->raw_device_count = (uint32_t)valid_count;

    /* Cross-protocol deduplication clusters. */
    size_t clusters = 0U;
    for (size_t i = 0; i < count; i++) {
        if (!active[i] || merged[i]) {
            continue;
        }
        clusters++;
        merged[i] = true;

        for (size_t j = i + 1; j < count; j++) {
            if (!active[j] || merged[j]) {
                continue;
            }
            if (records[i].protocol == records[j].protocol) {
                continue;
            }
            if (!records_within_window(&records[i], &records[j], cfg->dedup_window_ms)) {
                continue;
            }
            if (!rssi_signatures_match(records[i].rssi, records[j].rssi)) {
                continue;
            }
            merged[j] = true;
        }
    }

    result->deduplicated_count = (uint32_t)clusters;

    double lambda = (double)clusters / PD_DEVICES_PER_PERSON;
    result->estimate = lambda;
    result->lower_bound = poisson_lower_bound(lambda);
    result->upper_bound = poisson_upper_bound(lambda);

    free(merged);
    free(active);
    return 0;
}

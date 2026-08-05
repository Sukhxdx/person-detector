#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "ble/ble_scanner.h"
#include "core/aggregator.h"
#include "core/estimator.h"
#include "core/replay.h"
#include "core/types.h"
#include "utils/logger.h"
#include "wifi/wifi_scanner.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static pd_aggregator_t *g_aggregator = NULL;
static ble_scanner_t    g_ble_scanner;
static wifi_scanner_t   g_wifi_scanner;
static replay_feeder_t  g_replay_feeder;

static void signal_handler(int signo)
{
    (void)signo;
    g_running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
}

static void print_usage(const char *prog)
{
    (void)fprintf(stderr,
                  "Usage: %s [OPTIONS]\n"
                  "\n"
                  "Estimate nearby human count using BLE and Wi-Fi passive sensing.\n"
                  "\n"
                  "Options:\n"
                  "  --ble-dev DEV       HCI device name (default: hci0)\n"
                  "  --wifi-iface IFACE  Monitor mode interface (default: wlan0mon)\n"
                  "  --rssi-cutoff DBM   Minimum RSSI threshold (default: -85)\n"
                  "  --window MS         Cross-protocol dedup window (default: 500)\n"
                  "  --interval SEC      Output interval in seconds (default: 5)\n"
                  "  --ttl SEC           Device record TTL (default: 60)\n"
                  "  --json-out PATH     Append JSON telemetry to PATH\n"
                  "  --replay PATH       Replay JSONL traffic file (demo mode)\n"
                  "  --replay-loop       Loop replay file continuously\n"
                  "  --no-ble            Disable BLE scanner\n"
                  "  --no-wifi           Disable Wi-Fi scanner\n"
                  "  --verbose           Enable debug logging\n"
                  "  --help              Show this help message\n",
                  prog);
}

static void init_default_config(pd_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    (void)snprintf(cfg->ble_dev, sizeof(cfg->ble_dev), "hci0");
    (void)snprintf(cfg->wifi_iface, sizeof(cfg->wifi_iface), "wlan0mon");
    cfg->rssi_cutoff = PD_DEFAULT_RSSI_CUTOFF;
    cfg->dedup_window_ms = PD_DEFAULT_WINDOW_MS;
    cfg->output_interval_sec = (uint32_t)PD_DEFAULT_INTERVAL;
    cfg->ttl_sec = (uint32_t)PD_DEFAULT_TTL_SEC;
    cfg->verbose = false;
    cfg->json_enabled = false;
    cfg->replay_mode = false;
    cfg->replay_loop = false;
    cfg->ble_enabled = true;
    cfg->wifi_enabled = true;
    cfg->json_out[0] = '\0';
    cfg->replay_path[0] = '\0';
}

static int parse_arguments(int argc, char **argv, pd_config_t *cfg)
{
    static struct option long_opts[] = {
        {"ble-dev",     required_argument, NULL, 'b'},
        {"wifi-iface",  required_argument, NULL, 'w'},
        {"rssi-cutoff", required_argument, NULL, 'r'},
        {"window",      required_argument, NULL, 'd'},
        {"interval",    required_argument, NULL, 'i'},
        {"ttl",         required_argument, NULL, 't'},
        {"json-out",    required_argument, NULL, 'j'},
        {"replay",      required_argument, NULL, 'p'},
        {"replay-loop", no_argument,       NULL, 'l'},
        {"no-ble",      no_argument,       NULL, 'B'},
        {"no-wifi",     no_argument,       NULL, 'W'},
        {"verbose",     no_argument,       NULL, 'v'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:w:r:d:i:t:j:p:lvBWh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'b':
            (void)snprintf(cfg->ble_dev, sizeof(cfg->ble_dev), "%s", optarg);
            break;
        case 'w':
            (void)snprintf(cfg->wifi_iface, sizeof(cfg->wifi_iface), "%s", optarg);
            break;
        case 'r':
            cfg->rssi_cutoff = (int)strtol(optarg, NULL, 10);
            break;
        case 'd':
            cfg->dedup_window_ms = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'i':
            cfg->output_interval_sec = (uint32_t)strtoul(optarg, NULL, 10);
            if (cfg->output_interval_sec == 0U) {
                cfg->output_interval_sec = 1U;
            }
            break;
        case 't':
            cfg->ttl_sec = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'j':
            (void)snprintf(cfg->json_out, sizeof(cfg->json_out), "%s", optarg);
            cfg->json_enabled = true;
            break;
        case 'p':
            (void)snprintf(cfg->replay_path, sizeof(cfg->replay_path), "%s", optarg);
            cfg->replay_mode = true;
            break;
        case 'l':
            cfg->replay_loop = true;
            break;
        case 'B':
            cfg->ble_enabled = false;
            break;
        case 'W':
            cfg->wifi_enabled = false;
            break;
        case 'v':
            cfg->verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 1;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

static void emit_stdout_metrics(const pd_estimate_result_t *result)
{
    (void)printf("people=%.2f [%.2f, %.2f] devices=%u dedup=%u ble=%u wifi=%u randomized=%u ts=%ld\n",
                  result->estimate,
                  result->lower_bound,
                  result->upper_bound,
                  result->raw_device_count,
                  result->deduplicated_count,
                  result->ble_count,
                  result->wifi_count,
                  result->randomized_mac_count,
                  (long)result->timestamp);
    (void)fflush(stdout);
}

static int append_json_record(const pd_config_t *cfg, const pd_estimate_result_t *result)
{
    if (!cfg->json_enabled || cfg->json_out[0] == '\0') {
        return 0;
    }

    FILE *fp = fopen(cfg->json_out, "a");
    if (fp == NULL) {
        LOG_ERROR("Failed to open JSON output '%s': %s", cfg->json_out, strerror(errno));
        return -1;
    }

    (void)fprintf(fp,
                  "{\"timestamp\":%ld,"
                  "\"estimate\":%.3f,"
                  "\"lower_bound\":%.3f,"
                  "\"upper_bound\":%.3f,"
                  "\"raw_devices\":%u,"
                  "\"deduplicated_devices\":%u,"
                  "\"ble_count\":%u,"
                  "\"wifi_count\":%u,"
                  "\"randomized_mac_count\":%u}\n",
                  (long)result->timestamp,
                  result->estimate,
                  result->lower_bound,
                  result->upper_bound,
                  result->raw_device_count,
                  result->deduplicated_count,
                  result->ble_count,
                  result->wifi_count,
                  result->randomized_mac_count);

    (void)fclose(fp);
    return 0;
}

static void shutdown_scanners(void)
{
    replay_feeder_destroy(&g_replay_feeder);
    ble_scanner_destroy(&g_ble_scanner);
    wifi_scanner_destroy(&g_wifi_scanner);
}

static int run_estimation_cycle(const pd_config_t *cfg)
{
    (void)aggregator_prune_stale(g_aggregator);

    pd_device_record_t *records = NULL;
    size_t count = 0U;
    if (aggregator_snapshot(g_aggregator, &records, &count) < 0) {
        LOG_ERROR("Failed to snapshot aggregator state");
        return -1;
    }

    pd_estimate_result_t result;
    if (estimator_compute(records, count, cfg, &result) < 0) {
        LOG_ERROR("Estimator computation failed");
        aggregator_free_snapshot(records);
        return -1;
    }

    aggregator_free_snapshot(records);
    emit_stdout_metrics(&result);
    (void)append_json_record(cfg, &result);
    return 0;
}

int main(int argc, char **argv)
{
    pd_config_t cfg;
    init_default_config(&cfg);

    int parse_rc = parse_arguments(argc, argv, &cfg);
    if (parse_rc != 0) {
        return (parse_rc > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    logger_init(cfg.verbose ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
    install_signal_handlers();

    LOG_INFO("person_detector starting");
    if (cfg.replay_mode) {
        LOG_INFO("Replay mode: %s loop=%d", cfg.replay_path, cfg.replay_loop ? 1 : 0);
    } else {
        LOG_INFO("BLE device=%s Wi-Fi iface=%s RSSI cutoff=%d dBm window=%u ms interval=%u s",
                 cfg.ble_dev,
                 cfg.wifi_iface,
                 cfg.rssi_cutoff,
                 cfg.dedup_window_ms,
                 cfg.output_interval_sec);
    }

    g_aggregator = aggregator_create(cfg.ttl_sec);
    if (g_aggregator == NULL) {
        return EXIT_FAILURE;
    }

    if (cfg.replay_mode) {
        if (replay_feeder_init(&g_replay_feeder, cfg.replay_path, g_aggregator, cfg.replay_loop) < 0 ||
            replay_feeder_start(&g_replay_feeder) < 0) {
            LOG_ERROR("Replay feeder failed to start");
            aggregator_destroy(g_aggregator);
            return EXIT_FAILURE;
        }
    } else {
        if (cfg.ble_enabled) {
            if (ble_scanner_init(&g_ble_scanner, cfg.ble_dev, g_aggregator) < 0) {
                LOG_WARN("BLE scanner unavailable; continuing with Wi-Fi only");
            } else if (ble_scanner_start(&g_ble_scanner) < 0) {
                LOG_WARN("BLE scanner failed to start; continuing with Wi-Fi only");
                ble_scanner_destroy(&g_ble_scanner);
            }
        }

        if (cfg.wifi_enabled) {
            if (wifi_scanner_init(&g_wifi_scanner, cfg.wifi_iface, g_aggregator) < 0) {
                LOG_WARN("Wi-Fi scanner unavailable; continuing with BLE only");
            } else if (wifi_scanner_start(&g_wifi_scanner) < 0) {
                LOG_WARN("Wi-Fi scanner failed to start; continuing with BLE only");
                wifi_scanner_destroy(&g_wifi_scanner);
            }
        }
    }

    while (g_running) {
        for (uint32_t slept = 0; slept < cfg.output_interval_sec && g_running; slept++) {
            (void)sleep(1U);
        }
        if (!g_running) {
            break;
        }

        (void)run_estimation_cycle(&cfg);

        /* A non-looping replay is a finite job. Once the recording is consumed
           there is nothing further to observe, so report the final interval and
           exit instead of idling until the operator interrupts. */
        if (cfg.replay_mode && !cfg.replay_loop &&
            replay_feeder_is_finished(&g_replay_feeder)) {
            LOG_INFO("Replay complete; no further traffic to process");
            break;
        }
    }

    LOG_INFO("Shutting down person_detector");
    shutdown_scanners();
    aggregator_destroy(g_aggregator);
    g_aggregator = NULL;
    logger_shutdown();

    return EXIT_SUCCESS;
}

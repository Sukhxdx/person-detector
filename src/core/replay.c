#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "core/replay.h"
#include "utils/logger.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_mac_string(const char *str, pd_mac_t *mac)
{
    if (str == NULL || mac == NULL) {
        return -1;
    }

    unsigned int b[PD_MAC_LEN];
    int matched = sscanf(str,
                         "%x:%x:%x:%x:%x:%x",
                         &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (matched != 6) {
        return -1;
    }

    for (size_t i = 0; i < PD_MAC_LEN; i++) {
        mac->bytes[i] = (uint8_t)b[i];
    }
    return 0;
}

static pd_protocol_t parse_protocol(const char *json, const char *key_start)
{
    const char *p = strstr(json, key_start);
    if (p == NULL) {
        return PD_PROTO_BLE;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return PD_PROTO_BLE;
    }
    p++;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (strncmp(p, "\"wifi\"", 6) == 0) {
        return PD_PROTO_WIFI;
    }
    return PD_PROTO_BLE;
}

static int parse_device_field_int(const char *json, const char *field, int default_val)
{
    char pattern[64];
    (void)snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return default_val;
    }
    p += strlen(pattern);
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return (int)strtol(p, NULL, 10);
}

static int ingest_json_line(replay_feeder_t *feeder, const char *line)
{
    const char *mac_key = "\"mac\":";
    const char *mac_pos = strstr(line, mac_key);
    if (mac_pos == NULL) {
        return 0;
    }

    mac_pos += strlen(mac_key);
    while (*mac_pos != '\0' && (isspace((unsigned char)*mac_pos) || *mac_pos == '"')) {
        mac_pos++;
    }

    char mac_str[32];
    size_t i = 0U;
    while (mac_pos[i] != '\0' && mac_pos[i] != '"' && i < sizeof(mac_str) - 1U) {
        mac_str[i] = mac_pos[i];
        i++;
    }
    mac_str[i] = '\0';

    pd_mac_t mac;
    if (parse_mac_string(mac_str, &mac) < 0) {
        return 0;
    }

    pd_protocol_t proto = parse_protocol(line, "\"protocol\"");
    int rssi = parse_device_field_int(line, "rssi", PD_DEFAULT_RSSI_CUTOFF + 10);
    int tx_power = parse_device_field_int(line, "tx_power", PD_DEFAULT_TX_POWER);

    return aggregator_update(feeder->aggregator, &mac, proto, (int8_t)rssi, (int8_t)tx_power);
}

static int process_jsonl_file(replay_feeder_t *feeder, FILE *fp)
{
    char line[4096];
    int devices = 0;

    while (feeder->running && fgets(line, (int)sizeof(line), fp) != NULL) {
        if (line[0] == '\n' || line[0] == '\0') {
            continue;
        }

        const char *dev_key = "\"devices\"";
        const char *dev_pos = strstr(line, dev_key);
        if (dev_pos == NULL) {
            continue;
        }

        const char *cursor = dev_pos;
        while ((cursor = strstr(cursor, "\"mac\"")) != NULL) {
            const char *line_end = strchr(cursor, '\n');
            size_t slice_len = line_end != NULL ? (size_t)(line_end - cursor) : strlen(cursor);
            char slice[1024];
            if (slice_len >= sizeof(slice)) {
                slice_len = sizeof(slice) - 1U;
            }
            memcpy(slice, cursor, slice_len);
            slice[slice_len] = '\0';

            if (ingest_json_line(feeder, slice) == 0) {
                devices++;
            }
            cursor += 5;
        }

        usleep(200000U);
    }

    return devices;
}

static void *replay_feeder_thread(void *arg)
{
    replay_feeder_t *feeder = (replay_feeder_t *)arg;

    LOG_INFO("Replay feeder started: %s", feeder->path);

    do {
        FILE *fp = fopen(feeder->path, "r");
        if (fp == NULL) {
            LOG_ERROR("Cannot open replay file '%s': %s", feeder->path, strerror(errno));
            break;
        }

        (void)process_jsonl_file(feeder, fp);
        (void)fclose(fp);
    } while (feeder->running && feeder->loop);

    feeder->finished = true;
    LOG_INFO("Replay feeder exiting");
    return NULL;
}

bool replay_feeder_is_finished(const replay_feeder_t *feeder)
{
    return (feeder != NULL) && feeder->finished;
}

int replay_feeder_init(replay_feeder_t *feeder, const char *path, pd_aggregator_t *agg, bool loop)
{
    if (feeder == NULL || path == NULL || agg == NULL) {
        return -1;
    }

    memset(feeder, 0, sizeof(*feeder));
    feeder->aggregator = agg;
    feeder->loop = loop;
    (void)snprintf(feeder->path, sizeof(feeder->path), "%s", path);
    return 0;
}

int replay_feeder_start(replay_feeder_t *feeder)
{
    if (feeder == NULL) {
        return -1;
    }

    feeder->running = true;
    if (pthread_create(&feeder->thread, NULL, replay_feeder_thread, feeder) != 0) {
        feeder->running = false;
        LOG_ERROR("Failed to start replay feeder thread");
        return -1;
    }
    return 0;
}

void replay_feeder_stop(replay_feeder_t *feeder)
{
    if (feeder == NULL || !feeder->running) {
        return;
    }

    feeder->running = false;
    (void)pthread_join(feeder->thread, NULL);
}

void replay_feeder_destroy(replay_feeder_t *feeder)
{
    if (feeder == NULL) {
        return;
    }
    replay_feeder_stop(feeder);
}

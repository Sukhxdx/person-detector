#ifndef PERSON_DETECTOR_TYPES_H
#define PERSON_DETECTOR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define PD_MAC_LEN           6U
#define PD_HASH_LEN          8U
#define PD_DEFAULT_TTL_SEC   60
#define PD_DEFAULT_WINDOW_MS 500
#define PD_DEFAULT_INTERVAL  5
#define PD_DEFAULT_RSSI_CUTOFF (-85)
#define PD_DEFAULT_TX_POWER    (-59)
#define PD_PATH_LOSS_EXPONENT  2.7
#define PD_DEVICES_PER_PERSON  1.25
#define PD_RSSI_MATCH_TOLERANCE 4
#define PD_MAX_BLE_DEV         64
#define PD_MAX_WIFI_IFACE      32
#define PD_JSON_PATH_MAX       256
#define PD_REPLAY_PATH_MAX     512

typedef enum {
    PD_PROTO_BLE  = 0,
    PD_PROTO_WIFI = 1
} pd_protocol_t;

typedef struct {
    uint8_t bytes[PD_MAC_LEN];
} pd_mac_t;

typedef struct {
    uint8_t digest[PD_HASH_LEN];
} pd_mac_hash_t;

typedef struct {
    pd_mac_hash_t  hash;
    pd_protocol_t  protocol;
    int8_t         rssi;
    int8_t         tx_power;
    bool           locally_administered;
    struct timespec last_seen;
    struct timespec first_seen;
    uint32_t       observation_count;
} pd_device_record_t;

typedef struct {
    char     ble_dev[PD_MAX_BLE_DEV];
    char     wifi_iface[PD_MAX_WIFI_IFACE];
    int      rssi_cutoff;
    uint32_t dedup_window_ms;
    uint32_t output_interval_sec;
    uint32_t ttl_sec;
    char     json_out[PD_JSON_PATH_MAX];
    char     replay_path[PD_REPLAY_PATH_MAX];
    bool     verbose;
    bool     json_enabled;
    bool     replay_mode;
    bool     replay_loop;
    bool     ble_enabled;
    bool     wifi_enabled;
} pd_config_t;

typedef struct {
    double estimate;
    double lower_bound;
    double upper_bound;
    uint32_t raw_device_count;
    uint32_t deduplicated_count;
    uint32_t ble_count;
    uint32_t wifi_count;
    uint32_t randomized_mac_count;
    time_t   timestamp;
} pd_estimate_result_t;

#endif /* PERSON_DETECTOR_TYPES_H */

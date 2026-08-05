#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "ble/ble_scanner.h"
#include "utils/logger.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BLE_POLL_TIMEOUT_MS 500
#define BLE_SCAN_INTERVAL   0x0010U
#define BLE_SCAN_WINDOW     0x0010U

static int resolve_hci_device(const char *dev_name)
{
    if (dev_name == NULL || dev_name[0] == '\0') {
        return hci_get_route(NULL);
    }

    int dev_id = hci_devid(dev_name);
    if (dev_id < 0) {
        LOG_ERROR("Unknown BLE device '%s'", dev_name);
        return -1;
    }
    return dev_id;
}

static int configure_le_scan(int dd)
{
    if (hci_le_set_scan_parameters(dd,
                                   0x00, /* passive scan: listen only, never transmit */
                                   htobs(BLE_SCAN_INTERVAL),
                                   htobs(BLE_SCAN_WINDOW),
                                   0x00, /* public own address */
                                   0x00, /* accept all */
                                   1000) < 0) {
        LOG_ERROR("hci_le_set_scan_parameters failed: %s", strerror(errno));
        return -1;
    }

    /* Duplicate filtering stays off so repeat advertisements keep refreshing
       last_seen; otherwise still-present devices would age out via TTL. */
    if (hci_le_set_scan_enable(dd, 0x01, 0x00, 1000) < 0) {
        LOG_ERROR("hci_le_set_scan_enable failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int8_t parse_tx_power_from_ad(const uint8_t *data, uint8_t len)
{
    const size_t ad_len = (size_t)len;
    size_t offset = 0U;

    while (offset + 1U < ad_len) {
        size_t field_len = (size_t)data[offset];
        if (field_len == 0U) {
            break;
        }
        if (offset + field_len >= ad_len) {
            break;
        }
        uint8_t ad_type = data[offset + 1U];
        if (ad_type == 0x0AU && field_len >= 2U) {
            return (int8_t)data[offset + 2U];
        }
        offset += field_len + 1U;
    }

    return PD_DEFAULT_TX_POWER;
}

static void handle_le_advertising_report(ble_scanner_t *scanner,
                                         const uint8_t *data,
                                         uint8_t plen)
{
    if (scanner == NULL || data == NULL || plen < 1U) {
        return;
    }

    const size_t params_len = (size_t)plen;
    uint8_t num_reports = data[0];
    size_t offset = 1U;

    for (uint8_t r = 0; r < num_reports; r++) {
        /* Per-report header is 9 bytes (event type, address type, 6-byte address,
           data length) followed by the AD payload and a trailing RSSI byte. */
        if (offset + 10U > params_len) {
            break;
        }

        pd_mac_t mac;
        memcpy(mac.bytes, &data[offset + 2U], PD_MAC_LEN);

        size_t data_len = (size_t)data[offset + 8U];
        offset += 9U;

        if (offset + data_len + 1U > params_len) {
            break;
        }

        const uint8_t *ad_payload = &data[offset];
        int8_t tx_power = parse_tx_power_from_ad(ad_payload, (uint8_t)data_len);
        offset += data_len;

        int8_t rssi = (int8_t)data[offset];
        offset += 1U;

        (void)aggregator_update(scanner->aggregator,
                               &mac,
                               PD_PROTO_BLE,
                               rssi,
                               tx_power);
    }
}

static void process_hci_event(ble_scanner_t *scanner, const uint8_t *buf, ssize_t len)
{
    if (len < 3 || buf[0] != HCI_EVENT_PKT) {
        return;
    }

    uint8_t event = buf[1];
    uint8_t plen = buf[2];
    if ((ssize_t)(3 + plen) > len) {
        return;
    }

    const uint8_t *params = &buf[3];

    if (event != EVT_LE_META_EVENT || plen < 1U) {
        return;
    }

    uint8_t subevent = params[0];
    if (subevent != EVT_LE_ADVERTISING_REPORT) {
        return;
    }

    handle_le_advertising_report(scanner, &params[1], (uint8_t)(plen - 1U));
}

static void *ble_scanner_thread(void *arg)
{
    ble_scanner_t *scanner = (ble_scanner_t *)arg;
    uint8_t buf[HCI_MAX_EVENT_SIZE];

    LOG_INFO("BLE scanner thread started on %s", scanner->dev);

    while (scanner->running) {
        struct pollfd pfd;
        pfd.fd = scanner->hci_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, BLE_POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("BLE poll failed: %s", strerror(errno));
            break;
        }
        if (pr == 0) {
            continue;
        }

        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        ssize_t n = read(scanner->hci_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("BLE read failed: %s", strerror(errno));
            break;
        }

        process_hci_event(scanner, buf, n);
    }

    LOG_INFO("BLE scanner thread exiting");
    return NULL;
}

int ble_scanner_init(ble_scanner_t *scanner, const char *dev, pd_aggregator_t *agg)
{
    if (scanner == NULL || agg == NULL) {
        return -1;
    }

    memset(scanner, 0, sizeof(*scanner));
    scanner->aggregator = agg;
    scanner->hci_fd = -1;

    if (dev != NULL && dev[0] != '\0') {
        (void)snprintf(scanner->dev, sizeof(scanner->dev), "%s", dev);
    } else {
        (void)snprintf(scanner->dev, sizeof(scanner->dev), "hci0");
    }

    int dev_id = resolve_hci_device(scanner->dev);
    if (dev_id < 0) {
        return -1;
    }

    scanner->hci_fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (scanner->hci_fd < 0) {
        LOG_ERROR("Failed to open HCI socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_hci addr;
    memset(&addr, 0, sizeof(addr));
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = (uint16_t)dev_id;

    if (bind(scanner->hci_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind HCI socket: %s", strerror(errno));
        close(scanner->hci_fd);
        scanner->hci_fd = -1;
        return -1;
    }

    struct hci_filter filter;
    hci_filter_clear(&filter);
    hci_filter_set_ptype(HCI_EVENT_PKT, &filter);
    hci_filter_set_event(EVT_LE_META_EVENT, &filter);

    if (setsockopt(scanner->hci_fd, SOL_HCI, HCI_FILTER, &filter, sizeof(filter)) < 0) {
        LOG_ERROR("Failed to set HCI filter: %s", strerror(errno));
        close(scanner->hci_fd);
        scanner->hci_fd = -1;
        return -1;
    }

    int dd = hci_open_dev(dev_id);
    if (dd < 0) {
        LOG_ERROR("Failed to open HCI device: %s", strerror(errno));
        close(scanner->hci_fd);
        scanner->hci_fd = -1;
        return -1;
    }

    if (configure_le_scan(dd) < 0) {
        hci_close_dev(dd);
        close(scanner->hci_fd);
        scanner->hci_fd = -1;
        return -1;
    }

    hci_close_dev(dd);
    LOG_INFO("BLE scanner initialized on %s (dev_id=%d)", scanner->dev, dev_id);
    return 0;
}

int ble_scanner_start(ble_scanner_t *scanner)
{
    if (scanner == NULL || scanner->hci_fd < 0) {
        return -1;
    }

    scanner->running = true;
    if (pthread_create(&scanner->thread, NULL, ble_scanner_thread, scanner) != 0) {
        scanner->running = false;
        LOG_ERROR("Failed to create BLE scanner thread");
        return -1;
    }
    return 0;
}

void ble_scanner_stop(ble_scanner_t *scanner)
{
    if (scanner == NULL || !scanner->running) {
        return;
    }

    scanner->running = false;
    (void)pthread_join(scanner->thread, NULL);
}

void ble_scanner_destroy(ble_scanner_t *scanner)
{
    if (scanner == NULL) {
        return;
    }

    ble_scanner_stop(scanner);

    /* Disabling the scan belongs here rather than in stop(): init() enables
       scanning on the controller, so a scanner that was initialised but never
       started would otherwise leave the radio scanning after exit. */
    if (scanner->hci_fd >= 0) {
        int dev_id = hci_devid(scanner->dev);
        if (dev_id >= 0) {
            int dd = hci_open_dev(dev_id);
            if (dd >= 0) {
                (void)hci_le_set_scan_enable(dd, 0x00, 0x00, 1000);
                hci_close_dev(dd);
            }
        }

        close(scanner->hci_fd);
        scanner->hci_fd = -1;
    }
}

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "wifi/wifi_scanner.h"
#include "utils/logger.h"

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WIFI_POLL_TIMEOUT_MS 500

#define IEEE80211_FTYPE_MGMT      0x00
#define IEEE80211_STYPE_PROBE_REQ 0x04

/* Radiotap presence-bitmap bit assignments (www.radiotap.org). */
#define IEEE80211_RADIOTAP_DBM_ANTSIGNAL 5U
#define IEEE80211_RADIOTAP_EXT           31U

struct ieee80211_radiotap_header {
    uint8_t  it_version;
    uint8_t  it_pad;
    uint16_t it_len;
    uint32_t it_present;
} __attribute__((packed));

/* Each radiotap field is padded to its natural alignment measured from the start
   of the radiotap header, so offsets cannot be derived from sizes alone. An
   alignment of 0 marks a field this parser cannot skip over safely. */
struct radiotap_field {
    uint8_t align;
    uint8_t size;
};

static const struct radiotap_field radiotap_fields[] = {
    [0]  = { 8, 8 },   /* TSFT */
    [1]  = { 1, 1 },   /* FLAGS */
    [2]  = { 1, 1 },   /* RATE */
    [3]  = { 2, 4 },   /* CHANNEL */
    [4]  = { 2, 2 },   /* FHSS */
    [5]  = { 1, 1 },   /* DBM_ANTSIGNAL */
    [6]  = { 1, 1 },   /* DBM_ANTNOISE */
    [7]  = { 2, 2 },   /* LOCK_QUALITY */
    [8]  = { 2, 2 },   /* TX_ATTENUATION */
    [9]  = { 2, 2 },   /* DB_TX_ATTENUATION */
    [10] = { 1, 1 },   /* DBM_TX_POWER */
    [11] = { 1, 1 },   /* ANTENNA */
    [12] = { 1, 1 },   /* DB_ANTSIGNAL */
    [13] = { 1, 1 },   /* DB_ANTNOISE */
    [14] = { 2, 2 },   /* RX_FLAGS */
    [15] = { 2, 2 },   /* TX_FLAGS */
    [16] = { 1, 1 },   /* RTS_RETRIES */
    [17] = { 1, 1 },   /* DATA_RETRIES */
    [18] = { 0, 0 },   /* XCHANNEL: vendor-variable, stop here */
    [19] = { 1, 3 },   /* MCS */
    [20] = { 4, 8 },   /* AMPDU_STATUS */
    [21] = { 2, 12 },  /* VHT */
    [22] = { 8, 12 },  /* TIMESTAMP */
};

#define RADIOTAP_FIELD_COUNT (sizeof(radiotap_fields) / sizeof(radiotap_fields[0]))

/* Returns the radiotap header length on success (i.e. the offset of the 802.11
   frame), or -1 if no signal-strength field is present. */
static int parse_radiotap_rssi(const uint8_t *frame, size_t len, int8_t *rssi_out)
{
    if (len < sizeof(struct ieee80211_radiotap_header)) {
        return -1;
    }

    const struct ieee80211_radiotap_header *hdr =
        (const struct ieee80211_radiotap_header *)frame;

    uint16_t radiotap_len_raw;
    memcpy(&radiotap_len_raw, &hdr->it_len, sizeof(radiotap_len_raw));
    size_t radiotap_len = (size_t)le16toh(radiotap_len_raw);

    if (radiotap_len > len || radiotap_len < sizeof(*hdr)) {
        return -1;
    }

    uint32_t present_raw;
    memcpy(&present_raw, &hdr->it_present, sizeof(present_raw));
    uint32_t first_present = le32toh(present_raw);

    size_t offset = sizeof(*hdr);

    /* Bit 31 chains additional presence words in front of the field data. */
    uint32_t chain = first_present;
    while ((chain & (1U << IEEE80211_RADIOTAP_EXT)) != 0U) {
        if (offset + sizeof(uint32_t) > radiotap_len) {
            return -1;
        }
        uint32_t word;
        memcpy(&word, frame + offset, sizeof(word));
        chain = le32toh(word);
        offset += sizeof(uint32_t);
    }

    for (unsigned int bit = 0; bit < IEEE80211_RADIOTAP_EXT; bit++) {
        if ((first_present & (1U << bit)) == 0U) {
            continue;
        }
        if (bit >= RADIOTAP_FIELD_COUNT) {
            break;
        }

        size_t align = (size_t)radiotap_fields[bit].align;
        size_t size = (size_t)radiotap_fields[bit].size;
        if (align == 0U) {
            break;
        }

        size_t remainder = offset % align;
        if (remainder != 0U) {
            offset += align - remainder;
        }

        if (offset + size > radiotap_len) {
            break;
        }

        if (bit == IEEE80211_RADIOTAP_DBM_ANTSIGNAL) {
            *rssi_out = (int8_t)frame[offset];
            return (int)radiotap_len;
        }

        offset += size;
    }

    return -1;
}

static bool is_probe_request(const uint8_t *frame, size_t frame_len, pd_mac_t *mac_out)
{
    if (frame_len < 24U) {
        return false;
    }

    uint16_t fc = (uint16_t)((uint16_t)frame[0] | ((uint16_t)frame[1] << 8));
    uint8_t ftype = (uint8_t)((fc & 0x000CU) >> 2);
    uint8_t stype = (uint8_t)((fc & 0x00F0U) >> 4);

    if (ftype != IEEE80211_FTYPE_MGMT || stype != IEEE80211_STYPE_PROBE_REQ) {
        return false;
    }

    memcpy(mac_out->bytes, &frame[10], PD_MAC_LEN);
    return true;
}

static void process_wifi_frame(wifi_scanner_t *scanner, const uint8_t *buf, ssize_t len)
{
    int8_t rssi = PD_DEFAULT_TX_POWER;
    int radiotap_len = parse_radiotap_rssi(buf, (size_t)len, &rssi);
    if (radiotap_len < 0) {
        return;
    }

    const uint8_t *ieee80211 = buf + (size_t)radiotap_len;
    size_t ieee80211_len = (size_t)len - (size_t)radiotap_len;

    pd_mac_t mac;
    if (!is_probe_request(ieee80211, ieee80211_len, &mac)) {
        return;
    }

    (void)aggregator_update(scanner->aggregator,
                           &mac,
                           PD_PROTO_WIFI,
                           rssi,
                           PD_DEFAULT_TX_POWER);
}

static void *wifi_scanner_thread(void *arg)
{
    wifi_scanner_t *scanner = (wifi_scanner_t *)arg;
    uint8_t buf[4096];

    LOG_INFO("Wi-Fi scanner thread started on %s", scanner->iface);

    while (scanner->running) {
        struct pollfd pfd;
        pfd.fd = scanner->sock_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, WIFI_POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("Wi-Fi poll failed: %s", strerror(errno));
            break;
        }
        if (pr == 0) {
            continue;
        }

        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        ssize_t n = recv(scanner->sock_fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("Wi-Fi recv failed: %s", strerror(errno));
            break;
        }

        process_wifi_frame(scanner, buf, n);
    }

    LOG_INFO("Wi-Fi scanner thread exiting");
    return NULL;
}

int wifi_scanner_init(wifi_scanner_t *scanner, const char *iface, pd_aggregator_t *agg)
{
    if (scanner == NULL || iface == NULL || agg == NULL) {
        return -1;
    }

    memset(scanner, 0, sizeof(*scanner));
    scanner->aggregator = agg;
    scanner->sock_fd = -1;
    (void)snprintf(scanner->iface, sizeof(scanner->iface), "%s", iface);

    scanner->sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (scanner->sock_fd < 0) {
        LOG_ERROR("Failed to open AF_PACKET socket: %s", strerror(errno));
        return -1;
    }

    unsigned int ifindex = if_nametoindex(scanner->iface);
    if (ifindex == 0U) {
        LOG_ERROR("Unknown Wi-Fi interface '%s': %s", scanner->iface, strerror(errno));
        close(scanner->sock_fd);
        scanner->sock_fd = -1;
        return -1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = (int)ifindex;

    if (bind(scanner->sock_fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        LOG_ERROR("Failed to bind to interface '%s': %s", scanner->iface, strerror(errno));
        close(scanner->sock_fd);
        scanner->sock_fd = -1;
        return -1;
    }

    LOG_INFO("Wi-Fi scanner initialized on %s (ifindex=%u)", scanner->iface, ifindex);
    return 0;
}

int wifi_scanner_start(wifi_scanner_t *scanner)
{
    if (scanner == NULL || scanner->sock_fd < 0) {
        return -1;
    }

    scanner->running = true;
    if (pthread_create(&scanner->thread, NULL, wifi_scanner_thread, scanner) != 0) {
        scanner->running = false;
        LOG_ERROR("Failed to create Wi-Fi scanner thread");
        return -1;
    }
    return 0;
}

void wifi_scanner_stop(wifi_scanner_t *scanner)
{
    if (scanner == NULL || !scanner->running) {
        return;
    }

    scanner->running = false;
    (void)pthread_join(scanner->thread, NULL);
}

void wifi_scanner_destroy(wifi_scanner_t *scanner)
{
    if (scanner == NULL) {
        return;
    }

    wifi_scanner_stop(scanner);

    if (scanner->sock_fd >= 0) {
        close(scanner->sock_fd);
        scanner->sock_fd = -1;
    }
}

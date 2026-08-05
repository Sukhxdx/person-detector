#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "core/aggregator.h"
#include "core/estimator.h"
#include "utils/logger.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PD_AGG_BUCKETS 1024U

typedef struct pd_agg_node {
    pd_device_record_t record;
    struct pd_agg_node *next;
} pd_agg_node_t;

struct pd_aggregator {
    pd_agg_node_t    *buckets[PD_AGG_BUCKETS];
    uint32_t          ttl_sec;
    size_t            count;
    pthread_rwlock_t  lock;
};

/* pthread requires a mutable lock handle even for read-only access, so the
   const-correct public API casts here rather than dropping const everywhere. */
static pthread_rwlock_t *agg_lock(const pd_aggregator_t *agg)
{
    return (pthread_rwlock_t *)(uintptr_t)(const void *)&agg->lock;
}

static uint32_t hash_mac_digest(const pd_mac_hash_t *h)
{
    uint32_t acc = 2166136261U;
    for (size_t i = 0; i < PD_HASH_LEN; i++) {
        acc ^= h->digest[i];
        acc *= 16777619U;
    }
    return acc % PD_AGG_BUCKETS;
}

static void compute_mac_hash(const pd_mac_t *mac, pd_mac_hash_t *out)
{
    /* FNV-1a 64-bit folded digest for anonymized device identity. */
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < PD_MAC_LEN; i++) {
        hash ^= (uint64_t)mac->bytes[i];
        hash *= 1099511628211ULL;
    }
    for (size_t i = 0; i < PD_HASH_LEN; i++) {
        out->digest[i] = (uint8_t)((hash >> (i * 8U)) & 0xFFU);
    }
}

static pd_agg_node_t *find_node(pd_agg_node_t *head, const pd_mac_hash_t *hash)
{
    while (head != NULL) {
        if (memcmp(&head->record.hash, hash, sizeof(pd_mac_hash_t)) == 0) {
            return head;
        }
        head = head->next;
    }
    return NULL;
}

pd_aggregator_t *aggregator_create(uint32_t ttl_sec)
{
    pd_aggregator_t *agg = calloc(1, sizeof(*agg));
    if (agg == NULL) {
        LOG_ERROR("Failed to allocate aggregator: %s", strerror(errno));
        return NULL;
    }

    agg->ttl_sec = ttl_sec;
    if (pthread_rwlock_init(&agg->lock, NULL) != 0) {
        LOG_ERROR("Failed to init aggregator rwlock");
        free(agg);
        return NULL;
    }

    return agg;
}

void aggregator_destroy(pd_aggregator_t *agg)
{
    if (agg == NULL) {
        return;
    }

    for (size_t i = 0; i < PD_AGG_BUCKETS; i++) {
        pd_agg_node_t *node = agg->buckets[i];
        while (node != NULL) {
            pd_agg_node_t *next = node->next;
            free(node);
            node = next;
        }
    }

    (void)pthread_rwlock_destroy(&agg->lock);
    free(agg);
}

int aggregator_update(pd_aggregator_t *agg,
                      const pd_mac_t *mac,
                      pd_protocol_t protocol,
                      int8_t rssi,
                      int8_t tx_power)
{
    if (agg == NULL || mac == NULL) {
        return -1;
    }

    pd_mac_hash_t hash;
    compute_mac_hash(mac, &hash);

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }

    bool locally_admin = estimator_is_locally_administered(mac);
    uint32_t bucket = hash_mac_digest(&hash);

    (void)pthread_rwlock_wrlock(&agg->lock);

    pd_agg_node_t *node = find_node(agg->buckets[bucket], &hash);
    if (node != NULL) {
        node->record.rssi = rssi;
        node->record.tx_power = tx_power;
        node->record.last_seen = now;
        node->record.observation_count++;
        if (node->record.protocol != protocol) {
            /* Retain earliest protocol but note cross-protocol sighting via count. */
            node->record.observation_count++;
        }
    } else {
        node = calloc(1, sizeof(*node));
        if (node == NULL) {
            (void)pthread_rwlock_unlock(&agg->lock);
            LOG_ERROR("Failed to allocate device node");
            return -1;
        }
        node->record.hash = hash;
        node->record.protocol = protocol;
        node->record.rssi = rssi;
        node->record.tx_power = tx_power;
        node->record.locally_administered = locally_admin;
        node->record.first_seen = now;
        node->record.last_seen = now;
        node->record.observation_count = 1U;
        node->next = agg->buckets[bucket];
        agg->buckets[bucket] = node;
        agg->count++;
    }

    (void)pthread_rwlock_unlock(&agg->lock);
    return 0;
}

size_t aggregator_device_count(const pd_aggregator_t *agg)
{
    if (agg == NULL) {
        return 0U;
    }

    (void)pthread_rwlock_rdlock(agg_lock(agg));
    size_t count = agg->count;
    (void)pthread_rwlock_unlock(agg_lock(agg));
    return count;
}

int aggregator_snapshot(const pd_aggregator_t *agg,
                        pd_device_record_t **out_records,
                        size_t *out_count)
{
    if (agg == NULL || out_records == NULL || out_count == NULL) {
        return -1;
    }

    (void)pthread_rwlock_rdlock(agg_lock(agg));

    if (agg->count == 0U) {
        *out_records = NULL;
        *out_count = 0U;
        (void)pthread_rwlock_unlock(agg_lock(agg));
        return 0;
    }

    pd_device_record_t *records = calloc(agg->count, sizeof(*records));
    if (records == NULL) {
        (void)pthread_rwlock_unlock(agg_lock(agg));
        return -1;
    }

    size_t idx = 0U;
    for (size_t b = 0; b < PD_AGG_BUCKETS; b++) {
        const pd_agg_node_t *node = agg->buckets[b];
        while (node != NULL) {
            records[idx++] = node->record;
            node = node->next;
        }
    }

    (void)pthread_rwlock_unlock(agg_lock(agg));

    *out_records = records;
    *out_count = idx;
    return 0;
}

void aggregator_free_snapshot(pd_device_record_t *records)
{
    free(records);
}

size_t aggregator_prune_stale(pd_aggregator_t *agg)
{
    if (agg == NULL) {
        return 0U;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0U;
    }

    size_t pruned = 0U;

    (void)pthread_rwlock_wrlock(&agg->lock);

    for (size_t b = 0; b < PD_AGG_BUCKETS; b++) {
        pd_agg_node_t **cursor = &agg->buckets[b];
        while (*cursor != NULL) {
            double age_sec = difftime(now.tv_sec, (*cursor)->record.last_seen.tv_sec);
            if (age_sec > (double)agg->ttl_sec) {
                pd_agg_node_t *dead = *cursor;
                *cursor = dead->next;
                free(dead);
                if (agg->count > 0U) {
                    agg->count--;
                }
                pruned++;
            } else {
                cursor = &(*cursor)->next;
            }
        }
    }

    (void)pthread_rwlock_unlock(&agg->lock);

    if (pruned > 0U) {
        LOG_DEBUG("Pruned %zu stale device records", pruned);
    }

    return pruned;
}

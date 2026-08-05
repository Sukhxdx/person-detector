#ifndef PERSON_DETECTOR_AGGREGATOR_H
#define PERSON_DETECTOR_AGGREGATOR_H

#include "core/types.h"
#include <stddef.h>

typedef struct pd_aggregator pd_aggregator_t;

pd_aggregator_t *aggregator_create(uint32_t ttl_sec);
void aggregator_destroy(pd_aggregator_t *agg);

int aggregator_update(pd_aggregator_t *agg,
                      const pd_mac_t *mac,
                      pd_protocol_t protocol,
                      int8_t rssi,
                      int8_t tx_power);

size_t aggregator_device_count(const pd_aggregator_t *agg);

int aggregator_snapshot(const pd_aggregator_t *agg,
                        pd_device_record_t **out_records,
                        size_t *out_count);

void aggregator_free_snapshot(pd_device_record_t *records);

size_t aggregator_prune_stale(pd_aggregator_t *agg);

#endif /* PERSON_DETECTOR_AGGREGATOR_H */

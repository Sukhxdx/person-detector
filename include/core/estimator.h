#ifndef PERSON_DETECTOR_ESTIMATOR_H
#define PERSON_DETECTOR_ESTIMATOR_H

#include "core/types.h"
#include <stddef.h>

bool estimator_is_locally_administered(const pd_mac_t *mac);

double estimator_distance_m(int8_t rssi, int8_t tx_power, double path_loss_n);

int estimator_compute(const pd_device_record_t *records,
                      size_t count,
                      const pd_config_t *cfg,
                      pd_estimate_result_t *result);

#endif /* PERSON_DETECTOR_ESTIMATOR_H */

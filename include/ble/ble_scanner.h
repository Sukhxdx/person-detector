#ifndef PERSON_DETECTOR_BLE_SCANNER_H
#define PERSON_DETECTOR_BLE_SCANNER_H

#include "core/aggregator.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    char              dev[PD_MAX_BLE_DEV];
    pd_aggregator_t  *aggregator;
    int               hci_fd;
    volatile bool     running;
    pthread_t         thread;
} ble_scanner_t;

int  ble_scanner_init(ble_scanner_t *scanner, const char *dev, pd_aggregator_t *agg);
int  ble_scanner_start(ble_scanner_t *scanner);
void ble_scanner_stop(ble_scanner_t *scanner);
void ble_scanner_destroy(ble_scanner_t *scanner);

#endif /* PERSON_DETECTOR_BLE_SCANNER_H */

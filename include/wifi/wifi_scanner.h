#ifndef PERSON_DETECTOR_WIFI_SCANNER_H
#define PERSON_DETECTOR_WIFI_SCANNER_H

#include "core/aggregator.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    char              iface[PD_MAX_WIFI_IFACE];
    pd_aggregator_t  *aggregator;
    int               sock_fd;
    volatile bool     running;
    pthread_t         thread;
} wifi_scanner_t;

int  wifi_scanner_init(wifi_scanner_t *scanner, const char *iface, pd_aggregator_t *agg);
int  wifi_scanner_start(wifi_scanner_t *scanner);
void wifi_scanner_stop(wifi_scanner_t *scanner);
void wifi_scanner_destroy(wifi_scanner_t *scanner);

#endif /* PERSON_DETECTOR_WIFI_SCANNER_H */

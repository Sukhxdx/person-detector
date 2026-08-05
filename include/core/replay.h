#ifndef PERSON_DETECTOR_REPLAY_H
#define PERSON_DETECTOR_REPLAY_H

#include "core/aggregator.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    char              path[PD_REPLAY_PATH_MAX];
    pd_aggregator_t  *aggregator;
    volatile bool     running;
    bool              loop;
    pthread_t         thread;
} replay_feeder_t;

int  replay_feeder_init(replay_feeder_t *feeder, const char *path, pd_aggregator_t *agg, bool loop);
int  replay_feeder_start(replay_feeder_t *feeder);
void replay_feeder_stop(replay_feeder_t *feeder);
void replay_feeder_destroy(replay_feeder_t *feeder);

#endif /* PERSON_DETECTOR_REPLAY_H */

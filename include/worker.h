#ifndef CXL_NUMA_CSMA_WORKER_H
#define CXL_NUMA_CSMA_WORKER_H

#include "config.h"
#include "cxl_fabric.h"
#include "numa_backend.h"
#include "stats.h"

#include <stdatomic.h>

#define MAX_LAT_PER_THREAD 200000

typedef struct worker_arg {
    const config_t *cfg;
    cxl_fabric_t *fabric;
    unsigned int rng;
    thread_stats_t *stats;
} worker_arg_t;

extern atomic_int stop_flag;

void *worker_main(void *p);

#endif

#ifndef CXL_NUMA_CSMA_WORKER_H
#define CXL_NUMA_CSMA_WORKER_H

#include "config.h"
#include "cxl_fabric.h"
#include "numa_backend.h"
#include "stats.h"

#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

#define MAX_LAT_PER_THREAD 200000
#define AIMD_CWND_MAX 64

typedef struct {
    pthread_mutex_t lock;
    int cwnd;
    int inflight;
    int reservations;
    int acks_since_increase;
    int max_cwnd;
    uint64_t max_inflight;
    uint64_t last_decrease_ns;
    uint64_t decrease_cooldown_ns;
    uint64_t last_update_ns;
    long double cwnd_time_ns;
    long double inflight_time_ns;
    uint64_t observation_ns;
} aimd_controller_t;

typedef struct {
    double avg_cwnd;
    double avg_inflight;
    uint64_t max_inflight;
    int current_cwnd;
    int current_inflight;
} aimd_controller_metrics_t;

typedef struct worker_arg {
    const config_t *cfg;
    cxl_fabric_t *fabric;
    aimd_controller_t *aimd;
    unsigned int rng;
    thread_stats_t *stats;
} worker_arg_t;

extern atomic_int stop_flag;

int aimd_controller_init(aimd_controller_t *controller,
                         int initial_cwnd,
                         int max_cwnd,
                         uint64_t decrease_cooldown_ns);
void aimd_controller_destroy(aimd_controller_t *controller);
void aimd_controller_get_metrics(aimd_controller_t *controller,
                                 aimd_controller_metrics_t *metrics);

void *worker_main(void *p);

#endif

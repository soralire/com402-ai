#ifndef CXL_NUMA_CSMA_CXL_FABRIC_H
#define CXL_NUMA_CSMA_CXL_FABRIC_H

#include "numa_backend.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct cxl_request {
    struct cxl_request *next;
    pthread_mutex_t lock;
    pthread_cond_t done_cond;
    int done;
    uint64_t submit_ns;
    uint64_t complete_ns;
    unsigned int rng;
    int touches_per_req;
} cxl_request_t;

typedef struct {
    memory_region_t *region;
    int queue_depth;
    int device_workers;
    sem_t credits;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_not_empty;
    cxl_request_t *head;
    cxl_request_t *tail;
    pthread_t *workers;
    atomic_int stop;
} cxl_fabric_t;

int cxl_fabric_init(cxl_fabric_t *fabric,
                    memory_region_t *region,
                    int queue_depth,
                    int device_workers);
void cxl_fabric_destroy(cxl_fabric_t *fabric);

cxl_request_t *cxl_request_create(unsigned int rng, int touches_per_req);
void cxl_request_destroy(cxl_request_t *req);

int cxl_fabric_submit_blocking(cxl_fabric_t *fabric, cxl_request_t *req);
int cxl_fabric_submit_try(cxl_fabric_t *fabric, cxl_request_t *req);

int cxl_request_is_done(cxl_request_t *req);
uint64_t cxl_request_wait(cxl_request_t *req);

#endif

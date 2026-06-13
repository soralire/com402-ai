#include "cxl_fabric.h"
#include "utils.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int wait_for_credit(sem_t *credits) {
    while (sem_wait(credits) != 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static void enqueue_request(cxl_fabric_t *fabric, cxl_request_t *req) {
    req->next = NULL;

    pthread_mutex_lock(&fabric->queue_lock);
    if (fabric->tail) {
        fabric->tail->next = req;
    } else {
        fabric->head = req;
    }
    fabric->tail = req;
    pthread_cond_signal(&fabric->queue_not_empty);
    pthread_mutex_unlock(&fabric->queue_lock);
}

static cxl_request_t *dequeue_request(cxl_fabric_t *fabric) {
    cxl_request_t *req = fabric->head;
    if (!req) {
        return NULL;
    }

    fabric->head = req->next;
    if (!fabric->head) {
        fabric->tail = NULL;
    }
    req->next = NULL;
    return req;
}

static void complete_request(cxl_fabric_t *fabric, cxl_request_t *req) {
    uint64_t complete_ns = now_ns();

    pthread_mutex_lock(&req->lock);
    req->complete_ns = complete_ns;
    req->done = 1;
    pthread_cond_signal(&req->done_cond);
    pthread_mutex_unlock(&req->lock);

    sem_post(&fabric->credits);
}

static void *device_worker_main(void *p) {
    cxl_fabric_t *fabric = (cxl_fabric_t *)p;

    for (;;) {
        pthread_mutex_lock(&fabric->queue_lock);
        while (!fabric->head && !atomic_load_explicit(&fabric->stop, memory_order_relaxed)) {
            pthread_cond_wait(&fabric->queue_not_empty, &fabric->queue_lock);
        }

        cxl_request_t *req = dequeue_request(fabric);
        int should_stop = atomic_load_explicit(&fabric->stop, memory_order_relaxed);
        pthread_mutex_unlock(&fabric->queue_lock);

        if (!req) {
            if (should_stop) {
                break;
            }
            continue;
        }

        /*
         * Device service threads model the Type-3 Memory Device controller.
         * The request holds a CXL fabric credit until its completion returns.
         */
        memory_region_request(fabric->region, &req->rng, req->touches_per_req);
        complete_request(fabric, req);
    }

    return NULL;
}

int cxl_fabric_init(cxl_fabric_t *fabric,
                    memory_region_t *region,
                    int queue_depth,
                    int device_workers) {
    memset(fabric, 0, sizeof(*fabric));
    fabric->region = region;
    fabric->queue_depth = queue_depth;
    fabric->device_workers = device_workers;
    atomic_init(&fabric->stop, 0);

    if (sem_init(&fabric->credits, 0, (unsigned int)queue_depth) != 0) {
        return -1;
    }

    if (pthread_mutex_init(&fabric->queue_lock, NULL) != 0) {
        sem_destroy(&fabric->credits);
        return -1;
    }

    if (pthread_cond_init(&fabric->queue_not_empty, NULL) != 0) {
        pthread_mutex_destroy(&fabric->queue_lock);
        sem_destroy(&fabric->credits);
        return -1;
    }

    fabric->workers = calloc((size_t)device_workers, sizeof(*fabric->workers));
    if (!fabric->workers) {
        pthread_cond_destroy(&fabric->queue_not_empty);
        pthread_mutex_destroy(&fabric->queue_lock);
        sem_destroy(&fabric->credits);
        return -1;
    }

    for (int i = 0; i < device_workers; i++) {
        int rc = pthread_create(&fabric->workers[i], NULL, device_worker_main, fabric);
        if (rc != 0) {
            atomic_store_explicit(&fabric->stop, 1, memory_order_relaxed);
            pthread_cond_broadcast(&fabric->queue_not_empty);
            for (int j = 0; j < i; j++) {
                pthread_join(fabric->workers[j], NULL);
            }
            free(fabric->workers);
            fabric->workers = NULL;
            pthread_cond_destroy(&fabric->queue_not_empty);
            pthread_mutex_destroy(&fabric->queue_lock);
            sem_destroy(&fabric->credits);
            errno = rc;
            return -1;
        }
    }

    return 0;
}

void cxl_fabric_destroy(cxl_fabric_t *fabric) {
    if (!fabric) {
        return;
    }

    atomic_store_explicit(&fabric->stop, 1, memory_order_relaxed);
    pthread_mutex_lock(&fabric->queue_lock);
    pthread_cond_broadcast(&fabric->queue_not_empty);
    pthread_mutex_unlock(&fabric->queue_lock);

    for (int i = 0; i < fabric->device_workers; i++) {
        pthread_join(fabric->workers[i], NULL);
    }

    free(fabric->workers);
    fabric->workers = NULL;
    pthread_cond_destroy(&fabric->queue_not_empty);
    pthread_mutex_destroy(&fabric->queue_lock);
    sem_destroy(&fabric->credits);
}

cxl_request_t *cxl_request_create(unsigned int rng, int touches_per_req) {
    cxl_request_t *req = calloc(1, sizeof(*req));
    if (!req) {
        return NULL;
    }

    if (pthread_mutex_init(&req->lock, NULL) != 0) {
        free(req);
        return NULL;
    }

    if (pthread_cond_init(&req->done_cond, NULL) != 0) {
        pthread_mutex_destroy(&req->lock);
        free(req);
        return NULL;
    }

    req->submit_ns = now_ns();
    req->rng = rng;
    req->touches_per_req = touches_per_req;
    return req;
}

void cxl_request_destroy(cxl_request_t *req) {
    if (!req) {
        return;
    }

    pthread_cond_destroy(&req->done_cond);
    pthread_mutex_destroy(&req->lock);
    free(req);
}

int cxl_fabric_submit_blocking(cxl_fabric_t *fabric, cxl_request_t *req) {
    if (wait_for_credit(&fabric->credits) != 0) {
        return -1;
    }

    enqueue_request(fabric, req);
    return 0;
}

int cxl_fabric_submit_try(cxl_fabric_t *fabric, cxl_request_t *req) {
    if (sem_trywait(&fabric->credits) != 0) {
        return -1;
    }

    enqueue_request(fabric, req);
    return 0;
}

int cxl_request_is_done(cxl_request_t *req) {
    pthread_mutex_lock(&req->lock);
    int done = req->done;
    pthread_mutex_unlock(&req->lock);
    return done;
}

uint64_t cxl_request_wait(cxl_request_t *req) {
    pthread_mutex_lock(&req->lock);
    while (!req->done) {
        pthread_cond_wait(&req->done_cond, &req->lock);
    }
    uint64_t latency = req->complete_ns - req->submit_ns;
    pthread_mutex_unlock(&req->lock);
    return latency;
}

#include "worker.h"
#include "utils.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define AIMD_CWND_MAX 64

atomic_int stop_flag = 0;

static int stop_requested(void) {
    return atomic_load_explicit(&stop_flag, memory_order_relaxed);
}

static void load_sleep(int load, unsigned int *rng) {
    if (load < 100) {
        int idle_us = (100 - load) * 20;
        if (idle_us > 0) {
            usleep(xorshift32(rng) % (unsigned int)idle_us);
        }
    }
}

static void random_backoff(unsigned int *rng, int *window_us, int max_us) {
    usleep(xorshift32(rng) % (unsigned int)(*window_us));

    if (*window_us < max_us) {
        *window_us *= 2;
        if (*window_us > max_us) {
            *window_us = max_us;
        }
    }
}

static cxl_request_t *new_request(worker_arg_t *arg) {
    const config_t *cfg = arg->cfg;
    unsigned int req_rng = xorshift32(&arg->rng);
    return cxl_request_create(req_rng, cfg->touches_per_req);
}

static void finish_request(thread_stats_t *stats, cxl_request_t *req) {
    uint64_t latency_ns = cxl_request_wait(req);
    stats->success++;
    thread_stats_record_latency(stats, latency_ns);
    cxl_request_destroy(req);
}

static void run_random(worker_arg_t *arg) {
    thread_stats_t *stats = arg->stats;

    while (!stop_requested()) {
        load_sleep(arg->cfg->load, &arg->rng);

        cxl_request_t *req = new_request(arg);
        if (!req) {
            continue;
        }

        stats->attempts++;
        thread_stats_record_window(stats, 1, 1);

        /*
         * Random uses blocking credit acquisition. It keeps completion rate high
         * but includes fabric credit stalls in request latency.
         */
        if (cxl_fabric_submit_blocking(arg->fabric, req) != 0) {
            cxl_request_destroy(req);
            continue;
        }

        finish_request(stats, req);
    }
}

static void run_csma(worker_arg_t *arg) {
    thread_stats_t *stats = arg->stats;
    int backoff_window_us = 20;
    const int backoff_max_us = 2000;

    while (!stop_requested()) {
        load_sleep(arg->cfg->load, &arg->rng);

        cxl_request_t *req = new_request(arg);
        if (!req) {
            continue;
        }

        stats->attempts++;

        /*
         * CSMA-like is a non-blocking credit baseline: if the CXL Switch /
         * Type-3 queue is full, it backs off instead of waiting in the queue.
         */
        if (cxl_fabric_submit_try(arg->fabric, req) == 0) {
            thread_stats_record_window(stats, 1, 1);
            finish_request(stats, req);
            backoff_window_us = 20;
        } else {
            cxl_request_destroy(req);
            stats->retry++;
            stats->backoff++;
            thread_stats_record_window(stats, 1, 0);
            random_backoff(&arg->rng, &backoff_window_us, backoff_max_us);
        }
    }
}

static int reap_completed(cxl_request_t **inflight, int *count, thread_stats_t *stats) {
    int completed = 0;

    for (int i = 0; i < *count;) {
        if (!cxl_request_is_done(inflight[i])) {
            i++;
            continue;
        }

        finish_request(stats, inflight[i]);
        completed++;

        for (int j = i + 1; j < *count; j++) {
            inflight[j - 1] = inflight[j];
        }
        (*count)--;
    }

    return completed;
}

static int wait_one_completion(cxl_request_t **inflight, int *count, thread_stats_t *stats) {
    if (*count <= 0) {
        return 0;
    }

    finish_request(stats, inflight[0]);

    for (int i = 1; i < *count; i++) {
        inflight[i - 1] = inflight[i];
    }
    (*count)--;
    return 1;
}

static void update_aimd_on_completion(int completed, int *cwnd, int *acks_since_increase) {
    for (int i = 0; i < completed; i++) {
        (*acks_since_increase)++;
        if (*acks_since_increase >= *cwnd) {
            if (*cwnd < AIMD_CWND_MAX) {
                (*cwnd)++;
            }
            *acks_since_increase = 0;
        }
    }
}

static void run_aimd(worker_arg_t *arg) {
    thread_stats_t *stats = arg->stats;
    cxl_request_t *inflight[AIMD_CWND_MAX];
    int inflight_count = 0;
    int cwnd = 1;
    int acks_since_increase = 0;
    int backoff_window_us = 20;
    const int backoff_max_us = 2000;

    while (!stop_requested()) {
        load_sleep(arg->cfg->load, &arg->rng);

        int completed = reap_completed(inflight, &inflight_count, stats);
        update_aimd_on_completion(completed, &cwnd, &acks_since_increase);

        /*
         * AIMD now controls a real per-worker outstanding request window. The
         * CXL fabric credits still enforce the global queue_depth, so cwnd can
         * grow only when the shared Switch / Type-3 queue can absorb requests.
         */
        while (inflight_count < cwnd && !stop_requested()) {
            cxl_request_t *req = new_request(arg);
            if (!req) {
                break;
            }

            stats->attempts++;
            if (cxl_fabric_submit_try(arg->fabric, req) == 0) {
                inflight[inflight_count++] = req;
                thread_stats_record_window(stats, cwnd, inflight_count);
                continue;
            }

            cxl_request_destroy(req);
            stats->retry++;
            stats->backoff++;
            cwnd /= 2;
            if (cwnd < 1) {
                cwnd = 1;
            }
            acks_since_increase = 0;
            thread_stats_record_window(stats, cwnd, inflight_count);
            random_backoff(&arg->rng, &backoff_window_us, backoff_max_us);
            break;
        }

        if (inflight_count >= cwnd) {
            completed = wait_one_completion(inflight, &inflight_count, stats);
            update_aimd_on_completion(completed, &cwnd, &acks_since_increase);
        }

        if (completed > 0) {
            backoff_window_us = 20;
        }
        thread_stats_record_window(stats, cwnd, inflight_count);
    }

    while (inflight_count > 0) {
        int completed = reap_completed(inflight, &inflight_count, stats);
        if (completed == 0) {
            wait_one_completion(inflight, &inflight_count, stats);
        }
    }
}

void *worker_main(void *p) {
    worker_arg_t *arg = (worker_arg_t *)p;

    bind_current_thread_to_node(arg->cfg->cpu_node);

    if (arg->cfg->mode == 0) {
        run_random(arg);
    } else if (arg->cfg->mode == 1) {
        run_csma(arg);
    } else {
        run_aimd(arg);
    }

    return NULL;
}

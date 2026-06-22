#include "worker.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BACKOFF_MIN_US 200
#define BACKOFF_MAX_US 8000
#define AIMD_SLOT_WAIT_US 50

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

static void relax_backoff(int *window_us, int min_us) {
    *window_us /= 2;
    if (*window_us < min_us) {
        *window_us = min_us;
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

static void controller_update_time_locked(aimd_controller_t *controller,
                                          uint64_t now) {
    if (now < controller->last_update_ns) {
        controller->last_update_ns = now;
        return;
    }

    uint64_t delta = now - controller->last_update_ns;
    controller->cwnd_time_ns += (long double)controller->cwnd * (long double)delta;
    controller->inflight_time_ns +=
        (long double)controller->inflight * (long double)delta;
    controller->observation_ns += delta;
    controller->last_update_ns = now;
}

int aimd_controller_init(aimd_controller_t *controller,
                         int initial_cwnd,
                         int max_cwnd) {
    if (!controller || initial_cwnd < 1 || max_cwnd < initial_cwnd) {
        return -1;
    }

    memset(controller, 0, sizeof(*controller));
    if (pthread_mutex_init(&controller->lock, NULL) != 0) {
        return -1;
    }

    controller->cwnd = initial_cwnd;
    controller->max_cwnd = max_cwnd;
    controller->last_update_ns = now_ns();
    return 0;
}

void aimd_controller_destroy(aimd_controller_t *controller) {
    if (controller) {
        pthread_mutex_destroy(&controller->lock);
    }
}

void aimd_controller_get_metrics(aimd_controller_t *controller,
                                 aimd_controller_metrics_t *metrics) {
    memset(metrics, 0, sizeof(*metrics));

    pthread_mutex_lock(&controller->lock);
    controller_update_time_locked(controller, now_ns());

    if (controller->observation_ns > 0) {
        metrics->avg_cwnd =
            (double)(controller->cwnd_time_ns /
                     (long double)controller->observation_ns);
        metrics->avg_inflight =
            (double)(controller->inflight_time_ns /
                     (long double)controller->observation_ns);
    }
    metrics->max_inflight = controller->max_inflight;
    metrics->current_cwnd = controller->cwnd;
    metrics->current_inflight = controller->inflight;
    pthread_mutex_unlock(&controller->lock);
}

/*
 * Called by cxl_fabric.c while controller->lock is already held through the
 * request's optional completion_lock. Credit release and this accounting are
 * therefore atomic with respect to new AIMD submissions.
 */
static void aimd_on_device_completion_locked(void *ctx) {
    aimd_controller_t *controller = (aimd_controller_t *)ctx;

    controller_update_time_locked(controller, now_ns());

    if (controller->inflight > 0) {
        controller->inflight--;
    }

    controller->completion_count++;
    controller->acks_since_increase++;
    if (controller->acks_since_increase >= controller->cwnd) {
        if (controller->cwnd < controller->max_cwnd) {
            controller->cwnd++;
        }
        controller->acks_since_increase = 0;
    }
}

/*
 * Return values:
 *   1: submitted to the fabric;
 *   0: paced by the global cwnd, so no fabric admission was attempted;
 *  -1: fabric admission failed and the same logical request must be retried.
 */
static int aimd_try_submit(aimd_controller_t *controller,
                           cxl_fabric_t *fabric,
                           cxl_request_t *req,
                           int *cwnd_out,
                           int *inflight_out) {
    int result = 0;

    pthread_mutex_lock(&controller->lock);
    controller_update_time_locked(controller, now_ns());

    if (controller->inflight + controller->reservations >=
        controller->cwnd) {
        goto out;
    }

    controller->reservations++;
    if (cxl_fabric_submit_try(fabric, req) == 0) {
        controller->reservations--;
        controller->inflight++;
        if ((uint64_t)controller->inflight > controller->max_inflight) {
            controller->max_inflight = (uint64_t)controller->inflight;
        }
        result = 1;
        goto out;
    }

    controller->reservations--;
    result = -1;

    /*
     * Suppress repeated decreases from workers observing the same congestion
     * episode. After a decrease, at least the old window's worth of device
     * completions must occur before another multiplicative decrease.
     */
    if (controller->completion_count >=
        controller->next_decrease_completion) {
        int old_cwnd = controller->cwnd;
        controller->cwnd /= 2;
        if (controller->cwnd < 1) {
            controller->cwnd = 1;
        }
        controller->acks_since_increase = 0;
        controller->next_decrease_completion =
            controller->completion_count + (uint64_t)old_cwnd;
    }

out:
    *cwnd_out = controller->cwnd;
    *inflight_out = controller->inflight;
    pthread_mutex_unlock(&controller->lock);
    return result;
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

        if (cxl_fabric_submit_blocking(arg->fabric, req) != 0) {
            cxl_request_destroy(req);
            continue;
        }

        finish_request(stats, req);
    }
}

static void run_csma(worker_arg_t *arg) {
    thread_stats_t *stats = arg->stats;
    int backoff_window_us = BACKOFF_MIN_US;

    while (!stop_requested()) {
        load_sleep(arg->cfg->load, &arg->rng);

        /*
         * A logical request is allocated once and retained across admission
         * failures. Its measured latency therefore includes admission waiting
         * and backoff instead of silently discarding congested requests.
         */
        cxl_request_t *req = new_request(arg);
        if (!req) {
            continue;
        }

        while (!stop_requested()) {
            stats->attempts++;

            if (cxl_fabric_submit_try(arg->fabric, req) == 0) {
                thread_stats_record_window(stats, 1, 1);
                finish_request(stats, req);
                req = NULL;
                relax_backoff(&backoff_window_us, BACKOFF_MIN_US);
                break;
            }

            stats->retry++;
            stats->backoff++;
            thread_stats_record_window(stats, 1, 0);
            random_backoff(&arg->rng, &backoff_window_us, BACKOFF_MAX_US);
        }

        if (req) {
            cxl_request_destroy(req);
        }
    }
}

static int reap_completed(cxl_request_t **inflight,
                          int *count,
                          thread_stats_t *stats) {
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

static int wait_one_completion(cxl_request_t **inflight,
                               int *count,
                               thread_stats_t *stats) {
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

static void run_aimd(worker_arg_t *arg) {
    thread_stats_t *stats = arg->stats;
    aimd_controller_t *controller = arg->aimd;
    cxl_request_t *inflight[AIMD_PER_WORKER_MAX_INFLIGHT] = {0};
    cxl_request_t *pending = NULL;
    int inflight_count = 0;
    int backoff_window_us = BACKOFF_MIN_US;

    while (!stop_requested()) {
        int completed = reap_completed(inflight, &inflight_count, stats);
        if (completed > 0) {
            relax_backoff(&backoff_window_us, BACKOFF_MIN_US);
        }

        /*
         * Match the Blocking and CSMA per-worker concurrency: one worker owns
         * at most one submitted request. The shared cwnd controls aggregate
         * concurrency across workers, not private pipelining inside a worker.
         */
        if (inflight_count >= AIMD_PER_WORKER_MAX_INFLIGHT) {
            wait_one_completion(inflight, &inflight_count, stats);
            relax_backoff(&backoff_window_us, BACKOFF_MIN_US);
            continue;
        }

        if (!pending) {
            load_sleep(arg->cfg->load, &arg->rng);
            pending = new_request(arg);
            if (!pending) {
                usleep(AIMD_SLOT_WAIT_US);
                continue;
            }
            pending->completion_lock = &controller->lock;
            pending->completion_cb = aimd_on_device_completion_locked;
            pending->completion_ctx = controller;
        }

        int cwnd = 1;
        int global_inflight = 0;
        int submit_result = aimd_try_submit(controller,
                                            arg->fabric,
                                            pending,
                                            &cwnd,
                                            &global_inflight);
        if (submit_result == 0) {
            /*
             * Do not count controller pacing as a fabric retry. If this worker
             * owns completed requests, reap one so the global window can make
             * progress; otherwise briefly yield to workers holding requests.
             */
            if (inflight_count > 0) {
                wait_one_completion(inflight, &inflight_count, stats);
                relax_backoff(&backoff_window_us, BACKOFF_MIN_US);
            } else {
                usleep(AIMD_SLOT_WAIT_US);
            }
            continue;
        }

        stats->attempts++;
        if (submit_result > 0) {
            inflight[inflight_count++] = pending;
            pending = NULL;
            thread_stats_record_window(stats, cwnd, global_inflight);
            continue;
        }

        /*
         * Preserve the logical request, release the controller reservation,
         * and apply one global multiplicative decrease for this congestion
         * episode. The same request will be retried after backoff.
         */
        stats->retry++;
        stats->backoff++;
        thread_stats_record_window(stats, cwnd, global_inflight);
        random_backoff(&arg->rng, &backoff_window_us, BACKOFF_MAX_US);
    }

    if (pending) {
        cxl_request_destroy(pending);
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

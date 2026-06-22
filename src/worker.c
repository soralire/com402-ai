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
                         int max_cwnd,
                         uint64_t decrease_cooldown_ns) {
    if (!controller || initial_cwnd < 1 || max_cwnd < initial_cwnd) {
        return -1;
    }

    memset(controller, 0, sizeof(*controller));
    if (pthread_mutex_init(&controller->lock, NULL) != 0) {
        return -1;
    }

    controller->cwnd = initial_cwnd;
    controller->max_cwnd = max_cwnd;
    controller->decrease_cooldown_ns = decrease_cooldown_ns;
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

static int aimd_try_reserve(aimd_controller_t *controller,
                            int *cwnd_out,
                            int *inflight_out) {
    int reserved = 0;

    pthread_mutex_lock(&controller->lock);
    controller_update_time_locked(controller, now_ns());
    if (controller->inflight + controller->reservations <
        controller->cwnd) {
        controller->reservations++;
        reserved = 1;
    }
    *cwnd_out = controller->cwnd;
    *inflight_out = controller->inflight;
    pthread_mutex_unlock(&controller->lock);

    return reserved;
}

static void aimd_confirm_submission(aimd_controller_t *controller,
                                    int *cwnd_out,
                                    int *inflight_out) {
    pthread_mutex_lock(&controller->lock);
    controller_update_time_locked(controller, now_ns());

    if (controller->reservations > 0) {
        controller->reservations--;
    }
    controller->inflight++;
    if ((uint64_t)controller->inflight > controller->max_inflight) {
        controller->max_inflight = (uint64_t)controller->inflight;
    }

    *cwnd_out = controller->cwnd;
    *inflight_out = controller->inflight;
    pthread_mutex_unlock(&controller->lock);
}

static void aimd_cancel_reservation_and_decrease(
    aimd_controller_t *controller,
    int *cwnd_out,
    int *inflight_out) {
    uint64_t now = now_ns();

    pthread_mutex_lock(&controller->lock);
    controller_update_time_locked(controller, now);

    if (controller->reservations > 0) {
        controller->reservations--;
    }

    /*
     * Multiple workers can observe the same queue-full episode. Apply at most
     * one multiplicative decrease per control interval so one congestion event
     * does not cause every worker to halve the shared window independently.
     */
    if (now - controller->last_decrease_ns >=
        controller->decrease_cooldown_ns) {
        controller->cwnd /= 2;
        if (controller->cwnd < 1) {
            controller->cwnd = 1;
        }
        controller->acks_since_increase = 0;
        controller->last_decrease_ns = now;
    }

    *cwnd_out = controller->cwnd;
    *inflight_out = controller->inflight;
    pthread_mutex_unlock(&controller->lock);
}

static void aimd_on_completion(aimd_controller_t *controller,
                               int *cwnd_out,
                               int *inflight_out) {
    pthread_mutex_lock(&controller->lock);
    controller_update_time_locked(controller, now_ns());

    if (controller->inflight > 0) {
        controller->inflight--;
    }

    controller->acks_since_increase++;
    if (controller->acks_since_increase >= controller->cwnd) {
        if (controller->cwnd < controller->max_cwnd) {
            controller->cwnd++;
        }
        controller->acks_since_increase = 0;
    }

    *cwnd_out = controller->cwnd;
    *inflight_out = controller->inflight;
    pthread_mutex_unlock(&controller->lock);
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
                          thread_stats_t *stats,
                          aimd_controller_t *controller) {
    int completed = 0;

    for (int i = 0; i < *count;) {
        if (!cxl_request_is_done(inflight[i])) {
            i++;
            continue;
        }

        finish_request(stats, inflight[i]);
        completed++;

        int cwnd = 1;
        int global_inflight = 0;
        aimd_on_completion(controller, &cwnd, &global_inflight);
        thread_stats_record_window(stats, cwnd, global_inflight);

        for (int j = i + 1; j < *count; j++) {
            inflight[j - 1] = inflight[j];
        }
        (*count)--;
    }

    return completed;
}

static int wait_one_completion(cxl_request_t **inflight,
                               int *count,
                               thread_stats_t *stats,
                               aimd_controller_t *controller) {
    if (*count <= 0) {
        return 0;
    }

    finish_request(stats, inflight[0]);

    for (int i = 1; i < *count; i++) {
        inflight[i - 1] = inflight[i];
    }
    (*count)--;

    int cwnd = 1;
    int global_inflight = 0;
    aimd_on_completion(controller, &cwnd, &global_inflight);
    thread_stats_record_window(stats, cwnd, global_inflight);
    return 1;
}

static void run_aimd(worker_arg_t *arg) {
    thread_stats_t *stats = arg->stats;
    aimd_controller_t *controller = arg->aimd;
    cxl_request_t *inflight[AIMD_CWND_MAX] = {0};
    cxl_request_t *pending = NULL;
    int inflight_count = 0;
    int backoff_window_us = BACKOFF_MIN_US;

    while (!stop_requested()) {
        int completed =
            reap_completed(inflight, &inflight_count, stats, controller);
        if (completed > 0) {
            relax_backoff(&backoff_window_us, BACKOFF_MIN_US);
        }

        if (!pending) {
            load_sleep(arg->cfg->load, &arg->rng);
            pending = new_request(arg);
            if (!pending) {
                usleep(AIMD_SLOT_WAIT_US);
                continue;
            }
        }

        int cwnd = 1;
        int global_inflight = 0;
        if (!aimd_try_reserve(controller, &cwnd, &global_inflight)) {
            /*
             * Do not count controller pacing as a fabric retry. If this worker
             * owns completed requests, reap one so the global window can make
             * progress; otherwise briefly yield to workers holding requests.
             */
            if (inflight_count > 0) {
                wait_one_completion(
                    inflight, &inflight_count, stats, controller);
                relax_backoff(&backoff_window_us, BACKOFF_MIN_US);
            } else {
                usleep(AIMD_SLOT_WAIT_US);
            }
            continue;
        }

        stats->attempts++;
        if (cxl_fabric_submit_try(arg->fabric, pending) == 0) {
            aimd_confirm_submission(controller, &cwnd, &global_inflight);
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
        aimd_cancel_reservation_and_decrease(
            controller, &cwnd, &global_inflight);
        thread_stats_record_window(stats, cwnd, global_inflight);
        random_backoff(&arg->rng, &backoff_window_us, BACKOFF_MAX_US);
    }

    if (pending) {
        cxl_request_destroy(pending);
    }

    while (inflight_count > 0) {
        int completed =
            reap_completed(inflight, &inflight_count, stats, controller);
        if (completed == 0) {
            wait_one_completion(
                inflight, &inflight_count, stats, controller);
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

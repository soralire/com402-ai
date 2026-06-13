#include "worker.h"
#include "utils.h"

#include <errno.h>
#include <semaphore.h>
#include <unistd.h>

sem_t cxl_switch_queue;
atomic_int stop_flag = 0;

static int stop_requested(void) {
    return atomic_load_explicit(&stop_flag, memory_order_relaxed);
}

static int switch_queue_wait(void) {
    while (sem_wait(&cxl_switch_queue) != 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int switch_queue_try_acquire(void) {
    return sem_trywait(&cxl_switch_queue) == 0;
}

static void switch_queue_release(void) {
    sem_post(&cxl_switch_queue);
}

static void load_sleep(int load, unsigned int *rng) {
    if (load < 100) {
        int idle_us = (100 - load) * 20;
        if (idle_us > 0) {
            usleep(xorshift32(rng) % (unsigned int)idle_us);
        }
    }
}

static void run_random(worker_arg_t *arg) {
    const config_t *cfg = arg->cfg;
    thread_stats_t *stats = arg->stats;

    while (!stop_requested()) {
        load_sleep(cfg->load, &arg->rng);

        stats->attempts++;

        uint64_t t1 = now_ns();
        /* Random: blocking access to the shared CXL Switch queue. */
        if (switch_queue_wait() != 0) {
            continue;
        }
        memory_region_request(arg->region, &arg->rng, cfg->touches_per_req);
        switch_queue_release();
        uint64_t t2 = now_ns();

        stats->success++;
        thread_stats_record_latency(stats, t2 - t1);
    }
}

static void run_csma(worker_arg_t *arg) {
    const config_t *cfg = arg->cfg;
    thread_stats_t *stats = arg->stats;
    int backoff_window_us = 20;
    const int backoff_max_us = 2000;

    while (!stop_requested()) {
        load_sleep(cfg->load, &arg->rng);

        stats->attempts++;

        uint64_t t1 = now_ns();

        /* CSMA-like: non-blocking CXL Switch access with random backoff. */
        if (switch_queue_try_acquire()) {
            memory_region_request(arg->region, &arg->rng, cfg->touches_per_req);
            switch_queue_release();

            uint64_t t2 = now_ns();

            stats->success++;
            thread_stats_record_latency(stats, t2 - t1);
            backoff_window_us = 20;
        } else {
            stats->retry++;
            stats->backoff++;

            usleep(xorshift32(&arg->rng) % (unsigned int)backoff_window_us);

            if (backoff_window_us < backoff_max_us) {
                backoff_window_us *= 2;
                if (backoff_window_us > backoff_max_us) {
                    backoff_window_us = backoff_max_us;
                }
            }
        }
    }
}

static void run_aimd(worker_arg_t *arg) {
    const config_t *cfg = arg->cfg;
    thread_stats_t *stats = arg->stats;
    int cwnd = 1;
    const int cwnd_max = 32;
    int backoff_window_us = 20;
    const int backoff_max_us = 2000;

    while (!stop_requested()) {
        load_sleep(cfg->load, &arg->rng);

        /*
         * queue_depth controls global concurrent requests through the CXL Switch
         * and Type-3 device queue. cwnd controls this worker's synchronous
         * request-injection aggressiveness; multiple workers create real system-
         * level outstanding requests when queue_depth > 1.
         */
        int old_cwnd = cwnd;
        int sent = 0;
        int failed = 0;

        for (int i = 0; i < old_cwnd && !stop_requested(); i++) {
            stats->attempts++;
            uint64_t t1 = now_ns();

            if (switch_queue_try_acquire()) {
                memory_region_request(arg->region, &arg->rng, cfg->touches_per_req);
                switch_queue_release();

                uint64_t t2 = now_ns();

                stats->success++;
                thread_stats_record_latency(stats, t2 - t1);
                sent++;
            } else {
                stats->retry++;
                stats->backoff++;
                failed = 1;

                cwnd /= 2;
                if (cwnd < 1) {
                    cwnd = 1;
                }

                usleep(xorshift32(&arg->rng) % (unsigned int)backoff_window_us);

                if (backoff_window_us < backoff_max_us) {
                    backoff_window_us *= 2;
                    if (backoff_window_us > backoff_max_us) {
                        backoff_window_us = backoff_max_us;
                    }
                }

                break;
            }
        }

        if (!failed && sent == old_cwnd) {
            if (cwnd < cwnd_max) {
                cwnd++;
            }
            backoff_window_us = 20;
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

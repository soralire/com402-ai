#include "config.h"
#include "cxl_fabric.h"
#include "numa_backend.h"
#include "stats.h"
#include "utils.h"
#include "worker.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void destroy_thread_stats(thread_stats_t *stats, int count) {
    if (!stats) {
        return;
    }

    for (int i = 0; i < count; i++) {
        thread_stats_destroy(&stats[i]);
    }
}

static int init_worker_args(worker_arg_t *args,
                            thread_stats_t *stats,
                            int nthreads,
                            const config_t *cfg,
                            cxl_fabric_t *fabric) {
    /* 为每个 worker 准备独立随机种子和统计缓冲，避免线程之间共享统计结构。 */
    for (int i = 0; i < nthreads; i++) {
        if (thread_stats_init(&stats[i], MAX_LAT_PER_THREAD) != 0) {
            destroy_thread_stats(stats, i);
            return -1;
        }

        args[i].cfg = cfg;
        args[i].fabric = fabric;
        args[i].rng = cfg->seed + (unsigned int)i * 10007U;
        args[i].stats = &stats[i];
    }

    return 0;
}

static void join_workers(pthread_t *tids, int count) {
    for (int i = 0; i < count; i++) {
        pthread_join(tids[i], NULL);
    }
}

static void set_stop_flag(int value) {
    atomic_store_explicit(&stop_flag, value, memory_order_relaxed);
}

static int start_workers(pthread_t *tids,
                         worker_arg_t *args,
                         int nthreads,
                         int *created_threads) {
    *created_threads = 0;

    for (int i = 0; i < nthreads; i++) {
        int rc = pthread_create(&tids[i], NULL, worker_main, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "Error: pthread_create failed: %s\n", strerror(rc));
            set_stop_flag(1);
            join_workers(tids, *created_threads);
            *created_threads = 0;
            return -1;
        }

        (*created_threads)++;
    }

    return 0;
}

static void print_summary_csv(const config_t *cfg, const summary_stats_t *sum) {
    /* 每次运行输出一行 CSV，批量脚本会负责去掉重复 header。 */
    printf("track,load,seed,attempts,success,retry,backoff,"
           "delay_p50,delay_p95,delay_p99,goodput,threads,seconds,"
           "mem_node,cpu_node,mem_mb,touches_per_req,latency_samples,"
           "backend,queue_depth,device_workers,avg_cwnd,avg_inflight,max_inflight\n");
    printf("%s,%d,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f,%d,%d,%d,%d,%d,%d,%" PRIu64 ",type3_numa_node%d,%d,%d,%.2f,%.2f,%" PRIu64 "\n",
           mode_name(cfg->mode),
           cfg->load,
           cfg->seed,
           sum->attempts,
           sum->success,
           sum->retry,
           sum->backoff,
           sum->p50,
           sum->p95,
           sum->p99,
           sum->goodput,
           cfg->threads,
           cfg->seconds,
           cfg->mem_node,
           cfg->cpu_node,
           cfg->mem_mb,
           cfg->touches_per_req,
           sum->latency_count,
           cfg->mem_node,
           cfg->queue_depth,
           cfg->device_workers,
           sum->avg_cwnd,
           sum->avg_inflight,
           sum->max_inflight);
}

int main(int argc, char **argv) {
    config_t cfg;
    memory_region_t region = {0};
    cxl_fabric_t fabric = {0};
    pthread_t *tids = NULL;
    worker_arg_t *args = NULL;
    thread_stats_t *thread_stats = NULL;
    int created_threads = 0;
    int fabric_ready = 0;
    int stats_ready = 0;
    int status = 1;

    if (parse_config(argc, argv, &cfg) != 0) {
        return 1;
    }

    if (!numa_backend_available()) {
        fprintf(stderr, "Error: NUMA is not available. Check QEMU NUMA configuration.\n");
        return 1;
    }

    if (!numa_node_exists(cfg.mem_node)) {
        fprintf(stderr, "Error: mem_node=%d does not exist. Check `numactl -H`.\n", cfg.mem_node);
        return 1;
    }

    print_config_stderr(&cfg);

    /* 先分配后端内存并初始化 fabric，再启动 CPU worker 进行请求注入。 */
    if (memory_region_init(&region, cfg.mem_size, cfg.mem_node) != 0) {
        goto cleanup;
    }

    if (cxl_fabric_init(&fabric, &region, cfg.queue_depth, cfg.device_workers) != 0) {
        perror("cxl_fabric_init");
        goto cleanup;
    }
    fabric_ready = 1;

    tids = calloc((size_t)cfg.threads, sizeof(*tids));
    args = calloc((size_t)cfg.threads, sizeof(*args));
    thread_stats = calloc((size_t)cfg.threads, sizeof(*thread_stats));
    if (!tids || !args || !thread_stats) {
        fprintf(stderr, "Error: calloc failed\n");
        goto cleanup;
    }

    if (init_worker_args(args, thread_stats, cfg.threads, &cfg, &fabric) != 0) {
        fprintf(stderr, "Error: latency buffer allocation failed\n");
        goto cleanup;
    }
    stats_ready = 1;

    uint64_t start_ns = now_ns();
    set_stop_flag(0);

    if (start_workers(tids, args, cfg.threads, &created_threads) != 0) {
        goto cleanup;
    }

    sleep(cfg.seconds);
    set_stop_flag(1);
    join_workers(tids, created_threads);
    created_threads = 0;

    /* 汇总所有线程的成功请求、延迟分位数和窗口统计。 */
    uint64_t end_ns = now_ns();
    double elapsed_s = (double)(end_ns - start_ns) / 1000000000.0;

    summary_stats_t sum;
    if (summarize_stats(thread_stats, cfg.threads, elapsed_s, &sum) != 0) {
        fprintf(stderr, "Error: summarize_stats failed\n");
        goto cleanup;
    }

    print_summary_csv(&cfg, &sum);
    status = 0;

cleanup:
    if (created_threads > 0) {
        set_stop_flag(1);
        join_workers(tids, created_threads);
    }
    if (stats_ready) {
        destroy_thread_stats(thread_stats, cfg.threads);
    }
    if (fabric_ready) {
        cxl_fabric_destroy(&fabric);
    }
    memory_region_destroy(&region);
    free(thread_stats);
    free(args);
    free(tids);

    return status;
}

#ifndef CXL_NUMA_CSMA_STATS_H
#define CXL_NUMA_CSMA_STATS_H

#include <stdint.h>

typedef struct {
    uint64_t attempts;
    uint64_t success;
    uint64_t retry;
    uint64_t backoff;
    uint64_t cwnd_sum;
    uint64_t cwnd_samples;
    uint64_t inflight_sum;
    uint64_t inflight_samples;
    uint64_t max_inflight;
    uint64_t latency_count;
    uint64_t *latencies;
    uint64_t latency_capacity;
} thread_stats_t;

typedef struct {
    uint64_t attempts;
    uint64_t success;
    uint64_t retry;
    uint64_t backoff;
    uint64_t max_inflight;
    uint64_t latency_count;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
    double avg_cwnd;
    double avg_inflight;
    double goodput;
    double global_avg_cwnd;
    double global_avg_inflight;
    uint64_t global_max_inflight;
} summary_stats_t;

int thread_stats_init(thread_stats_t *s, uint64_t latency_capacity);
void thread_stats_destroy(thread_stats_t *s);
void thread_stats_record_latency(thread_stats_t *s, uint64_t ns);
void thread_stats_record_window(thread_stats_t *s, int cwnd, int inflight);

int summarize_stats(const thread_stats_t *stats, int nthreads, double elapsed_s, summary_stats_t *out);

#endif

#include "stats.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

int thread_stats_init(thread_stats_t *s, uint64_t latency_capacity) {
    memset(s, 0, sizeof(*s));
    s->latency_capacity = latency_capacity;
    s->latencies = calloc(latency_capacity, sizeof(uint64_t));
    return s->latencies ? 0 : -1;
}

void thread_stats_destroy(thread_stats_t *s) {
    free(s->latencies);
    s->latencies = NULL;
    s->latency_capacity = 0;
    s->latency_count = 0;
}

void thread_stats_record_latency(thread_stats_t *s, uint64_t ns) {
    /* 延迟样本数量有上限，避免长时间实验占用过多内存。 */
    if (s->latency_count < s->latency_capacity) {
        s->latencies[s->latency_count++] = ns;
    }
}

void thread_stats_record_window(thread_stats_t *s, int cwnd, int inflight) {
    /* 窗口和在途请求统计用于分析 AIMD 是否受到 fabric credit 限制。 */
    if (cwnd > 0) {
        s->cwnd_sum += (uint64_t)cwnd;
        s->cwnd_samples++;
    }

    if (inflight >= 0) {
        s->inflight_sum += (uint64_t)inflight;
        s->inflight_samples++;
        if ((uint64_t)inflight > s->max_inflight) {
            s->max_inflight = (uint64_t)inflight;
        }
    }
}

int summarize_stats(const thread_stats_t *stats, int nthreads, double elapsed_s, summary_stats_t *out) {
    memset(out, 0, sizeof(*out));

    /* 先合并线程级计数，再把全部延迟样本排序计算 p50/p95/p99。 */
    uint64_t cwnd_sum = 0;
    uint64_t cwnd_samples = 0;
    uint64_t inflight_sum = 0;
    uint64_t inflight_samples = 0;

    for (int i = 0; i < nthreads; i++) {
        out->attempts += stats[i].attempts;
        out->success += stats[i].success;
        out->retry += stats[i].retry;
        out->backoff += stats[i].backoff;
        cwnd_sum += stats[i].cwnd_sum;
        cwnd_samples += stats[i].cwnd_samples;
        inflight_sum += stats[i].inflight_sum;
        inflight_samples += stats[i].inflight_samples;
        if (stats[i].max_inflight > out->max_inflight) {
            out->max_inflight = stats[i].max_inflight;
        }
        out->latency_count += stats[i].latency_count;
    }

    uint64_t *all = NULL;
    if (out->latency_count > 0) {
        all = malloc(sizeof(uint64_t) * out->latency_count);
        if (!all) {
            return -1;
        }

        uint64_t pos = 0;
        for (int i = 0; i < nthreads; i++) {
            memcpy(&all[pos], stats[i].latencies, sizeof(uint64_t) * stats[i].latency_count);
            pos += stats[i].latency_count;
        }

        qsort(all, out->latency_count, sizeof(uint64_t), cmp_u64);
        out->p50 = percentile(all, out->latency_count, 50.0);
        out->p95 = percentile(all, out->latency_count, 95.0);
        out->p99 = percentile(all, out->latency_count, 99.0);
        free(all);
    }

    out->avg_cwnd = cwnd_samples > 0 ? (double)cwnd_sum / (double)cwnd_samples : 0.0;
    out->avg_inflight = inflight_samples > 0 ? (double)inflight_sum / (double)inflight_samples : 0.0;
    out->goodput = elapsed_s > 0.0 ? (double)out->success / elapsed_s : 0.0;
    return 0;
}

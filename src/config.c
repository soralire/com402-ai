#include "config.h"

#include <stdio.h>
#include <stdlib.h>

const char *mode_name(int mode) {
    if (mode == 0) return "random";
    if (mode == 1) return "csma";
    if (mode == 2) return "aimd";
    return "unknown";
}

void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s <mode> <load> <threads> <seconds> [mem_node] [cpu_node] [seed] [mem_mb] [touches_per_req] [queue_depth]\n\n"
            "Required:\n"
            "  mode            0=random blocking switch access, 1=csma non-blocking switch access, 2=aimd adaptive switch injection\n"
            "  load            1~100\n"
            "  threads         worker thread count\n"
            "  seconds         runtime seconds\n\n"
            "Optional:\n"
            "  mem_node        NUMA node used as Type-3 memory backend, default 1\n"
            "  cpu_node        NUMA node used as CPU execution node, default 0\n"
            "  seed            random seed, default 1\n"
            "  mem_mb          memory size in MB, default 512\n"
            "  touches_per_req cache-line touches per request, default 4096\n\n"
            "  queue_depth     CXL Switch / Type-3 device queue depth, default 1\n\n"
            "Examples:\n"
            "  %s 0 50 4 5 1 0 1 512 4096 1\n"
            "  %s 1 50 4 5 1 0 1 512 4096 8\n"
            "  %s 2 50 4 5 1 0 1 512 4096 8\n",
            prog, prog, prog, prog);
}

int parse_config(int argc, char **argv, config_t *cfg) {
    if (argc < 5) {
        print_usage(argv[0]);
        return -1;
    }

    cfg->mode = atoi(argv[1]);
    cfg->load = atoi(argv[2]);
    cfg->threads = atoi(argv[3]);
    cfg->seconds = atoi(argv[4]);
    cfg->mem_node = (argc > 5) ? atoi(argv[5]) : 1;
    cfg->cpu_node = (argc > 6) ? atoi(argv[6]) : 0;
    cfg->seed = (argc > 7) ? (unsigned int)strtoul(argv[7], NULL, 10) : 1U;
    cfg->mem_mb = (argc > 8) ? atoi(argv[8]) : 512;
    cfg->touches_per_req = (argc > 9) ? atoi(argv[9]) : 4096;
    cfg->queue_depth = (argc > 10) ? atoi(argv[10]) : 1;

    if (cfg->mode < 0 || cfg->mode > 2) {
        fprintf(stderr, "Error: mode must be 0, 1, or 2\n");
        return -1;
    }

    if (cfg->load < 1 || cfg->load > 100) {
        fprintf(stderr, "Error: load must be between 1 and 100\n");
        return -1;
    }

    if (cfg->threads < 1 || cfg->threads > 256) {
        fprintf(stderr, "Error: threads must be between 1 and 256\n");
        return -1;
    }

    if (cfg->seconds < 1) {
        fprintf(stderr, "Error: seconds must be positive\n");
        return -1;
    }

    if (cfg->mem_mb < 1) {
        fprintf(stderr, "Error: mem_mb must be positive\n");
        return -1;
    }

    if (cfg->touches_per_req < 1) {
        fprintf(stderr, "Error: touches_per_req must be positive\n");
        return -1;
    }

    if (cfg->queue_depth < 1) {
        fprintf(stderr, "Error: queue_depth must be positive\n");
        return -1;
    }

    cfg->mem_size = (size_t)cfg->mem_mb * 1024UL * 1024UL;
    return 0;
}

void print_config_stderr(const config_t *cfg) {
    fprintf(stderr,
            "Config: mode=%s load=%d threads=%d seconds=%d mem_node=%d cpu_node=%d "
            "seed=%u mem_mb=%d touches_per_req=%d queue_depth=%d "
            "backend=NUMA-node%d-as-Type-3-memory cxl_switch_queue_depth=%d\n",
            mode_name(cfg->mode),
            cfg->load,
            cfg->threads,
            cfg->seconds,
            cfg->mem_node,
            cfg->cpu_node,
            cfg->seed,
            cfg->mem_mb,
            cfg->touches_per_req,
            cfg->queue_depth,
            cfg->mem_node,
            cfg->queue_depth);
}

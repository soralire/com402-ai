# cxl_numa_csma

`cxl_numa_csma` is a Linux NUMA-based CXL experiment simulator. It does not
implement a hardware CXL protocol stack. The goal is to study request-injection
and contention-control policies over a CXL-like memory fabric.

## Experiment Model

```text
CPU workers
  -> CXL.mem request injection
  -> CXL Switch / Type-3 queue credits
  -> Type-3 device service threads
  -> NUMA remote memory backend
  -> completion returned to worker
```

- NUMA Node0 is the CPU execution node by default.
- NUMA Node1 is the remote memory backend used as a simulated Type-3 Memory
  Device by default.
- `queue_depth` models the number of concurrent CXL fabric / Type-3 queue
  credits. A request occupies one credit from submission until completion.
- `device_workers` models Type-3 device-side service parallelism.
- AIMD's `cwnd` is a per-worker outstanding request window, while `queue_depth`
  is the global fabric/device capacity.

This is still a software model, but it is closer to a real CXL.mem system than a
single host-side mutex: workers submit requests, device service threads execute
the backend memory access, and completions release fabric credits.

## Build

```bash
make clean && make
```

The project uses `pthread`, POSIX semaphores, Linux NUMA APIs, and C11 atomics.
The Makefile builds with `-std=gnu11 -pthread -lnuma`.

## Usage

```text
./cxl_numa_csma <mode> <load> <threads> <seconds> [mem_node] [cpu_node] [seed] [mem_mb] [touches_per_req] [queue_depth] [device_workers]
```

Required arguments:

- `mode`: `0=random`, `1=csma`, `2=aimd`
- `load`: request load, `1~100`
- `threads`: worker thread count, `1~256`
- `seconds`: runtime in seconds

Optional arguments:

- `mem_node`: NUMA node used as Type-3 memory backend, default `1`
- `cpu_node`: NUMA node used as CPU execution node, default `0`
- `seed`: random seed, default `1`
- `mem_mb`: backend memory size in MB, default `512`
- `touches_per_req`: cache-line touches per request, default `4096`
- `queue_depth`: CXL Switch / Type-3 queue credits, default `1`
- `device_workers`: Type-3 device service threads, default `1`

## Modes

- `0` Random: blocking credit acquisition. It waits for a CXL fabric credit,
  submits one request, then waits for completion.
- `1` CSMA-like: non-blocking credit acquisition with random backoff. Queue-full
  events count as retry/backoff.
- `2` AIMD: adaptive outstanding request control. Each worker maintains a `cwnd`
  and may keep multiple requests in flight. Completion events grow `cwnd`;
  queue-full events halve `cwnd`.

All modes share the same Type-3 backend, queue credits, request size, and device
service threads. Only the request-injection policy changes.

## Examples

```bash
./cxl_numa_csma 0 50 8 5 1 0 1 512 4096 4 1
./cxl_numa_csma 1 50 16 5 1 0 1 512 4096 8 1
./cxl_numa_csma 2 50 16 5 1 0 1 512 4096 8 2
```

Each run prints one CSV header and one result row:

```text
track,load,seed,attempts,success,retry,backoff,delay_p50,delay_p95,delay_p99,goodput,threads,seconds,mem_node,cpu_node,mem_mb,touches_per_req,latency_samples,backend,queue_depth,device_workers,avg_cwnd,avg_inflight,max_inflight
```

The `backend` field uses `type3_numa_nodeX` to show which NUMA node backs the
simulated Type-3 memory device.

## Batch Experiments

The default batch script focuses on AIMD-relevant oversubscription:

```text
modes:          0 1 2
loads:          10 30 50 70 90
seeds:          1 2 3
queue_depths:   4 8
threads:        8 16
device_workers: 1 2
duration:       5 seconds
```

This plan is designed to keep `threads > queue_depth` in several cases, so the
CXL Switch / Type-3 credits create real backpressure. `device_workers=1/2`
separates switch-credit contention from backend service parallelism.

Run the default sweep:

```bash
./scripts/run_batch.sh
```

Run a smaller custom sweep:

```bash
QUEUE_DEPTH=8 THREADS=16 DEVICE_WORKERS=1 LOADS="50 90" SEEDS="1" ./scripts/run_batch.sh
```

Run the full default shape explicitly:

```bash
QUEUE_DEPTHS="4 8" THREADS_LIST="8 16" DEVICE_WORKERS_LIST="1 2" ./scripts/run_batch.sh
```

`run_batch.sh` writes raw CSV to `results_numa_raw.csv` and a duplicate-header
filtered CSV to `results_numa_clean.csv` under `OUT_DIR`. It also writes
`experiment_plan.txt`, which records the fixed parameters, sweep parameters, and
how to interpret the thread/queue/device-worker experiment.

## Code Layout

- `src/config.c`: command-line parsing and configuration validation.
- `src/main.c`: experiment setup, worker lifecycle, cleanup, and CSV output.
- `src/cxl_fabric.c`: asynchronous CXL Switch / Type-3 request queue, credits,
  device service threads, and completions.
- `src/worker.c`: Random, CSMA-like, and AIMD request-injection policies.
- `src/stats.c`: per-thread latency, window, inflight, and summary statistics.
- `src/numa_backend.c`: NUMA allocation, CPU binding, and Type-3 backend memory
  requests.
- `scripts/run_batch.sh`: batch experiment runner with environment-variable
  overrides.

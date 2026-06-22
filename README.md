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
- AIMD's `cwnd` is one global outstanding-request window shared by all CPU
  workers, while `queue_depth` is the global fabric/device credit capacity.

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
- `1` CSMA-like: non-blocking credit acquisition with exponential random
  backoff. A logical request is retained and retried until it is submitted or
  the experiment stops.
- `2` AIMD: adaptive outstanding request control using one global shared
  `cwnd` across all workers. Completion events grow the global window;
  queue-full events reduce it at most once per completed-window interval.
  Each worker owns at most one submitted request, matching the per-worker
  concurrency of the Blocking and CSMA modes. The maximum global window is
  `queue_depth + 1`, allowing one congestion probe.

All modes share the same Type-3 backend, queue credits, request size, and device
service threads. Only the request-injection policy changes.

For result interpretation, mode 0 is the blocking/no-retry baseline. Modes 1
and 2 preserve a logical request across admission retries, so their measured
latency includes admission waiting and backoff. `success / attempts` is an
admission-attempt success rate, not a logical-request completion percentage.

## Examples

```bash
./cxl_numa_csma 0 50 8 5 1 0 1 512 4096 4 1
./cxl_numa_csma 1 50 16 5 1 0 1 512 4096 8 1
./cxl_numa_csma 2 50 16 5 1 0 1 512 4096 8 2
```

Each run prints one CSV header and one result row:

```text
track,load,seed,attempts,success,retry,backoff,delay_p50,delay_p95,delay_p99,goodput,threads,seconds,mem_node,cpu_node,mem_mb,touches_per_req,latency_samples,backend,queue_depth,device_workers,avg_cwnd,avg_inflight,max_inflight,elapsed_s,global_avg_cwnd,global_avg_inflight,global_max_inflight
```

For AIMD, `avg_cwnd`, `avg_inflight`, and the explicit `global_*` fields are
global time-weighted controller measurements. For the non-AIMD modes, the
explicit `global_*` fields are zero.

An AIMD request registers an optional device-completion callback. The callback
updates global in-flight state at the same device-completion event that returns
the fabric credit; worker scheduling therefore cannot delay controller
accounting. Blocking and CSMA requests do not register this callback.

The `backend` field uses `type3_numa_nodeX` to show which NUMA node backs the
simulated Type-3 memory device.

## Experiments

Experiment scripts are organized by experiment under `experiments/`, while all
outputs are grouped under `results/` by experiment name.

```text
experiments/
  thread_queue_sweep/     original thread/queue/device-worker sweep
  near_remote_memory/     local-vs-remote NUMA memory comparison

results/
  thread_queue_sweep/     outputs from the original sweep
  near_remote_memory/     outputs from the local-vs-remote comparison
```

### Thread/Queue Sweep

The default experiment isolates worker/credit oversubscription by fixing load,
queue depth, and device parallelism:

```text
modes:          0 1 2
load:           100
seeds:          1 2 3 4 5
queue_depth:    4
threads:        1 2 4 8 16 32
device_workers: 1
duration:       15 seconds
```

The main independent variable is `threads / queue_depth`. Values at or below
one are the low-contention region; values above one oversubscribe the fixed
fabric credits.

Run the default sweep:

```bash
./experiments/thread_queue_sweep/run_thread_queue_sweep.sh
```

Run the blocking baseline alone:

```bash
MODES="0" ./experiments/thread_queue_sweep/run_thread_queue_sweep.sh
```

Custom sweeps remain available through environment variables:

```bash
LOADS="100" QUEUE_DEPTHS="4" THREADS_LIST="1 2 4 8 16 32" DEVICE_WORKERS_LIST="1" ./experiments/thread_queue_sweep/run_thread_queue_sweep.sh
```

The script writes `thread_queue_raw.csv`, `thread_queue_clean.csv`, and
`experiment_plan.txt` to `results/thread_queue_sweep/`.

Generate figures:

```bash
python3 experiments/thread_queue_sweep/plot_thread_queue_sweep.py
```

### Near/Remote Memory Comparison

The near/remote comparison keeps the experiment small and changes only the
backend memory node:

```text
cpu_node:        0
mem_nodes:       0 1
threads:         8
duration:        10 seconds
seed:            1
touches_per_req: 4096
loads:           10 30 50 70 90
modes:           0 1 2
queue_depth:     4
device_workers:  1
```

Run it:

```bash
./experiments/near_remote_memory/run_near_remote.sh
```

The script writes `near_remote_raw.csv`, `near_remote_clean.csv`, and
`experiment_plan.txt` to `results/near_remote_memory/`.

Generate figures:

```bash
python3 experiments/near_remote_memory/plot_near_remote.py
```

## Code Layout

- `src/config.c`: command-line parsing and configuration validation.
- `src/main.c`: experiment setup, worker lifecycle, cleanup, and CSV output.
- `src/cxl_fabric.c`: asynchronous CXL Switch / Type-3 request queue, credits,
  device service threads, and completions.
- `src/worker.c`: Random, CSMA-like, and AIMD request-injection policies.
- `src/stats.c`: per-thread latency, window, inflight, and summary statistics.
- `src/numa_backend.c`: NUMA allocation, CPU binding, and Type-3 backend memory
  requests.
- `experiments/thread_queue_sweep/`: original thread/queue sweep runner and
  plotting script.
- `experiments/near_remote_memory/`: local-vs-remote memory comparison runner
  and plotting script.
- `scripts/`: compatibility wrappers for older command paths.

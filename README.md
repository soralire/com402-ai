# cxl_numa_csma

`cxl_numa_csma` is a Linux NUMA-based CXL experiment simulator. It does not
implement a hardware CXL protocol stack. The goal is to study contention-control
policies over a CXL-like shared memory path.

## Experiment Model

```text
CPU workers -> CXL Switch queue -> Type-3 Memory Device -> NUMA remote memory backend
```

- NUMA Node0 is the CPU execution node by default.
- NUMA Node1 is the remote memory backend used as a simulated Type-3 Memory
  Device by default.
- `cxl_switch_queue` is a POSIX semaphore that models the shared bottleneck in a
  CXL Switch or memory fabric.
- `queue_depth` controls how many requests may concurrently enter the simulated
  Type-3 memory backend.

## Build

```bash
make clean && make
```

The project uses `pthread`, POSIX semaphores, Linux NUMA APIs, and C11 atomics.
The Makefile builds with `-std=gnu11 -pthread -lnuma`.

## Usage

```text
./cxl_numa_csma <mode> <load> <threads> <seconds> [mem_node] [cpu_node] [seed] [mem_mb] [touches_per_req] [queue_depth]
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
- `queue_depth`: CXL Switch / Type-3 device queue depth, default `1`

## Modes

- `0` Random: blocking access to the CXL Switch queue.
- `1` CSMA-like: non-blocking switch access; retry and random backoff on switch
  contention.
- `2` AIMD: adaptive request injection. `cwnd` controls per-worker injection
  aggressiveness, while `queue_depth` controls global concurrent requests.

`queue_depth=1` degenerates to the original single-channel model.
`queue_depth=4/8/16/32` simulates a deeper CXL Switch or Type-3 Memory Device
queue. With multiple workers and `queue_depth>1`, the system can have multiple
outstanding memory requests.

## Examples

```bash
./cxl_numa_csma 0 50 4 5 1 0 1 512 4096 1
./cxl_numa_csma 1 50 4 5 1 0 1 512 4096 8
./cxl_numa_csma 2 50 4 5 1 0 1 512 4096 8
```

Each run prints one CSV header and one result row:

```text
track,load,seed,attempts,success,retry,backoff,delay_p50,delay_p95,delay_p99,goodput,threads,seconds,mem_node,cpu_node,mem_mb,touches_per_req,latency_samples,backend,queue_depth
```

The `backend` field uses `type3_numa_nodeX` to show which NUMA node backs the
simulated Type-3 memory device.

## Batch Experiments

The default batch script runs a queue-depth sweep:

```text
modes:        0 1 2
loads:        10 30 50 70 90
seeds:        1 2 3
queue_depths: 1 4 8 16 32
threads:      4
duration:     5 seconds
```

This gives a balanced experiment plan:

- `queue_depth=1` is the baseline single-channel CXL Switch model.
- `queue_depth=4/8/16/32` explores deeper CXL Switch / Type-3 device queues.
- `load=10/30/50/70/90` covers light load through near-saturation load.
- `mode=0/1/2` compares Random, CSMA-like, and AIMD under identical queue
  settings.
- `seed=1/2/3` gives repeated runs for basic variance checking.

Run the full default sweep:

```bash
./scripts/run_batch.sh
```

Run one queue depth for a smaller experiment:

```bash
QUEUE_DEPTH=8 THREADS=4 DURATION=5 ./scripts/run_batch.sh
```

Run a custom sweep:

```bash
QUEUE_DEPTHS="1 8 32" LOADS="30 70 90" SEEDS="1" ./scripts/run_batch.sh
```

`run_batch.sh` writes raw CSV to `results_numa_raw.csv` and a duplicate-header
filtered CSV to `results_numa_clean.csv` under `OUT_DIR`. It also writes
`experiment_plan.txt`, which records the fixed parameters, sweep parameters, and
how to interpret the queue-depth experiment.

## Code Layout

- `src/config.c`: command-line parsing and configuration validation.
- `src/main.c`: experiment setup, worker lifecycle, cleanup, and CSV output.
- `src/worker.c`: Random, CSMA-like, and AIMD request policies.
- `src/stats.c`: per-thread latency collection and summary statistics.
- `src/numa_backend.c`: NUMA allocation, CPU binding, and Type-3 backend memory
  requests.
- `scripts/run_batch.sh`: batch experiment runner with environment-variable
  overrides.

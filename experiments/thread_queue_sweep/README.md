# Worker/Credit Oversubscription Experiment

This experiment compares the existing injection policies while changing one
primary contention variable: the number of CPU workers competing for a fixed
number of global fabric credits.

Default configuration:

```text
cpu_node=0
mem_node=1
threads=1 2 4 8 16 32
seconds=15
seeds=1 2 3 4 5
touches_per_req=4096
load=100
modes=0 1 2
queue_depth=4
device_workers=1
```

The main x-axis is:

```text
oversubscription = threads / queue_depth
```

The policies use different pacing semantics:

- `random` blocks until a credit is available and is the no-rejection baseline.
- `csma` retains the same logical request and retries it with exponential
  backoff when no credit is available.
- `aimd` uses one global shared congestion window across all workers and also
  retains the same logical request across retries. Each AIMD worker owns at
  most one submitted request, and device completion immediately releases both
  the fabric credit and the global AIMD in-flight slot.
- Retry-based latency includes admission waiting and backoff.
- Acceptance rate means successful admissions divided by admission attempts.

For this reason, logical-request p99 must be read together with attempt success
rate and retries per completion.

Run from the project root:

```bash
make clean && make
./experiments/thread_queue_sweep/run_thread_queue_sweep.sh
python3 experiments/thread_queue_sweep/plot_thread_queue_sweep.py
```

For a fast AIMD-only validation run:

```bash
make clean && make
bash experiments/thread_queue_sweep/run_aimd_validation.sh
```

The validation wrapper runs only mode `2`, using three seeds and ten seconds
per point by default. It writes results to `results/aimd_validation/` and
generates the figures automatically. Override the quick defaults when needed:

```bash
DURATION=15 SEEDS="1 2 3 4 5" \
bash experiments/thread_queue_sweep/run_aimd_validation.sh
```

Outputs are written to `results/thread_queue_sweep/`.

The plotting script generates:

```text
oversubscription_vs_goodput
oversubscription_vs_logical_request_p99
oversubscription_vs_acceptance
oversubscription_vs_rejections_per_completion
goodput_vs_logical_request_p99_tradeoff
aimd_global_window
```

Custom environment variables remain supported. Run the blocking baseline alone
with:

```bash
MODES="0" ./experiments/thread_queue_sweep/run_thread_queue_sweep.sh
```

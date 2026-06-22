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

The current implementation has different admission semantics:

- `random` blocks until a credit is available and is the no-rejection baseline.
- `csma` and `aimd` discard an attempt when no credit is available.
- Their `retry` counter is therefore interpreted as admission rejection.
- Their latency percentiles cover admitted and completed requests only.

For this reason, admitted-request p99 must be read together with acceptance
rate and admission rejections per completion.

Run from the project root:

```bash
make clean && make
./experiments/thread_queue_sweep/run_thread_queue_sweep.sh
python3 experiments/thread_queue_sweep/plot_thread_queue_sweep.py
```

Outputs are written to `results/thread_queue_sweep/`.

The plotting script generates:

```text
oversubscription_vs_goodput
oversubscription_vs_admitted_p99
oversubscription_vs_acceptance
goodput_vs_admitted_p99_tradeoff
```

Custom environment variables remain supported. Run the blocking baseline alone
with:

```bash
MODES="0" ./experiments/thread_queue_sweep/run_thread_queue_sweep.sh
```

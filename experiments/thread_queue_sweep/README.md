# Thread/Queue Sweep Experiment

This directory contains the original policy and contention sweep for
`cxl_numa_csma`.

Default configuration:

```text
cpu_node=0
mem_node=1
threads=8 16
seconds=5
seeds=1 2 3
touches_per_req=4096
loads=10 30 50 70 90
modes=0 1 2
queue_depth=4 8
device_workers=1 2
```

Run from the project root:

```bash
make clean && make
./experiments/thread_queue_sweep/run_thread_queue_sweep.sh
python3 experiments/thread_queue_sweep/plot_thread_queue_sweep.py
```

Outputs are written to `results/thread_queue_sweep/`.

The older comprehensive plotter is also available here:

```bash
python3 experiments/thread_queue_sweep/plot_thread_queue_comprehensive.py
```

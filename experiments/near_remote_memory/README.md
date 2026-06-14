# Near vs Remote Memory Experiment

This directory contains a small, isolated experiment for comparing local NUMA
backend memory and remote NUMA backend memory in `cxl_numa_csma`.

Default configuration:

```text
cpu_node=0
mem_node=0 1
threads=8
seconds=10
seed=1
touches_per_req=4096
loads=10 30 50 70 90
modes=0 1 2
queue_depth=4
device_workers=1
```

Run from the project root:

```bash
make clean && make
./experiments/near_remote_memory/run_near_remote.sh
python3 experiments/near_remote_memory/plot_near_remote.py
```

Outputs are written to `results/near_remote_memory/`.

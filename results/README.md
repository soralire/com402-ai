# Results Layout

Experiment outputs are grouped by experiment name:

```text
results/
  thread_queue_sweep/   original thread/queue/device-worker sweep results
  near_remote_memory/   local-vs-remote memory comparison results
```

Current normalized file names:

- `results/thread_queue_sweep/thread_queue_raw.csv`
- `results/thread_queue_sweep/thread_queue_clean.csv`
- `results/near_remote_memory/near_remote_raw.csv`
- `results/near_remote_memory/near_remote_clean.csv`

Legacy files with older names are kept under each experiment's `legacy_*`
subdirectories when present.

#!/usr/bin/env bash
set -euo pipefail

# Batch experiment script for cxl_numa_csma.
#
# Experimental design:
#   1. queue_depth=4/8 keeps the CXL Switch / Type-3 queue moderately loaded.
#   2. threads=8/16 intentionally exceeds queue_depth to recreate contention.
#   3. device_workers=1/2 models Type-3 device service parallelism.
#   4. load=10/30/50/70/90 covers light, medium, high, and near-saturation load.
#   5. modes=0/1/2 compares Random, CSMA-like, and AIMD under the same fabric.
#   6. seeds=1/2/3 provide repeated runs for basic variance checking.
#
# Common overrides:
#   QUEUE_DEPTHS="4 8" THREADS_LIST="8 16" DEVICE_WORKERS_LIST="1 2" DURATION=5 ./scripts/run_batch.sh
#   QUEUE_DEPTH=8 THREADS=16 DEVICE_WORKERS=1 LOADS="50 90" SEEDS="1" ./scripts/run_batch.sh

BIN="${BIN:-./cxl_numa_csma}"
OUT_DIR="${OUT_DIR:-results}"
RAW="${RAW:-$OUT_DIR/results_numa_raw.csv}"
CLEAN="${CLEAN:-$OUT_DIR/results_numa_clean.csv}"
PLAN="${PLAN:-$OUT_DIR/experiment_plan.txt}"

THREADS_LIST="${THREADS_LIST:-${THREADS:-8 16}}"
DURATION="${DURATION:-5}"
MEM_NODE="${MEM_NODE:-1}"
CPU_NODE="${CPU_NODE:-0}"
MEM_MB="${MEM_MB:-512}"
TOUCHES="${TOUCHES:-4096}"
DEVICE_WORKERS_LIST="${DEVICE_WORKERS_LIST:-${DEVICE_WORKERS:-1 2}}"

MODES="${MODES:-0 1 2}"
LOADS="${LOADS:-10 30 50 70 90}"
SEEDS="${SEEDS:-1 2 3}"
QUEUE_DEPTHS="${QUEUE_DEPTHS:-${QUEUE_DEPTH:-4 8}}"

count_words() {
  local count=0
  local item
  for item in $1; do
    count=$((count + 1))
  done
  echo "$count"
}

mkdir -p "$OUT_DIR"
rm -f "$RAW" "$CLEAN" "$PLAN"

MODE_COUNT=$(count_words "$MODES")
LOAD_COUNT=$(count_words "$LOADS")
SEED_COUNT=$(count_words "$SEEDS")
QUEUE_DEPTH_COUNT=$(count_words "$QUEUE_DEPTHS")
THREAD_COUNT=$(count_words "$THREADS_LIST")
DEVICE_WORKER_COUNT=$(count_words "$DEVICE_WORKERS_LIST")
TOTAL_RUNS=$((MODE_COUNT * LOAD_COUNT * SEED_COUNT * QUEUE_DEPTH_COUNT * THREAD_COUNT * DEVICE_WORKER_COUNT))

{
  echo "Experiment plan for cxl_numa_csma"
  echo
  echo "Model:"
  echo "  CPU workers -> CXL Switch queue -> Type-3 Memory Device -> NUMA remote memory backend"
  echo
  echo "Fixed configuration:"
  echo "  bin=$BIN"
  echo "  duration=$DURATION"
  echo "  mem_node=$MEM_NODE"
  echo "  cpu_node=$CPU_NODE"
  echo "  mem_mb=$MEM_MB"
  echo "  touches_per_req=$TOUCHES"
  echo
  echo "Sweep configuration:"
  echo "  modes=$MODES"
  echo "  loads=$LOADS"
  echo "  seeds=$SEEDS"
  echo "  threads=$THREADS_LIST"
  echo "  queue_depths=$QUEUE_DEPTHS"
  echo "  device_workers=$DEVICE_WORKERS_LIST"
  echo "  total_runs=$TOTAL_RUNS"
  echo
  echo "Interpretation:"
  echo "  threads > queue_depth recreates CXL Switch contention after queue_depth was expanded."
  echo "  queue_depth controls global concurrent slots in the CXL Switch / Type-3 queue."
  echo "  device_workers controls Type-3 backend service parallelism."
  echo "  Compare retry rate, goodput, p99 latency, and avg_cwnd across modes at the same load."
} | tee "$PLAN"

echo "[INFO] Checking NUMA topology"
numactl -H

echo "[INFO] Running $TOTAL_RUNS experiments"
run_id=0
for device_workers in $DEVICE_WORKERS_LIST; do
  for threads in $THREADS_LIST; do
    for queue_depth in $QUEUE_DEPTHS; do
      for seed in $SEEDS; do
        for load in $LOADS; do
          for mode in $MODES; do
            run_id=$((run_id + 1))
            echo "[RUN $run_id/$TOTAL_RUNS] mode=$mode load=$load threads=$threads seed=$seed queue_depth=$queue_depth device_workers=$device_workers"
            "$BIN" "$mode" "$load" "$threads" "$DURATION" \
              "$MEM_NODE" "$CPU_NODE" "$seed" "$MEM_MB" "$TOUCHES" "$queue_depth" "$device_workers" >> "$RAW"
          done
        done
      done
    done
  done
done

# Each program run prints a CSV header. Keep the first one only.
awk 'NR==1 || $1 !~ /^track/' "$RAW" > "$CLEAN"

echo "[INFO] Raw results:   $RAW"
echo "[INFO] Clean results: $CLEAN"
echo "[INFO] Plan file:     $PLAN"

echo "[INFO] Checking percentile order: p50 <= p95 <= p99"
BAD=$(awk -F, 'NR>1 && ($8>$9 || $9>$10) {print $0}' "$CLEAN" || true)
if [[ -n "$BAD" ]]; then
  echo "[WARN] Bad percentile rows found:"
  echo "$BAD"
else
  echo "[OK] Percentile order is valid"
fi

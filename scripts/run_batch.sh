#!/usr/bin/env bash
set -euo pipefail

# Batch experiment script for cxl_numa_csma.
#
# Experimental design:
#   1. queue_depth=1 is the baseline single-channel CXL Switch model.
#   2. queue_depth=4/8/16/32 explores deeper CXL Switch / Type-3 queues.
#   3. load=10/30/50/70/90 covers light, medium, high, and near-saturation load.
#   4. modes=0/1/2 compares Random, CSMA-like, and AIMD under the same fabric.
#   5. seeds=1/2/3 provide repeated runs for basic variance checking.
#
# Common overrides:
#   QUEUE_DEPTH=8 THREADS=4 DURATION=5 ./scripts/run_batch.sh
#   QUEUE_DEPTHS="1 8 32" LOADS="30 70 90" SEEDS="1" ./scripts/run_batch.sh

BIN="${BIN:-./cxl_numa_csma}"
OUT_DIR="${OUT_DIR:-results}"
RAW="${RAW:-$OUT_DIR/results_numa_raw.csv}"
CLEAN="${CLEAN:-$OUT_DIR/results_numa_clean.csv}"
PLAN="${PLAN:-$OUT_DIR/experiment_plan.txt}"

THREADS="${THREADS:-4}"
DURATION="${DURATION:-5}"
MEM_NODE="${MEM_NODE:-1}"
CPU_NODE="${CPU_NODE:-0}"
MEM_MB="${MEM_MB:-512}"
TOUCHES="${TOUCHES:-4096}"

MODES="${MODES:-0 1 2}"
LOADS="${LOADS:-10 30 50 70 90}"
SEEDS="${SEEDS:-1 2 3}"
QUEUE_DEPTHS="${QUEUE_DEPTHS:-${QUEUE_DEPTH:-1 4 8 16 32}}"

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
TOTAL_RUNS=$((MODE_COUNT * LOAD_COUNT * SEED_COUNT * QUEUE_DEPTH_COUNT))

{
  echo "Experiment plan for cxl_numa_csma"
  echo
  echo "Model:"
  echo "  CPU workers -> CXL Switch queue -> Type-3 Memory Device -> NUMA remote memory backend"
  echo
  echo "Fixed configuration:"
  echo "  bin=$BIN"
  echo "  threads=$THREADS"
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
  echo "  queue_depths=$QUEUE_DEPTHS"
  echo "  total_runs=$TOTAL_RUNS"
  echo
  echo "Interpretation:"
  echo "  queue_depth=1 is the original single-channel baseline."
  echo "  queue_depth>1 models concurrent slots in the CXL Switch / Type-3 queue."
  echo "  Compare goodput and p99 latency across modes at the same load and queue_depth."
} | tee "$PLAN"

echo "[INFO] Checking NUMA topology"
numactl -H

echo "[INFO] Running $TOTAL_RUNS experiments"
run_id=0
for queue_depth in $QUEUE_DEPTHS; do
  for seed in $SEEDS; do
    for load in $LOADS; do
      for mode in $MODES; do
        run_id=$((run_id + 1))
        echo "[RUN $run_id/$TOTAL_RUNS] mode=$mode load=$load seed=$seed queue_depth=$queue_depth"
        "$BIN" "$mode" "$load" "$THREADS" "$DURATION" \
          "$MEM_NODE" "$CPU_NODE" "$seed" "$MEM_MB" "$TOUCHES" "$queue_depth" >> "$RAW"
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

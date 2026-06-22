#!/usr/bin/env bash
set -euo pipefail

# Minimal, interpretable worker/credit oversubscription experiment.
#
# The default run fixes load, queue depth, and device parallelism, then varies
# only the worker count. Environment variables can still override every sweep
# dimension when a broader experiment is intentionally required.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BIN="${BIN:-$PROJECT_DIR/cxl_numa_csma}"
OUT_DIR="${OUT_DIR:-$PROJECT_DIR/results/thread_queue_sweep}"
RAW="${RAW:-$OUT_DIR/thread_queue_raw.csv}"
CLEAN="${CLEAN:-$OUT_DIR/thread_queue_clean.csv}"
PLAN="${PLAN:-$OUT_DIR/experiment_plan.txt}"

THREADS_LIST="${THREADS_LIST:-${THREADS:-1 2 4 8 16 32}}"
DURATION="${DURATION:-15}"
MEM_NODE="${MEM_NODE:-1}"
CPU_NODE="${CPU_NODE:-0}"
MEM_MB="${MEM_MB:-512}"
TOUCHES="${TOUCHES:-4096}"
DEVICE_WORKERS_LIST="${DEVICE_WORKERS_LIST:-${DEVICE_WORKERS:-1}}"

MODES="${MODES:-0 1 2}"
LOADS="${LOADS:-100}"
SEEDS="${SEEDS:-1 2 3 4 5}"
QUEUE_DEPTHS="${QUEUE_DEPTHS:-${QUEUE_DEPTH:-4}}"

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
  echo "Worker/credit oversubscription experiment for cxl_numa_csma"
  echo
  echo "Model:"
  echo "  CPU workers -> CXL Switch queue -> Type-3 Memory Device -> NUMA remote memory backend"
  echo
  echo "Fixed/default configuration:"
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
  echo "  oversubscription = threads / queue_depth."
  echo "  oversubscription <= 1 is the low-contention region."
  echo "  oversubscription > 1 creates competition for fixed fabric credits."
  echo "  Random is interpreted as the blocking/no-rejection baseline."
  echo "  CSMA/AIMD retain the same logical request across admission retries."
  echo "  Each AIMD worker owns at most one submitted request."
  echo "  AIMD device completion immediately releases its global in-flight slot."
  echo "  Their p99 values include admission waiting and backoff."
  echo "  acceptance rate means successful fabric admissions / admission attempts."
  echo "  Compare goodput, logical-request p99, attempt success rate, and retries per completion."
} | tee "$PLAN"

if command -v numactl >/dev/null 2>&1; then
  echo "[INFO] Checking NUMA topology"
  numactl -H
else
  echo "[WARN] numactl not found; continuing without topology print"
fi

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

echo "[INFO] Checking request accounting: attempts = success + retry"
BAD_ACCOUNTING=$(awk -F, 'NR>1 && ($4 != $5 + $6) {print $0}' "$CLEAN" || true)
if [[ -n "$BAD_ACCOUNTING" ]]; then
  echo "[WARN] Request-accounting rows found:"
  echo "$BAD_ACCOUNTING"
else
  echo "[OK] Request accounting is valid"
fi

echo "[INFO] Checking retry accounting: retry = backoff"
BAD_RETRY=$(awk -F, 'NR>1 && ($6 != $7) {print $0}' "$CLEAN" || true)
if [[ -n "$BAD_RETRY" ]]; then
  echo "[WARN] Retry/backoff mismatch rows found:"
  echo "$BAD_RETRY"
else
  echo "[OK] Retry/backoff accounting is valid"
fi

echo "[INFO] Checking AIMD global controller metrics"
BAD_AIMD=$(awk -F, 'NR>1 && $1=="aimd" && ($26 < 1 || $26 > $20 + 1 || $27 < 0 || $27 > $20 || $27 > $2 || $28 < 0 || $28 > $20 || $28 > $2) {print $0}' "$CLEAN" || true)
if [[ -n "$BAD_AIMD" ]]; then
  echo "[WARN] Invalid AIMD global-controller rows found:"
  echo "$BAD_AIMD"
else
  echo "[OK] AIMD global-controller metrics are in range"
fi

echo "[INFO] Plot command:"
echo "  python3 \"$SCRIPT_DIR/plot_thread_queue_sweep.py\" --csv \"$CLEAN\" --out-dir \"$OUT_DIR/figures\""

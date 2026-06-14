#!/usr/bin/env bash
set -euo pipefail

# 近端/远端内存对比实验。
# 默认只改变 mem_node，其它参数保持固定，避免和之前的大规模 sweep 混在一起。

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BIN="${BIN:-$PROJECT_DIR/cxl_numa_csma}"
OUT_DIR="${OUT_DIR:-$PROJECT_DIR/results/near_remote_memory}"
RAW="${RAW:-$OUT_DIR/near_remote_raw.csv}"
CLEAN="${CLEAN:-$OUT_DIR/near_remote_clean.csv}"
PLAN="${PLAN:-$OUT_DIR/experiment_plan.txt}"

CPU_NODE="${CPU_NODE:-0}"
MEM_NODES="${MEM_NODES:-0 1}"
THREADS="${THREADS:-8}"
DURATION="${DURATION:-10}"
SEED="${SEED:-1}"
TOUCHES="${TOUCHES:-4096}"
LOADS="${LOADS:-10 30 50 70 90}"
MODES="${MODES:-0 1 2}"
QUEUE_DEPTH="${QUEUE_DEPTH:-4}"
DEVICE_WORKERS="${DEVICE_WORKERS:-1}"
MEM_MB="${MEM_MB:-512}"

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
MEM_NODE_COUNT=$(count_words "$MEM_NODES")
TOTAL_RUNS=$((MODE_COUNT * LOAD_COUNT * MEM_NODE_COUNT))

{
  echo "Near vs remote memory experiment for cxl_numa_csma"
  echo
  echo "Purpose:"
  echo "  Compare NUMA-local backend memory and NUMA-remote backend memory under the same CXL-like fabric settings."
  echo "  When numactl is available, every run is wrapped with --cpunodebind=$CPU_NODE so worker and device threads stay on the CPU node."
  echo
  echo "Fixed configuration:"
  echo "  bin=$BIN"
  echo "  cpu_node=$CPU_NODE"
  echo "  threads=$THREADS"
  echo "  duration=$DURATION"
  echo "  seed=$SEED"
  echo "  mem_mb=$MEM_MB"
  echo "  touches_per_req=$TOUCHES"
  echo "  queue_depth=$QUEUE_DEPTH"
  echo "  device_workers=$DEVICE_WORKERS"
  echo
  echo "Sweep configuration:"
  echo "  mem_nodes=$MEM_NODES"
  echo "  modes=$MODES"
  echo "  loads=$LOADS"
  echo "  total_runs=$TOTAL_RUNS"
  echo
  echo "Interpretation:"
  echo "  mem_node=cpu_node is the local-memory baseline."
  echo "  mem_node!=cpu_node is the remote-memory backend, used here as the CXL-like Type-3 memory path."
  echo "  Compare p50/p95/p99 latency, goodput, retry rate, and backoff rate at the same mode/load."
} | tee "$PLAN"

if command -v numactl >/dev/null 2>&1; then
  echo "[INFO] NUMA topology"
  numactl -H
  RUN_CMD=(numactl "--cpunodebind=$CPU_NODE" "$BIN")
else
  echo "[WARN] numactl not found; continuing without topology print"
  RUN_CMD=("$BIN")
fi

echo "[INFO] Running $TOTAL_RUNS experiments"
run_id=0
for mem_node in $MEM_NODES; do
  for load in $LOADS; do
    for mode in $MODES; do
      run_id=$((run_id + 1))
      echo "[RUN $run_id/$TOTAL_RUNS] mode=$mode load=$load cpu_node=$CPU_NODE mem_node=$mem_node"
      "${RUN_CMD[@]}" "$mode" "$load" "$THREADS" "$DURATION" \
        "$mem_node" "$CPU_NODE" "$SEED" "$MEM_MB" "$TOUCHES" "$QUEUE_DEPTH" "$DEVICE_WORKERS" >> "$RAW"
    done
  done
done

# 程序每次运行都会输出 CSV header，这里只保留第一行 header。
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

echo "[INFO] Plot command:"
echo "  python3 \"$SCRIPT_DIR/plot_near_remote.py\" --csv \"$CLEAN\" --out-dir \"$OUT_DIR/figures\""

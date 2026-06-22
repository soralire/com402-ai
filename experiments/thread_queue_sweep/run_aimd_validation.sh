#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Fast AIMD-only regression: 6 worker counts x 3 seeds x 10 seconds.
# Override DURATION or SEEDS when a longer validation is required.
OUT_DIR="${OUT_DIR:-$PROJECT_DIR/results/aimd_validation}"
DURATION="${DURATION:-10}"
SEEDS="${SEEDS:-1 2 3}"
PYTHON="${PYTHON:-python3}"

MODES="2" \
OUT_DIR="$OUT_DIR" \
DURATION="$DURATION" \
SEEDS="$SEEDS" \
"$SCRIPT_DIR/run_thread_queue_sweep.sh"

"$PYTHON" "$SCRIPT_DIR/plot_thread_queue_sweep.py" \
  --csv "$OUT_DIR/thread_queue_clean.csv" \
  --out-dir "$OUT_DIR/figures"

echo "[OK] AIMD-only validation completed"
echo "  CSV:     $OUT_DIR/thread_queue_clean.csv"
echo "  Summary: $OUT_DIR/figures/summary_by_oversubscription.csv"

#!/usr/bin/env bash
# Run person_detector validation capture session.
# Requires Linux, root privileges, and built binary.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${ROOT_DIR}/validation/output"
BINARY="${ROOT_DIR}/build-debug/person_detector"
DURATION="${1:-60}"
INTERVAL="${2:-5}"

mkdir -p "${OUTPUT_DIR}"
STAMP="$(date +%Y%m%d_%H%M%S)"
JSONL="${OUTPUT_DIR}/run_${STAMP}.jsonl"

echo "Building debug binary..."
make -C "${ROOT_DIR}" debug

if [[ "${EUID}" -ne 0 ]]; then
  echo "WARNING: Run with sudo for live BLE/Wi-Fi capture."
  echo "Falling back to replay demo mode..."
  timeout "${DURATION}" "${BINARY}" \
    --replay "${ROOT_DIR}/validation/sample_traffic.jsonl" \
    --replay-loop \
    --interval "${INTERVAL}" \
    --json-out "${JSONL}" \
    --verbose || true
else
  timeout "${DURATION}" "${BINARY}" \
    --interval "${INTERVAL}" \
    --json-out "${JSONL}" \
    --verbose || true
fi

echo "Capture complete: ${JSONL}"
echo "Analyze with: python3 validation/analyze_results.py --estimates ${JSONL} --ground-truth validation/ground_truth_template.csv"

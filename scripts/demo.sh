#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

echo "=== Person Detector Demo ==="

if [[ -f build-debug/person_detector ]]; then
  BIN=build-debug/person_detector
elif [[ -f build/person_detector ]]; then
  BIN=build/person_detector
else
  echo "Building..."
  make debug
  BIN=build-debug/person_detector
fi

JSONL="/tmp/person_detector_demo_$$.jsonl"
echo "Running replay demo for 20 seconds..."
timeout 20 "${BIN}" \
  --replay validation/sample_traffic.jsonl \
  --replay-loop \
  --interval 2 \
  --json-out "${JSONL}" \
  --verbose || true

echo ""
echo "=== Demo complete ==="
echo "JSON output: ${JSONL}"
if [[ -f "${JSONL}" ]]; then
  echo "Last estimate:"
  tail -1 "${JSONL}"
fi

echo ""
echo "Run validation analysis:"
echo "  make analyze-validation"

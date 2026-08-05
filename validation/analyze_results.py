#!/usr/bin/env python3
"""Analyze person_detector estimates against ground-truth counts."""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple


@dataclass
class EstimateRow:
    timestamp: int
    estimate: float
    lower_bound: float
    upper_bound: float


@dataclass
class GroundTruthRow:
    timestamp: int
    actual: int
    location: str
    notes: str


@dataclass
class MatchedRow:
    timestamp: int
    actual: int
    estimate: float
    error: float
    within_bounds: bool


def load_estimates(path: Path) -> List[EstimateRow]:
    rows: List[EstimateRow] = []
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            rows.append(
                EstimateRow(
                    timestamp=int(obj["timestamp"]),
                    estimate=float(obj["estimate"]),
                    lower_bound=float(obj.get("lower_bound", 0.0)),
                    upper_bound=float(obj.get("upper_bound", obj["estimate"])),
                )
            )
    return rows


def load_ground_truth(path: Path) -> List[GroundTruthRow]:
    rows: List[GroundTruthRow] = []
    with path.open(encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        for rec in reader:
            rows.append(
                GroundTruthRow(
                    timestamp=int(rec["timestamp"]),
                    actual=int(rec["actual_people_count"]),
                    location=rec.get("location", ""),
                    notes=rec.get("notes", ""),
                )
            )
    return rows


def match_rows(
    estimates: List[EstimateRow], ground_truth: List[GroundTruthRow], max_delta: int = 10
) -> List[MatchedRow]:
    matched: List[MatchedRow] = []
    for gt in ground_truth:
        best: EstimateRow | None = None
        best_delta = max_delta + 1
        for est in estimates:
            delta = abs(est.timestamp - gt.timestamp)
            if delta < best_delta:
                best_delta = delta
                best = est
        if best is None or best_delta > max_delta:
            continue
        error = abs(best.estimate - gt.actual)
        within = best.lower_bound <= gt.actual <= best.upper_bound
        matched.append(
            MatchedRow(
                timestamp=gt.timestamp,
                actual=gt.actual,
                estimate=best.estimate,
                error=error,
                within_bounds=within,
            )
        )
    return matched


def compute_metrics(matched: List[MatchedRow]) -> Tuple[float, float, float, float]:
    if not matched:
        return 0.0, 0.0, 0.0, 0.0
    errors = [m.error for m in matched]
    mae = sum(errors) / len(errors)
    rmse = math.sqrt(sum(e * e for e in errors) / len(errors))
    coverage = sum(1 for m in matched if m.within_bounds) / len(matched)
    bias = sum(m.estimate - m.actual for m in matched) / len(matched)
    return mae, rmse, coverage, bias


def write_report(path: Path, matched: List[MatchedRow], mae: float, rmse: float, coverage: float, bias: float) -> None:
    lines = [
        "# Validation Results Summary",
        "",
        "| Metric | Value |",
        "|--------|-------|",
        f"| Samples | {len(matched)} |",
        f"| MAE (persons) | {mae:.3f} |",
        f"| RMSE (persons) | {rmse:.3f} |",
        f"| 95% CI Coverage | {coverage * 100:.1f}% |",
        f"| Mean Bias | {bias:+.3f} |",
        "",
        "## Matched Intervals",
        "",
        "| Timestamp | Actual | Estimate | Abs Error | In CI |",
        "|-----------|--------|----------|-----------|-------|",
    ]
    for m in matched:
        lines.append(
            f"| {m.timestamp} | {m.actual} | {m.estimate:.2f} | {m.error:.2f} | {'yes' if m.within_bounds else 'no'} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze person_detector validation results")
    parser.add_argument("--estimates", type=Path, required=True)
    parser.add_argument("--ground-truth", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("validation/results_summary.md"))
    args = parser.parse_args()

    estimates = load_estimates(args.estimates)
    ground_truth = load_ground_truth(args.ground_truth)
    matched = match_rows(estimates, ground_truth)

    mae, rmse, coverage, bias = compute_metrics(matched)

    print("Validation Metrics")
    print("==================")
    print(f"Samples:         {len(matched)}")
    print(f"MAE:             {mae:.3f} persons")
    print(f"RMSE:            {rmse:.3f} persons")
    print(f"CI Coverage:     {coverage * 100:.1f}%")
    print(f"Mean Bias:       {bias:+.3f} persons")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_report(args.output, matched, mae, rmse, coverage, bias)
    print(f"\nReport written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

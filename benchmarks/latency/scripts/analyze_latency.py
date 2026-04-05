#!/usr/bin/env python3
"""Summarize alpbook_latency_benchmark CSV output (column latency_ns, nanoseconds).

By default the first 10% of samples in file order are dropped (warmup / steady-state
discard). Use ``--warmup-pct 0`` to analyze every row.

Percentiles use linear interpolation between closest ranks (same idea as
numpy.percentile with method="linear"): for p in (0,100], position is
(p/100) * (n - 1) in the sorted array, with linear blend of neighbors.
"""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from pathlib import Path


def percentile_linear(sorted_samples: list[int], p: float) -> float:
    """p in (0, 100]; returns float (may be non-integer for interpolation)."""
    n = len(sorted_samples)
    if n == 1:
        return float(sorted_samples[0])
    if p <= 0:
        return float(sorted_samples[0])
    if p >= 100:
        return float(sorted_samples[-1])
    pos = (p / 100.0) * (n - 1)
    lo = int(pos)
    hi = min(lo + 1, n - 1)
    frac = pos - lo
    return sorted_samples[lo] + frac * (sorted_samples[hi] - sorted_samples[lo])


def drop_warmup_prefix(values: list[int], warmup_pct: float) -> tuple[list[int], int]:
    """Remove the first floor(n * warmup_pct / 100) samples (file order). Returns (kept, dropped)."""
    if warmup_pct <= 0 or not values:
        return values, 0
    n = len(values)
    drop = int(n * warmup_pct / 100.0)
    if drop <= 0:
        return values, 0
    if drop >= n:
        return [], n
    return values[drop:], drop


def load_latencies(path: Path) -> tuple[list[int], str | None]:
    """
    Returns (values, header_error).
    header_error is set if the file has a header row but the column is not latency_ns.
    """
    raw = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if not raw:
        return [], None

    values: list[int] = []
    header_error: str | None = None

    # Try CSV with header
    first = raw[0].strip()
    if first.lower() == "latency_ns" or ("," in first and "latency" in first.lower()):
        reader = csv.DictReader(raw)
        if reader.fieldnames is None:
            return [], "Could not parse CSV header."
        fields = [f.strip() if f else "" for f in reader.fieldnames]
        if "latency_ns" not in fields:
            header_error = f"Expected column 'latency_ns', got fields: {reader.fieldnames!r}"
            return [], header_error
        for row in reader:
            if row is None:
                continue
            cell = (row.get("latency_ns") or "").strip()
            if not cell:
                continue
            try:
                values.append(int(cell))
            except ValueError as e:
                raise ValueError(f"Non-integer latency_ns: {cell!r}") from e
        return values, None

    # Single column, no header (or plain integers per line)
    for line in raw:
        line = line.strip()
        if not line:
            continue
        if "," in line:
            parts = [p.strip() for p in line.split(",")]
            cell = parts[0] if parts else ""
        else:
            cell = line
        try:
            values.append(int(cell))
        except ValueError as e:
            raise ValueError(f"Non-integer line: {line!r}") from e
    return values, None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze latency_ns CSV from alpbook_latency_benchmark."
    )
    parser.add_argument(
        "csv_path",
        type=Path,
        help="Path to CSV file (header latency_ns recommended).",
    )
    parser.add_argument(
        "--warmup-pct",
        type=float,
        default=10.0,
        metavar="PCT",
        help="Drop first PCT%% of samples in file order before stats (default: 10). Use 0 for no drop.",
    )
    args = parser.parse_args()
    path: Path = args.csv_path

    if not path.is_file():
        print(f"error: not a file: {path}", file=sys.stderr)
        return 2

    try:
        values, header_err = load_latencies(path)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if header_err:
        print(f"error: {header_err}", file=sys.stderr)
        return 1

    if not values:
        print("error: no latency samples found (empty or missing data rows).", file=sys.stderr)
        return 1

    w = args.warmup_pct
    if w < 0 or w > 100:
        print("error: --warmup-pct must be between 0 and 100.", file=sys.stderr)
        return 1

    raw_n = len(values)
    values, dropped = drop_warmup_prefix(values, w)
    if not values:
        print(
            "error: after warmup discard, no samples left (increase input size or lower --warmup-pct).",
            file=sys.stderr,
        )
        return 1

    negatives = [x for x in values if x < 0]
    if negatives:
        print(
            f"warning: {len(negatives)} negative latency sample(s) (clock/TSC issue?)",
            file=sys.stderr,
        )

    values_sorted = sorted(values)
    n = len(values_sorted)
    pct_levels: tuple[tuple[float, str], ...] = (
        (50, "p50_ns"),
        (90, "p90_ns"),
        (95, "p95_ns"),
        (99, "p99_ns"),
        (99.9, "p99_9_ns"),
    )

    print(f"file: {path}")
    print(f"raw_samples: {raw_n}")
    if dropped:
        print(f"warmup_dropped: {dropped} ({w:g}% prefix in file order)")
    print(f"samples: {n}")
    print(f"min_ns: {values_sorted[0]}")
    print(f"max_ns: {values_sorted[-1]}")
    print(f"mean_ns: {statistics.mean(values_sorted):.6g}")
    print(f"median_ns: {statistics.median(values_sorted):.6g}")
    for p, label in pct_levels:
        v = percentile_linear(values_sorted, p)
        print(f"{label}: {v:.6g}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

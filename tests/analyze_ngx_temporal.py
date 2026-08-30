#!/usr/bin/env python3
"""Measure DLSS-G temporal index ordering from NGX luma probe captures."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def mad(first: bytes, second: bytes) -> float:
    return sum(abs(a - b) for a, b in zip(first, second)) / len(first)


def projected_position(previous: bytes, generated: bytes, current: bytes) -> float:
    numerator = 0
    denominator = 0
    for before, sample, after in zip(previous, generated, current):
        direction = after - before
        numerator += (sample - before) * direction
        denominator += direction * direction
    return numerator / denominator if denominator else math.nan


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        return [row for row in csv.DictReader(stream) if row.get("grayOffset")]


def load_frame(blob: bytes, row: dict[str, str]) -> bytes | None:
    try:
        offset = int(row["grayOffset"])
    except (KeyError, TypeError, ValueError):
        return None
    width = int(row.get("grayWidth") or 64)
    height = int(row.get("grayHeight") or 36)
    frame_bytes = width * height
    if offset < 0 or offset + frame_bytes > len(blob):
        return None
    return blob[offset : offset + frame_bytes]


def analyze(
    run_dir: Path,
    min_batch: int,
    max_batch: int | None,
    endpoint_threshold: float,
) -> dict:
    generated_path = next(run_dir.glob("MfgUnlock-ngx-output-*.csv"))
    real_path = next(run_dir.glob("MfgUnlock-ngx-real-*.csv"))
    gray_path = next(run_dir.glob("MfgUnlock-ngx-output-*.gray"))
    blob = gray_path.read_bytes()

    real: dict[int, bytes] = {}
    for row in read_rows(real_path):
        frame = load_frame(blob, row)
        if frame is not None:
            real[int(row["batch"])] = frame

    generated: dict[int, dict[int, bytes]] = {}
    counts: dict[int, int] = {}
    for row in read_rows(generated_path):
        frame = load_frame(blob, row)
        if frame is None:
            continue
        batch = int(row["batch"])
        index = int(row["index"])
        generated.setdefault(batch, {})[index] = frame
        counts[batch] = int(row["count"])

    progress_by_index: dict[int, list[float]] = {}
    projection_by_index: dict[int, list[float]] = {}
    forward_projection_by_index: dict[int, list[float]] = {}
    step_fraction_by_slot: dict[int, list[float]] = {}
    endpoint_changes: list[float] = []
    spacing_cv: list[float] = []
    progress_errors: list[float] = []
    complete = 0
    moving = 0
    monotonic = 0
    distance_monotonic = 0

    for batch in sorted(generated):
        count = counts[batch]
        if (
            batch < min_batch
            or (max_batch is not None and batch > max_batch)
            or batch - 1 not in real
            or batch not in real
        ):
            continue
        outputs = generated[batch]
        if any(index not in outputs for index in range(1, count + 1)):
            continue
        complete += 1
        previous = real[batch - 1]
        current = real[batch]
        endpoint_change = mad(previous, current)
        if endpoint_change < endpoint_threshold:
            continue
        moving += 1
        endpoint_changes.append(endpoint_change)

        progress: list[float] = []
        distances_from_previous: list[float] = []
        distances_to_current: list[float] = []
        chain = [previous]
        for index in range(1, count + 1):
            sample = outputs[index]
            before_distance = mad(previous, sample)
            after_distance = mad(sample, current)
            denominator = before_distance + after_distance
            position = before_distance / denominator if denominator else math.nan
            projection = projected_position(previous, sample, current)
            progress.append(position)
            distances_from_previous.append(before_distance)
            distances_to_current.append(after_distance)
            progress_by_index.setdefault(index, []).append(position)
            projection_by_index.setdefault(index, []).append(projection)
            if batch + 1 in real:
                forward_projection_by_index.setdefault(index, []).append(
                    projected_position(real[batch], sample, real[batch + 1])
                )
            expected = index / (count + 1)
            progress_errors.append(abs(position - expected))
            chain.append(sample)
        chain.append(current)

        if all(progress[i] < progress[i + 1] for i in range(len(progress) - 1)):
            monotonic += 1
        if (all(distances_from_previous[i] < distances_from_previous[i + 1]
                for i in range(len(distances_from_previous) - 1))
                and all(distances_to_current[i] > distances_to_current[i + 1]
                        for i in range(len(distances_to_current) - 1))):
            distance_monotonic += 1

        steps = [mad(chain[i], chain[i + 1]) for i in range(len(chain) - 1)]
        step_sum = sum(steps)
        if step_sum > 0:
            fractions = [step / step_sum for step in steps]
            for slot, fraction in enumerate(fractions):
                step_fraction_by_slot.setdefault(slot, []).append(fraction)
            mean_step = statistics.fmean(steps)
            spacing_cv.append(statistics.pstdev(steps) / mean_step if mean_step else 0.0)

    count_values = set(counts.values())
    generated_count = next(iter(count_values)) if len(count_values) == 1 else None
    expected_step = 1.0 / (generated_count + 1) if generated_count else math.nan

    return {
        "run": str(run_dir),
        "generated_count": generated_count,
        "complete_batches_after_minimum": complete,
        "moving_batches": moving,
        "endpoint_mad": {
            "median": percentile(endpoint_changes, 0.5),
            "p10": percentile(endpoint_changes, 0.1),
            "p90": percentile(endpoint_changes, 0.9),
        },
        "progress_by_index": {
            str(index): {
                "expected": index / (generated_count + 1) if generated_count else math.nan,
                "median_distance_ratio": percentile(values, 0.5),
                "p10_distance_ratio": percentile(values, 0.1),
                "p90_distance_ratio": percentile(values, 0.9),
                "median_projection": percentile(projection_by_index[index], 0.5),
                "median_projection_if_paired_with_next_interval": percentile(
                    forward_projection_by_index.get(index, []), 0.5
                ),
            }
            for index, values in sorted(progress_by_index.items())
        },
        "strictly_monotonic_progress_fraction": monotonic / moving if moving else math.nan,
        "both_endpoint_distances_monotonic_fraction": (
            distance_monotonic / moving if moving else math.nan
        ),
        "median_absolute_progress_error": percentile(progress_errors, 0.5),
        "spacing_cv": {
            "median": percentile(spacing_cv, 0.5),
            "p90": percentile(spacing_cv, 0.9),
        },
        "step_fraction_by_slot": {
            str(slot): {
                "expected": expected_step,
                "median": percentile(values, 0.5),
                "p10": percentile(values, 0.1),
                "p90": percentile(values, 0.9),
            }
            for slot, values in sorted(step_fraction_by_slot.items())
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dirs", nargs="+", type=Path)
    parser.add_argument("--min-batch", type=int, default=50)
    parser.add_argument("--max-batch", type=int)
    parser.add_argument("--endpoint-threshold", type=float, default=0.25)
    arguments = parser.parse_args()
    results = [
        analyze(
            path.resolve(),
            arguments.min_batch,
            arguments.max_batch,
            arguments.endpoint_threshold,
        )
        for path in arguments.run_dirs
    ]
    print(json.dumps(results, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()

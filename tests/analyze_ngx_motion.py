#!/usr/bin/env python3
"""Estimate generated-frame motion positions with local phase-independent NCC."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

import numpy as np
from scipy.ndimage import uniform_filter


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.nan
    return float(np.percentile(np.asarray(values, dtype=np.float64), fraction * 100.0))


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        return [row for row in csv.DictReader(stream) if row.get("grayOffset")]


def load_frame(blob: bytes, row: dict[str, str]) -> np.ndarray | None:
    width = int(row.get("grayWidth") or 64)
    height = int(row.get("grayHeight") or 36)
    offset = int(row["grayOffset"])
    count = width * height
    if offset < 0 or offset + count > len(blob):
        return None
    return np.frombuffer(blob, dtype=np.uint8, count=count, offset=offset).reshape(
        height, width
    ).astype(np.float32)


def shifted(image: np.ndarray, dx: int, dy: int) -> np.ndarray:
    result = np.zeros_like(image)
    height, width = image.shape
    source_left = max(0, dx)
    source_right = width + min(0, dx)
    source_top = max(0, dy)
    source_bottom = height + min(0, dy)
    target_left = max(0, -dx)
    target_right = width - max(0, dx)
    target_top = max(0, -dy)
    target_bottom = height - max(0, dy)
    result[target_top:target_bottom, target_left:target_right] = image[
        source_top:source_bottom, source_left:source_right
    ]
    return result


def ncc_flow(
    reference: np.ndarray, target: np.ndarray, search_radius: int, window: int
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    reference_mean = uniform_filter(reference, window, mode="nearest")
    reference_sq_mean = uniform_filter(reference * reference, window, mode="nearest")
    reference_variance = np.maximum(reference_sq_mean - reference_mean**2, 1.0)
    scores: list[np.ndarray] = []
    offsets: list[tuple[int, int]] = []
    for dy in range(-search_radius, search_radius + 1):
        for dx in range(-search_radius, search_radius + 1):
            candidate = shifted(target, dx, dy)
            candidate_mean = uniform_filter(candidate, window, mode="nearest")
            candidate_sq_mean = uniform_filter(
                candidate * candidate, window, mode="nearest"
            )
            covariance = (
                uniform_filter(reference * candidate, window, mode="nearest")
                - reference_mean * candidate_mean
            )
            candidate_variance = np.maximum(
                candidate_sq_mean - candidate_mean**2, 1.0
            )
            scores.append(
                covariance / np.sqrt(reference_variance * candidate_variance)
            )
            offsets.append((dx, dy))
    score_stack = np.stack(scores)
    best_index = np.argmax(score_stack, axis=0)
    best_score = np.take_along_axis(
        score_stack, best_index[np.newaxis, :, :], axis=0
    )[0]
    dx = np.empty(reference.shape, dtype=np.float32)
    dy = np.empty(reference.shape, dtype=np.float32)
    for index, (offset_x, offset_y) in enumerate(offsets):
        mask = best_index == index
        dx[mask] = offset_x
        dy[mask] = offset_y

    side = search_radius * 2 + 1
    x_index = best_index % side
    y_index = best_index // side
    rows, columns = np.indices(reference.shape)
    center = score_stack[best_index, rows, columns]

    valid_x = (x_index > 0) & (x_index < side - 1)
    left_index = np.maximum(best_index - 1, 0)
    right_index = np.minimum(best_index + 1, score_stack.shape[0] - 1)
    left = score_stack[left_index, rows, columns]
    right = score_stack[right_index, rows, columns]
    denominator_x = left - 2.0 * center + right
    delta_x = np.zeros(reference.shape, dtype=np.float32)
    stable_x = valid_x & (np.abs(denominator_x) > 1e-5)
    delta_x[stable_x] = np.clip(
        0.5 * (left[stable_x] - right[stable_x]) / denominator_x[stable_x],
        -0.5,
        0.5,
    )

    valid_y = (y_index > 0) & (y_index < side - 1)
    up_index = np.maximum(best_index - side, 0)
    down_index = np.minimum(best_index + side, score_stack.shape[0] - 1)
    up = score_stack[up_index, rows, columns]
    down = score_stack[down_index, rows, columns]
    denominator_y = up - 2.0 * center + down
    delta_y = np.zeros(reference.shape, dtype=np.float32)
    stable_y = valid_y & (np.abs(denominator_y) > 1e-5)
    delta_y[stable_y] = np.clip(
        0.5 * (up[stable_y] - down[stable_y]) / denominator_y[stable_y],
        -0.5,
        0.5,
    )
    return dx + delta_x, dy + delta_y, best_score, reference_variance


def analyze(
    run_dir: Path,
    min_batch: int,
    batch_stride: int,
    search_radius: int,
    window: int,
    grid_stride: int,
) -> dict:
    generated_path = next(run_dir.glob("MfgUnlock-ngx-output-*.csv"))
    real_path = next(run_dir.glob("MfgUnlock-ngx-real-*.csv"))
    gray_path = next(run_dir.glob("MfgUnlock-ngx-output-*.gray"))
    blob = gray_path.read_bytes()

    real: dict[int, np.ndarray] = {}
    for row in read_rows(real_path):
        frame = load_frame(blob, row)
        if frame is not None:
            real[int(row["batch"])] = frame

    generated: dict[int, dict[int, np.ndarray]] = {}
    counts: dict[int, int] = {}
    for row in read_rows(generated_path):
        frame = load_frame(blob, row)
        if frame is None:
            continue
        batch = int(row["batch"])
        generated.setdefault(batch, {})[int(row["index"])] = frame
        counts[batch] = int(row["count"])

    count_values = set(counts.values())
    generated_count = next(iter(count_values)) if len(count_values) == 1 else None
    position_by_index: dict[int, list[float]] = {}
    accepted_features: list[int] = []
    endpoint_magnitudes: list[float] = []
    batches = 0

    for batch in sorted(generated):
        if batch < min_batch or (batch - min_batch) % batch_stride != 0:
            continue
        count = counts[batch]
        if batch - 1 not in real or batch not in real:
            continue
        if any(index not in generated[batch] for index in range(1, count + 1)):
            continue
        previous = real[batch - 1]
        current = real[batch]
        end_x, end_y, end_score, texture = ncc_flow(
            previous, current, search_radius, window
        )
        height, width = previous.shape
        margin = search_radius + window // 2 + 1
        grid = np.zeros(previous.shape, dtype=bool)
        grid[margin : height - margin : grid_stride,
             margin : width - margin : grid_stride] = True
        magnitude_sq = end_x**2 + end_y**2
        valid = (
            grid
            & (texture > 80.0)
            & (end_score > 0.72)
            & (magnitude_sq > 0.12**2)
            & (magnitude_sq < (search_radius - 0.6) ** 2)
        )
        if np.count_nonzero(valid) < 3:
            continue

        generated_flows = []
        for index in range(1, count + 1):
            flow_x, flow_y, score, _ = ncc_flow(
                previous, generated[batch][index], search_radius, window
            )
            generated_flows.append((flow_x, flow_y, score))
            valid &= score > 0.62
        feature_count = int(np.count_nonzero(valid))
        if feature_count < 3:
            continue

        batches += 1
        accepted_features.append(feature_count)
        endpoint_magnitudes.append(float(np.median(np.sqrt(magnitude_sq[valid]))))
        for index, (flow_x, flow_y, _) in enumerate(generated_flows, start=1):
            positions = (
                flow_x[valid] * end_x[valid] + flow_y[valid] * end_y[valid]
            ) / magnitude_sq[valid]
            positions = positions[np.isfinite(positions)]
            positions = positions[(positions > -0.75) & (positions < 1.75)]
            if positions.size:
                position_by_index.setdefault(index, []).append(
                    float(np.median(positions))
                )

    return {
        "run": str(run_dir),
        "generated_count": generated_count,
        "sampled_batches": batches,
        "median_accepted_features_per_batch": percentile(accepted_features, 0.5),
        "median_endpoint_motion_pixels": percentile(endpoint_magnitudes, 0.5),
        "motion_position_by_index": {
            str(index): {
                "expected": index / (generated_count + 1) if generated_count else math.nan,
                "median": percentile(values, 0.5),
                "p10": percentile(values, 0.1),
                "p90": percentile(values, 0.9),
                "median_absolute_error": percentile(
                    [
                        abs(value - index / (generated_count + 1))
                        for value in values
                    ],
                    0.5,
                ) if generated_count else math.nan,
            }
            for index, values in sorted(position_by_index.items())
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dirs", nargs="+", type=Path)
    parser.add_argument("--min-batch", type=int, default=50)
    parser.add_argument("--batch-stride", type=int, default=4)
    parser.add_argument("--search-radius", type=int, default=5)
    parser.add_argument("--window", type=int, default=9)
    parser.add_argument("--grid-stride", type=int, default=4)
    arguments = parser.parse_args()
    results = [
        analyze(
            path.resolve(),
            arguments.min_batch,
            arguments.batch_stride,
            arguments.search_radius,
            arguments.window,
            arguments.grid_stride,
        )
        for path in arguments.run_dirs
    ]
    print(json.dumps(results, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()

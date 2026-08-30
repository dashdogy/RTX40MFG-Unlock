#!/usr/bin/env python3
"""Analyze a final-DL4RT scalar sweep and decide whether a per-index LUT is viable."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Iterable


TRACE_PATTERN = re.compile(
    r"^\[DL4RT_SWEEP\] n=(?P<n>\d+) slot=(?P<slot>\d+) "
    r"scalarIndex=(?P<scalar>\d+) nativeBits=(?P<native>[0-9a-fA-F]{8}) "
    r"injectedBits=(?P<injected>[0-9a-fA-F]{8})$"
)


def percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(value for value in values if math.isfinite(value))
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
        width = int(row.get("grayWidth") or 128)
        height = int(row.get("grayHeight") or 72)
    except (KeyError, TypeError, ValueError):
        return None
    frame_bytes = width * height
    if offset < 0 or offset + frame_bytes > len(blob):
        return None
    return blob[offset : offset + frame_bytes]


def summarize(values: list[float]) -> dict[str, float | int]:
    return {
        "samples": len(values),
        "median": percentile(values, 0.5),
        "p10": percentile(values, 0.1),
        "p25": percentile(values, 0.25),
        "p75": percentile(values, 0.75),
        "p90": percentile(values, 0.9),
    }


def rank(values: list[float]) -> list[float]:
    order = sorted(range(len(values)), key=values.__getitem__)
    ranks = [0.0] * len(values)
    cursor = 0
    while cursor < len(order):
        end = cursor + 1
        while end < len(order) and values[order[end]] == values[order[cursor]]:
            end += 1
        average = (cursor + end - 1) / 2.0
        for location in range(cursor, end):
            ranks[order[location]] = average
        cursor = end
    return ranks


def pearson(first: list[float], second: list[float]) -> float:
    if len(first) != len(second) or len(first) < 2:
        return math.nan
    first_mean = statistics.fmean(first)
    second_mean = statistics.fmean(second)
    numerator = sum(
        (left - first_mean) * (right - second_mean)
        for left, right in zip(first, second)
    )
    first_energy = sum((value - first_mean) ** 2 for value in first)
    second_energy = sum((value - second_mean) ** 2 for value in second)
    denominator = math.sqrt(first_energy * second_energy)
    return numerator / denominator if denominator else math.nan


def spearman(first: list[float], second: list[float]) -> float:
    return pearson(rank(first), rank(second))


def finite_or_none(value: float) -> float | None:
    return value if math.isfinite(value) else None


def json_safe(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {key: json_safe(item) for key, item in value.items()}
    if isinstance(value, list):
        return [json_safe(item) for item in value]
    return value


def summarize_response(curve: list[dict]) -> dict:
    curve_with_data = [
        row for row in curve if row["distance_ratio"]["samples"] > 0
    ]
    curve_scalars = [row["scalar"] for row in curve_with_data]
    curve_medians = [row["distance_ratio"]["median"] for row in curve_with_data]
    adjacent_pairs = list(zip(curve_medians, curve_medians[1:]))
    increasing_fraction = (
        sum(left < right for left, right in adjacent_pairs) / len(adjacent_pairs)
        if adjacent_pairs
        else math.nan
    )
    return {
        "scalar_minimum": min(curve_scalars) if curve_scalars else math.nan,
        "scalar_maximum": max(curve_scalars) if curve_scalars else math.nan,
        "scalar_count": len(curve_with_data),
        "distance_ratio_median_minimum": min(curve_medians) if curve_medians else math.nan,
        "distance_ratio_median_maximum": max(curve_medians) if curve_medians else math.nan,
        "curve_spearman": spearman(curve_scalars, curve_medians),
        "adjacent_increasing_fraction": increasing_fraction,
    }


def build_candidate_lut(
    curve: list[dict], target_tolerance: float, maximum_spread: float
) -> tuple[list[dict], bool]:
    targets: list[dict] = []
    supported = True
    for output_index in range(1, 6):
        expected = output_index / 6.0
        candidates = []
        for row in curve:
            summary = row["distance_ratio"]
            if not summary["samples"]:
                continue
            spread = summary["p90"] - summary["p10"]
            candidates.append(
                (
                    abs(summary["median"] - expected),
                    spread,
                    abs(row["scalar"]),
                    row,
                )
            )
        candidates.sort(key=lambda item: item[:3])
        if not candidates:
            targets.append(
                {
                    "native_index": output_index,
                    "expected": expected,
                    "reachable": False,
                    "reason": "no samples",
                }
            )
            supported = False
            continue

        error, spread, _, row = candidates[0]
        summary = row["distance_ratio"]
        enough_samples = summary["samples"] >= 3
        reachable = error <= target_tolerance and enough_samples
        stable = spread <= maximum_spread
        targets.append(
            {
                "native_index": output_index,
                "expected": expected,
                "candidate_scalar_index": row.get("scalar_index"),
                "candidate_scalar": row["scalar"],
                "candidate_bits": row["bits"],
                "distance_ratio": summary,
                "projection": row["projection"],
                "native_index_distance_ratio": row.get("by_native_index", {})
                .get(str(output_index), {})
                .get("distance_ratio"),
                "absolute_error": error,
                "p10_p90_spread": spread,
                "enough_samples": enough_samples,
                "stable": stable,
                "reachable": reachable,
            }
        )
        supported = supported and reachable and stable
    return targets, supported


def analyze(
    run_dir: Path,
    min_batch: int,
    endpoint_threshold: float,
    target_tolerance: float,
    maximum_spread: float,
) -> dict:
    map_path = run_dir / "scalar-map.json"
    trace_path = run_dir / "dl4rt-scalar-sweep.cdb.log"
    generated_path = next(run_dir.glob("MfgUnlock-ngx-output-*.csv"))
    real_path = next(run_dir.glob("MfgUnlock-ngx-real-*.csv"))
    gray_path = next(run_dir.glob("MfgUnlock-ngx-output-*.gray"))

    scalar_map = json.loads(map_path.read_text(encoding="utf-8-sig"))
    scalars = scalar_map["scalars"]
    scalar_count = len(scalars)
    scalar_values = [float(item["value"]) for item in scalars]
    scalar_bits = [str(item["bits"]).upper() for item in scalars]

    trace_rows: dict[int, dict[str, int | str]] = {}
    trace_errors: list[str] = []
    for line in trace_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = TRACE_PATTERN.match(line.strip())
        if not match:
            continue
        dispatch = int(match.group("n"))
        row = {
            "slot": int(match.group("slot")),
            "scalar_index": int(match.group("scalar")),
            "native_bits": match.group("native").upper(),
            "injected_bits": match.group("injected").upper(),
        }
        expected_scalar = dispatch % scalar_count
        if row["slot"] != dispatch % 5:
            trace_errors.append(f"dispatch {dispatch}: slot mismatch")
        if row["scalar_index"] != expected_scalar:
            trace_errors.append(f"dispatch {dispatch}: scalar-index mismatch")
        if row["injected_bits"] != scalar_bits[expected_scalar]:
            trace_errors.append(f"dispatch {dispatch}: injected-bits mismatch")
        trace_rows[dispatch] = row

    if trace_rows and sorted(trace_rows) != list(range(max(trace_rows) + 1)):
        trace_errors.append("DL2 dispatch trace is not contiguous from zero")

    blob = gray_path.read_bytes()
    real: dict[int, bytes] = {}
    for row in read_rows(real_path):
        frame = load_frame(blob, row)
        if frame is not None:
            real[int(row["batch"])] = frame

    generated_rows = read_rows(generated_path)
    generated_by_batch: dict[int, dict[int, bytes]] = defaultdict(dict)
    count_by_batch: dict[int, int] = {}
    for row in generated_rows:
        frame = load_frame(blob, row)
        if frame is None:
            continue
        batch = int(row["batch"])
        index = int(row["index"])
        generated_by_batch[batch][index] = frame
        count_by_batch[batch] = int(row["count"])

    inferred_pre_attach_rows = len(generated_rows) - len(trace_rows)
    recorded_pre_attach_rows = scalar_map.get("preAttachGeneratedRows")
    pre_attach_rows = (
        int(recorded_pre_attach_rows)
        if recorded_pre_attach_rows is not None
        else inferred_pre_attach_rows
    )
    if pre_attach_rows < 0 or pre_attach_rows % 5:
        trace_errors.append(
            f"cannot align {len(generated_rows)} generated rows with "
            f"{len(trace_rows)} traced dispatches"
        )
        pre_attach_batches = 0
    else:
        pre_attach_batches = pre_attach_rows // 5
    if recorded_pre_attach_rows is not None and pre_attach_rows != inferred_pre_attach_rows:
        trace_errors.append(
            "recorded pre-attach row count does not match capture/trace row difference"
        )

    dispatch_by_output: dict[tuple[int, int], int] = {}
    for row_ordinal, row in enumerate(generated_rows):
        if row_ordinal < pre_attach_rows:
            continue
        dispatch_by_output[(int(row["batch"]), int(row["index"]))] = (
            row_ordinal - pre_attach_rows
        )

    distance_by_scalar: dict[int, list[float]] = defaultdict(list)
    projection_by_scalar: dict[int, list[float]] = defaultdict(list)
    distance_by_scalar_index: dict[tuple[int, int], list[float]] = defaultdict(list)
    projection_by_scalar_index: dict[tuple[int, int], list[float]] = defaultdict(list)
    endpoint_changes: list[float] = []
    complete_batches = 0
    moving_batches = 0
    analyzed_outputs = 0
    missing_trace_dispatches: list[int] = []

    for batch in sorted(generated_by_batch):
        count = count_by_batch[batch]
        outputs = generated_by_batch[batch]
        if (
            batch < min_batch
            or batch - 1 not in real
            or batch not in real
        ):
            continue
        if count != 5 or any(index not in outputs for index in range(1, count + 1)):
            continue
        if any((batch, index) not in dispatch_by_output for index in range(1, count + 1)):
            continue
        complete_batches += 1
        previous = real[batch - 1]
        current = real[batch]
        endpoint_change = mad(previous, current)
        if endpoint_change < endpoint_threshold:
            continue
        moving_batches += 1
        endpoint_changes.append(endpoint_change)

        for native_index in range(1, count + 1):
            dispatch = dispatch_by_output[(batch, native_index)]
            if dispatch not in trace_rows:
                missing_trace_dispatches.append(dispatch)
                continue
            scalar_index = dispatch % scalar_count
            sample = outputs[native_index]
            before_distance = mad(previous, sample)
            after_distance = mad(sample, current)
            denominator = before_distance + after_distance
            distance_ratio = before_distance / denominator if denominator else math.nan
            projection = projected_position(previous, sample, current)
            if math.isfinite(distance_ratio):
                distance_by_scalar[scalar_index].append(distance_ratio)
                distance_by_scalar_index[(scalar_index, native_index)].append(distance_ratio)
            if math.isfinite(projection):
                projection_by_scalar[scalar_index].append(projection)
                projection_by_scalar_index[(scalar_index, native_index)].append(projection)
            analyzed_outputs += 1

    curve: list[dict] = []
    for scalar_index, value in enumerate(scalar_values):
        distance_summary = summarize(distance_by_scalar.get(scalar_index, []))
        projection_summary = summarize(projection_by_scalar.get(scalar_index, []))
        by_native_index = {}
        for native_index in range(1, 6):
            by_native_index[str(native_index)] = {
                "distance_ratio": summarize(
                    distance_by_scalar_index.get((scalar_index, native_index), [])
                ),
                "projection": summarize(
                    projection_by_scalar_index.get((scalar_index, native_index), [])
                ),
            }
        curve.append(
            {
                "scalar_index": scalar_index,
                "scalar": value,
                "bits": scalar_bits[scalar_index],
                "distance_ratio": distance_summary,
                "projection": projection_summary,
                "by_native_index": by_native_index,
            }
        )

    response = summarize_response(curve)
    targets, candidate_supported = build_candidate_lut(
        curve, target_tolerance, maximum_spread
    )
    result = {
        "run": str(run_dir),
        "method": (
            "Exact float bits were written to network+0x88 immediately before each "
            "Flatten_EndpointDL2Net_89 dispatch. Scalars rotate through all five native indices."
        ),
        "thresholds": {
            "min_batch": min_batch,
            "endpoint_mad": endpoint_threshold,
            "target_absolute_error": target_tolerance,
            "maximum_p10_p90_spread": maximum_spread,
        },
        "trace_validation": {
            "dispatches": len(trace_rows),
            "pre_attach_generated_rows": pre_attach_rows,
            "pre_attach_generated_batches": pre_attach_batches,
            "errors": trace_errors[:50],
            "missing_analyzed_dispatches": sorted(set(missing_trace_dispatches))[:50],
            "valid": not trace_errors and not missing_trace_dispatches,
        },
        "capture": {
            "generated_rows": len(generated_rows),
            "complete_batches_after_minimum": complete_batches,
            "moving_batches": moving_batches,
            "analyzed_outputs": analyzed_outputs,
            "endpoint_mad": summarize(endpoint_changes),
        },
        "response": response,
        "candidate_lut_supported": candidate_supported and not trace_errors,
        "candidate_lut": targets,
        "curve": curve,
    }
    return result


def combine_results(results: list[dict]) -> dict:
    curve = sorted(
        [row for result in results for row in result["curve"]],
        key=lambda row: row["scalar"],
    )
    scalar_values = [row["scalar"] for row in curve]
    if len(scalar_values) != len(set(scalar_values)):
        raise ValueError("Combined sweep runs contain duplicate scalar values")
    thresholds = results[0]["thresholds"]
    targets, candidate_supported = build_candidate_lut(
        curve,
        thresholds["target_absolute_error"],
        thresholds["maximum_p10_p90_spread"],
    )
    trace_errors = []
    missing = []
    for result in results:
        label = Path(result["run"]).name
        trace_errors.extend(
            f"{label}: {message}" for message in result["trace_validation"]["errors"]
        )
        missing.extend(
            f"{label}: {dispatch}"
            for dispatch in result["trace_validation"]["missing_analyzed_dispatches"]
        )
    endpoint_medians = [
        result["capture"]["endpoint_mad"]["median"] for result in results
    ]
    trace_valid = all(result["trace_validation"]["valid"] for result in results)
    return {
        "run": [result["run"] for result in results],
        "method": (
            "Combined exact-bit final-DL4RT sweep. Each scalar was written to "
            "network+0x88 immediately before Flatten_EndpointDL2Net_89 dispatch, "
            "with coprime schedules rotating values through native output slots."
        ),
        "thresholds": thresholds,
        "trace_validation": {
            "runs": len(results),
            "dispatches": sum(
                result["trace_validation"]["dispatches"] for result in results
            ),
            "pre_attach_generated_rows": sum(
                result["trace_validation"]["pre_attach_generated_rows"]
                for result in results
            ),
            "pre_attach_generated_batches": sum(
                result["trace_validation"]["pre_attach_generated_batches"]
                for result in results
            ),
            "errors": trace_errors,
            "missing_analyzed_dispatches": missing,
            "valid": trace_valid,
        },
        "capture": {
            "generated_rows": sum(result["capture"]["generated_rows"] for result in results),
            "complete_batches_after_minimum": sum(
                result["capture"]["complete_batches_after_minimum"] for result in results
            ),
            "moving_batches": sum(result["capture"]["moving_batches"] for result in results),
            "analyzed_outputs": sum(
                result["capture"]["analyzed_outputs"] for result in results
            ),
            "endpoint_mad": summarize(endpoint_medians),
        },
        "response": summarize_response(curve),
        "candidate_lut_supported": candidate_supported and trace_valid,
        "candidate_lut": targets,
        "curve": curve,
    }


def markdown_report(result: dict) -> str:
    response = result["response"]
    trace = result["trace_validation"]
    capture = result["capture"]
    supported = result["candidate_lut_supported"]
    lines = [
        "# Ada DL4RT scalar sweep",
        "",
        "## Outcome",
        "",
        (
            "A per-index inverse LUT is supported by this sweep and requires a separate "
            "confirmation capture."
            if supported
            else "This sweep does not support a per-index inverse LUT correction."
        ),
        "",
        "The injected scalar response covered median distance-ratio positions "
        f"`{response['distance_ratio_median_minimum']:.3f}` through "
        f"`{response['distance_ratio_median_maximum']:.3f}` across scalar inputs "
        f"`{response['scalar_minimum']:.3f}` through `{response['scalar_maximum']:.3f}`.",
        "",
        "## Data quality",
        "",
        f"- Final DL2 overrides recorded: `{trace['dispatches']}`",
        f"- Trace/schedule validation: `{'pass' if trace['valid'] else 'fail'}`",
        f"- Moving complete batches: `{capture['moving_batches']}`",
        f"- Analyzed generated frames: `{capture['analyzed_outputs']}`",
        f"- Median endpoint MAD: `{capture['endpoint_mad']['median']:.3f}`",
        f"- Median-curve Spearman correlation: `{response['curve_spearman']:.3f}`",
        f"- Adjacent increasing fraction: `{response['adjacent_increasing_fraction']:.3f}`",
        "",
        "## Best per-index candidates",
        "",
        "| Output index | Required | Best scalar | Measured median | Error | P10-P90 | Reachable |",
        "|---:|---:|---:|---:|---:|---:|:---:|",
    ]
    for target in result["candidate_lut"]:
        if "candidate_scalar" not in target:
            lines.append(
                f"| {target['native_index']} | {target['expected']:.3f} | n/a | n/a | n/a | n/a | no |"
            )
            continue
        summary = target["distance_ratio"]
        lines.append(
            f"| {target['native_index']} | {target['expected']:.3f} | "
            f"{target['candidate_scalar']:.3f} | {summary['median']:.3f} | "
            f"{target['absolute_error']:.3f} | {target['p10_p90_spread']:.3f} | "
            f"{'yes' if target['reachable'] else 'no'} |"
        )

    lines.extend(
        [
            "",
            "## Scalar response curve",
            "",
            "| Scalar | Samples | Distance median | P10 | P90 | Projection median |",
            "|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in result["curve"]:
        distance = row["distance_ratio"]
        projection = row["projection"]
        lines.append(
            f"| {row['scalar']:.3f} | {distance['samples']} | "
            f"{distance['median']:.3f} | {distance['p10']:.3f} | "
            f"{distance['p90']:.3f} | {projection['median']:.3f} |"
        )
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dirs", nargs="+", type=Path)
    parser.add_argument("--min-batch", type=int, default=10)
    parser.add_argument("--endpoint-threshold", type=float, default=0.25)
    parser.add_argument("--target-tolerance", type=float, default=0.05)
    parser.add_argument("--maximum-spread", type=float, default=0.20)
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--markdown-output", type=Path)
    arguments = parser.parse_args()

    results = [
        analyze(
            run_dir.resolve(),
            arguments.min_batch,
            arguments.endpoint_threshold,
            arguments.target_tolerance,
            arguments.maximum_spread,
        )
        for run_dir in arguments.run_dirs
    ]
    result = results[0] if len(results) == 1 else combine_results(results)
    serialized = json.dumps(json_safe(result), indent=2, allow_nan=False)
    if arguments.json_output:
        arguments.json_output.write_text(serialized + "\n", encoding="utf-8")
    if arguments.markdown_output:
        arguments.markdown_output.write_text(markdown_report(result), encoding="utf-8")
    print(serialized)


if __name__ == "__main__":
    main()

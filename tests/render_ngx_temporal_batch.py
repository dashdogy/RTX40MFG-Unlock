#!/usr/bin/env python3
"""Render one NGX temporal probe batch as a labeled PNG strip."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from PIL import Image, ImageDraw


LABEL_HEIGHT = 24


def rows_by_batch(path: Path) -> dict[int, dict[int, dict[str, str]]]:
    result: dict[int, dict[int, dict[str, str]]] = {}
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            if not row.get("grayOffset"):
                continue
            result.setdefault(int(row["batch"]), {})[int(row["index"])] = row
    return result


def dimensions(row: dict[str, str]) -> tuple[int, int]:
    return int(row.get("grayWidth") or 64), int(row.get("grayHeight") or 36)


def frame(blob: bytes, row: dict[str, str], scale: int) -> Image.Image:
    width, height = dimensions(row)
    frame_bytes = width * height
    offset = int(row["grayOffset"])
    source = Image.frombytes("L", (width, height), blob[offset : offset + frame_bytes])
    return source.resize((width * scale, height * scale), Image.Resampling.NEAREST)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("batch", type=int)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()

    run_dir = arguments.run_dir.resolve()
    generated = rows_by_batch(next(run_dir.glob("MfgUnlock-ngx-output-*.csv")))
    real = rows_by_batch(next(run_dir.glob("MfgUnlock-ngx-real-*.csv")))
    blob = next(run_dir.glob("MfgUnlock-ngx-output-*.gray")).read_bytes()
    batch = arguments.batch
    rows = [("previous real", real[batch - 1][0])]
    rows.extend(
        (f"generated {index}", generated[batch][index])
        for index in sorted(generated[batch])
    )
    rows.append(("current real", real[batch][0]))

    width, height = dimensions(rows[0][1])
    scale = max(1, 256 // width)
    cell_width = width * scale
    image = Image.new("L", (cell_width * len(rows), height * scale + LABEL_HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    for position, (label, row) in enumerate(rows):
        left = position * cell_width
        image.paste(frame(blob, row, scale), (left, LABEL_HEIGHT))
        draw.text((left + 4, 5), label, fill=255)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(arguments.output)


if __name__ == "__main__":
    main()

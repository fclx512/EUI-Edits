"""Numerical analysis of archived reference images; never controls or edits a UI.

Measures selection-color coverage and text ink row bands in a declared ROI.
Ink bounds are not caret bounds, and contiguous selection coverage is not a row count.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
from PIL import Image


def runs(values):
    result = []
    for y in values:
        if result and y == result[-1][1] + 1:
            result[-1][1] = y
        else:
            result.append([y, y])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--roi", type=int, nargs=4, required=True, metavar=("X", "Y", "W", "H"))
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--color-tolerance", type=int, default=12)
    args = parser.parse_args()
    source = Image.open(args.image)
    image_format = source.format
    image = source.convert("RGB")
    x, y, w, h = args.roi
    if min(x, y) < 0 or min(w, h) <= 0 or x + w > image.width or y + h > image.height:
        raise ValueError("ROI outside image")
    pixels = image.load()
    colors = Counter(pixels[xx, yy] for yy in range(y, y + h) for xx in range(x, x + w))
    candidates = [(count, color) for color, count in colors.items()
                  if min(color) > 160 and max(color) - min(color) >= 12]
    if not candidates:
        raise ValueError("No dominant light selection color in this ROI")
    count, selected = max(candidates)
    background_rows = []
    ink_rows = []
    for yy in range(y, y + h):
        row = [pixels[xx, yy] for xx in range(x, x + w)]
        if sum(max(abs(a - b) for a, b in zip(color, selected)) <= args.color_tolerance
               for color in row) >= max(3, w // 10):
            background_rows.append(yy)
        if sum(max(color) < 140 for color in row) >= 2:
            ink_rows.append(yy)
    ink_bands = runs(ink_rows)
    result = {"method": "archived-image-ROI-numerical-analysis", "image": str(args.image),
              "image_format": image_format, "color_tolerance": args.color_tolerance,
              "image_sha256": hashlib.sha256(args.image.read_bytes()).hexdigest(),
              "raster_size": [image.width, image.height], "roi": args.roi,
              "selection_rgb": selected, "selection_pixels_in_roi": count,
              "selection_coverage_y_runs_inclusive": runs(background_rows),
              "ink_y_runs_inclusive": ink_bands,
              "ink_run_start_deltas": [b[0] - a[0] for a, b in zip(ink_bands, ink_bands[1:])],
              "limitations": ["ROI, color and threshold are fixture-specific; inspect source screenshot first.",
                              "JPEG compression and text ink variations prevent exact per-row geometry inference.",
                              "Ink runs may fragment; do not infer row height from glyph ink height.",
                              "Raster pixels need a recorded scale before comparison to CSS/EUI logical units."]}
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()

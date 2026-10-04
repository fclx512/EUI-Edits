#!/usr/bin/env python3
"""Rebuild About PNGs and Windows ICO frames from assets/icon.svg.

Build tools/rasterize_svg.cpp with MSVC first; it emits 8x RGBA samples and
Pillow downsamples them with Lanczos for each exact physical target size.
From a VS x64 developer prompt, run:
  cl /nologo /EHsc /std:c++17 /I . tools/rasterize_svg.cpp /Fo:build/rasterize_svg.obj /Fe:build/rasterize_svg.exe
  python tools/generate_about_icon_rasters.py --rasterizer build/rasterize_svg.exe
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
ABOUT_SIZES = (48, 60, 72, 96, 120, 144)
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def render(rasterizer: Path, svg: Path, size: int, destination: Path | None = None) -> Image.Image:
    with tempfile.TemporaryDirectory(prefix="eui-icon-") as temp:
        raw = Path(temp) / "pixels.rgba"
        subprocess.run([str(rasterizer), str(svg), str(size), str(raw)], check=True)
        sample = size * 8
        with open(raw, "rb") as source:
            data = source.read()
        image = Image.frombytes("RGBA", (sample, sample), data)
        image = image.resize((size, size), Image.Resampling.LANCZOS)
        if destination is not None:
            image.save(destination, format="PNG", optimize=True)
        return image


def write_ico(frames: dict[int, Image.Image], destination: Path) -> None:
    encoded: list[bytes] = []
    for size, image in frames.items():
        from io import BytesIO
        stream = BytesIO()
        image.save(stream, format="PNG", optimize=True)
        encoded.append(stream.getvalue())
    count = len(frames)
    offset = 6 + count * 16
    directory = bytearray(struct.pack("<HHH", 0, 1, count))
    payload = bytearray()
    for size, png in zip(frames, encoded):
        dim = 0 if size == 256 else size
        directory += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset)
        payload += png
        offset += len(png)
    destination.write_bytes(directory + payload)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rasterizer", type=Path, required=True, help="compiled tools/rasterize_svg.exe")
    args = parser.parse_args()
    svg = ROOT / "assets" / "icon.svg"
    about_dir = ROOT / "apps" / "neo_editor" / "assets" / "about"
    about_dir.mkdir(parents=True, exist_ok=True)
    for size in ABOUT_SIZES:
        render(args.rasterizer, svg, size, about_dir / f"icon-{size}.png")
    icon_frames = {size: render(args.rasterizer, svg, size)
                   for size in ICO_SIZES}
    write_ico(icon_frames, ROOT / "assets" / "icon.ico")


if __name__ == "__main__":
    main()

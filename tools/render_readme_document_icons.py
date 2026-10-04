#!/usr/bin/env python3
"""Extract the small and full-size document icon frames for the README plate."""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


KINDS = ("md", "txt", "code", "data", "file")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_dir = root / "apps" / "neo_editor" / "assets" / "icons"
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for kind in KINDS:
        with Image.open(source_dir / f"{kind}.ico") as icon:
            for size in (256, 16):
                frame = icon.ico.getimage((size, size)).convert("RGBA")
                frame.save(args.output_dir / f"{kind}-{size}.png", format="PNG", optimize=True)


if __name__ == "__main__":
    main()

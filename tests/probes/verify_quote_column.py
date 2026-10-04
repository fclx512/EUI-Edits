"""Check ui_review screenshots for quote bars detached from the reading column.

Requires Pillow. Pass settled 1400px-wide captures made at effective scale 1.25
with readable_width=1. This checks the observed regression, not all quote layouts.
"""
import argparse
from PIL import Image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("images", nargs="+")
    parser.add_argument("--theme", type=int, choices=(0, 1), required=True)
    args = parser.parse_args()
    expected = (152, 115, 247) if args.theme else (138, 92, 244)
    columns = []
    for path in args.images:
        with Image.open(path) as source:
            rgb = source.convert("RGB")
            width, height = rgb.size
            if width != 1400:
                raise AssertionError("this fixed probe expects a 1400px client screenshot")
            pixels = rgb.load()
            for x in range(2, width - 20):
                contiguous = 0
                for y in range(60, height - 45):
                    if all(abs(pixels[x, y][c] - expected[c]) <= 5 for c in range(3)):
                        contiguous += 1
                        # Individual 30px line bands have antialiased boundary rows.
                        if contiguous >= 20:
                            columns.append(x)
                            break
                    else:
                        contiguous = 0
    if not columns:
        raise AssertionError("no long quote accent found; wrong fixture/scroll position")
    if min(columns) < 140:
        raise AssertionError(f"quote bars remain at window edge: x={min(columns)}")
    print(f"quote column PASS: x={min(columns)}..{max(columns)}")


if __name__ == "__main__":
    main()

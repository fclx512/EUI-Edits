"""Raster preview of all theme-aware in-app document icon families."""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw
from generate_document_icons import render_icon

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parents[3] / "build-win32" / "evidence" / "ui-document-icons"
KINDS = ("txt", "md", "code", "data", "file")
SIZES = (16, 20, 24, 32, 48, 64)
PALETTES = {
    "light": {"page": "#ffffff", "fold": "#e8edf6", "edge": "#7c91b3",
              "txt": "#397bd1", "md": "#7962ce", "code": "#168879",
              "data": "#d47b25", "file": "#20242a"},
    "dark": {"page": "#1e2226", "fold": "#2e343d", "edge": "#8295b0",
             "txt": "#6fa8f0", "md": "#a58cf5", "code": "#48aa9a",
             "data": "#e5a04d", "file": "#e6e8ec"},
}
BACKGROUNDS = {"light": "#f7f8fa", "dark": "#191c20"}
MAGNIFY = 5
CELL = 344


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    sheet = Image.new("RGB", (CELL * len(SIZES), CELL * 2 * len(KINDS)), "white")
    draw = ImageDraw.Draw(sheet)
    for family_index, kind in enumerate(KINDS):
        for theme_index, theme in enumerate(("light", "dark")):
            row = family_index * 2 + theme_index
            band = Image.new("RGB", sheet.size, BACKGROUNDS[theme])
            sheet.paste(band, (0, row * CELL))
            draw.text((12, row * CELL + 8), f"{theme} · {kind}", fill="#c6cbd2" if theme == "dark" else "#596270")
            for column, size in enumerate(SIZES):
                frame = render_icon(kind, size, PALETTES[theme], detailed=False)
                frame.save(OUT / f"{theme}_{kind}_{size}.png")
                zoom = frame.resize((size * MAGNIFY, size * MAGNIFY), Image.Resampling.NEAREST)
                x = column * CELL + (CELL - zoom.width) // 2
                y = row * CELL + 42 + (CELL - 48 - zoom.height) // 2
                sheet.paste(zoom, (x, y), zoom)
                draw.text((x, row * CELL + CELL - 25), f"{size}px", fill="#c6cbd2" if theme == "dark" else "#596270")
    path = OUT / "contact_sheet.png"
    sheet.save(path)
    print(f"wrote {path} ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()

"""Generate matching document-family SVG, Windows ICO and C++ geometry."""
from __future__ import annotations

import struct
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageColor, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent
PREVIEW_DIR = ROOT.parents[3] / "build-win32/evidence/document-family"
SIZES = (16, 20, 24, 32, 48, 64, 128, 256)
SUPERSAMPLE = 8
PAPER, FOLD, EDGE = "#f6f8fc", "#e8edf6", "#7c91b3"
ACCENTS = {"txt": "#397bd1", "md": "#7962ce", "code": "#168879",
           "data": "#d47b25", "file": "#20242a"}
RULE_LEFT, RULE_RIGHT, RULE_THICKNESS = 6.0, 17.4, 1.05
RULES = {kind: (15.4, 17.5, 19.6) for kind in ACCENTS}
RULES["file"] = (11.2, 13.3, 15.4, 17.5, 19.6)
MARKS = {
    "txt": ("txtMark",), "md": ("mdMark",),
    "code": ("codeLeft", "codeRight"),
    "data": ("dataLeft", "dataRight"), "file": (),
}
TXT_MARK = [(6, 6.6), (13.2, 6.6), (13.2, 8.3), (10.45, 8.3),
            (10.45, 13.2), (8.75, 13.2), (8.75, 8.3), (6, 8.3)]
MD_MARK = [(6, 6.6), (7.5, 6.6), (9.6, 9.9), (11.7, 6.6), (13.2, 6.6),
           (13.2, 13.2), (11.5, 13.2), (11.5, 9.4), (10.25, 11.5),
           (8.95, 11.5), (7.7, 9.4), (7.7, 13.2), (6, 13.2)]
CODE_LEFT = [(10.0, 6.5), (7.1, 9.6), (10.0, 12.7), (8.8, 13.8), (4.6, 9.6), (8.8, 5.4)]
CODE_RIGHT = [(14.0, 5.4), (18.2, 9.6), (14.0, 13.8), (12.8, 12.7),
              (15.7, 9.6), (12.8, 6.5)]
DATA_LEFT = [(9.5, 5.2), (7.7, 5.2), (6.4, 5.8), (5.9, 7), (5.9, 8.6),
             (5.5, 9.6), (5.9, 10.6), (5.9, 12.2), (6.4, 13.4), (7.7, 14),
             (9.5, 14), (9.5, 12.4), (7.9, 12.4), (7.5, 12.1), (7.5, 10.1),
             (6.9, 9.6), (7.5, 9.1), (7.5, 7.1), (7.9, 6.8), (9.5, 6.8)]
DATA_RIGHT = [(14.5, 5.2), (16.3, 5.2), (17.6, 5.8), (18.1, 7), (18.1, 8.6),
              (18.5, 9.6), (18.1, 10.6), (18.1, 12.2), (17.6, 13.4), (16.3, 14),
              (14.5, 14), (14.5, 12.4), (16.1, 12.4), (16.5, 12.1), (16.5, 10.1),
              (17.1, 9.6), (16.5, 9.1), (16.5, 7.1), (16.1, 6.8), (14.5, 6.8)]
# Match M/T's left-weighted visual centre and reserve clear paper before the fold.
# Keep vertical strokes substantial at 16px while narrowing the paired symbols.
def inset_mark(points):
    return [(6.0 + (x - 4.6) * (7.2 / 13.9), 6.6 + (y - 5.2) * .75)
            for x, y in points]

CODE_LEFT, CODE_RIGHT = inset_mark(CODE_LEFT), inset_mark(CODE_RIGHT)
DATA_LEFT, DATA_RIGHT = inset_mark(DATA_LEFT), inset_mark(DATA_RIGHT)
FOLD_EDGE = [(14.1, 2.4), (15.3, 2.4), (15.3, 6.4), (16.4, 7.5),
             (20, 7.5), (20, 8.7), (15.9, 8.7), (14.1, 6.9)]
FOLD_PAPER = [(15.3, 3.5), (19.5, 7.5), (16.4, 7.5), (15.3, 6.4)]


def page_contour(inset: float) -> list[tuple[float, float]]:
    left, right, top, bottom = 2.8 + inset, 21.2 - inset, 1.2 + inset, 22.8 - inset
    radius, shift = 2.6 - inset, inset * 2 ** .5
    points = [(left + radius, top), (15.3 + inset - shift, top),
              (right, 7.1 + shift - inset), (right, bottom - radius)]

    def corner(start, control, end):
        for step in range(1, 9):
            t, u = step / 8, 1 - step / 8
            points.append((u*u*start[0] + 2*u*t*control[0] + t*t*end[0],
                           u*u*start[1] + 2*u*t*control[1] + t*t*end[1]))

    corner((right, bottom-radius), (right, bottom), (right-radius, bottom))
    points.append((left+radius, bottom))
    corner((left+radius, bottom), (left, bottom), (left, bottom-radius))
    points.append((left, top+radius))
    corner((left, top+radius), (left, top), (left+radius, top))
    return points


GEOMETRY = {"pageOuter": page_contour(0), "pageInner": page_contour(1.2),
            "foldEdge": FOLD_EDGE, "foldPaper": FOLD_PAPER, "mdMark": MD_MARK,
            "txtMark": TXT_MARK, "codeLeft": CODE_LEFT, "codeRight": CODE_RIGHT,
            "dataLeft": DATA_LEFT, "dataRight": DATA_RIGHT}


def render_icon(kind: str, size: int, palette=None, detailed=None) -> Image.Image:
    detailed = (palette is None and size >= 64) if detailed is None else detailed
    palette = palette or {"page": PAPER, "fold": FOLD, "edge": EDGE, **ACCENTS}
    canvas = Image.new("RGBA", (size * SUPERSAMPLE, size * SUPERSAMPLE))
    draw = ImageDraw.Draw(canvas)
    q = lambda value: round(value * size / 24 * SUPERSAMPLE)

    def polygon(name, color):
        draw.polygon([(q(x), q(y)) for x, y in GEOMETRY[name]], fill=color)

    polygon("pageOuter", palette["edge"])
    polygon("pageInner", palette["page"])
    polygon("foldEdge", palette["edge"])
    polygon("foldPaper", palette["fold"])
    for name in MARKS[kind]:
        polygon(name, palette[kind])
    for y in RULES[kind]:
        draw.rounded_rectangle((q(RULE_LEFT), q(y-RULE_THICKNESS/2),
                                q(RULE_RIGHT), q(y+RULE_THICKNESS/2)),
                               radius=max(1, q(.2)), fill=palette[kind])
    if detailed:
        canvas = render_detailed(kind, canvas, q)
    return canvas.resize((size, size), Image.Resampling.LANCZOS)


def render_detailed(kind: str, flat: Image.Image, q) -> Image.Image:
    def mask_for(name):
        mask = Image.new("L", flat.size)
        ImageDraw.Draw(mask).polygon([(q(x), q(y)) for x, y in GEOMETRY[name]], fill=255)
        return mask

    def gradient(top, bottom):
        a, b = ImageColor.getrgb(top), ImageColor.getrgb(bottom)
        strip = Image.new("RGBA", (1, flat.height))
        strip.putdata([tuple(round(x+(y-x)*i/(flat.height-1)) for x, y in zip(a, b))+(255,)
                       for i in range(flat.height)])
        return strip.resize(flat.size)

    def paste_gradient(target, top, bottom, mask):
        target.paste(gradient(top, bottom), (0, 0), mask)

    outer, inner = mask_for("pageOuter"), mask_for("pageInner")
    result = Image.new("RGBA", flat.size)
    shadow = Image.new("RGBA", flat.size, "#53627d")
    shifted = Image.new("L", flat.size)
    shifted.paste(outer, (0, q(.24)))
    shadow.putalpha(shifted.filter(ImageFilter.GaussianBlur(q(.22))).point(lambda a: round(a*.2)))
    result.alpha_composite(shadow)
    edge = {"md": ("#a092c7", "#76659e"), "txt": ("#7b9bb6", "#5b7f9d"),
            "code": ("#78a99f", "#527b74"), "data": ("#c49b70", "#967653"),
            "file": ("#99a3b2", "#707a88")}[kind]
    paste_gradient(flat, *edge, outer)
    paper = ("#ffffff", "#f0ecf9") if kind == "md" else ("#ffffff", "#eef2f9")
    paste_gradient(flat, *paper, inner)
    fold = mask_for("foldEdge").filter(ImageFilter.GaussianBlur(q(.12)))
    depth = Image.new("RGBA", flat.size, "#79649d" if kind == "md" else "#5d7396")
    depth.putalpha(fold.point(lambda a: round(a*.18)))
    flat.alpha_composite(depth)
    paste_gradient(flat, *edge, mask_for("foldEdge"))
    fold_paper = ("#fcfaff", "#dfd5f0") if kind == "md" else ("#fafcff", "#d7e0ee")
    paste_gradient(flat, *fold_paper, mask_for("foldPaper"))
    accent = {"md": ("#9477df", "#6950bd"), "txt": ("#4a95de", "#2e6cb6"),
              "code": ("#48aa9a", "#087365"), "data": ("#e5a04d", "#bd6817"),
              "file": ("#353a42", "#171b21")}[kind]
    for name in MARKS[kind]:
        paste_gradient(flat, *accent, mask_for(name))
    rules = Image.new("L", flat.size)
    rule_draw = ImageDraw.Draw(rules)
    for y in RULES[kind]:
        rule_draw.rounded_rectangle((q(RULE_LEFT), q(y-RULE_THICKNESS/2),
                                     q(RULE_RIGHT), q(y+RULE_THICKNESS/2)),
                                    radius=max(1, q(.2)), fill=255)
    paste_gradient(flat, *accent, rules)
    highlight = ImageDraw.Draw(flat)
    highlight.line([(q(4.1), q(7)), (q(4.1), q(18.7))], fill="#ffffff", width=max(1, q(.08)))
    result.alpha_composite(flat)
    return result


def write_geometry_header() -> None:
    lines = ["// Generated by assets/icons/generate_document_icons.py. Do not edit by hand.",
             "#pragma once", "#include <array>", '#include "core/render/render_types.h"',
             "namespace neo::document_icon {"]
    for name, points in GEOMETRY.items():
        lines.append(f"inline constexpr std::array<core::Vec2, {len(points)}> {name} {{{{")
        lines.extend(f"    {{{x:.6f}f, {y:.6f}f}}," for x, y in points)
        lines.append("}};")
    for name, values in ((f"{kind}Rules", RULES[kind]) for kind in RULES):
        lines.append(f"inline constexpr std::array<float, {len(values)}> {name} {{" +
                     ", ".join(f"{value}f" for value in values) + "};")
    for name, value in (("ruleLeft", RULE_LEFT), ("ruleRight", RULE_RIGHT),
                        ("ruleThickness", RULE_THICKNESS)):
        lines.append(f"inline constexpr float {name} = {value}f;")
    lines.extend(["} // namespace neo::document_icon", ""])
    (ROOT.parents[1] / "ui/document_icon_geometry.h").write_text("\n".join(lines), encoding="utf-8")


def write_svg(kind: str) -> None:
    lines = ['<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">']
    shapes = [("pageOuter", EDGE), ("pageInner", PAPER), ("foldEdge", EDGE), ("foldPaper", FOLD)]
    shapes.extend((name, ACCENTS[kind]) for name in MARKS[kind])
    for name, color in shapes:
        points = " ".join(f"{x:g},{y:g}" for x, y in GEOMETRY[name])
        lines.append(f'  <polygon points="{points}" fill="{color}"/>')
    for y in RULES[kind]:
        lines.append(f'  <rect x="{RULE_LEFT:g}" y="{y-RULE_THICKNESS/2:g}" width="{RULE_RIGHT-RULE_LEFT:g}" height="{RULE_THICKNESS:g}" rx="0.2" fill="{ACCENTS[kind]}"/>')
    (ROOT / f"{kind}.svg").write_text("\n".join(lines + ["</svg>", ""]), encoding="utf-8")


def write_ico(path: Path, frames) -> None:
    payloads = []
    for _, frame in frames:
        stream = BytesIO()
        frame.save(stream, format="PNG", compress_level=9)
        payloads.append(stream.getvalue())
    offset, entries = 6 + 16 * len(frames), bytearray()
    for (size, _), payload in zip(frames, payloads):
        dimension = 0 if size == 256 else size
        entries.extend(struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32,
                                   len(payload), offset))
        offset += len(payload)
    path.write_bytes(struct.pack("<HHH", 0, 1, len(frames)) + entries + b"".join(payloads))


def main() -> None:
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
    write_geometry_header()
    sheet = Image.new("RGB", (1200, len(ACCENTS)*300+10), "#f7f8fa")
    draw = ImageDraw.Draw(sheet)
    for row, kind in enumerate(ACCENTS):
        frames = [(size, render_icon(kind, size)) for size in SIZES]
        write_ico(ROOT / f"{kind}.ico", frames)
        write_svg(kind)
        x = 35
        for size in (16, 20, 32, 64, 256):
            frame = dict(frames)[size]
            frame.save(PREVIEW_DIR / f"{kind}_{size}.png")
            zoom = frame.resize((size*4, size*4), Image.Resampling.NEAREST) if size <= 64 else frame
            sheet.paste(zoom, (x, row*300+20), zoom)
            draw.text((x, row*300+282), f"{kind} {size}px", fill="#606a78")
            x += zoom.width + 20
    sheet.save(PREVIEW_DIR / "contact_sheet.png")
    print(f"Generated 5 icon families at {len(SIZES)} sizes and matching C++ geometry in {PREVIEW_DIR}")


if __name__ == "__main__":
    main()

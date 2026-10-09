"""Create a local, isolated reference vault from the reviewed Markdown fixtures.

No GUI control, plugins, global Obsidian settings or existing vault changes.
"""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    fixture_dir = Path(__file__).resolve().parents[1] / "reference" / "obsidian_basic"
    fixtures = sorted(fixture_dir.glob("*.md"))
    if len(fixtures) != 4:
        raise ValueError("Expected four reviewed fixture files")
    args.out.mkdir(parents=True, exist_ok=False)
    profile = args.out / ".obsidian"
    profile.mkdir()
    app = {"livePreview": True, "readableLineLength": True,
           "showInlineTitle": False, "showLineNumber": True}
    appearance = {"theme": "moonstone", "cssTheme": "", "baseFontSize": 16,
                  "textFontFamily": "Microsoft YaHei", "monospaceFontFamily": "Consolas",
                  "enabledCssSnippets": []}
    for name, value in (("app.json", app), ("appearance.json", appearance)):
        (profile / name).write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    manifest = []
    for fixture in fixtures:
        text = fixture.read_text(encoding="utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")
        data = text.encode("utf-8")
        (args.out / fixture.name).write_bytes(data)
        manifest.append({"name": fixture.name, "utf8_bytes": len(data),
                         "unicode_code_points": len(text), "physical_lines": len(text.splitlines()),
                         "sha256_utf8_lf": hashlib.sha256(data).hexdigest()})
    (args.out.parent / (args.out.name + "-fixture-manifest.json")).write_text(
        json.dumps({"canonicalization": "UTF-8, no BOM, LF", "fixtures": manifest},
                   ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"vault": str(args.out.resolve()), "fixtures": manifest}, ensure_ascii=False))


if __name__ == "__main__":
    main()

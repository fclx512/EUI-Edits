"""Read installed Obsidian ASAR CSS declarations; no GUI control or app changes.

This records declared rules, not computed runtime CSS or a rendering oracle.
Only short relevant declarations are retained; the complete CSS is not copied.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


VARIABLES = (
    "font-text-size", "font-text", "font-monospace", "line-height-normal",
    "line-height-tight", "file-line-width", "file-margins", "p-spacing",
    "heading-spacing", "text-selection", "code-size", "code-radius",
    "code-normal", "code-background", "list-indent", "list-spacing",
    "blockquote-border-thickness", "blockquote-border-color",
    "blockquote-padding-x", "table-line-height", "table-text-size",
    "table-cell-padding", "table-border-width", "table-column-min-width",
    "h1-size", "h2-size", "h3-size", "h4-size", "h5-size", "h6-size",
    "h1-line-height", "h2-line-height", "h3-line-height", "h4-line-height",
    "h5-line-height", "h6-line-height",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asar", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    raw = args.asar.read_bytes()
    if len(raw) < 16:
        raise ValueError("ASAR header truncated")
    header_size = struct.unpack_from("<I", raw, 4)[0]
    json_size = struct.unpack_from("<I", raw, 12)[0]
    header = json.loads(raw[16:16 + json_size])
    data_start = 8 + header_size

    def read_entry(name):
        entry = header
        for part in name.split("/"):
            entry = entry["files"][part]
        if entry.get("unpacked") or "offset" not in entry:
            raise ValueError(f"Entry not packed: {name}")
        start = data_start + int(entry["offset"])
        size = int(entry["size"])
        if start < data_start or start + size > len(raw):
            raise ValueError(f"Entry out of range: {name}")
        return raw[start:start + size]

    css_bytes = read_entry("app.css")
    css = css_bytes.decode("utf-8")
    package = json.loads(read_entry("package.json"))
    variables = {name: [] for name in VARIABLES}
    selection_rules = []
    for match in re.finditer(r"([^{}]+)\{([^{}]*)\}", css):
        selector, body = match.group(1).strip(), match.group(2)
        for name, value in re.findall(r"--([\w-]+)\s*:\s*([^;{}]+);", body):
            if name in variables:
                variables[name].append({"selector": selector, "value": value.strip(),
                                        "css_char_offset": match.start()})
        if "cm-selectionBackground" in selector or "cm-table-widget.has-selection" in selector:
            declarations = re.findall(r"([\w-]+)\s*:\s*([^;{}]+);", body)
            selection_rules.append({"selector": selector, "declarations": dict(declarations),
                                    "css_char_offset": match.start()})
    result = {
        "method": "installed-ASAR-declared-CSS-not-computed-style",
        "asar_path": str(args.asar.resolve()), "asar_sha256": hashlib.sha256(raw).hexdigest(),
        "package_version": package.get("version"),
        "css_entry": "app.css", "css_sha256": hashlib.sha256(css_bytes).hexdigest(),
        "variables": variables, "selection_rules": selection_rules,
        "limitations": ["Cascade, inherited values, user settings, mode and font fallback need GUI verification.",
                        "A CSS declaration does not specify caret mapping, selected source bytes or visible latency."],
    }
    args.out.mkdir(parents=True, exist_ok=False)
    (args.out / "css-reference.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"version": result["package_version"], "asar_sha256": result["asar_sha256"],
                      "css_sha256": result["css_sha256"], "variables_found": sum(bool(v) for v in variables.values()),
                      "out": str(args.out.resolve())}, ensure_ascii=False))


if __name__ == "__main__":
    main()

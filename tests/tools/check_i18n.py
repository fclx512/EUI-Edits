#!/usr/bin/env python3
"""Check EUI-Edits' C++ message catalog and literal translation calls.

This intentionally uses only the Python standard library. Dynamic message IDs
are reported for review, but are not guessed or treated as failures.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "apps" / "neo_editor" / "model"
APP = ROOT / "apps" / "neo_editor"
RESOURCE_FILES = ("i18n_core.inc", "i18n_safety.inc", "i18n_ui.inc")
PLACEHOLDER_RE = re.compile(r"\{([A-Za-z_][A-Za-z0-9_]*)\}")
CALL_RE = re.compile(r"(?:neo::)?i18n::(tr|format)\s*\(")
MACRO_RE = re.compile(r"\bNEO_I18N\s*\(")
PAIR_KEY_RE = re.compile(r"\{\s*\{?\s*\"([^\"\\]+)\"\s*,")


def mask_non_code(text: str) -> str:
    """Blank comments and literals while preserving offsets and newlines."""
    out = list(text)
    n = len(text)
    i = 0

    def blank(start: int, end: int) -> None:
        for p in range(start, end):
            if out[p] not in "\r\n":
                out[p] = " "

    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i + 2)
            if j < 0:
                j = n
            blank(i, j)
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
            continue
        # C++ raw strings: R"delimiter(contents)delimiter" and prefixed forms.
        raw = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', text[i:])
        if raw:
            delimiter = raw.group(1)
            close = ")" + delimiter + '"'
            j = text.find(close, i + raw.end())
            j = n if j < 0 else j + len(close)
            blank(i, j)
            i = j
            continue
        # Ordinary string / character literal, including an optional encoding prefix.
        prefixed = re.match(r'(?:u8|u|U|L)?(["\'])', text[i:])
        if prefixed:
            quote = prefixed.group(1)
            j = i + prefixed.end()
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                j += 1
            blank(i, min(j, n))
            i = min(j, n)
            continue
        i += 1
    return "".join(out)


def skip_space(text: str, pos: int) -> int:
    while pos < len(text) and text[pos].isspace():
        pos += 1
    return pos


def read_cpp_string(text: str, pos: int) -> tuple[str, int] | None:
    """Read one ordinary UTF-8 C++ string literal and decode common escapes."""
    pos = skip_space(text, pos)
    prefix = re.match(r"(?:u8|u|U|L)?", text[pos:]).group(0)  # noqa: F841
    quote_pos = pos + len(prefix)
    if quote_pos >= len(text) or text[quote_pos] != '"':
        return None
    i = quote_pos + 1
    chars: list[str] = []
    while i < len(text):
        char = text[i]
        if char == '"':
            return "".join(chars), i + 1
        if char != "\\":
            chars.append(char)
            i += 1
            continue
        i += 1
        if i >= len(text):
            return None
        esc = text[i]
        simple = {"n": "\n", "r": "\r", "t": "\t", "b": "\b", "f": "\f", "v": "\v",
                  "a": "\a", "\\": "\\", '"': '"', "'": "'", "?": "?"}
        if esc in simple:
            chars.append(simple[esc])
            i += 1
        elif esc in "01234567":
            j = i + 1
            while j < min(i + 3, len(text)) and text[j] in "01234567":
                j += 1
            chars.append(chr(int(text[i:j], 8)))
            i = j
        elif esc == "x":
            j = i + 1
            while j < len(text) and text[j] in "0123456789abcdefABCDEF":
                j += 1
            if j == i + 1:
                chars.append("x")
                i += 1
            else:
                chars.append(chr(int(text[i + 1:j], 16)))
                i = j
        elif esc in ("u", "U"):
            digits = 4 if esc == "u" else 8
            token = text[i + 1:i + 1 + digits]
            if len(token) == digits and all(c in "0123456789abcdefABCDEF" for c in token):
                chars.append(chr(int(token, 16)))
                i += digits + 1
            else:
                return None
        elif esc == "\n":
            i += 1
        elif esc == "\r" and i + 1 < len(text) and text[i + 1] == "\n":
            i += 2
        else:
            # Preserve implementation-defined escapes as the escaped character.
            chars.append(esc)
            i += 1
    return None


def parse_resources() -> tuple[dict[str, tuple[str, str, str]], list[str]]:
    messages: dict[str, tuple[str, str, str]] = {}
    errors: list[str] = []
    for filename in RESOURCE_FILES:
        path = CATALOG / filename
        if not path.is_file():
            errors.append(f"missing resource file: {path.relative_to(ROOT)}")
            continue
        text = path.read_text(encoding="utf-8-sig")
        code = mask_non_code(text)
        for match in MACRO_RE.finditer(code):
            pos = match.end()
            values: list[str] = []
            for index in range(3):
                parsed = read_cpp_string(text, pos)
                if parsed is None:
                    line = text.count("\n", 0, match.start()) + 1
                    errors.append(f"{filename}:{line}: NEO_I18N argument {index + 1} is not a string literal")
                    break
                value, pos = parsed
                values.append(value)
                pos = skip_space(text, pos)
                if index < 2:
                    if pos >= len(text) or text[pos] != ",":
                        line = text.count("\n", 0, match.start()) + 1
                        errors.append(f"{filename}:{line}: expected comma after NEO_I18N argument {index + 1}")
                        break
                    pos += 1
            if len(values) != 3:
                continue
            message_id, zh, en = values
            line = text.count("\n", 0, match.start()) + 1
            loc = f"{filename}:{line}"
            if not message_id:
                errors.append(f"{loc}: empty message ID")
                continue
            if message_id in messages:
                prior = messages[message_id][2]
                errors.append(f"{loc}: duplicate message ID {message_id!r} (first at {prior})")
                continue
            messages[message_id] = (zh, en, loc)
            if not zh:
                errors.append(f"{loc}: empty Simplified Chinese text for {message_id!r}")
            if not en:
                errors.append(f"{loc}: empty English text for {message_id!r}")
            zh_fields = set(PLACEHOLDER_RE.findall(zh))
            en_fields = set(PLACEHOLDER_RE.findall(en))
            if zh_fields != en_fields:
                errors.append(
                    f"{loc}: placeholder mismatch for {message_id!r}: "
                    f"zh={sorted(zh_fields)}, en={sorted(en_fields)}"
                )
    return messages, errors


def first_argument_end(text: str, masked: str, pos: int) -> int:
    """Return the first top-level comma or call-closing parenthesis."""
    parens = brackets = braces = 0
    i = pos
    while i < len(text):
        if text[i] == '"' or text[i] == "'":
            quote = text[i]
            i += 1
            while i < len(text):
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        char = masked[i]
        if char == "(":
            parens += 1
        elif char == ")":
            if parens == 0 and brackets == 0 and braces == 0:
                return i
            parens -= 1
        elif char == "[":
            brackets += 1
        elif char == "]":
            brackets -= 1
        elif char == "{":
            braces += 1
        elif char == "}":
            braces -= 1
        elif char == "," and parens == 0 and brackets == 0 and braces == 0:
            return i
        i += 1
    return len(text)


def line_number(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def scan_calls(messages: dict[str, tuple[str, str, str]]) -> tuple[list[str], list[str], int]:
    errors: list[str] = []
    dynamic: list[str] = []
    literal_count = 0
    source_paths = sorted(
        p for p in APP.rglob("*")
        if p.is_file() and p.suffix.lower() in {".h", ".hh", ".hpp", ".c", ".cc", ".cpp", ".cxx", ".inl", ".inc"}
        and p.name not in RESOURCE_FILES
    )
    for path in source_paths:
        try:
            text = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError as exc:
            errors.append(f"{path.relative_to(ROOT)}: source is not UTF-8 ({exc})")
            continue
        code = mask_non_code(text)
        for match in CALL_RE.finditer(code):
            kind = match.group(1)
            open_paren = code.find("(", match.start(), match.end())
            arg_pos = skip_space(text, open_paren + 1)
            parsed = read_cpp_string(text, arg_pos)
            first_end = first_argument_end(text, code, arg_pos)
            first_expr = text[arg_pos:first_end].strip().replace("\n", " ")
            loc = f"{path.relative_to(ROOT)}:{line_number(text, match.start())}"
            if parsed is None:
                if first_expr:
                    dynamic.append(f"{loc}: i18n::{kind}({first_expr})")
                else:
                    errors.append(f"{loc}: could not parse first argument to i18n::{kind}")
                continue
            message_id, after_literal = parsed
            after_literal = skip_space(text, after_literal)
            # Adjacent C++ literals still form one compile-time string ID.
            pieces = [message_id]
            while after_literal < len(text) and text[after_literal] == '"':
                adjacent = read_cpp_string(text, after_literal)
                if adjacent is None:
                    break
                pieces.append(adjacent[0])
                after_literal = skip_space(text, adjacent[1])
            if after_literal < len(text) and text[after_literal] not in ",)":
                dynamic.append(f"{loc}: i18n::{kind}({first_expr})")
                continue
            message_id = "".join(pieces)
            literal_count += 1
            if message_id not in messages:
                errors.append(f"{loc}: unknown message ID {message_id!r}")
                continue
            if kind != "format":
                continue
            zh, en, _ = messages[message_id]
            expected = set(PLACEHOLDER_RE.findall(zh))
            call_close = find_call_close(code, open_paren)
            if call_close is None:
                errors.append(f"{loc}: could not find closing parenthesis for i18n::format")
                continue
            remainder = text[after_literal:call_close]
            remainder_masked = mask_comments_only(remainder)
            actual = set(PAIR_KEY_RE.findall(remainder_masked))
            if actual != expected:
                errors.append(
                    f"{loc}: format fields for {message_id!r} do not match catalog: "
                    f"expected={sorted(expected)}, provided={sorted(actual)}"
                )
    return errors, dynamic, literal_count


def mask_comments_only(text: str) -> str:
    """Blank comments but keep string literal contents for initializer-key matching."""
    out = list(text)
    i = 0
    while i < len(text):
        if text.startswith("//", i):
            j = text.find("\n", i + 2)
            j = len(text) if j < 0 else j
            for p in range(i, j):
                out[p] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = len(text) if j < 0 else j + 2
            for p in range(i, j):
                if out[p] not in "\r\n":
                    out[p] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def find_call_close(masked: str, open_paren: int) -> int | None:
    depth = 0
    for i in range(open_paren, len(masked)):
        if masked[i] == "(":
            depth += 1
        elif masked[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return None


def main() -> int:
    global ROOT, CATALOG, APP
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="EUI-Edits repository root")
    args = parser.parse_args()
    ROOT = args.root.resolve()
    CATALOG = ROOT / "apps" / "neo_editor" / "model"
    APP = ROOT / "apps" / "neo_editor"

    messages, errors = parse_resources()
    call_errors, dynamic, literal_count = scan_calls(messages)
    errors.extend(call_errors)

    for error in errors:
        print(f"ERROR: {error}")
    for item in dynamic:
        print(f"REVIEW: dynamic message ID: {item}")
    if errors:
        print(f"i18n check failed: {len(errors)} error(s), {len(messages)} message(s), {literal_count} literal call(s), {len(dynamic)} dynamic call(s)")
        return 1
    print(f"i18n check passed: {len(messages)} message(s), {literal_count} literal call(s), {len(dynamic)} dynamic call(s) for review")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

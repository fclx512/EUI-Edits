"""Capture fixed Markdown screenshots and assert the drawn divider's active state.

Run after building build-win32/neo_editor: python tests/probes/verify_markdown_visual.py
The screenshots are checked-in evidence under tests/assets/markdown/screenshots/.
The app runs with isolated APPDATA, so a user's open editor and settings are untouched.

The probe requires exclusive use of the machine: EUI-Edits is single-instance via the
Local\\EUI-Edits.SingleInstance mutex, so a user's running editor would swallow the
launched process (and we would drive the wrong window). The probe therefore refuses to
start while another instance is alive and never kills user processes.
"""

import ctypes
import hashlib
import os
import subprocess
import tempfile
import time
from ctypes import wintypes

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import win_capture as cad
from win_capture import (assert_no_foreign_instance, assert_unlocked, capture_client,
                         ensure_foreground)

EXE = os.environ.get("NEO_PROBE_EXE") or os.path.join(
    REPO, "build-win32", "Release", "neo_editor.exe")
DOC = os.path.join(REPO, "tests", "assets", "markdown", "divider_visual.md")
COVERAGE = os.path.join(REPO, "tests", "assets", "markdown", "live_preview_coverage.md")
FRONTMATTER = os.path.join(REPO, "tests", "assets", "markdown", "frontmatter_coverage.md")
OBSIDIAN_CSS = os.path.join(REPO, "参考", "obsidian-style", "app.css")
OUT = os.path.join(REPO, "tests", "assets", "markdown", "screenshots")
SETTINGS = ""
RUNTIME = ""


def settings_dict():
    result = {}
    if os.path.exists(SETTINGS):
        with open(SETTINGS, encoding="utf-8", errors="replace") as source:
            for line in source:
                if "=" in line:
                    key, value = line.rstrip("\r\n").split("=", 1)
                    result[key] = value
    return result


def write_settings(values):
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    with open(SETTINGS, "w", encoding="utf-8", newline="\n") as target:
        for key, value in values.items():
            target.write(f"{key}={value}\n")


def window_for_pid(pid):
    matches = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def callback(hwnd, unused):
        owner = wintypes.DWORD()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and cad.user32.IsWindowVisible(hwnd):
            matches.append(hwnd)
        return True

    cad.user32.EnumWindows(callback, 0)
    return matches[0] if len(matches) == 1 else None


def assert_owned(process, hwnd, where=""):
    """The captured window must still be this probe's own, live process."""
    if process.poll() is not None:
        raise RuntimeError(f"{where}: EUI-Edits (pid={process.pid}) already exited "
                           f"with {process.returncode}")
    if not cad.user32.IsWindow(hwnd):
        raise RuntimeError(f"{where}: window handle is no longer valid")
    owner = wintypes.DWORD()
    cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    if owner.value != process.pid:
        raise RuntimeError(f"{where}: window belongs to pid {owner.value}, "
                           f"not the probe process {process.pid}")


def launch():
    assert_no_foreign_instance("launch")
    env = os.environ.copy()
    env["APPDATA"] = RUNTIME
    process = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env)
    hwnd = None
    for _ in range(100):
        time.sleep(0.1)
        hwnd = window_for_pid(process.pid)
        if hwnd:
            break
        if process.poll() is not None:
            raise RuntimeError(
                f"EUI-Edits exited early (pid={process.pid}, exit={process.returncode}); "
                "a running instance likely swallowed this launch")
    if not hwnd:
        process.terminate()
        raise RuntimeError("EUI-Edits window did not appear")
    assert_owned(process, hwnd, "launch")
    time.sleep(2.0)
    if not ensure_foreground(hwnd):
        raise RuntimeError("EUI-Edits did not become the foreground window")
    time.sleep(0.5)
    return process, hwnd


def close(process, hwnd):
    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def rgb(pixels, width, x, y):
    i = (y * width + x) * 4
    return (pixels[i + 2], pixels[i + 1], pixels[i])


def rule_rows(width, height, pixels, expected):
    """Find a near-solid rule across the editor, ignoring menus and status bar."""
    rows = []
    left, right = 80, min(width - 80, 850)
    for y in range(85, min(height - 70, 500)):
        matching = 0
        for x in range(left, right):
            pixel = rgb(pixels, width, x, y)
            if all(abs(pixel[c] - expected[c]) <= 3 for c in range(3)):
                matching += 1
        if matching >= (right - left) * 0.95:
            rows.append(y)
    return rows


def capture(theme, expected, prefix="divider", theme_file=""):
    values = settings_dict()
    values.update({
        "vault": os.path.dirname(DOC), "last_file": DOC, "mode": "0",
        "line_numbers": "0", "readable_width": "0", "show_status_bar": "0",
        "editor_font_size": "16", "editor_font_file": "", "ui_scale": "1",
        "theme": str(theme),
    })
    if theme_file:
        values["last_theme_file"] = theme_file
    else:
        values.pop("last_theme_file", None)
    write_settings(values)
    process, hwnd = launch()
    try:
        assert_unlocked("divider visual")
        assert_owned(process, hwnd, "divider visual")
        cad.click(hwnd, 120, 455)  # park caret in the final paragraph, away from the divider
        time.sleep(0.5)
        width, height, pixels = capture_client(hwnd)
        label = "light" if theme else "dark"
        cad.write_png(os.path.join(OUT, f"{prefix}-{label}.png"), width, height, pixels)
        rows = rule_rows(width, height, pixels, expected)
        if len(rows) != 2 or rows[1] != rows[0] + 1:
            raise AssertionError(f"theme {theme}: expected one 2px visible divider, got rows {rows}")

        # The rule occupies the ordinary text row; clicking its middle reveals Markdown source.
        assert_owned(process, hwnd, "divider visual active")
        assert_unlocked("divider visual active")
        if not ensure_foreground(hwnd):
            raise RuntimeError("lost foreground before clicking divider")
        cad.click(hwnd, 150, rows[0])
        time.sleep(0.6)
        active_w, active_h, active_pixels = capture_client(hwnd)
        active_rows = rule_rows(active_w, active_h, active_pixels, expected)
        if active_rows:
            raise AssertionError(f"theme {theme}: rule still visible with caret on source: {active_rows}")
        cad.write_png(os.path.join(OUT, f"{prefix}-{label}-active.png"), active_w, active_h, active_pixels)
        print(f"{prefix}/{label}: visible 2px rule at y={rows[0]}..{rows[1]}; hidden when active")
    finally:
        close(process, hwnd)


def capture_coverage(theme):
    values = settings_dict()
    values.update({
        "vault": os.path.dirname(COVERAGE), "last_file": COVERAGE, "mode": "0",
        "line_numbers": "0", "readable_width": "0", "show_status_bar": "0",
        "editor_font_size": "16", "editor_font_file": "", "ui_scale": "1",
        "theme": str(theme),
    })
    values.pop("last_theme_file", None)
    write_settings(values)
    process, hwnd = launch()
    try:
        label = "light" if theme else "dark"
        assert_unlocked("coverage")
        assert_owned(process, hwnd, "coverage")
        cad.click(hwnd, 130, 670)
        signatures = []
        for page in range(1, 7):
            time.sleep(0.5)
            assert_owned(process, hwnd, f"coverage/{label} page {page}")
            width, height, pixels = capture_client(hwnd)
            signatures.append(hashlib.sha256(pixels).digest())
            cad.write_png(os.path.join(OUT, f"coverage-{label}-p{page}.png"),
                          width, height, pixels)
            if not ensure_foreground(hwnd):
                raise RuntimeError("lost foreground during coverage capture")
            # The editor handles mouse-wheel scrolling; PageDown only moves the caret.
            cad.user32.mouse_event(0x0800, 0, 0, (-720) & 0xFFFFFFFF, 0)
        if len(set(signatures)) != len(signatures):
            raise AssertionError(f"coverage/{label}: duplicate screenshot after scrolling")
        print(f"coverage/{label}: six distinct pages captured")
    finally:
        close(process, hwnd)


def capture_frontmatter(theme, expected):
    values = settings_dict()
    values.update({
        "vault": os.path.dirname(FRONTMATTER), "last_file": FRONTMATTER, "mode": "0",
        "line_numbers": "0", "readable_width": "0", "show_status_bar": "0",
        "editor_font_size": "16", "editor_font_file": "", "ui_scale": "1",
        "theme": str(theme),
    })
    values.pop("last_theme_file", None)
    write_settings(values)
    process, hwnd = launch()
    try:
        assert_unlocked("frontmatter")
        assert_owned(process, hwnd, "frontmatter")
        cad.click(hwnd, 130, 650)
        time.sleep(0.5)
        width, height, pixels = capture_client(hwnd)
        rows = rule_rows(width, height, pixels, expected)
        if len(rows) != 2 or rows[1] != rows[0] + 1:
            raise AssertionError(f"frontmatter/{theme}: expected only the body divider, got {rows}")
        label = "light" if theme else "dark"
        cad.write_png(os.path.join(OUT, f"frontmatter-{label}.png"), width, height, pixels)
        print(f"frontmatter/{label}: only body divider at y={rows[0]}..{rows[1]}")
    finally:
        close(process, hwnd)


def main():
    global SETTINGS, RUNTIME
    cad.make_dpi_aware()
    assert_unlocked("divider visual startup")
    os.makedirs(OUT, exist_ok=True)
    previous_foreground = cad.user32.GetForegroundWindow()
    with tempfile.TemporaryDirectory(prefix="neo-markdown-visual-") as RUNTIME:
        SETTINGS = os.path.join(RUNTIME, "EUI-Edits", "settings.ini")
        capture(0, (56, 62, 71))
        capture(1, (216, 222, 228))
        capture_coverage(0)
        capture_coverage(1)
        capture_frontmatter(0, (56, 62, 71))
        capture_frontmatter(1, (216, 222, 228))
        if os.path.isfile(OBSIDIAN_CSS):
            capture(0, (54, 54, 54), "obsidian-default", OBSIDIAN_CSS)
            capture(1, (224, 224, 224), "obsidian-default", OBSIDIAN_CSS)
        else:
            print("obsidian-default: local reference app.css unavailable; optional capture skipped")
    if previous_foreground:
        cad.user32.SetForegroundWindow(previous_foreground)


if __name__ == "__main__":
    main()

"""Capture one Markdown document's editor view to a PNG, for visual verification.

Why: some acceptance items are visual only — list markers/indent alignment, quote bars,
fold/selection geometry. This probe renders a given document in the real editor with an
isolated APPDATA and saves the client area, so the result can be compared against
Obsidian Live Preview under the same conditions.

Usage:
    python tests/probes/capture_markdown.py <doc.md> <out.png> [--exe PATH]
                                            [--theme 0|1] [--font-size 16] [--scale 1]

Preconditions (checked, never worked around):
    * no other EUI-Edits instance is running (single-instance mutex would swallow the launch);
    * the workstation is unlocked (a locked desktop yields captures of the lock screen);
    * the launched window belongs to the launched PID and can be brought to the foreground.

The app runs with isolated APPDATA, so the user's settings/recovery are untouched.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import win_capture as cad
from win_capture import (assert_no_foreign_instance, assert_unlocked, capture_client,
                         ensure_foreground)

DEFAULT_EXE = os.path.join(REPO, "build", "Release", "neo_editor.exe")


def window_for_pid(pid):
    matches = []
    callback_type = cad.ctypes.WINFUNCTYPE(cad.ctypes.c_bool, cad.ctypes.c_void_p,
                                           cad.ctypes.c_void_p)

    @callback_type
    def callback(hwnd, unused):
        owner = cad.ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, cad.ctypes.byref(owner))
        if owner.value == pid and cad.user32.IsWindowVisible(hwnd):
            matches.append(hwnd)
        return True

    cad.user32.EnumWindows(callback, 0)
    return matches[0] if len(matches) == 1 else None


def write_settings(settings_path, doc, theme, font_size, scale):
    os.makedirs(os.path.dirname(settings_path), exist_ok=True)
    values = {
        "vault": os.path.dirname(doc),
        "last_file": doc,
        "mode": "0",
        "line_numbers": "0",
        "readable_width": "0",
        "show_status_bar": "0",
        "editor_font_size": str(font_size),
        "editor_font_file": "",
        "ui_scale": str(scale),
        "theme": str(theme),
    }
    with open(settings_path, "w", encoding="utf-8", newline="\n") as target:
        for key, value in values.items():
            target.write(f"{key}={value}\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("doc")
    parser.add_argument("out")
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--theme", type=int, default=0, choices=(0, 1))
    parser.add_argument("--font-size", type=int, default=16)
    parser.add_argument("--scale", default="1")
    parser.add_argument("--settle", type=float, default=3.0,
                        help="seconds to wait after the window appears before capturing")
    parser.add_argument("--width", type=int, default=0,
                        help="resize the client area to this width before capturing (0 = keep)")
    parser.add_argument("--height", type=int, default=0,
                        help="resize the client area to this height before capturing (0 = keep)")
    args = parser.parse_args()

    doc = os.path.abspath(args.doc)
    if not os.path.isfile(doc):
        raise SystemExit(f"document not found: {doc}")
    if not os.path.isfile(args.exe):
        raise SystemExit(f"exe not found: {args.exe}")

    cad.make_dpi_aware()
    assert_unlocked("capture_markdown startup")
    assert_no_foreign_instance("capture_markdown")

    with tempfile.TemporaryDirectory(prefix="neo-capture-md-") as runtime:
        settings = os.path.join(runtime, "EUI-Edits", "settings.ini")
        write_settings(settings, doc, args.theme, args.font_size, args.scale)
        env = os.environ.copy()
        env["APPDATA"] = runtime
        process = subprocess.Popen([args.exe], cwd=os.path.dirname(args.exe), env=env)
        try:
            hwnd = None
            for _ in range(200):
                time.sleep(0.1)
                hwnd = window_for_pid(process.pid)
                if hwnd:
                    break
                if process.poll() is not None:
                    raise SystemExit(f"EUI-Edits exited early (exit={process.returncode})")
            if not hwnd:
                raise SystemExit("EUI-Edits window did not appear")
            time.sleep(args.settle)
            if args.width > 0 or args.height > 0:
                # SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE：只改尺寸，不动位置/前台
                rect = cad.wintypes.RECT()
                cad.user32.GetClientRect(hwnd, cad.ctypes.byref(rect))
                width = args.width if args.width > 0 else rect.right
                height = args.height if args.height > 0 else rect.bottom
                cad.user32.SetWindowPos(hwnd, None, 0, 0, width, height, 0x0002 | 0x0004 | 0x0010)
                time.sleep(1.0)
            assert_unlocked("capture_markdown")
            if not ensure_foreground(hwnd):
                raise SystemExit("EUI-Edits did not become the foreground window")
            time.sleep(0.4)
            width, height, pixels = capture_client(hwnd)
            os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
            cad.write_png(args.out, width, height, pixels)
            print(f"captured {width}x{height} -> {args.out}")
        finally:
            if process.poll() is None:
                cad.user32.PostMessageW(hwnd, 0x0010, 0, 0) if hwnd else None
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()

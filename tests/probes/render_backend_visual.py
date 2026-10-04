"""Capture a renderer experiment's UI states with isolated settings and owned windows.

Uses the existing foreground/desktop checks. Records settled screenshots, not
animation smoothness or GPU-hang acceptance. Never edits the sample document.
"""
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import tempfile
import time

import win_capture as cad
from capture_markdown import window_for_pid, write_settings


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--doc", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--software", choices=("0", "1"), default="1")
    parser.add_argument("--theme", type=int, choices=(0, 1), default=0)
    parser.add_argument("--scale", default="1")
    parser.add_argument("--extended", action="store_true")
    parser.add_argument("--vault", action="store_true")
    args = parser.parse_args()
    exe, doc, out = map(os.path.abspath, (args.exe, args.doc, args.out))
    cad.make_dpi_aware()
    cad.assert_unlocked("renderer comparison")
    cad.assert_no_foreign_instance("renderer comparison")
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="neo-renderer-visual-") as runtime:
        write_settings(os.path.join(runtime, "EUI-Edits", "settings.ini"), doc, args.theme, 16, args.scale)
        if args.extended:
            with open(os.path.join(runtime, "EUI-Edits", "settings.ini"), "a", encoding="utf-8") as cfg:
                cfg.write(f"show_status_bar=1\nreadable_width=1\nmode={1 if args.vault else 0}\n")
        env = os.environ.copy()
        env.update(APPDATA=runtime, NEO_LIVE_RESIZE="0", NEO_D2D_SOFTWARE=args.software,
                   NEO_WIN32_DC="1")
        with open(os.path.join(out, "renderer.log"), "w", encoding="utf-8") as log:
            process = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env, stderr=log)
            hwnd = None
            captures = []

            def owned():
                cad.assert_unlocked("renderer comparison")
                if process.poll() is not None:
                    raise RuntimeError(f"probe exited: {process.returncode}")
                owner = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                if owner.value != process.pid or not cad.ensure_foreground(hwnd):
                    raise RuntimeError("probe no longer owns the foreground window")

            def capture(label):
                owned()
                time.sleep(.7)
                width, height, pixels = cad.capture_client(hwnd)
                cad.write_png(os.path.join(out, label + ".png"), width, height, pixels)
                captures.append({"state": label, "width": width, "height": height})

            def click(x, y):
                owned()
                cad.click(hwnd, int(round(x)), int(round(y)))

            def resize(w, h):
                owned()
                cad.user32.SetWindowPos(hwnd, None, 0, 0, w, h, 0x0002 | 0x0004)
                time.sleep(.5)

            def wheel(ticks):
                owned()
                w, h, _ = cad.capture_client(hwnd)
                point = cad.wintypes.POINT(w * 3 // 4, h // 2)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                cad.user32.SetCursorPos(point.x, point.y)
                for _ in range(abs(ticks)):
                    owned()
                    cad.user32.mouse_event(0x0800, 0, 0, (120 if ticks > 0 else -120) & 0xffffffff, 0)
                    time.sleep(.08)

            def chord(*keys):
                owned()
                for key in keys:
                    cad.user32.keybd_event(key, 0, 0, 0)
                for key in reversed(keys):
                    cad.user32.keybd_event(key, 0, 2, 0)
                time.sleep(.5)

            try:
                for _ in range(150):
                    hwnd = window_for_pid(process.pid)
                    if hwnd or process.poll() is not None:
                        break
                    time.sleep(.1)
                if not hwnd:
                    raise RuntimeError("probe window missing")
                owned()
                bounds = cad.wintypes.RECT()
                cad.user32.GetClientRect(hwnd, ctypes.byref(bounds))
                parked = cad.wintypes.POINT(bounds.right // 2, bounds.bottom - 60)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(parked))
                cad.user32.SetCursorPos(parked.x, parked.y)
                time.sleep(2)
                capture("editor")
                # Coordinates derive from this document's captured menu row. The
                # settings button is at the right edge; menu buttons are at left.
                width, height, _ = cad.capture_client(hwnd)
                scale = float(args.scale)
                dpi = cad.user32.GetDpiForWindow(hwnd) / 96.0
                unit = scale * dpi
                if args.extended:
                    for page in range(1, 5):
                        wheel(-7)
                        capture(f"markdown-{page}")
                    wheel(100)
                    resize(760, 720)
                    capture("editor-compact")
                    click(400, 250)
                    chord(0x11, 0x46)
                    capture("find-compact")
                    chord(0x1B)
                    resize(width + 16, height + 39)
                click(40 * scale, 20 * scale)
                capture("file-menu")
                click(width * .5, 55 * scale)
                click(width - 42 * scale, 20 * scale)
                capture("settings")
                # Programmatic sizes test relayout/repaint. They are deliberately
                # distinguished from sustained interactive border dragging.
                owned()
                cad.user32.SetWindowPos(hwnd, None, 0, 0, int(width * .75), int(height * .85), 0x0002 | 0x0004)
                capture("settings-narrow")
                if args.extended:
                    resize(760, 850)
                    capture("settings-compact")
                    compactWidth, compactHeight, _ = cad.capture_client(hwnd)
                    click(compactWidth * .50, 85 * unit)
                    capture("settings-editor-compact")
                    click(compactWidth * .83, 85 * unit)
                    capture("settings-system-compact")
                    click(compactWidth * .17, 85 * unit)
                    capture("settings-appearance-compact")
                    wheel(-20)
                    capture("settings-scrolled-compact")
                    # Last appearance card is Fonts; click its lower control.
                    stacked = compactWidth / unit - 52 < 456
                    click(130 * unit if stacked else compactWidth - 100 * unit,
                          compactHeight - 154 * unit)
                    capture("font-picker-compact")
            finally:
                if process.poll() is None and hwnd:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                    try:
                        process.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        raise RuntimeError(f"probe PID {process.pid} did not close; left for inspection")
            with open(exe, "rb") as source:
                sha = hashlib.file_digest(source, "sha256").hexdigest()
            metadata = dict(exe=exe, sha256=sha, doc=doc, software=args.software,
                            theme=args.theme, ui_scale=args.scale, pid=process.pid,
                            extended=args.extended, vault=args.vault,
                            system_dpi=dpi * 96, effective_scale=unit, win32_dc="1",
                            exit_code=process.returncode, captures=captures,
                            limitations="Settled screenshots and SetWindowPos only; no drag or IME acceptance")
            with open(os.path.join(out, "conditions.json"), "w", encoding="utf-8") as target:
                json.dump(metadata, target, ensure_ascii=False, indent=2)
    print(f"captured {len(captures)} UI states -> {out}")


if __name__ == "__main__":
    main()

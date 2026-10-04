"""Owned Win32 GUI regression for Markdown hit targets and selection at real DPI.

Edits only a disposable sample under a unique temporary APPDATA. Saves screenshot
and cursor/click evidence; never launches over an existing EUI-Edits instance.
"""
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

import win_capture as cad
from capture_markdown import window_for_pid, write_settings


class CursorInfo(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("flags", ctypes.c_uint),
                ("hCursor", ctypes.c_void_p), ("pt", cad.wintypes.POINT)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--theme", type=int, default=1)
    ap.add_argument("--scale", type=float, default=1)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked("Markdown interaction")
    cad.assert_no_foreign_instance("Markdown interaction")
    source = ("Plain 中文 English baseline\n"
              "- [ ] Task unchecked text\n"
              "- [x] Task checked text\n"
              "> Quote first line\n"
              "> > Quote nested line\n"
              "> - [ ] Quoted task text\n"
              "\n[Local link](missing-note.md)\n\n"
              "- [ ] Long task " + "中文 English words " * 20 + "\n\nTail\n")
    results = []
    with tempfile.TemporaryDirectory(prefix="neo-md-interaction-") as temp:
        doc = Path(temp) / "interaction.md"
        doc.write_text(source, encoding="utf-8")
        write_settings(str(Path(temp) / "EUI-Edits/settings.ini"), str(doc),
                       args.theme, 16, str(args.scale))
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE="1", NEO_WIN32_DC="1", NEO_LIVE_RESIZE="0")
        proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
        hwnd = None

        def owned():
            cad.assert_unlocked("Markdown interaction")
            assert proc.poll() is None and cad.ensure_foreground(hwnd), "Owned foreground lost"
            pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            assert pid.value == proc.pid

        def chord(*keys):
            owned()
            for key in keys:
                cad.user32.keybd_event(key, 0, 0, 0)
            for key in reversed(keys):
                cad.user32.keybd_event(key, 0, 2, 0)
            time.sleep(.3)

        def move(x, y):
            owned()
            point = cad.wintypes.POINT(round(x), round(y))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
            cad.user32.SetCursorPos(point.x, point.y)
            time.sleep(.8)

        def capture(label):
            owned()
            w, h, p = cad.capture_client(hwnd)
            cad.write_png(str(out / (label + ".png")), w, h, p)

        def saved():
            chord(0x11, 0x53)
            return doc.read_text(encoding="utf-8")

        def check(name, condition):
            results.append({"name": name, "passed": bool(condition)})
            assert condition, name

        def cursor_handle():
            current = CursorInfo()
            current.cbSize = ctypes.sizeof(current)
            assert cad.user32.GetCursorInfo(ctypes.byref(current)), ctypes.get_last_error()
            return current.hCursor

        try:
            for _ in range(100):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            assert hwnd
            owned()
            cad.user32.SetWindowPos(hwnd, None, 0, 0, 1200, 850, 0x0002 | 0x0004)
            time.sleep(1)
            unit = cad.user32.GetDpiForWindow(hwnd) / 96 * args.scale
            # Menu = 34 logical px; text origin = 2 + .2em, inset = 18.
            x = 18 * unit
            top = (34 + 2 + 16 * .2) * unit
            row = 24 * unit
            task_x, task_y = x + 8 * unit, top + row * 1.5
            capture("initial")
            move(task_x, task_y)
            task_cursor = cursor_handle()
            move(x + 150 * unit, task_y)
            text_cursor = cursor_handle()
            check("task cursor differs from text IBeam", task_cursor != text_cursor)
            move(x + 19 * unit, task_y)
            check("checkbox gap retains text cursor", cursor_handle() == text_cursor)
            move(x + 35 * unit, top + row * 7.5)
            check("link has clickable cursor", cursor_handle() == task_cursor)
            cad.click(hwnd, round(task_x), round(task_y))
            check("one checkbox click toggles exactly one task", saved() == source.replace("- [ ] Task unchecked", "- [x] Task unchecked", 1))
            capture("task-toggled")
            chord(0x11, 0x5A)
            check("undo restores task", saved() == source)
            cad.click(hwnd, round(task_x), round(top + row * 2.5))
            check("checked task toggles off once", saved() == source.replace("- [x] Task checked", "- [ ] Task checked", 1))
            chord(0x11, 0x5A)
            check("undo restores checked task", saved() == source)
            move(task_x, task_y)
            cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
            move(task_x + 140 * unit, task_y + row)
            cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(.3)
            check("drag out of checkbox cancels toggle", saved() == source)
            quoted_x, quoted_y = x + (12 + 8) * unit, top + row * 5.5
            cad.click(hwnd, round(quoted_x), round(quoted_y))
            check("quoted checkbox uses indented hit target", saved() == source.replace("> - [ ] Quoted", "> - [x] Quoted", 1))
            chord(0x11, 0x5A)
            check("undo restores quoted task", saved() == source)
            # Drag across real text after checkbox actions; this must still work.
            move(x + 1 * unit, top + row * .5)
            cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
            move(x + 220 * unit, top + row * .5)
            cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(.3)
            capture("text-selection")
            chord(0x11, 0x43)
            cad.user32.GetClipboardData.restype = ctypes.c_void_p
            cad.kernel32.GlobalLock.restype = ctypes.c_void_p
            cad.kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
            cad.kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
            assert cad.user32.OpenClipboard(hwnd)
            try:
                data = cad.user32.GetClipboardData(13)
                pointer = cad.kernel32.GlobalLock(data)
                assert pointer
                selected = ctypes.wstring_at(pointer)
                cad.kernel32.GlobalUnlock(data)
            finally:
                cad.user32.CloseClipboard()
            check("text drag selects actual first-line text", selected.startswith("Plain 中文 English") and "Task" not in selected)
            check("selection preserves document", saved() == source)
            move(x + 150 * unit, task_y)
            check("text cursor restored after actions", cursor_handle() == text_cursor)
            move(x + 13 * unit, top + row * 3.5)
            cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
            move(x + 210 * unit, top + row * 4.5)
            cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(.3)
            capture("quote-selection")
            check("quote selection preserves document", saved() == source)
        finally:
            if proc.poll() is None and hwnd:
                cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                proc.wait(timeout=8)
            (out / "conditions.json").write_text(json.dumps(dict(
                exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                theme=args.theme, ui_scale=args.scale, system_dpi=96 * unit / args.scale,
                checks=results, exit_code=proc.returncode), indent=2), encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()

"""Owned-window visual probe for the settings font picker.

The probe creates a disposable Markdown file and isolated APPDATA, visits the
Appearance settings and Code font picker, captures the default preview/list,
opens the native font import dialog and cancels it, then optionally selects the
Consolas candidate row. It never opens or changes a user document or settings.
The candidate row is adjustable because installed font catalogs vary by machine.

Examples:
  python -B tests/probes/font_picker.py --exe build/Release/neo_editor.exe --out build/font-picker
  python -B tests/probes/font_picker.py --exe build/Release/neo_editor.exe --out build/font-picker-150 --scale 1.5
  python -B tests/probes/font_picker.py --exe build/Release/neo_editor.exe --out build/font-picker-compact --compact
"""
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import win_capture as cad
from capture_markdown import window_for_pid, write_settings

WM_CLOSE = 0x0010
VK_ESCAPE = 0x1B
GW_OWNER = 4


def top_windows_for_pid(pid):
    """Return visible top-level HWNDs belonging to pid, with class and owner."""
    matches = []
    enum_proc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    cad.user32.EnumWindows.argtypes = [enum_proc, ctypes.c_void_p]
    cad.user32.EnumWindows.restype = ctypes.c_bool
    cad.user32.GetWindow.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    cad.user32.GetWindow.restype = ctypes.c_void_p
    cad.user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    cad.user32.GetClassNameW.restype = ctypes.c_int

    @enum_proc
    def callback(hwnd, _):
        owner_pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner_pid))
        if owner_pid.value == pid and cad.user32.IsWindowVisible(hwnd):
            class_name = ctypes.create_unicode_buffer(128)
            cad.user32.GetClassNameW(hwnd, class_name, len(class_name))
            matches.append({"hwnd": hwnd, "class": class_name.value,
                            "owner": cad.user32.GetWindow(hwnd, GW_OWNER)})
        return True

    cad.user32.EnumWindows(callback, 0)
    return matches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(REPO / "build" / "Release" / "neo_editor.exe"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--theme", type=int, choices=(0, 1), default=1,
                    help="default 1 is the light theme")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="application UI scale on top of system DPI")
    ap.add_argument("--compact", action="store_true",
                    help="resize to a 760x720 logical-pixel client area")
    ap.add_argument("--width", type=int, default=0,
                    help="optional client width in physical pixels; 0 keeps the current width")
    ap.add_argument("--height", type=int, default=0,
                    help="optional client height in physical pixels; 0 keeps the current height")
    ap.add_argument("--consolas-index", type=int, default=2,
                    help="candidate zero-based index in the filtered code-font list; -1 skips selection")
    ap.add_argument("--settle", type=float, default=1.0)
    args = ap.parse_args()

    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    if not exe.is_file():
        raise SystemExit(f"EUI-Edits executable not found: {exe}")
    if args.scale <= 0 or args.consolas_index < -1:
        raise SystemExit("--scale must be positive and --consolas-index must be -1 or greater")
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked("font picker probe")
    cad.assert_no_foreign_instance("font picker probe")

    results = {
        "exe": str(exe), "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "theme": args.theme, "theme_name": "light" if args.theme == 1 else "dark",
        "ui_scale": args.scale, "requested_consolas_index": args.consolas_index,
        "captures": [], "events": [],
        "limitations": "Window coordinates are source-derived and uncalibrated here; verify screenshots and candidate font row on the target machine.",
    }
    proc = hwnd = dialog_hwnd = None
    log = None

    with tempfile.TemporaryDirectory(prefix="neo-font-picker-") as runtime:
        runtime_path = Path(runtime)
        doc = runtime_path / "font-picker-sample.md"
        doc.write_text("# Font picker sample\n\n```cpp\nconst int answer = 42;\nreturn answer;\n```\n",
                       encoding="utf-8", newline="\n")
        settings = runtime_path / "EUI-Edits" / "settings.ini"
        write_settings(str(settings), str(doc), args.theme, 16, str(args.scale))
        with settings.open("a", encoding="utf-8", newline="\n") as cfg:
            cfg.write("ui_scale=" + str(args.scale) + "\nmode=0\nshow_status_bar=0\n")
        env = os.environ.copy()
        env.update(APPDATA=runtime, NEO_LIVE_RESIZE="0", NEO_WIN32_DC="1", NEO_D2D_SOFTWARE="1")
        log = (out / "neo_editor.log").open("w", encoding="utf-8")

        try:
            proc = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stderr=log)

            def owned():
                cad.assert_unlocked("font picker probe")
                if proc.poll() is not None:
                    raise RuntimeError(f"probe process exited early: {proc.returncode}")
                if not hwnd:
                    raise RuntimeError("owned main window is unavailable")
                pid = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value != proc.pid or not cad.ensure_foreground(hwnd):
                    raise RuntimeError("probe lost owned foreground window")

            def capture(label, target_hwnd=None, require_main=True):
                if require_main:
                    owned()
                else:
                    fg = cad.user32.GetForegroundWindow()
                    pid = ctypes.c_ulong()
                    cad.user32.GetWindowThreadProcessId(target_hwnd, ctypes.byref(pid))
                    if pid.value != proc.pid or fg != target_hwnd:
                        raise RuntimeError("native dialog is not the foreground window owned by this probe PID")
                time.sleep(args.settle)
                width, height, pixels = cad.capture_client(target_hwnd or hwnd)
                cad.write_png(str(out / f"{label}.png"), width, height, pixels)
                results["captures"].append({"state": label, "width": width, "height": height,
                                            "hwnd_kind": "owned_dialog" if target_hwnd else "main"})
                return width, height, pixels

            def click(x, y):
                owned()
                cad.click(hwnd, int(round(x)), int(round(y)))

            def wheel(ticks, x=None, y=None):
                owned()
                w, h, _ = cad.capture_client(hwnd)
                point = cad.wintypes.POINT(x if x is not None else w * 3 // 4,
                                           y if y is not None else h // 2)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                cad.user32.SetCursorPos(point.x, point.y)
                for _ in range(abs(ticks)):
                    owned()
                    cad.user32.mouse_event(0x0800, 0, 0,
                                           (120 if ticks > 0 else -120) & 0xffffffff, 0)
                    time.sleep(.06)

            def find_owned_dialog(timeout=15):
                deadline = time.time() + timeout
                while time.time() < deadline:
                    for item in top_windows_for_pid(proc.pid):
                        # Common File Dialog class; require an owner relationship
                        # to the exact main window before sending any input.
                        if item["class"] == "#32770" and item["owner"] == hwnd:
                            return item["hwnd"]
                    if proc.poll() is not None:
                        raise RuntimeError("probe exited while waiting for native font dialog")
                    time.sleep(.1)
                raise RuntimeError("owned native file dialog did not appear")

            def cancel_dialog(dlg):
                nonlocal dialog_hwnd
                dialog_hwnd = dlg
                pid = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(dlg, ctypes.byref(pid))
                if pid.value != proc.pid or cad.user32.GetWindow(dlg, GW_OWNER) != hwnd:
                    raise RuntimeError("refusing to interact with a dialog not owned by the probe window")
                if not cad.ensure_foreground(dlg):
                    raise RuntimeError("could not foreground the probe-owned file dialog")
                capture("04-native-import-dialog", dlg, require_main=False)
                # Escape cancels the native picker. A WM_CLOSE fallback is still
                # scoped to the verified modal dialog if it remains visible.
                cad.user32.keybd_event(VK_ESCAPE, 0, 0, 0)
                cad.user32.keybd_event(VK_ESCAPE, 0, 2, 0)
                deadline = time.time() + 4
                while cad.user32.IsWindow(dlg) and time.time() < deadline:
                    time.sleep(.1)
                if cad.user32.IsWindow(dlg):
                    cad.user32.PostMessageW(dlg, WM_CLOSE, 0, 0)
                    deadline = time.time() + 3
                    while cad.user32.IsWindow(dlg) and time.time() < deadline:
                        time.sleep(.1)
                if cad.user32.IsWindow(dlg):
                    raise RuntimeError("probe-owned native font dialog did not cancel/close")
                dialog_hwnd = None
                if not cad.ensure_foreground(hwnd):
                    raise RuntimeError("main probe window did not regain foreground after cancel")
                results["events"].append({"name": "import_dialog_cancelled", "passed": True})

            for _ in range(150):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            if not hwnd:
                raise RuntimeError("owned EUI-Edits window did not appear")
            owned()
            dpi = cad.user32.GetDpiForWindow(hwnd) / 96.0
            unit = dpi * args.scale
            rect = cad.wintypes.RECT()
            cad.user32.GetClientRect(hwnd, ctypes.byref(rect))
            width, height = rect.right, rect.bottom
            if args.compact:
                width, height = round(760 * unit), round(720 * unit)
            if args.width > 0:
                width = args.width
            if args.height > 0:
                height = args.height
            if args.compact or args.width > 0 or args.height > 0:
                cad.user32.SetWindowPos(hwnd, None, 0, 0, width, height, 0x0002 | 0x0004 | 0x0010)
                time.sleep(.6)
                rect = cad.wintypes.RECT()
                cad.user32.GetClientRect(hwnd, ctypes.byref(rect))
                width, height = rect.right, rect.bottom

            results.update(pid=proc.pid, system_dpi=96.0 * dpi, effective_scale=unit,
                           client_width=width, client_height=height, compact=args.compact,
                           sample=str(doc), sample_sha256=hashlib.sha256(doc.read_bytes()).hexdigest(),
                           runtime_appdata_isolated=True,
                           target_system_dpi_note="Default workflow expects a 125% Windows display; actual GetDpiForWindow is recorded and never forced.")
            capture("01-editor-start")

            # Menu-bar Settings title is right aligned; this click is derived from
            # its 10px padding plus approximate 42px text/control width. For a
            # screen-specific layout, --width/--height and screenshots are the
            # calibration controls.
            logical_width = width / unit
            click((logical_width - 38.0) * unit, 17.0 * unit)
            capture("02-settings-open")

            # Appearance is the first vertical nav entry on wide screens and the
            # first third-width segment on compact screens.
            logical_height = height / unit
            header = 58.0
            compact_nav = logical_width < 840.0
            if compact_nav:
                appearance_x = logical_width / 6.0
                appearance_y = header + 27.0
            else:
                appearance_x = 82.0
                appearance_y = header + 40.0
            click(appearance_x * unit, appearance_y * unit)
            capture("03-appearance")

            # Derive the Fonts slot from settings_panel.h's source geometry:
            # 26px page padding, 44px section title, four prior rows, row height,
            # and the control-column formula. Font slots are the fifth Appearance row.
            page_width = min(952.0, logical_width if compact_nav else logical_width - 176.0)
            page_x = 0.0 if compact_nav else 176.0 + (logical_width - 176.0 - page_width) / 2.0
            page_top = header + (54.0 if compact_nav else 0.0)
            content_width = max(0.0, page_width - 52.0)
            control_width = min(max(0.0, content_width - 32.0), 176.0)
            stacked = content_width < control_width + 14.0 * 20.0
            control_x = 16.0 if stacked else content_width - control_width - 16.0
            row_height = max(84.0, 14.0 + 12.0 * 2.0 + 46.0) + (38.0 if stacked else 0.0)
            control_y = row_height - 28.0 - 20.0 if stacked else (row_height - 8.0 - 28.0) / 2.0
            footer_rule_y = max(0.0, (logical_height - (header + (54.0 if compact_nav else 0.0))) - 86.0)
            scroll_height = max(0.0, footer_rule_y - 24.0 - 8.0)
            content_height = 44.0 + row_height * 5.0 + 22.0
            max_offset = max(0.0, content_height - scroll_height)
            if max_offset > 0:
                wheel(-30, int(width * .7), int(height * .75))
                capture("03-appearance-scrolled")
            fonts_x = page_x + 26.0 + control_x + control_width * .55
            fonts_y = page_top + 24.0 + 44.0 + 4.0 * row_height + control_y + 14.0 - max_offset
            click(fonts_x * unit, fonts_y * unit)
            capture("04-font-picker-default")

            # Code tab in the segmented control at x=26..366, y=60..88 logical.
            # Use the center of the middle segment and capture system default.
            click((page_x + 26.0 + 340.0 * .50) * unit, (page_top + 74.0) * unit)
            code_default = capture("05-code-font-default")
            results["font_picker_layout"] = {"page_width_logical": page_width,
                                             "page_x_logical": page_x,
                                             "content_width_logical": content_width,
                                             "stacked": stacked,
                                             "font_slot_logical": [fonts_x, fonts_y],
                                             "estimated_scroll_max": max_offset,
                                             "code_tab_logical": [196.0, 74.0]}

            # Import button is at the far right of the content region: x = 26 +
            # contentWidth - min(124, contentWidth*.38)/2; y=110.
            import_width = min(124.0, content_width * .38)
            import_x = 26.0 + content_width - import_width / 2.0
            click((page_x + import_x) * unit, (page_top + 110.0) * unit)
            dialog_hwnd = find_owned_dialog()
            cancel_dialog(dialog_hwnd)
            capture("06-code-font-after-import-cancel")

            if args.consolas_index >= 0:
                # The code list is sorted with common fonts first. Current
                # Windows 11 systems commonly put Cascadia Code, Cascadia Mono,
                # then Consolas in these first rows. Keep the index adjustable;
                # never claim the selected family based on coordinates alone.
                font_row_height = max(22, round(14 * 1.70))
                sample_height = max(13.0, 14.0 * 1.16) * 3.0 + 2.0
                list_y = 136.0 + sample_height + 8.0
                candidate_y = list_y + (args.consolas_index + .5) * font_row_height
                candidate_x = 26.0 + min(content_width - 20.0, 320.0) * .5
                if candidate_y < footer_rule_y - 12.0:
                    click((page_x + candidate_x) * unit, (page_top + candidate_y) * unit)
                    capture("07-consolas-candidate-selected")
                    results["events"].append({"name": "candidate_font_row_clicked", "index": args.consolas_index,
                                              "passed": True, "verified_family": False})
                    if args.consolas_index == 2:
                        saved_before = dict(line.split("=", 1) for line in settings.read_text(encoding="utf-8").splitlines() if "=" in line).get("code_font_file", "")
                        invalid = runtime_path / "损坏字体.ttf"
                        invalid.write_bytes(b"invalid-font" * 10)
                        click((page_x + import_x) * unit, (page_top + 110.0) * unit)
                        dialog_hwnd = find_owned_dialog()
                        if not cad.ensure_foreground(dialog_hwnd):
                            raise RuntimeError("owned import dialog lost foreground")
                        # Type through the actual foreground dialog's filename focus.
                        # Explorer pickers may expose hidden compatibility edits,
                        # so SetWindowText is not a reliable visible-input path.
                        class KeyInput(ctypes.Structure):
                            _fields_ = [("vk", ctypes.c_ushort), ("scan", ctypes.c_ushort),
                                        ("flags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                                        ("extra", ctypes.c_void_p)]
                        class InputUnion(ctypes.Union):
                            _fields_ = [("key", KeyInput), ("padding", ctypes.c_uint64 * 4)]
                        class Input(ctypes.Structure):
                            _fields_ = [("kind", ctypes.c_ulong), ("data", InputUnion)]
                        cad.user32.SendInput.argtypes = [ctypes.c_uint, ctypes.POINTER(Input), ctypes.c_int]
                        units = str(invalid).encode("utf-16-le")
                        entries = []
                        for i in range(0, len(units), 2):
                            scan = int.from_bytes(units[i:i+2], "little")
                            for flags in (4, 6):  # KEYEVENTF_UNICODE, then KEYUP
                                event = Input()
                                event.kind = 1
                                event.data.key = KeyInput(0, scan, flags, 0, None)
                                entries.append(event)
                        native_inputs = (Input * len(entries))(*entries)
                        if cad.user32.SendInput(len(entries), native_inputs, ctypes.sizeof(Input)) != len(entries):
                            raise RuntimeError("owned filename typing failed")
                        time.sleep(.2)
                        capture("08a-invalid-import-filename", dialog_hwnd, require_main=False)
                        # Use the dialog's real default-button keyboard path.
                        cad.user32.keybd_event(0x0D, 0, 0, 0)
                        cad.user32.keybd_event(0x0D, 0, 2, 0)
                        deadline = time.time() + 8
                        while cad.user32.IsWindow(dialog_hwnd) and time.time() < deadline:
                            time.sleep(.05)
                        if cad.user32.IsWindow(dialog_hwnd):
                            raise RuntimeError("invalid font dialog did not return")
                        dialog_hwnd = None
                        capture("08-invalid-import-keeps-consolas")
                        saved_after = dict(line.split("=", 1) for line in settings.read_text(encoding="utf-8").splitlines() if "=" in line).get("code_font_file", "")
                        results["events"].append({"name": "invalid_import_preserves_selected_consolas", "passed":
                            saved_before == saved_after and Path(saved_after).name.lower() == "consola.ttf"})
                else:
                    # Scroll the font list viewport, not the main settings page.
                    list_x = int((page_x + 26 + content_width * .5) * unit)
                    list_center_y = int((page_top + 24 + list_y + 90) * unit)
                    wheel(-8, list_x, list_center_y)
                    capture("07-font-list-scrolled")
                    results["events"].append({"name": "candidate_font_row_not_visible", "index": args.consolas_index,
                                              "passed": False, "verified_family": False})
            else:
                results["events"].append({"name": "consolas_selection_skipped", "passed": True})

        except Exception as exc:
            results["error"] = f"{type(exc).__name__}: {exc}"
            if dialog_hwnd and cad.user32.IsWindow(dialog_hwnd) and proc and proc.poll() is None:
                pid = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(dialog_hwnd, ctypes.byref(pid))
                if pid.value == proc.pid and cad.user32.GetWindow(dialog_hwnd, GW_OWNER) == hwnd:
                    cad.user32.PostMessageW(dialog_hwnd, WM_CLOSE, 0, 0)
            if hwnd and proc and proc.poll() is None:
                try:
                    if cad.ensure_foreground(hwnd):
                        w, h, pixels = cad.capture_client(hwnd)
                        cad.write_png(str(out / "failure-current-window.png"), w, h, pixels)
                        results["captures"].append({"state": "failure-current-window", "width": w,
                                                    "height": h, "hwnd_kind": "main"})
                except Exception as capture_error:
                    results["failure_capture_error"] = str(capture_error)
            raise
        finally:
            if proc and proc.poll() is None and hwnd:
                # Only post WM_CLOSE to the HWND verified against our own PID.
                pid = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value == proc.pid:
                    cad.user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    results["close_error"] = f"PID {proc.pid} did not exit after WM_CLOSE; process left running"
            results["exit_code"] = proc.returncode if proc else None
            results["sample_after_sha256"] = hashlib.sha256(doc.read_bytes()).hexdigest() if doc.exists() else None
            results["sample_preserved"] = results.get("sample_sha256") == results["sample_after_sha256"]
            if settings.exists():
                saved = dict(line.split("=", 1) for line in settings.read_text(encoding="utf-8").splitlines() if "=" in line)
                results["saved_code_font"] = saved.get("code_font_file", "")
                if args.consolas_index == 2 and not results.get("error"):
                    results["events"].append({"name": "consolas_path_persisted", "passed":
                        Path(results["saved_code_font"]).name.lower() == "consola.ttf"})
            (out / "conditions.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
            if log:
                log.close()

    print(json.dumps({"out": str(out), "captures": len(results["captures"]),
                      "events": results["events"], "error": results.get("error")},
                     ensure_ascii=False, indent=2))
    if results.get("close_error") or not results["sample_preserved"] or any(not e["passed"] for e in results["events"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()

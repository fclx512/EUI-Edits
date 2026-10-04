"""Real-window probes for tab close-anchor interruption rules (plan §11).

Each case runs the supplied executable in its own APPDATA/TEMP and owns every
input/capture by PID and foreground checks. Screenshots are full-client PNGs;
the corresponding unmodified top-strip BGRA bytes and sampled accent pixels are
also retained as evidence. This script is intentionally not run by unit tests.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

PROBE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(PROBE_DIR))

import win_capture as cad
from capture_markdown import window_for_pid
import tab_menu_visual as visual

EXPECTED_PHYSICAL = (2500, 1500)
EXPECTED_DPI = 120
UI_SCALE = 0.8
EXPECTED_EFFECTIVE_SCALE = 1.0
MENU_FONT = 14
MENU_BAR_DIP = 34.0
BAR_INSET_DIP = 1.0
CLOSE_CENTER_FROM_BAR_DIP = 160.5

SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
MOUSEEVENTF_WHEEL = 0x0800
WM_CLOSE = 0x0010
WM_CHAR = 0x0102
VK_ESCAPE = 0x1B
VK_CONTROL = 0x11
VK_END = 0x23
VK_W = 0x57

# Explicit pointer-sized signatures keep HWND/HANDLE values intact on 64-bit
# Windows.  win_capture supplies the shared DLL objects and helper routines.
_ct = cad.ctypes
_wt = cad.wintypes
_PTR = _ct.c_void_p
_WPARAM = _ct.c_size_t
_LPARAM = _ct.c_ssize_t
cad.user32.GetForegroundWindow.restype = _PTR
cad.user32.GetWindowThreadProcessId.argtypes = [_PTR, _ct.POINTER(_ct.c_ulong)]
cad.user32.GetWindowThreadProcessId.restype = _ct.c_ulong
cad.user32.GetDpiForWindow.argtypes = [_PTR]
cad.user32.GetDpiForWindow.restype = _ct.c_uint
cad.user32.SetWindowPos.argtypes = [_PTR, _PTR, _ct.c_int, _ct.c_int,
                                    _ct.c_int, _ct.c_int, _ct.c_uint]
cad.user32.SetWindowPos.restype = _wt.BOOL
cad.user32.GetClientRect.argtypes = [_PTR, _ct.POINTER(_wt.RECT)]
cad.user32.GetClientRect.restype = _wt.BOOL
cad.user32.GetWindowRect.argtypes = [_PTR, _ct.POINTER(_wt.RECT)]
cad.user32.GetWindowRect.restype = _wt.BOOL
cad.user32.ClientToScreen.argtypes = [_PTR, _ct.POINTER(_wt.POINT)]
cad.user32.ClientToScreen.restype = _wt.BOOL
cad.user32.GetCursorPos.argtypes = [_ct.POINTER(_wt.POINT)]
cad.user32.GetCursorPos.restype = _wt.BOOL
cad.user32.SetCursorPos.argtypes = [_ct.c_int, _ct.c_int]
cad.user32.SetCursorPos.restype = _wt.BOOL
cad.user32.PostMessageW.argtypes = [_PTR, _ct.c_uint, _WPARAM, _LPARAM]
cad.user32.PostMessageW.restype = _wt.BOOL
cad.user32.mouse_event.argtypes = [_wt.DWORD, _wt.DWORD, _wt.DWORD,
                                   _wt.DWORD, _WPARAM]
cad.user32.mouse_event.restype = None
cad.user32.keybd_event.argtypes = [_wt.BYTE, _wt.BYTE, _wt.DWORD, _WPARAM]
cad.user32.keybd_event.restype = None
cad.user32.MapVirtualKeyW.argtypes = [_wt.UINT, _wt.UINT]
cad.user32.MapVirtualKeyW.restype = _wt.UINT
cad.user32.WindowFromPoint.argtypes = [_wt.POINT]
cad.user32.WindowFromPoint.restype = _PTR
cad.user32.GetAncestor.argtypes = [_PTR, _wt.UINT]
cad.user32.GetAncestor.restype = _PTR
cad.user32.IsWindowVisible.argtypes = [_PTR]
cad.user32.IsWindowVisible.restype = _wt.BOOL
cad.user32.IsIconic.argtypes = [_PTR]
cad.user32.IsIconic.restype = _wt.BOOL
cad.user32.ShowWindow.argtypes = [_PTR, _ct.c_int]
cad.user32.ShowWindow.restype = _wt.BOOL
cad.user32.BringWindowToTop.argtypes = [_PTR]
cad.user32.BringWindowToTop.restype = _wt.BOOL
cad.user32.SetForegroundWindow.argtypes = [_PTR]
cad.user32.SetForegroundWindow.restype = _wt.BOOL
cad.user32.GetWindowTextW.argtypes = [_PTR, _ct.c_wchar_p, _ct.c_int]
cad.user32.GetWindowTextW.restype = _ct.c_int
cad.user32.GetDC.argtypes = [_PTR]
cad.user32.GetDC.restype = _PTR
cad.user32.ReleaseDC.argtypes = [_PTR, _PTR]
cad.user32.ReleaseDC.restype = _ct.c_int
cad.gdi32.CreateCompatibleDC.argtypes = [_PTR]
cad.gdi32.CreateCompatibleDC.restype = _PTR
cad.gdi32.CreateCompatibleBitmap.argtypes = [_PTR, _ct.c_int, _ct.c_int]
cad.gdi32.CreateCompatibleBitmap.restype = _PTR
cad.gdi32.SelectObject.argtypes = [_PTR, _PTR]
cad.gdi32.SelectObject.restype = _PTR
cad.gdi32.BitBlt.argtypes = [_PTR, _ct.c_int, _ct.c_int, _ct.c_int,
                             _ct.c_int, _PTR, _ct.c_int, _ct.c_int, _wt.DWORD]
cad.gdi32.BitBlt.restype = _wt.BOOL
cad.gdi32.GetDIBits.argtypes = [_PTR, _PTR, _wt.UINT, _wt.UINT, _PTR,
                                _PTR, _wt.UINT]
cad.gdi32.GetDIBits.restype = _ct.c_int
cad.gdi32.DeleteObject.argtypes = [_PTR]
cad.gdi32.DeleteObject.restype = _wt.BOOL
cad.gdi32.DeleteDC.argtypes = [_PTR]
cad.gdi32.DeleteDC.restype = _wt.BOOL


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(path):
    return json.loads(path.read_text(encoding="utf-8"))


def stable_records(snapshot):
    return [{"id": int(record["id"]), "path": record.get("path", ""),
             "dirty": bool(record.get("dirty", False))}
            for record in snapshot.get("records", [])]


class ProbeRun:
    def __init__(self, exe, expected_hash, root, label, theme):
        self.exe = exe
        self.expected_hash = expected_hash
        self.root = root
        self.label = label
        self.theme = theme
        self.case_dir = root / label
        self.case_dir.mkdir(parents=True, exist_ok=False)
        self.config = self.case_dir / "appdata" / "EUI-Edits"
        self.config.mkdir(parents=True)
        self.temp = self.case_dir / "temp"
        self.temp.mkdir()
        self.proc = None
        self.hwnd = None
        self.unit = 1.0
        self.bar_y = round(MENU_BAR_DIP * 0.5)
        self.result = {"name": label, "checks": [], "captures": [], "events": [],
                       "manifest_snapshots": {}, "pixel_samples": {}, "passed": False}

    @property
    def manifest_path(self):
        return self.config / "session" / "manifest.json"

    def check(self, name, condition, detail=None):
        entry = {"name": name, "passed": bool(condition), "detail": detail}
        self.result["checks"].append(entry)
        print(("PASS " if condition else "FAIL ") + self.label + ": " + name,
              "" if detail is None else detail, flush=True)
        return bool(condition)

    def require(self, name, condition, detail=None):
        if not self.check(name, condition, detail):
            raise RuntimeError(name + (": " + str(detail) if detail is not None else ""))

    def owned(self):
        cad.assert_unlocked(self.label)
        if self.proc is None or self.hwnd is None or self.proc.poll() is not None:
            raise RuntimeError("owned probe process is not alive")
        owner = cad.ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(self.hwnd, cad.ctypes.byref(owner))
        if owner.value != self.proc.pid:
            raise RuntimeError(f"window PID {owner.value} != owned PID {self.proc.pid}")
        foreground = cad.user32.GetForegroundWindow()
        if foreground != self.hwnd:
            foreground_owner = cad.ctypes.c_ulong()
            if not foreground or not cad.user32.GetWindowThreadProcessId(
                    foreground, cad.ctypes.byref(foreground_owner)):
                raise RuntimeError("foreground window is not owned by the probe")
            root_owner = cad.user32.GetAncestor(foreground, 3)  # GA_ROOTOWNER
            if foreground_owner.value != self.proc.pid or root_owner != self.hwnd:
                raise RuntimeError("owned window or its modal dialog lost foreground")

    def snapshot(self, name):
        deadline = time.time() + 8.0
        last_error = None
        while time.time() < deadline:
            try:
                value = manifest(self.manifest_path)
                self.result["manifest_snapshots"][name] = {
                    "active": int(value.get("active", 0)),
                    "order": stable_records(value),
                }
                return value
            except (OSError, ValueError, KeyError, TypeError) as exc:
                last_error = str(exc)
                time.sleep(0.05)
        raise RuntimeError(f"manifest unavailable for {name}: {last_error}")

    def wait_records(self, predicate, label, timeout=8.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                last = manifest(self.manifest_path)
                if predicate(last):
                    self.result["manifest_snapshots"][label] = {
                        "active": int(last.get("active", 0)), "order": stable_records(last)}
                    return last
            except (OSError, ValueError, KeyError, TypeError):
                pass
            time.sleep(0.05)
        raise RuntimeError(f"manifest did not reach {label}; last={stable_records(last or {})}")

    def bars(self, raw, width):
        return visual.detect_bars(raw, width, self.bar_y, 0, width - 1, self.unit)

    def capture(self, name):
        self.owned()
        width, height, raw = cad.capture_client(self.hwnd)
        png = self.case_dir / f"{name}.png"
        cad.write_png(str(png), width, height, raw)
        top = max(0, self.bar_y - 8)
        bottom = min(height, self.bar_y + 10)
        strip = raw[top * width * 4:bottom * width * 4]
        raw_path = self.case_dir / f"{name}-strip.bgra"
        raw_path.write_bytes(strip)
        bars = self.bars(raw, width)
        samples = []
        for index, (x0, x1, rgb) in enumerate(bars):
            x = min(width - 1, max(0, x0 + 1))
            y = min(height - 1, max(0, self.bar_y + 1))
            pos = (y * width + x) * 4
            samples.append({"index": index, "bar_x_px": x0, "bar_right_px": x1,
                            "rgb": list(rgb), "sample_xy_px": [x, y],
                            "sample_rgba": [raw[pos + 2], raw[pos + 1], raw[pos], raw[pos + 3]]})
        self.result["captures"].append({"name": name, "png": png.name, "png_sha256": digest(png),
                                        "client_px": [width, height],
                                        "raw_strip": raw_path.name, "raw_strip_sha256": digest(raw_path),
                                        "raw_strip_y_px": [top, bottom], "raw_strip_bytes": len(strip),
                                        "bars": samples,
                                        "leftmost_bar_x_px": bars[0][0] if bars else None})
        return width, height, raw, bars

    def client_rect(self):
        self.owned()
        rect = cad.wintypes.RECT()
        if not cad.user32.GetClientRect(self.hwnd, cad.ctypes.byref(rect)):
            raise OSError("GetClientRect failed")
        width, height = rect.right - rect.left, rect.bottom - rect.top
        _, _, raw, bars = self.capture("client-rect-sample")
        return width, height, raw, bars

    def cursor(self):
        point = cad.wintypes.POINT()
        if not cad.user32.GetCursorPos(cad.ctypes.byref(point)):
            raise OSError("GetCursorPos failed")
        return point.x, point.y

    def move_to_client_px(self, x, y):
        self.owned()
        point = cad.wintypes.POINT(int(x), int(y))
        if not cad.user32.ClientToScreen(self.hwnd, cad.ctypes.byref(point)):
            raise OSError("ClientToScreen failed")
        if not cad.user32.SetCursorPos(point.x, point.y):
            raise OSError("SetCursorPos failed")
        time.sleep(0.2)

    def click_client_px(self, x, y):
        self.move_to_client_px(x, y)
        self.owned()
        screen = cad.wintypes.POINT(int(x), int(y))
        cad.user32.ClientToScreen(self.hwnd, cad.ctypes.byref(screen))
        hit = cad.user32.WindowFromPoint(screen)
        if cad.user32.GetAncestor(hit, 2) != self.hwnd:
            raise RuntimeError("click location is covered by a foreign window")
        cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.06)
        cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(0.45)
        self.owned()

    def key(self, code, control=False):
        self.owned()
        codes = ([VK_CONTROL] if control else []) + [code]
        for value in codes:
            cad.user32.keybd_event(value, cad.user32.MapVirtualKeyW(value, 0), 0, 0)
        time.sleep(0.08)
        for value in reversed(codes):
            cad.user32.keybd_event(value, cad.user32.MapVirtualKeyW(value, 0), 2, 0)
        time.sleep(0.4)
        self.owned()

    def open_forward(self, path):
        self.owned()
        request = self.temp / "EUI-Edits.next-open"
        stage = self.temp / "owned-open.tmp"
        stage.write_text(str(path), encoding="utf-8")
        stage.replace(request)
        self.key(0x10)  # Shift consumes the app's owned deferred-open request.
        self.require("deferred open was consumed", not request.exists(), path.name)

    def start(self, first_path=None, expect_manifest=True):
        settings = self.config / "settings.ini"
        settings.write_text(
            f"mode=1\nui_scale={UI_SCALE}\nui_font_size={MENU_FONT}\n"
            f"theme={self.theme}\nanimations=0\nui_language=zh-CN\nshow_status_bar=1\n",
            encoding="utf-8")
        env = dict(os.environ, APPDATA=str(self.config.parent), TEMP=str(self.temp),
                   TMP=str(self.temp), NEO_SINGLE_INSTANCE="0", NEO_D2D_SOFTWARE="1",
                   NEO_WIN32_DC="1", NEO_PROBE_ALLOW_FOREIGN="1")
        command = [str(self.exe)] + ([str(first_path)] if first_path else [])
        self.proc = subprocess.Popen(command, cwd=str(self.exe.parent), env=env)
        for _ in range(160):
            self.hwnd = window_for_pid(self.proc.pid)
            if self.hwnd or self.proc.poll() is not None:
                break
            time.sleep(0.1)
        self.require("owned window appeared", self.hwnd is not None,
                     f"pid={self.proc.pid}; exit={self.proc.poll()}")
        self.require("owned window foreground",
                     cad.ensure_foreground(self.hwnd) and cad.user32.GetForegroundWindow() == self.hwnd,
                     f"pid={self.proc.pid}")
        owner = cad.ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(self.hwnd, cad.ctypes.byref(owner))
        self.require("window belongs to child PID", owner.value == self.proc.pid,
                     f"window={owner.value}, child={self.proc.pid}")
        cad.user32.SetWindowPos(self.hwnd, None, 0, 0, *EXPECTED_PHYSICAL,
                                SWP_NOZORDER | SWP_NOACTIVATE)
        time.sleep(0.7)
        self.owned()
        dpi = int(cad.user32.GetDpiForWindow(self.hwnd))
        self.unit = (dpi / 96.0) * UI_SCALE
        self.result["process"] = {"pid": self.proc.pid, "window": int(self.hwnd),
                                  "dpi": dpi, "ui_scale": UI_SCALE,
                                  "effective_scale": self.unit,
                                  "physical_desktop_px": [cad.user32.GetSystemMetrics(0),
                                                          cad.user32.GetSystemMetrics(1)]}
        self.require("physical desktop accommodates requested 2500x1500 window",
                     all(actual >= requested for actual, requested in
                         zip(self.result["process"]["physical_desktop_px"], EXPECTED_PHYSICAL)),
                     self.result["process"]["physical_desktop_px"])
        self.require("window DPI is 120", dpi == EXPECTED_DPI, dpi)
        self.require("effective UI scale is 1.0",
                     abs(self.unit - EXPECTED_EFFECTIVE_SCALE) <= 0.01, self.unit)
        if expect_manifest:
            self.wait_records(lambda snap: bool(snap.get("records")), "startup")
        else:
            time.sleep(0.3)
            self.require("untouched blank startup has no session manifest",
                         not self.manifest_path.exists(), str(self.manifest_path))

    def close_clean_tail(self):
        before = self.snapshot("before_tail_close")
        records = stable_records(before)
        self.require("tail fixture contains at least two clean pages",
                     len(records) >= 2 and not records[-1]["dirty"], records)
        width, _, raw, bars = self.capture("normal-before-tail-close")
        self.require("all fixture tabs visible before tail close", len(bars) == len(records),
                     {"bars": len(bars), "records": records})
        self.result["normalregion_leftmost_px"] = bars[0][0] if bars else None
        target_id = records[-1]["id"]
        self.result["tail_close"] = {"tab_id": target_id,
                                      "before_order": [record["id"] for record in records]}
        close_x = bars[-1][0] + round(CLOSE_CENTER_FROM_BAR_DIP * self.unit)
        self.move_to_client_px(close_x, self.bar_y)
        pointer_before = self.cursor()
        screen = cad.wintypes.POINT(close_x, self.bar_y)
        cad.user32.ClientToScreen(self.hwnd, cad.ctypes.byref(screen))
        self.require("tail pointer at its close center", pointer_before == (screen.x, screen.y),
                     {"actual": pointer_before, "expected": [screen.x, screen.y]})
        self.owned()
        cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.06)
        cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
        self.wait_records(lambda snap: len(snap.get("records", [])) == len(records) - 1,
                          "tail_closed")
        pointer_after = self.cursor()
        self.require("fixed pointer stayed in place after clean tail close",
                     pointer_after == pointer_before, {"before": pointer_before,
                                                       "after": pointer_after})
        after = self.snapshot("after_tail_close")
        after_records = stable_records(after)
        expected = [record["id"] for record in records[:-1]]
        observed = [record["id"] for record in after_records]
        self.require("only the intended tail TabId closed", observed == expected,
                     {"target": target_id, "before": [r["id"] for r in records],
                      "after": observed})
        _, _, offset_raw, offset_bars = self.capture("tail-offset-before-interruption")
        offset_left = offset_bars[0][0] if offset_bars else None
        expected_offset = round(180.0 * self.unit)
        self.result["tail_close"].update({"after_order": observed,
                                          "leftmost_before_px": self.result["normalregion_leftmost_px"],
                                          "leftmost_offset_px": offset_left,
                                          "expected_offset_px": expected_offset})
        self.require("tail close created the expected positive draw offset",
                     offset_left is not None and
                     abs(offset_left - self.result["normalregion_leftmost_px"] - expected_offset) <= 2,
                     {"normal": self.result["normalregion_leftmost_px"],
                      "offset": offset_left, "expected_delta": expected_offset})
        return before, after, bars, offset_bars

    def verify_released(self, label, snapshot_name):
        snapshot = self.snapshot(snapshot_name)
        _, _, raw, bars = self.capture(label + "-after")
        left = bars[0][0] if bars else None
        normal = self.result["normalregion_leftmost_px"]
        self.result["pixel_samples"][label] = {"normalregion_leftmost_px": normal,
                                                "after_leftmost_px": left,
                                                "after_leftmost_rgb": bars[0][2] if bars else None,
                                                "remaining_order": [r["id"] for r in stable_records(snapshot)]}
        self.require(label + " restored normalregion leftmost pixel",
                     left is not None and normal is not None and abs(left - normal) <= 2,
                     self.result["pixel_samples"][label])
        return snapshot

    def stop_normally(self, dirty_target_id=None):
        self.owned()
        if dirty_target_id is not None:
            before = self.snapshot("before_explicit_dirty_discard")
            before_records = stable_records(before)
            order = [record["id"] for record in before_records]
            self.require("known dirty target is still present and dirty",
                         any(record["id"] == dirty_target_id and record["dirty"]
                             for record in before_records), before_records)
            active = int(before["active"])
            active_index = order.index(active)
            target_index = order.index(dirty_target_id)
            for _ in range((target_index - active_index) % len(order)):
                self.key(0x09, control=True)
            selected = self.wait_records(lambda snap: int(snap.get("active", 0)) == dirty_target_id,
                                         "known_dirty_target_selected")
            self.require("explicit-discard target is the active known TabId",
                         int(selected["active"]) == dirty_target_id,
                         {"target": dirty_target_id, "active": selected["active"]})
            self.key(VK_W, control=True)
            self.capture("normal-exit-explicit-discard-dialog")
            self.key(0x25)  # Cancel is initially selected; Left selects Discard.
            self.key(0x0D)
            deadline = time.time() + 8.0
            after = None
            while time.time() < deadline:
                self.owned()
                if self.manifest_path.exists():
                    try:
                        candidate = manifest(self.manifest_path)
                        candidate_order = [record["id"] for record in stable_records(candidate)]
                        if dirty_target_id not in candidate_order:
                            after = candidate
                            break
                    except (OSError, ValueError, KeyError, TypeError):
                        pass
                elif not list(self.manifest_path.parent.glob("body-*.utf8")):
                    # Discarding the sole untitled draft can leave only the
                    # default blank page, which intentionally has no manifest.
                    after = None
                    break
                time.sleep(0.05)
            after_order = ([record["id"] for record in stable_records(after)]
                           if after is not None else [])
            replacement_blank = (len(order) == 1 and after is not None and
                                 len(stable_records(after)) == 1 and
                                 not stable_records(after)[0]["path"] and
                                 not stable_records(after)[0]["dirty"] and
                                 stable_records(after)[0]["id"] != dirty_target_id)
            self.require("explicit discard removed only the known dirty TabId",
                         dirty_target_id not in after_order and
                         (after_order == [item for item in order if item != dirty_target_id]
                          or replacement_blank),
                         {"target": dirty_target_id, "before": order, "after": after_order})
            self.result["manifest_snapshots"]["after_explicit_dirty_discard"] = {
                "active": int(after.get("active", 0)) if after is not None else 0,
                "order": stable_records(after) if after is not None else [],
                "manifest_absent_clean_blank": after is None,
            }
        current = (self.snapshot("before_normal_process_close")
                   if self.manifest_path.exists() else {"records": []})
        self.require("all remaining pages are clean before process close",
                     not any(record["dirty"] for record in stable_records(current)),
                     stable_records(current))
        self.owned()
        cad.user32.PostMessageW(self.hwnd, WM_CLOSE, 0, 0)
        self.proc.wait(timeout=20)
        self.result.setdefault("exits", []).append(self.proc.returncode)
        self.require("process exited normally", self.proc.returncode == 0, self.proc.returncode)
        self.require("normal exit cleared isolated session manifest",
                     not self.manifest_path.exists(), str(self.manifest_path))

    def report(self, passed):
        self.result["passed"] = bool(passed)
        path = self.case_dir / "case.json"
        path.write_text(json.dumps(self.result, ensure_ascii=False, indent=2), encoding="utf-8")


def make_fixtures(root, count=5):
    base = root / "fixtures"
    base.mkdir(parents=True, exist_ok=True)
    files = []
    for index in range(count + 3):
        path = base / f"close-{index:02d}.md"
        path.write_text(f"# Close fixture {index}\n\nclean body {index}\n", encoding="utf-8")
        files.append(path)
    return files


def populate(run, files, dirty_first=False):
    run.start(files[0])
    initial = run.wait_records(lambda snap: len(snap.get("records", [])) == 1,
                               "one_initial_tab")
    if dirty_first:
        run.click_client_px(500, 400)
        run.key(VK_END, control=True)
        for char in "DIRTY-CANCEL-PROBE":
            run.owned()
            cad.user32.PostMessageW(run.hwnd, WM_CHAR, ord(char), 1)
        dirty_id = int(run.wait_records(
            lambda snap: len(snap.get("records", [])) == 1 and snap["records"][0].get("dirty"),
            "first_tab_dirty")["records"][0]["id"])
        run.result["dirty_target_tab_id"] = dirty_id
    else:
        dirty_id = None
    for path in files[1:5]:
        run.open_forward(path)
    snapshot = run.wait_records(lambda snap: len(snap.get("records", [])) == 5,
                                "five_fixture_tabs")
    records = stable_records(snapshot)
    run.require("fixture ids are unique", len({record["id"] for record in records}) == 5,
                [record["id"] for record in records])
    if dirty_first:
        run.require("dirty target retained its dirty state",
                    any(record["id"] == dirty_id and record["dirty"] for record in records), records)
    return dirty_id


def run_interrupt(exe, expected_hash, root, files, label, interrupt, theme):
    run = ProbeRun(exe, expected_hash, root, label, theme)
    passed = False
    try:
        dirty_id = populate(run, files, dirty_first=(interrupt == "dirty_cancel"))
        _, after_tail, _, offset_bars = run.close_clean_tail()
        order = [record["id"] for record in stable_records(after_tail)]
        width, height, _, _ = run.capture("offset-ready")
        if interrupt == "wheel":
            # Standard wheel over the tab strip maps to horizontal tab scrolling.
            current = run.cursor()
            run.owned()
            cad.user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, 120, 0)
            time.sleep(0.5)
            run.result["events"].append({"kind": "tab_strip_wheel", "delta": 120,
                                         "cursor_before": current,
                                         "cursor_after": run.cursor()})
            run.verify_released("wheel_interrupt", "after_wheel")
        elif interrupt == "resize":
            before_w, before_h, _, _ = run.client_rect()
            outer = cad.wintypes.RECT()
            cad.user32.GetWindowRect(run.hwnd, cad.ctypes.byref(outer))
            new_width = max(1200, EXPECTED_PHYSICAL[0] - 160)
            cad.user32.SetWindowPos(run.hwnd, None, outer.left, outer.top, new_width,
                                    EXPECTED_PHYSICAL[1], SWP_NOZORDER | SWP_NOACTIVATE)
            time.sleep(0.8)
            run.owned()
            after_w, after_h, _, _ = run.client_rect()
            run.result["events"].append({"kind": "resize", "client_before": [before_w, before_h],
                                         "client_after": [after_w, after_h]})
            run.require("resize changed client width", abs(after_w - before_w) >= 100,
                        [before_w, after_w])
            run.verify_released("resize_interrupt", "after_resize")
        elif interrupt in ("select_current", "select_other"):
            snapshot = run.snapshot("before_select")
            records = stable_records(snapshot)
            active = int(snapshot["active"])
            index = next(i for i, record in enumerate(records) if record["id"] == active)
            if interrupt == "select_other":
                index = (index - 1) % len(records)
            _, _, raw, bars = run.capture("before_explicit_select")
            run.require("visible bars line up with manifest ids", len(bars) == len(records),
                        {"bars": len(bars), "ids": [r["id"] for r in records]})
            chosen = records[index]["id"]
            x = bars[index][0] + round(65 * run.unit)
            run.click_client_px(x, run.bar_y)
            after = run.wait_records(lambda snap: int(snap.get("active", 0)) == chosen,
                                     "explicit_selection")
            run.result["events"].append({"kind": interrupt, "selected_tab_id": chosen,
                                         "active_before": active,
                                         "active_after": int(after["active"])})
            run.verify_released(interrupt, "after_explicit_select")
        elif interrupt == "new_open":
            new_path = files[5]
            run.open_forward(new_path)
            after = run.wait_records(lambda snap: len(snap.get("records", [])) == 5,
                                     "new_file_opened")
            run.result["events"].append({"kind": "new_open", "path": str(new_path),
                                         "active_after": int(after["active"])})
            run.verify_released("new_open_interrupt", "after_new_open")
        elif interrupt == "dirty_cancel":
            snapshot = run.snapshot("before_dirty_close")
            records = stable_records(snapshot)
            index = next(i for i, record in enumerate(records) if record["id"] == dirty_id)
            _, _, raw, bars = run.capture("before_dirty_close")
            run.require("dirty target is visible", len(bars) == len(records),
                        {"bars": len(bars), "records": records})
            x = bars[index][0] + round(CLOSE_CENTER_FROM_BAR_DIP * run.unit)
            run.click_client_px(x, run.bar_y)
            dialog_snapshot = run.wait_records(
                lambda snap: int(snap.get("active", 0)) == dirty_id and
                any(int(rec["id"]) == dirty_id and rec.get("dirty") for rec in snap.get("records", [])),
                "dirty_confirmation_open")
            run.capture("dirty-confirmation-dialog")
            run.key(VK_ESCAPE)
            after = run.wait_records(
                lambda snap: len(snap.get("records", [])) == len(records) and
                int(snap.get("active", 0)) == dirty_id and
                any(int(rec["id"]) == dirty_id and rec.get("dirty") for rec in snap.get("records", [])),
                "dirty_cancel")
            run.result["events"].append({"kind": "dirty_confirmation_cancel",
                                         "target_tab_id": dirty_id,
                                         "order_before": [r["id"] for r in records],
                                         "order_after": [r["id"] for r in stable_records(after)],
                                         "dialog_active": int(dialog_snapshot["active"])})
            run.verify_released("dirty_cancel_interrupt", "after_dirty_cancel")
        else:
            raise ValueError("unknown interruption: " + interrupt)
        run.stop_normally(dirty_id if interrupt == "dirty_cancel" else None)
        passed = all(check["passed"] for check in run.result["checks"])
        return passed
    except Exception as exc:
        run.result["exception"] = f"{type(exc).__name__}: {exc}"
        print("FAIL " + label + ": " + run.result["exception"], flush=True)
        return False
    finally:
        if run.proc is not None and run.proc.poll() is None and run.hwnd:
            if cad.user32.GetForegroundWindow() != run.hwnd:
                cad.ensure_foreground(run.hwnd)
            cad.user32.PostMessageW(run.hwnd, WM_CLOSE, 0, 0)
            try:
                run.proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                run.result["abnormal_cleanup_terminate"] = True
                run.proc.terminate()
                try:
                    run.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        run.report(passed)


def run_last_close_and_reopen(exe, expected_hash, root, files, theme):
    run = ProbeRun(exe, expected_hash, root, "last_close_blank_reopen", theme)
    passed = False
    try:
        run.start(files[0])
        before = run.wait_records(lambda snap: len(snap.get("records", [])) == 1,
                                  "one_file_before_last_close")
        old_id = int(before["records"][0]["id"])
        _, _, raw, bars = run.capture("last-file-before-close")
        run.require("one real file tab before last close", len(bars) == 1 and
                    bool(before["records"][0].get("path")),
                    {"bars": len(bars), "record": stable_records(before)})
        x = bars[0][0] + round(CLOSE_CENTER_FROM_BAR_DIP * run.unit)
        run.click_client_px(x, run.bar_y)
        blank = run.wait_records(
            lambda snap: len(snap.get("records", [])) == 1 and
            not snap["records"][0].get("path", "") and
            not snap["records"][0].get("dirty", False), "single_blank_after_last_close")
        run.capture("single-blank-after-last-close")
        run.require("last close created exactly one new blank TabId",
                    int(blank["records"][0]["id"]) != old_id and int(blank["active"]) ==
                    int(blank["records"][0]["id"]),
                    {"old_id": old_id, "blank": stable_records(blank)})
        run.stop_normally()
        run.require("normal close clears the private session manifest",
                    not run.manifest_path.exists(), str(run.manifest_path))

        run.proc = None
        run.hwnd = None
        run.start(None, expect_manifest=False)
        run.capture("blank-reopen-before-edit")
        marker = f"BLANK-REOPEN-PROBE-{run.proc.pid}"
        run.click_client_px(500, 400)
        for char in marker:
            run.owned()
            cad.user32.PostMessageW(run.hwnd, WM_CHAR, ord(char), 1)

        def only_marker_is_recorded(snapshot):
            records = snapshot.get("records", [])
            if len(records) != 1 or not records[0].get("dirty") or records[0].get("path"):
                return False
            body_name = records[0].get("body")
            if not body_name:
                return False
            body_path = run.manifest_path.parent / body_name
            return body_path.is_file() and body_path.read_text(encoding="utf-8") == marker

        edited = run.wait_records(only_marker_is_recorded, "blank_reopen_unique_marker")
        run.require("blank reopen body contains only the unique marker",
                    only_marker_is_recorded(edited), stable_records(edited))
        run.capture("blank-reopen-unique-marker")
        run.stop_normally(int(edited["records"][0]["id"]))
        run.require("reopened normal close clears manifest", not run.manifest_path.exists())
        passed = all(check["passed"] for check in run.result["checks"])
        return passed
    except Exception as exc:
        run.result["exception"] = f"{type(exc).__name__}: {exc}"
        print("FAIL last_close_blank_reopen: " + run.result["exception"], flush=True)
        return False
    finally:
        if run.proc is not None and run.proc.poll() is None and run.hwnd:
            if cad.user32.GetForegroundWindow() != run.hwnd:
                cad.ensure_foreground(run.hwnd)
            cad.user32.PostMessageW(run.hwnd, WM_CLOSE, 0, 0)
            try:
                run.proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                run.proc.terminate()
                try:
                    run.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        run.report(passed)


def main():
    parser = argparse.ArgumentParser(description="Real-window tests for close-anchor interruption behavior")
    parser.add_argument("--exe", required=True)
    parser.add_argument("--expectedhash", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--theme", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    if not exe.is_file():
        raise SystemExit(f"executable does not exist: {exe}")
    if not out.parent.exists():
        raise SystemExit(f"output parent does not exist: {out.parent}")
    actual_hash = digest(exe)
    if actual_hash.lower() != args.expectedhash.lower():
        raise SystemExit(f"SHA256 mismatch: actual={actual_hash}, expected={args.expectedhash.lower()}")
    out.mkdir(parents=False, exist_ok=False)
    cad.make_dpi_aware()
    cad.assert_unlocked("tab close interruptions")
    fixtures = make_fixtures(out)
    report = {"exe": str(exe), "sha256": actual_hash,
              "expectedhash": args.expectedhash.lower(), "theme": args.theme,
              "required_environment": {"physical_px": list(EXPECTED_PHYSICAL),
                                       "dpi": EXPECTED_DPI, "ui_scale": UI_SCALE,
                                       "effective_scale": EXPECTED_EFFECTIVE_SCALE},
              "scenarios": [], "unverified": [
                  "wheel interruption sends a real wheel event over the tab strip; the five-tab fixture fits the 2500 px normal region, so nonzero tabScroll is not asserted."]}
    scenarios = [
        ("wheel_interrupt", "wheel", False),
        ("resize_interrupt", "resize", False),
        ("select_current_interrupt", "select_current", False),
        ("select_other_interrupt", "select_other", False),
        ("new_open_interrupt", "new_open", False),
        ("dirty_cancel_interrupt", "dirty_cancel", True),
    ]
    for label, kind, dirty in scenarios:
        passed = run_interrupt(exe, actual_hash, out, fixtures, label, kind, args.theme)
        report["scenarios"].append({"name": label, "passed": passed})
    report["scenarios"].append({"name": "last_close_blank_reopen",
                                "passed": run_last_close_and_reopen(exe, actual_hash, out,
                                                                    fixtures, args.theme)})
    report["passed"] = all(item["passed"] for item in report["scenarios"])
    report_path = out / "report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"report: {report_path}", flush=True)
    print(f"SHA256: {actual_hash}", flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

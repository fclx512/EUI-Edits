"""README idle memory, visible background windows; no global mouse/key input.

Launch with SW_SHOWNOACTIVATE, keep an owned window at 1280x800, sample EX2,
and request normal WM_CLOSE. Small tabs use the existing private-file/event
open protocol directly, without the secondary launcher that activates windows.
This measures memory, not real input acceptance or input-to-display latency.
"""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone, timedelta
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import shutil
import time

import readme_memory as old
import win_capture as cad
from memory_save import Counters

ROOT = Path(__file__).resolve().parents[2]
FIELDS = {"working_set_bytes": "ws", "private_working_set_bytes": "private_ws",
          "private_commit_bytes": "private"}
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
user = ctypes.WinDLL("user32", use_last_error=True)
gdi = ctypes.WinDLL("gdi32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.OpenMutexW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
kernel.OpenMutexW.restype = wintypes.HANDLE
kernel.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
kernel.OpenEventW.restype = wintypes.HANDLE
kernel.SetEvent.argtypes = [wintypes.HANDLE]
user.GetForegroundWindow.restype = wintypes.HWND
user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
user.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
user.SetWindowLongPtrW.restype = ctypes.c_ssize_t
user.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user.GetDpiForWindow.argtypes = [wintypes.HWND]
user.GetDpiForWindow.restype = wintypes.UINT
user.IsWindowVisible.argtypes = [wintypes.HWND]
user.IsIconic.argtypes = [wintypes.HWND]
user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
psapi.GetProcessMemoryInfo.restype = wintypes.BOOL


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def owned_window(pid):
    found = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    @callback_type
    def callback(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.EnumWindows(callback, 0)
    return found[0] if len(found) == 1 else None


def geometry(hwnd, pid):
    owner = wintypes.DWORD()
    user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    assert owner.value == pid, "window ownership changed"
    outer, client = wintypes.RECT(), wintypes.RECT()
    assert user.GetWindowRect(hwnd, ctypes.byref(outer)) and user.GetClientRect(hwnd, ctypes.byref(client))
    return {"outer_pixels": [outer.right-outer.left, outer.bottom-outer.top],
            "client_pixels": [client.right, client.bottom], "dpi": user.GetDpiForWindow(hwnd),
            "foreground": user.GetForegroundWindow() == hwnd, "minimized": bool(user.IsIconic(hwnd)),
            "visible": bool(user.IsWindowVisible(hwnd))}


def responsive(hwnd):
    reply = ctypes.c_size_t()
    deadline = time.monotonic() + 90
    error = 0
    # SMTO_ABORTIFHUNG can abort after the OS hung-thread threshold even while
    # the first large layout is progressing. Retry within the declared 90s load
    # budget; a vanished HWND or an exhausted budget still fails the run.
    while time.monotonic() < deadline:
        ctypes.set_last_error(0)  # SendMessageTimeout may leave LastError unchanged.
        if user.SendMessageTimeoutW(hwnd, 0, 0, 0, 2, 1000, ctypes.byref(reply)):
            return
        error = ctypes.get_last_error()
        if error == 1400:
            break
        time.sleep(.1)
    raise OSError(error, "owned window did not respond to WM_NULL within load budget")


def keep_nonactivating(hwnd, pid):
    """A fixture-only style on the owned process; never activate another app."""
    owner = wintypes.DWORD()
    user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    assert owner.value == pid, "refusing to style an unrelated window"
    original = user.GetWindowLongPtrW(hwnd, -20)
    ctypes.set_last_error(0)
    previous = user.SetWindowLongPtrW(hwnd, -20, original | 0x08000000)
    if not previous and ctypes.get_last_error():
        raise ctypes.WinError(ctypes.get_last_error())
    assert user.GetWindowLongPtrW(hwnd, -20) & 0x08000000
    return {"no_activate": True, "original_exstyle": original,
            "scope": "owned HWND background measurement fixture; product EXE unchanged"}


def capture(hwnd, path):
    """PrintWindow to an offscreen DC, never BitBlt the user's desktop."""
    r = wintypes.RECT(); assert user.GetWindowRect(hwnd, ctypes.byref(r))
    width, height = r.right-r.left, r.bottom-r.top
    user.GetWindowDC.argtypes = [wintypes.HWND]; user.GetWindowDC.restype = wintypes.HDC
    user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    user.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
    gdi.CreateCompatibleDC.argtypes = [wintypes.HDC]; gdi.CreateCompatibleDC.restype = wintypes.HDC
    gdi.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]; gdi.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    gdi.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]; gdi.SelectObject.restype = wintypes.HGDIOBJ
    gdi.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
    gdi.DeleteObject.argtypes = [wintypes.HGDIOBJ]; gdi.DeleteDC.argtypes = [wintypes.HDC]
    dc = user.GetWindowDC(hwnd); mem = gdi.CreateCompatibleDC(dc)
    bitmap = gdi.CreateCompatibleBitmap(dc, width, height); previous = gdi.SelectObject(mem, bitmap)
    try:
        if not user.PrintWindow(hwnd, mem, 2):
            return {"status": "SKIP", "reason": "PrintWindow failed"}
        header = cad.BITMAPINFOHEADER()
        header.biSize = ctypes.sizeof(header); header.biWidth = width; header.biHeight = -height
        header.biPlanes = 1; header.biBitCount = 32
        pixels = ctypes.create_string_buffer(width*height*4)
        gdi.SelectObject(mem, previous)
        assert gdi.GetDIBits(mem, bitmap, 0, height, pixels, ctypes.byref(header), 0) == height
        cad.write_png(str(path), width, height, pixels.raw)
        return {"status": "OBSERVED", "path": path.name, "sha256": sha(path), "method": "PrintWindow; visual inspection required"}
    finally:
        gdi.SelectObject(mem, previous); gdi.DeleteObject(bitmap); gdi.DeleteDC(mem); user.ReleaseDC(hwnd, dc)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--repetitions", type=int, default=3)
    ap.add_argument("--scenario", choices=("all", "blank", "small_tabs", "plain", "markdown"), default="all")
    ap.add_argument("--document", help="Copy and measure one source-bound text fixture instead of README scenes")
    ap.add_argument("--compare-exe", help="Frozen baseline EXE; custom document uses three ABBA batches by default")
    args = ap.parse_args()
    assert ctypes.sizeof(ctypes.c_void_p) == 8 and args.repetitions > 0
    cad.make_dpi_aware()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    assert exe.is_file(); out.mkdir(parents=True, exist_ok=False)
    baseline = Path(args.compare_exe).resolve() if args.compare_exe else None
    assert not baseline or (baseline.is_file() and args.document), "comparison requires a custom document"
    documents = old.fixtures(out/"fixtures")
    if args.document:
        source = Path(args.document).resolve()
        destination = out/"fixtures"/source.name
        assert not destination.exists(), "custom fixture name collides with README fixtures"
        shutil.copy2(source, destination)
        assert sha(source) == sha(destination)
        documents = {"custom": [destination]}
    settings = ("ui_language=zh-CN\ntheme=1\neditor_font_size=16\nui_font_size=14\n"
                "ui_scale=1\nmode=0\nline_numbers=1\nreadable_width=1\nshow_status_bar=1\nanimations=1\n")
    report = {"date": datetime.now(timezone(timedelta(hours=8))).isoformat(), "exe": str(exe), "sha256": sha(exe),
              "machine": old.machine(), "settings_ini": settings, "renderer": "default software D2D / DC; NEO_* stripped",
              "scope": "visible background window; owned HWND has WS_EX_NOACTIVATE; no global mouse/keyboard or activation; not interactive acceptance",
              "sampling": {"repetitions": args.repetitions, "settle_seconds": 10, "samples_per_launch": 20,
                           "interval_seconds": .5, "api": "PROCESS_MEMORY_COUNTERS_EX2", "working_set_trimmed": False},
              "documents": {name: [{"name": p.name, "bytes": p.stat().st_size, "lines": len(p.read_bytes().splitlines()), "sha256": sha(p)} for p in files] for name, files in documents.items()},
              "runs": [], "measurement_status": "OBSERVED", "correctness_status": "NOT_RUN", "complete": False}
    if baseline:
        report["comparison"] = {"baseline_exe": str(baseline), "baseline_sha256": sha(baseline),
                                "candidate_exe": str(exe), "candidate_sha256": sha(exe),
                                "order": "ABBA per batch; baseline,candidate,candidate,baseline",
                                "batches": args.repetitions}
    def persist():
        temporary = out/"report.tmp.json"
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(out/"report.json")
    persist()
    scenarios = ["custom"] if args.document else (["blank", "small_tabs", "plain", "markdown"] if args.scenario == "all" else [args.scenario])
    for repeat in range(1, args.repetitions+1):
        order = scenarios if repeat % 2 or args.document else sorted(scenarios, key=("blank", "small_tabs", "markdown", "plain").index)
        sequence = [(scenario, "current", exe, 0) for scenario in order] if not baseline else [
            ("custom", role, target, position) for position,(role,target) in enumerate([
                ("baseline", baseline), ("candidate", exe), ("candidate", exe), ("baseline", baseline)])]
        for scenario, role, launch_exe, position in sequence:
            mutex = kernel.OpenMutexW(0x100000, False, "Local\\EUI-Edits.SingleInstance")
            if mutex:
                kernel.CloseHandle(mutex); raise RuntimeError("another EUI-Edits owns the singleton; left untouched")
            run_name = f"{scenario}-{repeat}" if not baseline else f"{role}-{repeat}-{position}"
            run_dir = out/run_name; run_dir.mkdir()
            profile = run_dir/"appdata"/"EUI-Edits"; profile.mkdir(parents=True)
            temp = run_dir/"temp"; temp.mkdir()
            (profile/"settings.ini").write_text(settings, encoding="utf-8")
            env = {k:v for k,v in os.environ.items() if not k.startswith("NEO_")}
            env.update(APPDATA=str(profile.parent), LOCALAPPDATA=str(profile.parent), TEMP=str(temp), TMP=str(temp))
            startup = subprocess.STARTUPINFO(); startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 4
            run = {"scenario": scenario, "repeat": repeat, "samples": [], "status": "NOT_RUN", "evidence_directory": run_dir.name}
            if baseline: run.update(role=role, position=position, exe_sha256=sha(launch_exe))
            report["runs"].append(run); persist()
            handle = hwnd = proc = None
            try:
                with (run_dir/"app.log").open("wb") as log:
                    files = documents[scenario]
                    proc = subprocess.Popen([str(launch_exe)] + [str(p) for p in files[:1]], cwd=launch_exe.parent, env=env, stdout=log, stderr=log, startupinfo=startup)
                    run["pid"] = proc.pid
                    deadline = time.monotonic()+90
                    while time.monotonic()<deadline and proc.poll() is None:
                        hwnd = owned_window(proc.pid)
                        if hwnd: break
                        time.sleep(.1)
                    assert hwnd and proc.poll() is None, "owned window unavailable"
                    run["window_id"] = hwnd
                    assert not geometry(hwnd, proc.pid)["foreground"], "startup activated the window"
                    run["window_fixture"] = keep_nonactivating(hwnd, proc.pid)
                    # Keep the owned window beneath other applications, so normal
                    # desktop clicks cannot hit a newly exposed test window.
                    assert user.SetWindowPos(hwnd, wintypes.HWND(1), 80, 80, 1280, 800, 0x10), "background sizing failed"
                    responsive(hwnd)
                    for path in files[1:]:
                        # Same source-bound request channel as single-instance open,
                        # but skip the launcher's activation of the existing window.
                        target = temp/"EUI-Edits.next-open"; pending = temp/"open.tmp"
                        pending.write_text(str(path), encoding="utf-8"); pending.replace(target)
                        event = kernel.OpenEventW(2, False, "Local\\EUI-Edits.OpenRequest")
                        assert event, "open event unavailable"
                        try: assert kernel.SetEvent(event)
                        finally: kernel.CloseHandle(event)
                        deadline = time.monotonic()+10
                        while target.exists() and time.monotonic()<deadline: time.sleep(.1)
                        assert not target.exists(), "open request not consumed"
                        time.sleep(1); responsive(hwnd)
                    time.sleep(10); responsive(hwnd)
                    initial = geometry(hwnd, proc.pid); run["geometry"] = initial
                    assert initial["outer_pixels"] == [1280,800] and initial["dpi"] == 120
                    assert initial["client_pixels"] == [1262,753], "client differs from historical fixture"
                    handle = kernel.OpenProcess(0x410, False, proc.pid); assert handle
                    started = time.monotonic()
                    for i in range(20):
                        assert proc.poll() is None
                        geo = geometry(hwnd, proc.pid)
                        if not (geo == initial and geo["visible"] and not geo["minimized"] and not geo["foreground"]):
                            run["failure_geometry"] = geo
                            raise RuntimeError("window state drift")
                        assert not cad.direct_child_process_ids(proc.pid), "unexpected child process"
                        counters = Counters(); counters.cb = ctypes.sizeof(counters)
                        assert psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
                        assert 0 < counters.private_ws <= counters.ws and counters.private > 0
                        run["samples"].append({"seconds": round(time.monotonic()-started,3), **{key:getattr(counters, member) for key,member in FIELDS.items()}})
                        time.sleep(.5)
                    run["medians"] = {key:statistics.median(row[key] for row in run["samples"]) for key in FIELDS}
                    run["screenshot"] = capture(hwnd, run_dir/"window.png")
                    assert user.PostMessageW(hwnd, 0x10, 0, 0)
                    run["exit_code"] = proc.wait(timeout=30)
                    assert run["exit_code"] == 0, "normal exit failed"
                    run["status"] = "PASS"
            except Exception as exc:
                run["status"] = "FAIL"; run["error"] = repr(exc)
                run["process_exit_code_at_failure"] = proc.poll() if proc else None
                persist()
                # Do not terminate an interfered-with process; retain it for inspection.
                raise
            finally:
                if handle: kernel.CloseHandle(handle)
                persist()
            print(scenario, repeat, role, position, {key:round(value/1048576,2) for key,value in run["medians"].items()}, flush=True)
    assert sha(exe) == report["sha256"], "EXE drift"
    if baseline: assert sha(baseline) == report["comparison"]["baseline_sha256"], "baseline EXE drift"
    for items in report["documents"].values():
        for item in items: assert sha(out/"fixtures"/item["name"]) == item["sha256"]
    report["summary"] = {}
    for group in ("baseline", "candidate") if baseline else scenarios:
        runs = [r for r in report["runs"] if (r.get("role") == group if baseline else r["scenario"] == group) and r["status"] == "PASS"]
        assert len(runs) == args.repetitions * (2 if baseline else 1)
        report["summary"][group] = {key:{"median_bytes":statistics.median(r["medians"][key] for r in runs), "min_bytes":min(s[key] for r in runs for s in r["samples"]), "max_bytes":max(s[key] for r in runs for s in r["samples"])} for key in FIELDS}
    report["complete"] = True; report["correctness_status"] = "PASS"; persist()
    print("COMPLETE", str(out/"report.json"), flush=True)


if __name__ == "__main__":
    main()

"""Measure the released EXE for README data, without trimming its working set.

Uses a fresh profile for every launch, an owned foreground window, PSAPI EX2
byte counters, retained fixtures/screenshots, and normal WM_CLOSE exit.
"""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

import win_capture as cad
from capture_markdown import window_for_pid
from memory_save import Counters

ROOT = Path(__file__).resolve().parents[2]
FIELDS = {"working_set_bytes": "ws", "private_working_set_bytes": "private_ws",
          "private_commit_bytes": "private"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixtures(directory):
    directory.mkdir()
    paths = {}
    # Exact size and line count: 9 MiB / 128 bytes = 73,728 LF-ended lines.
    for name, prefix in (("plain", "Text row: "),
                         ("markdown", "- **bold** and `code`: ")):
        row = (prefix + "a" * (127 - len(prefix)) + "\n").encode("ascii")
        path = directory / ("large.txt" if name == "plain" else "large.md")
        path.write_bytes(row * (9 * 1024 * 1024 // 128))
        paths[name] = [path]
    paths["small_tabs"] = []
    for number in range(1, 4):
        path = directory / f"small-{number}.md"
        text = (f"# 文稿 {number}\n\n轻量编辑器的内存测量。\n\n"
                "- [x] 标题与列表\n- [ ] 下一步\n\n> 引用内容\n\n"
                "```cpp\nint main() { return 0; }\n```\n\n"
                "| 操作 | 说明 |\n| --- | --- |\n| 标签 | 切换文稿 |\n\n"
                + "正文包含 **强调**、`代码` 和普通文字。\n\n" * 12)
        path.write_bytes(text.encode("utf-8"))
        paths["small_tabs"].append(path)
    paths["blank"] = []
    return paths


def machine():
    command = "@{os=(Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber);cpu=(Get-CimInstance Win32_Processor | Select-Object Name);gpu=(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion);ram_bytes=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory} | ConvertTo-Json -Depth 4 -Compress"
    result = subprocess.run(["powershell", "-NoProfile", "-Command", command],
                            capture_output=True, check=True)
    # The JSON values that matter for measurement are ASCII; preserve OS text too.
    return json.loads(result.stdout.decode("utf-8", errors="replace"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--out", required=True, help="New evidence directory")
    parser.add_argument("--resume", action="store_true", help="Resume an incomplete report; keep invalid attempts")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--settle", type=float, default=10)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--interval", type=float, default=.5)
    parser.add_argument("--scenario", choices=("all", "blank", "small_tabs", "plain", "markdown"), default="all")
    args = parser.parse_args()
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        parser.error("Use 64-bit Python for this EX2 probe")
    if args.repetitions < 1 or args.samples < 2 or args.settle < 0 or args.interval <= 0:
        parser.error("Invalid sampling parameters")
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    if not exe.is_file():
        parser.error("EXE does not exist")
    cad.make_dpi_aware()
    cad.assert_unlocked("README memory")
    cad.assert_no_foreign_instance("README memory")
    previous = None
    if args.resume:
        previous = json.loads((out / "report.json").read_text(encoding="utf-8"))
        if previous["sha256"] != digest(exe):
            parser.error("Resume EXE hash differs")
        for field, expected in (("repetitions", args.repetitions), ("settle_seconds", args.settle),
                                ("samples_per_launch", args.samples), ("interval_seconds", args.interval)):
            if previous["sampling"][field] != expected:
                parser.error("Resume sampling parameters differ")
        documents = {name: [out / "fixtures" / item["name"] for item in items]
                     for name, items in previous["documents"].items()}
        for items in previous["documents"].values():
            for item in items:
                if digest(out / "fixtures" / item["name"]) != item["sha256"]:
                    parser.error("Resume fixture changed")
    else:
        out.mkdir(parents=True, exist_ok=False)
        documents = fixtures(out / "fixtures")
    scenarios = list(documents) if args.scenario == "all" else [args.scenario]
    # Small scenarios first; alternate the large pair across repetitions.
    scenarios.sort(key=("blank", "small_tabs", "plain", "markdown").index)
    settings = ("ui_language=zh-CN\ntheme=1\neditor_font_size=16\nui_font_size=14\n"
                "ui_scale=1\nmode=0\nline_numbers=1\nreadable_width=1\n"
                "show_status_bar=1\nanimations=1\n")
    report = {"date": datetime.now(timezone(timedelta(hours=8))).isoformat(),
              "exe": str(exe), "sha256": digest(exe), "machine": machine(),
              "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "settings_ini": settings, "renderer": "Win32/Direct2D default (software + DC per source); no NEO_* overrides",
              "sampling": {"repetitions": args.repetitions, "settle_seconds": args.settle,
                           "samples_per_launch": args.samples, "interval_seconds": args.interval,
                           "statistic": "median of per-launch medians; range over all retained idle samples",
                           "api": "GetProcessMemoryInfo / PROCESS_MEMORY_COUNTERS_EX2",
                           "unit": "bytes; divide by 1048576 for MiB", "working_set_trimmed": False},
              "documents": {}, "runs": []}
    for name, files in documents.items():
        report["documents"][name] = [{"name": file.name, "bytes": file.stat().st_size,
            "lines": len(file.read_bytes().splitlines()), "sha256": digest(file)} for file in files]
    if previous:
        report = previous
        report.setdefault("resume_dates", []).append(datetime.now(timezone(timedelta(hours=8))).isoformat())

    def valid(run):
        return not run.get("error") and run.get("exit_code") == 0 and len(run["samples"]) == args.samples

    def persist():
        (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    cad.user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    cad.user32.GetDpiForWindow.argtypes = [wintypes.HWND]
    cad.user32.GetDpiForWindow.restype = wintypes.UINT
    cad.user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    cad.user32.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
        wintypes.LPARAM, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
    persist()
    for repeat in range(args.repetitions):
        ordered = scenarios if repeat % 2 == 0 else sorted(scenarios, key=("blank", "small_tabs", "markdown", "plain").index)
        for name in ordered:
            if any(valid(run) and run["scenario"] == name and run["repeat"] == repeat + 1 for run in report["runs"]):
                continue
            cad.assert_no_foreign_instance("next README launch")
            run_dir = out / f"{name}-{repeat + 1}"
            if run_dir.exists():
                run_dir = out / f"{name}-{repeat + 1}-retry-{len(report['runs'])}"
            run_dir.mkdir()
            appdata, temp = run_dir / "appdata", run_dir / "temp"
            profile = appdata / "EUI-Edits"
            profile.mkdir(parents=True)
            temp.mkdir()
            (profile / "settings.ini").write_text(settings, encoding="utf-8")
            env = {k: v for k, v in os.environ.items() if not k.startswith("NEO_")}
            env.update(APPDATA=str(appdata), TEMP=str(temp), TMP=str(temp))
            files = documents[name]
            run = {"scenario": name, "repeat": repeat + 1, "samples": [], "evidence_directory": run_dir.name}
            report["runs"].append(run)
            hwnd = handle = None
            proc = None
            try:
                with (run_dir / "app.log").open("w", encoding="utf-8") as log:
                    proc = subprocess.Popen([str(exe)] + ([str(files[0])] if files else []),
                        cwd=exe.parent, env=env, stdout=log, stderr=log)
                    run["pid"] = proc.pid
                    deadline = time.monotonic() + 90
                    while time.monotonic() < deadline:
                        hwnd = window_for_pid(proc.pid)
                        if hwnd or proc.poll() is not None:
                            break
                        time.sleep(.1)
                    if not hwnd or proc.poll() is not None:
                        raise RuntimeError("Owned window did not start")
                    if not cad.ensure_foreground(hwnd):
                        raise RuntimeError("Foreground unavailable")
                    # One fixed window size; no resize stress or scrolling.
                    cad.user32.SetWindowPos(hwnd, None, 80, 80, 1280, 800, 4)
                    def owned():
                        cad.assert_unlocked("README memory sampling")
                        owner = wintypes.DWORD()
                        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                        if proc.poll() is not None or owner.value != proc.pid or cad.user32.GetForegroundWindow() != hwnd:
                            raise RuntimeError("Owned foreground lost")

                    for file in files[1:]:
                        forwarded = subprocess.run([str(exe), str(file)], cwd=exe.parent, env=env, timeout=30)
                        if forwarded.returncode != 0:
                            raise RuntimeError("Document forwarding failed")
                        time.sleep(1)
                    if name == "small_tabs":
                        # Visit each tab so the scenario includes retained presentation caches.
                        for number in (1, 2, 3):
                            owned()
                            for key in (0x11, 0x30 + number):
                                cad.user32.keybd_event(key, 0, 0, 0)
                            for key in (0x30 + number, 0x11):
                                cad.user32.keybd_event(key, 0, 2, 0)
                            time.sleep(.5)
                    owned()
                    reply = ctypes.c_size_t()
                    if not cad.user32.SendMessageTimeoutW(hwnd, 0, 0, 0, 2, 90000, ctypes.byref(reply)):
                        raise RuntimeError("Window did not respond before settling")
                    time.sleep(args.settle)
                    owned()
                    run["dpi"] = cad.user32.GetDpiForWindow(hwnd)
                    rect = wintypes.RECT()
                    cad.user32.GetClientRect(hwnd, ctypes.byref(rect))
                    run["client_pixels"] = [rect.right, rect.bottom]
                    title = ctypes.create_unicode_buffer(512)
                    cad.user32.GetWindowTextW(hwnd, title, len(title))
                    run["title"] = title.value
                    handle = kernel.OpenProcess(0x410, False, proc.pid)
                    if not handle:
                        raise ctypes.WinError(ctypes.get_last_error())
                    started = time.monotonic()
                    for _ in range(args.samples):
                        owned()
                        if cad.direct_child_process_ids(proc.pid):
                            raise RuntimeError("Expected one app process; found children")
                        counters = Counters()
                        counters.cb = ctypes.sizeof(counters)
                        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                            raise ctypes.WinError(ctypes.get_last_error())
                        if not (0 < counters.private_ws <= counters.ws and counters.private > 0):
                            raise RuntimeError("Invalid EX2 memory counters")
                        run["samples"].append({"seconds": round(time.monotonic() - started, 3),
                            **{field: getattr(counters, member) for field, member in FIELDS.items()}})
                        time.sleep(args.interval)
                    run["process_count"] = 1
                    run["medians"] = {field: statistics.median(row[field] for row in run["samples"]) for field in FIELDS}
                    owned()
                    width, height, pixels = cad.capture_client(hwnd)
                    cad.write_png(str(run_dir / "window.png"), width, height, pixels)
            except Exception as error:
                run["error"] = repr(error)
                raise
            finally:
                if handle:
                    kernel.CloseHandle(handle)
                if hwnd and proc and proc.poll() is None:
                    cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    try:
                        run["exit_code"] = proc.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        run["exit_error"] = "WM_CLOSE did not complete; owned process left for inspection"
                persist()
            if run.get("exit_code") != 0:
                raise RuntimeError("Normal exit failed")
            print(name, repeat + 1, {field.removesuffix("_bytes") + "_mib": round(value / 1048576, 2)
                  for field, value in run["medians"].items()}, flush=True)
    if digest(exe) != report["sha256"]:
        raise RuntimeError("EXE changed during measurement")
    report["summary"] = {}
    for name in scenarios:
        runs = [run for run in report["runs"] if run["scenario"] == name and valid(run)]
        if len(runs) != args.repetitions:
            raise RuntimeError("Incomplete repetitions")
        report["summary"][name] = {field: {
            "median_bytes": statistics.median(run["medians"][field] for run in runs),
            "min_bytes": min(row[field] for run in runs for row in run["samples"]),
            "max_bytes": max(row[field] for run in runs for row in run["samples"])} for field in FIELDS}
    report["complete"] = True
    persist()
    print("COMPLETE", str(out / "report.json"), flush=True)


if __name__ == "__main__":
    main()

"""Owned-window scrolling/resize memory trace; no edits, trimming or process killing."""
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import tempfile
import threading
import time
from ctypes import wintypes

import win_capture as cad
from capture_markdown import window_for_pid, write_settings


class Counters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ("peak_ws", "ws", "peak_paged", "paged",
                                            "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile", "private")]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--doc", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--software", default="1", choices=("0", "1"))
    parser.add_argument("--live-resize", default="0", choices=("0", "1"))
    parser.add_argument("--drag", action="store_true")
    args = parser.parse_args()
    exe, doc, out = map(os.path.abspath, (args.exe, args.doc, args.out))
    cad.make_dpi_aware()
    cad.assert_unlocked("interactive memory")
    cad.assert_no_foreign_instance("interactive memory")
    os.makedirs(out, exist_ok=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    samples, stages = [], []
    stop = threading.Event()
    with tempfile.TemporaryDirectory(prefix="neo-interaction-") as runtime:
        write_settings(os.path.join(runtime, "EUI-Edits", "settings.ini"), doc, 0, 16, "1")
        env = dict(os.environ, APPDATA=runtime, NEO_D2D_SOFTWARE=args.software, NEO_LIVE_RESIZE=args.live_resize)
        with open(os.path.join(out, "renderer.log"), "w", encoding="utf-8") as log:
            process = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env, stderr=log)
            handle = kernel.OpenProcess(0x0410, False, process.pid)
            if not handle:
                raise RuntimeError("cannot sample owned process")
            started = time.monotonic()

            def sampler():
                while not stop.is_set():
                    c = Counters()
                    c.cb = ctypes.sizeof(c)
                    if psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb):
                        samples.append(dict(seconds=round(time.monotonic()-started, 3),
                                            ws_mib=c.ws/1048576, private_mib=c.private/1048576,
                                            process_peak_ws_mib=c.peak_ws/1048576))
                    stop.wait(.1)

            thread = threading.Thread(target=sampler)
            thread.start()
            hwnd = None

            def owned():
                cad.assert_unlocked("interactive memory")
                owner = wintypes.DWORD()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                if process.poll() is not None or owner.value != process.pid or not cad.ensure_foreground(hwnd):
                    raise RuntimeError("owned window unavailable")

            def stage(name, wait=1):
                owned()
                time.sleep(wait)
                stages.append(dict(name=name, **samples[-1]))

            def screenshot(name):
                owned()
                w, h, pixels = cad.capture_client(hwnd)
                cad.write_png(os.path.join(out, name+".png"), w, h, pixels)

            try:
                for _ in range(150):
                    hwnd = window_for_pid(process.pid)
                    if hwnd or process.poll() is not None:
                        break
                    time.sleep(.1)
                if not hwnd:
                    raise RuntimeError("window missing")
                owned()
                client, rect = wintypes.RECT(), wintypes.RECT()
                cad.user32.GetClientRect(hwnd, ctypes.byref(client))
                cad.user32.GetWindowRect(hwnd, ctypes.byref(rect))
                point = wintypes.POINT(client.right//2, client.bottom//2)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                cad.user32.SetCursorPos(point.x, point.y)
                stage("idle", 3)
                screenshot("before")
                for cycle in range(3):
                    for direction in (-1, 1):
                        owned()
                        for _ in range(60):
                            cad.user32.mouse_event(0x0800, 0, 0, (direction*120) & 0xffffffff, 0)
                            time.sleep(.025)
                    stage(f"scroll-{cycle+1}")
                screenshot("after-scroll")
                for cycle in range(3):
                    for n in range(24):
                        owned()
                        width = 800 + (n % 12)*65
                        height = 650 + (n % 8)*25
                        cad.user32.SetWindowPos(hwnd, None, 0, 0, width, height, 0x0002 | 0x0004)
                        time.sleep(.06)
                    stage(f"resize-{cycle+1}")
                cad.user32.SetWindowPos(hwnd, None, rect.left, rect.top, rect.right-rect.left,
                                        rect.bottom-rect.top, 0x0004)
                stage("restored", 2)
                if args.drag:
                    owned()
                    cad.user32.GetWindowRect(hwnd, ctypes.byref(rect))
                    # Observed real HWND right sizing border; mouse is released even on failure.
                    x, y = rect.right-2, (rect.top+rect.bottom)//2
                    cad.user32.SetCursorPos(x, y)
                    cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
                    try:
                        for n in range(80):
                            owner = wintypes.DWORD()
                            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                            if owner.value != process.pid or cad.user32.GetForegroundWindow() != hwnd:
                                raise RuntimeError("lost ownership during drag")
                            cad.user32.SetCursorPos(x-int(280*abs((n%40)-20)/20), y)
                            time.sleep(.05)
                    finally:
                        cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                    stage("dragged", 2)
                stage("settled", 4)
                screenshot("after")
            finally:
                stop.set()
                thread.join()
                kernel.CloseHandle(handle)
                if process.poll() is None and hwnd:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                    try:
                        process.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        raise RuntimeError(f"probe PID {process.pid} did not close; left for inspection")
                with open(exe, "rb") as source:
                    sha = hashlib.file_digest(source, "sha256").hexdigest()
                metadata = dict(exe=exe, sha256=sha, doc=doc, bytes=os.path.getsize(doc),
                                software=args.software, live_resize=args.live_resize, drag=args.drag,
                                exit_code=process.returncode, stages=stages, samples=samples,
                                max_sample_ws_mib=max(s["ws_mib"] for s in samples),
                                max_sample_private_mib=max(s["private_mib"] for s in samples),
                                peak_ws_mib=max(s["process_peak_ws_mib"] for s in samples))
                with open(os.path.join(out, "trace.json"), "w", encoding="utf-8") as target:
                    json.dump(metadata, target, ensure_ascii=False, indent=2)
    print(json.dumps({k: metadata[k] for k in ("exe", "bytes", "exit_code", "peak_ws_mib", "max_sample_private_mib", "stages")}, ensure_ascii=False))


if __name__ == "__main__":
    main()

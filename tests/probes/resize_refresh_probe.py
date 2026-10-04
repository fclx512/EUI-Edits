"""Owned HWND border drags, final-size/CPU/pixel evidence and optional internal trace.

Captures are desktop samples, not a claim of display scanout refresh rate.
No user file edits, work-set trimming, clipboard writes, or process termination.
"""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import time
from ctypes import wintypes

import win_capture as cad
from capture_markdown import window_for_pid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--doc', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--animations', type=int, default=0)
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--motion-ms', type=float, default=12)
    args = ap.parse_args()
    if args.seconds <= 0 or args.motion_ms <= 0:
        ap.error('seconds and motion-ms must be positive')
    exe, out, source = (Path(p).resolve() for p in (args.exe, args.out, args.doc))
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('resize refresh')
    cad.assert_no_foreign_instance('resize refresh')
    captures, sizes, stages = [], [], []
    display = {}
    idle_cpu = {}
    process = None
    hwnd = None
    with tempfile.TemporaryDirectory(prefix='neo-resize-') as temp:
        doc = Path(temp) / source.name
        doc.write_bytes(source.read_bytes())
        initial_hash = hashlib.sha256(doc.read_bytes()).hexdigest()
        settings = Path(temp) / 'EUI-Edits/settings.ini'
        settings.parent.mkdir()
        values = dict(vault='', last_file=str(doc), mode=1, theme=args.theme,
                      editor_font_size=16, ui_scale=1, line_numbers=1,
                      readable_width=1, show_status_bar=1, animations=args.animations)
        settings.write_text(''.join(f'{k}={v}\n' for k, v in values.items()), encoding='utf-8')
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1',
                   NEO_LIVE_RESIZE='1', NEO_RESIZE_TRACE='1', NEO_GPU_STATS='0')
        log_path = out / 'renderer.log'
        with log_path.open('w', encoding='utf-8') as log:
            process = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stderr=log)

            def cpu_seconds():
                times = [wintypes.FILETIME() for _ in range(4)]
                get_times = ctypes.windll.kernel32.GetProcessTimes
                get_times.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
                if not get_times(int(process._handle), *(ctypes.byref(t) for t in times)):
                    raise ctypes.WinError()
                return sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in times[2:]) / 1e7

            def owned():
                cad.assert_unlocked('resize refresh')
                owner = wintypes.DWORD()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                if process.poll() is not None or owner.value != process.pid:
                    raise RuntimeError('owned process/window unavailable')
                if cad.user32.GetForegroundWindow() != hwnd and not cad.ensure_foreground(hwnd):
                    raise RuntimeError('owned foreground unavailable')

            def client():
                r = wintypes.RECT()
                cad.user32.GetClientRect(hwnd, ctypes.byref(r))
                return [r.right, r.bottom]

            def shot(label):
                nonlocal display
                owned()
                class MonitorInfo(ctypes.Structure):
                    _fields_ = [('cbSize', wintypes.DWORD), ('monitor', wintypes.RECT),
                                ('work', wintypes.RECT), ('flags', wintypes.DWORD),
                                ('device', wintypes.WCHAR*32)]
                cad.user32.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
                cad.user32.MonitorFromWindow.restype = wintypes.HANDLE
                monitor = MonitorInfo()
                monitor.cbSize = ctypes.sizeof(monitor)
                mode = ctypes.create_string_buffer(220)  # DEVMODEW, dmSize at byte 68
                ctypes.c_ushort.from_buffer(mode,68).value = 220
                cad.user32.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
                cad.user32.EnumDisplaySettingsW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_void_p]
                if cad.user32.GetMonitorInfoW(cad.user32.MonitorFromWindow(hwnd,2),ctypes.byref(monitor)) and \
                   cad.user32.EnumDisplaySettingsW(monitor.device,0xffffffff,mode):
                    display = dict(device=monitor.device,
                                   refresh_hz=ctypes.c_uint.from_buffer(mode,184).value,
                                   dpi=cad.user32.GetDpiForWindow(hwnd))
                w, h, pixels = cad.capture_client(hwnd)
                cad.write_png(str(out / (label + '.png')), w, h, pixels)
                # Sample the newly exposed outer strips; pure-black samples are
                # retained for review, not automatically classified as a bug.
                black = total = 0
                for y in range(8, h-8, 4):
                    for x in range(max(8, w-40), w-8, 4):
                        i = (y*w+x)*4
                        black += max(pixels[i:i+3]) <= 2
                        total += 1
                captures.append(dict(label=label, client=[w,h], black_ratio=black/max(1,total)))

            try:
                for _ in range(150):
                    hwnd = window_for_pid(process.pid)
                    if hwnd or process.poll() is not None:
                        break
                    time.sleep(.1)
                if not hwnd:
                    raise RuntimeError('window missing')
                owned()
                cad.user32.SetWindowPos(hwnd, None, 80, 50, 1100, 760, 0x0004)
                time.sleep(2)
                shot('before')
                # True mouse resizing enters Windows' native sizing modal loop.
                for direction in ('right', 'bottom', 'corner'):
                    owned()
                    r = wintypes.RECT()
                    cad.user32.GetWindowRect(hwnd, ctypes.byref(r))
                    x = r.right-2 if direction != 'bottom' else (r.left+r.right)//2
                    y = r.bottom-2 if direction != 'right' else (r.top+r.bottom)//2
                    cad.user32.SetCursorPos(x,y)
                    time.sleep(.08)
                    started = time.monotonic()
                    cad.user32.mouse_event(0x0002,0,0,0,0)
                    try:
                        n = 0
                        while time.monotonic()-started < args.seconds/3:
                            owned()
                            phase = (time.monotonic()-started)*2*math.pi/2.5
                            dx = round(140*math.sin(phase)) if direction != 'bottom' else 0
                            dy = round(90*math.sin(phase)) if direction != 'right' else 0
                            cad.user32.SetCursorPos(x+dx,y+dy)
                            sizes.append(dict(direction=direction, seconds=time.monotonic()-started, client=client()))
                            if n % 100 == 35:
                                shot(f'{direction}-{n}')
                            n += 1
                            time.sleep(args.motion_ms/1000)
                    finally:
                        cad.user32.mouse_event(0x0004,0,0,0,0)
                    immediate = client()
                    time.sleep(.2)
                    after_200ms = client()
                    time.sleep(.2)
                    stages.append(dict(direction=direction, release_request=immediate,
                                       after_200ms=after_200ms, settled=client()))
                    shot(direction+'-released')
                idle_started = time.monotonic()
                cpu_started = cpu_seconds()
                time.sleep(2)
                elapsed = time.monotonic() - idle_started
                cpu_elapsed = cpu_seconds() - cpu_started
                idle_cpu = dict(seconds=elapsed, cpu_seconds=cpu_elapsed,
                                percent_one_core=cpu_elapsed/elapsed*100)
                shot('after')
            finally:
                if process and process.poll() is None and hwnd:
                    cad.user32.PostMessageW(hwnd,0x0010,0,0)
                    process.wait(timeout=12)
        trace = None
        for line in log_path.read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('[resize-trace] '):
                trace = json.loads(line[len('[resize-trace] '):])
        metrics = {}
        if trace:
            events = trace['events']
            for kind in ('update','draw','bind','cache-allocate','end-draw'):
                durations = sorted(e[2] for e in events if e[0] == kind)
                if durations:
                    metrics[kind] = dict(count=len(durations), **{
                        f'p{p}_ms': durations[min(len(durations)-1, math.ceil(len(durations)*p/100)-1)]
                        for p in (50,95,99)})
            metrics['present_count'] = sum(e[0] == 'present' for e in events)
            metrics['cover_count'] = sum(e[0] == 'cover' for e in events)
            presents = [e for e in events if e[0] == 'present']
            metrics['last_present_size'] = presents[-1][3:5] if presents else None
            drag_metrics = []
            entered = None
            for event in events:
                if event[0] == 'drag-enter':
                    entered = event[1]
                elif event[0] == 'drag-exit' and entered is not None:
                    times = [p[1] for p in presents if entered <= p[1] <= event[1]]
                    gaps = sorted(b-a for a,b in zip(times,times[1:]))
                    after = next((p[1]-event[1] for p in presents if p[1] >= event[1]), None)
                    drag_metrics.append(dict(duration_ms=event[1]-entered, presents=len(times),
                        internal_presents_per_second=len(times)*1000/max(1,event[1]-entered),
                        interval_p50_ms=gaps[len(gaps)//2] if gaps else None,
                        interval_p95_ms=gaps[min(len(gaps)-1,math.ceil(len(gaps)*.95)-1)] if gaps else None,
                        final_frame_delay_ms=after))
                    entered = None
            metrics['drags'] = drag_metrics
            (out/'trace.json').write_text(json.dumps(trace, indent=2), encoding='utf-8')
        result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                      pid=process.pid, exit_code=process.returncode, theme=args.theme,
                      animations=args.animations, seconds=args.seconds, motion_ms=args.motion_ms, captures=captures,
                      stages=stages, sizes=sizes, metrics=metrics,
                      document_unchanged=hashlib.sha256(doc.read_bytes()).hexdigest()==initial_hash,
                      display=display, idle_cpu=idle_cpu,
                      display_sampling='desktop screenshots, not scanout fps')
        (out/'conditions.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        assert result['exit_code'] == 0 and result['document_unchanged']
        # mouse_event queues delivery. A query made immediately after it may
        # precede the last native size message; check stability after delivery.
        assert all(s['after_200ms']==s['settled'] for s in stages)
        if trace:
            assert trace['dropped']==0
            assert metrics['last_present_size']==stages[-1]['settled']
        print(json.dumps({k:result[k] for k in ('sha256','exit_code','theme','animations','metrics','stages')}, ensure_ascii=False))


if __name__ == '__main__':
    main()

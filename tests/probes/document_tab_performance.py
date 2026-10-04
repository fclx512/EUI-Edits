"""Tab switch performance probe (visible-latency + same-PID memory), isolated per run.

Measures, for a given EXE, the time from a Ctrl+Tab keystroke injection until the
client area repaint *settles* on a different frame (two consecutive identical
captures that differ from the pre-switch frame). That is an upper bound on visible
latency: it includes capture cost and the poll interval, and is never a substitute
for the app-side trace (parsed separately when the build supports NEO_TABS_TRACE).

Never drives a foreign instance: NEO_SINGLE_INSTANCE=0 + private APPDATA/TEMP.
"""
import argparse, ctypes, hashlib, json, os, statistics, subprocess, threading, time
from pathlib import Path
import win_capture as cad
from capture_markdown import window_for_pid
from memory_save import Counters


def percentile(values, pct):
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    k = (len(ordered) - 1) * pct / 100.0
    lo = int(k)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--scale', type=float, default=1.0); ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--font', type=int, default=14)
    ap.add_argument('--tabs', type=int, default=10)
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--library-files', type=int, default=100)
    ap.add_argument('--body-kib', type=int, default=1)
    ap.add_argument('--label', default='run')
    ap.add_argument('--timeout', type=float, default=3.0)
    ap.add_argument('--memory-interval', type=float, default=0.1)
    args = ap.parse_args()
    if args.memory_interval < 0.02: ap.error('--memory-interval must be >= 0.02 seconds')
    exe = Path(args.exe).resolve(); out = Path(args.out).resolve(); out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware(); cad.assert_unlocked('tab performance')

    config = out/'appdata/EUI-Edits'; config.mkdir(parents=True)
    temp = out/'temp'; temp.mkdir()
    project = out/'perf-lib'; project.mkdir()

    # Deterministic fixture: a vault of `library_files` entries plus `tabs` documents
    # whose first line is a unique marker and whose body is `body_kib` KiB.
    for i in range(args.library_files):
        (project/f'f{i:05d}.md').write_text(f"# lib {i}\n\nfiller\n", encoding='utf-8')
    for i in range(args.tabs):
        # Per-tab body text keeps the sampled region unique to the active page.
        lines = "".join(f"MARK{i:02d} line {j:05d} tab body content\n" for j in range(64))
        body = (lines * ((args.body_kib * 1024) // len(lines) + 1)) if args.body_kib else ""
        (project/f'doc{i:02d}.md').write_text(f"# MARK{i:02d}\n\n{body}", encoding='utf-8')

    (config/'settings.ini').write_text(
        f'last_file={project/"doc00.md"}\nmode=1\nui_scale={args.scale}\nui_font_size={args.font}\n'
        f'theme={args.theme}\nanimations=0\nui_language=en\nshow_status_bar=1\n', encoding='utf-8')
    trace_path = out/'trace.csv'
    env = dict(os.environ, APPDATA=str(config.parent), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1',
               NEO_TABS_TRACE=str(trace_path))

    result = dict(label=args.label, exe=str(exe),
                  sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  tabs=args.tabs, rounds=args.rounds, library_files=args.library_files,
                  body_kib=args.body_kib, scale=args.scale, theme=args.theme, font=args.font,
                  switches=[], memory=[], notes=[])
    result['configuration'] = {key: os.environ.get(key) for key in (
        'NEO_TAB_CACHE_BUDGET_MIB', 'NEO_TAB_CACHE_PAGES',
        'NEO_VIEWPORT_METRICS_OFF', 'NEO_COMPACT_DECORATIONS_OFF')}
    result['memory_samples'] = []; result['memory_interval_s'] = args.memory_interval
    memory_stop = threading.Event(); memory_thread = None

    def sample_memory(pid):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        psapi = ctypes.WinDLL('psapi', use_last_error=True)
        kernel.OpenProcess.argtypes = [cad.wintypes.DWORD, cad.wintypes.BOOL, cad.wintypes.DWORD]
        kernel.OpenProcess.restype = cad.wintypes.HANDLE
        kernel.CloseHandle.argtypes = [cad.wintypes.HANDLE]
        psapi.GetProcessMemoryInfo.argtypes = [cad.wintypes.HANDLE, ctypes.POINTER(Counters), cad.wintypes.DWORD]
        handle = kernel.OpenProcess(0x410, False, pid)
        if not handle:
            result['notes'].append('Memory sampler could not open owned PID'); return
        started = time.perf_counter()
        try:
            while not memory_stop.is_set():
                counters = Counters(); counters.cb = ctypes.sizeof(counters)
                if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                    result['notes'].append('Memory sampler query failed'); break
                result['memory_samples'].append(dict(t_s=time.perf_counter()-started, pid=pid,
                    private_commit_mib=counters.private/1048576,
                    private_ws_mib=counters.private_ws/1048576, ws_mib=counters.ws/1048576,
                    peak_ws_mib=counters.peak_ws/1048576,
                    peak_pagefile_mib=counters.peak_pagefile/1048576))
                memory_stop.wait(args.memory_interval)
        finally:
            kernel.CloseHandle(handle)

    def stop_memory():
        memory_stop.set()
        if memory_thread is not None: memory_thread.join(timeout=5)

    proc = None; hwnd = None; unit = 1.0
    def owned():
        cad.assert_unlocked('tab performance'); pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        assert proc.poll() is None and pid.value == proc.pid and cad.user32.GetForegroundWindow() == hwnd, 'owned foreground lost'
    def raw_key(vk, down):
        cad.user32.keybd_event(vk, cad.user32.MapVirtualKeyW(vk, 0), 0 if down else 2, 0)
    def memory(stage):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True); psapi = ctypes.WinDLL('psapi', use_last_error=True)
        kernel.OpenProcess.argtypes = [cad.wintypes.DWORD, cad.wintypes.BOOL, cad.wintypes.DWORD]
        kernel.OpenProcess.restype = cad.wintypes.HANDLE
        kernel.CloseHandle.argtypes = [cad.wintypes.HANDLE]
        psapi.GetProcessMemoryInfo.argtypes = [cad.wintypes.HANDLE, ctypes.POINTER(Counters), cad.wintypes.DWORD]
        handle = kernel.OpenProcess(0x410, False, proc.pid); assert handle
        try:
            counters = Counters(); counters.cb = ctypes.sizeof(counters)
            assert psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
            entry = dict(stage=stage, ws_mib=counters.ws/1048576, private_mib=counters.private/1048576)
            result['memory'].append(entry); return entry
        finally:
            kernel.CloseHandle(handle)
    def start():
        nonlocal proc, hwnd, unit, memory_thread
        proc = subprocess.Popen([str(exe), str(project/'doc00.md')], cwd=exe.parent, env=env)
        result['pid'] = proc.pid
        memory_thread = threading.Thread(target=sample_memory, args=(proc.pid,), daemon=True)
        memory_thread.start()
        for _ in range(150):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None: break
            time.sleep(0.1)
        assert hwnd, 'main window missing'
        assert cad.ensure_foreground(hwnd), 'cannot foreground the launched window'
        owned()
        cad.user32.SetWindowPos(hwnd, None, 30, 30, 1320, 880, 4); time.sleep(0.5)
        unit = cad.user32.GetDpiForWindow(hwnd)/96*args.scale
    def open_forward(path):
        owned()
        request = temp/'EUI-Edits.next-open'; stage = temp/'perf-open.tmp'
        stage.write_text(str(path), encoding='utf-8'); stage.replace(request)
        raw_key(0x10, True); time.sleep(0.12); raw_key(0x10, False); time.sleep(0.55); owned()
    def capture_raw():
        return capture_region()

    def capture_region():
        # Only the editor's first lines: a full-client BitBlt (~4.6MB) costs more than
        # the switch itself and would dominate the timing. A small region keeps the
        # poll loop honest (~1-3 ms/capture).
        rx = int(290 * unit); ry = int(38 * unit)
        rw = int(520 * unit); rh = int(130 * unit)
        origin = cad.wintypes.POINT(0, 0); cad.user32.ClientToScreen(hwnd, ctypes.byref(origin))
        screen_dc = cad.user32.GetDC(0); mem = cad.gdi32.CreateCompatibleDC(screen_dc)
        bmp = cad.gdi32.CreateCompatibleBitmap(screen_dc, rw, rh); cad.gdi32.SelectObject(mem, bmp)
        cad.gdi32.BitBlt(mem, 0, 0, rw, rh, screen_dc, origin.x + rx, origin.y + ry, 0x00CC0020)
        header = cad.BITMAPINFOHEADER(); header.biSize = ctypes.sizeof(header)
        header.biWidth = rw; header.biHeight = -rh; header.biPlanes = 1; header.biBitCount = 32
        buffer = ctypes.create_string_buffer(rw * rh * 4)
        cad.gdi32.GetDIBits(mem, bmp, 0, rh, buffer, ctypes.byref(header), 0)
        cad.gdi32.DeleteObject(bmp); cad.gdi32.DeleteDC(mem); cad.user32.ReleaseDC(0, screen_dc)
        return buffer.raw

    def settle(prev, timeout):
        t0 = time.perf_counter(); last = None; stable = 0
        while time.perf_counter() - t0 < timeout:
            cur = capture_raw()
            if cur == last:
                stable += 1
            else:
                stable = 0
            last = cur
            if stable >= 1 and cur != prev:
                return (time.perf_counter() - t0) * 1000.0, cur
            time.sleep(0.002)
        return None, last

    try:
        start()
        for i in range(1, args.tabs):
            open_forward(project/f'doc{i:02d}.md')
        memory('after-open')
        # Warm every page twice so layout/plan caches are hot where the build keeps them.
        for _ in range(2):
            for _ in range(args.tabs):
                raw_key(0x11, True); raw_key(0x09, True); raw_key(0x09, False); raw_key(0x11, False)
                time.sleep(0.12)
        memory('after-warm')
        prev = capture_raw()
        for r in range(args.rounds):
            for _ in range(args.tabs):
                owned()
                prev = capture_raw()
                raw_key(0x11, True); time.sleep(0.01)
                t0 = time.perf_counter()
                raw_key(0x09, True); raw_key(0x09, False)
                raw_key(0x11, False)
                latency, prev = settle(prev, args.timeout)
                wall = (time.perf_counter() - t0) * 1000.0
                result['switches'].append(dict(round=r, latency_ms=latency, wall_ms=wall))
                time.sleep(0.05)
        memory('steady')
        stop_memory()
        # Close cleanly so shutdown path is exercised too.
        owned(); cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
        proc.wait(timeout=15)
        result['exit_code'] = proc.returncode
    finally:
        stop_memory()
        if proc is not None and proc.poll() is None and hwnd:
            try:
                cad.ensure_foreground(hwnd); cad.user32.PostMessageW(hwnd, 0x10, 0, 0); proc.wait(timeout=8)
            except Exception:
                proc.kill()
        latencies = [s['latency_ms'] for s in result['switches'] if s['latency_ms'] is not None]
        missing = sum(1 for s in result['switches'] if s['latency_ms'] is None)
        result['summary'] = dict(
            samples=len(latencies), missing=missing,
            p50_ms=percentile(latencies, 50), p95_ms=percentile(latencies, 95),
            p99_ms=percentile(latencies, 99), max_ms=max(latencies) if latencies else None,
            min_ms=min(latencies) if latencies else None)
        if result['memory_samples']:
            values = [item['private_commit_mib'] for item in result['memory_samples']]
            result['memory_summary'] = dict(samples=len(values),
                sampled_peak_private_commit_mib=max(values),
                last_private_commit_mib=values[-1], min_private_commit_mib=min(values))
        if trace_path.exists():
            stages = {}
            for line in trace_path.read_text(encoding='utf-8', errors='replace').splitlines()[1:]:
                parts = line.split(',')
                if len(parts) >= 5 and parts[3] == 'switch':
                    for token in parts[4].split():
                        if token.startswith('elapsed_us='):
                            stages.setdefault('switch_us', []).append(int(token.split('=')[1]))
            result['trace'] = {k: dict(count=len(v), p50_us=percentile(v, 50), p95_us=percentile(v, 95),
                                      max_us=max(v)) for k, v in stages.items()}
        (out/'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k: result[k] for k in ('label', 'sha256', 'summary') if k in result}, ensure_ascii=False))


if __name__ == '__main__':
    main()

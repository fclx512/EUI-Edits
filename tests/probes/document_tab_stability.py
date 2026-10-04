"""Same-PID long-run stability probe: fixed workload, sampled memory/handles.

Runs a fixed 10-page workload (switch / short edit+undo / scroll) for a configured
duration, sampling WS/private commit/threads/handles every few seconds, with periodic
idle windows. Reports per-5-minute private-commit medians and the post-warm slope so a
slow leak is visible without treating allocator retention as a leak.

Isolated per run (NEO_SINGLE_INSTANCE=0 + private APPDATA/TEMP); never touches a user
instance. Normal close only.
"""
import argparse, ctypes, hashlib, json, os, statistics, subprocess, time
from pathlib import Path
import win_capture as cad
from capture_markdown import window_for_pid
from memory_save import Counters


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--minutes', type=float, default=30.0)
    ap.add_argument('--tabs', type=int, default=10)
    ap.add_argument('--sample-seconds', type=float, default=5.0)
    ap.add_argument('--body-kib', type=int, default=64)
    ap.add_argument('--label', default='stability')
    args = ap.parse_args()
    exe = Path(args.exe).resolve(); out = Path(args.out).resolve(); out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware(); cad.assert_unlocked('stability')

    config = out/'appdata/EUI-Edits'; config.mkdir(parents=True)
    temp = out/'temp'; temp.mkdir(); project = out/'lib'; project.mkdir()
    lines = "".join(f"line {i:05d} fixed stability workload content\n" for i in range(64))
    body = lines * ((args.body_kib * 1024) // len(lines) + 1)
    for i in range(args.tabs):
        (project/f'd{i:02d}.md').write_text(f"# S{i:02d}\n\n{body}", encoding='utf-8')
    (config/'settings.ini').write_text(
        f'last_file={project/"d00.md"}\nmode=1\nui_scale=1.0\nui_font_size=14\ntheme=1\nanimations=0\nui_language=en\n',
        encoding='utf-8')
    env = dict(os.environ, APPDATA=str(config.parent), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
    result = dict(label=args.label, exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  minutes=args.minutes, tabs=args.tabs, samples=[], notes=[])
    proc = None; hwnd = None
    def owned():
        cad.assert_unlocked('stability'); pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        assert proc.poll() is None and pid.value == proc.pid and cad.ensure_foreground(hwnd), 'owned foreground lost'
    def vk(v, down):
        cad.user32.keybd_event(v, cad.user32.MapVirtualKeyW(v, 0), 0 if down else 2, 0)
    def tap(*keys):
        owned()
        for k in keys: vk(k, True)
        for k in reversed(keys): vk(k, False)
        time.sleep(0.05)
    def counters():
        kernel = ctypes.WinDLL('kernel32', use_last_error=True); psapi = ctypes.WinDLL('psapi', use_last_error=True)
        kernel.OpenProcess.argtypes = [cad.wintypes.DWORD, cad.wintypes.BOOL, cad.wintypes.DWORD]
        kernel.OpenProcess.restype = cad.wintypes.HANDLE; kernel.CloseHandle.argtypes = [cad.wintypes.HANDLE]
        psapi.GetProcessMemoryInfo.argtypes = [cad.wintypes.HANDLE, ctypes.POINTER(Counters), cad.wintypes.DWORD]
        handle = kernel.OpenProcess(0x410, False, proc.pid); assert handle
        try:
            c = Counters(); c.cb = ctypes.sizeof(c)
            assert psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb)
            return c
        finally:
            kernel.CloseHandle(handle)
    def threads_handles():
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [cad.wintypes.DWORD, cad.wintypes.BOOL, cad.wintypes.DWORD]
        kernel.OpenProcess.restype = cad.wintypes.HANDLE; kernel.CloseHandle.argtypes = [cad.wintypes.HANDLE]
        kernel.GetProcessHandleCount.argtypes = [cad.wintypes.HANDLE, ctypes.POINTER(cad.wintypes.DWORD)]
        handle = kernel.OpenProcess(0x0400, False, proc.pid)  # PROCESS_QUERY_INFORMATION
        if not handle:
            return None, None
        try:
            count = cad.wintypes.DWORD()
            kernel.GetProcessHandleCount(handle, ctypes.byref(count))
            return count.value, None
        finally:
            kernel.CloseHandle(handle)
    try:
        proc = subprocess.Popen([str(exe), str(project/'d00.md')], cwd=exe.parent, env=env)
        for _ in range(150):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None: break
            time.sleep(0.1)
        assert hwnd, 'main window missing'
        cad.user32.SetWindowPos(hwnd, None, 30, 30, 1320, 880, 4); time.sleep(0.8)
        # Warm up: open all pages and cycle through them twice.
        for i in range(1, args.tabs):
            owned(); stage = temp/'stab-open.tmp'; stage.write_text(str(project/f'd{i:02d}.md'), encoding='utf-8')
            stage.replace(temp/'EUI-Edits.next-open'); vk(0x10, True); time.sleep(0.1); vk(0x10, False); time.sleep(0.5)
        for _ in range(2):
            for _ in range(args.tabs):
                tap(0x11, 0x09); time.sleep(0.08)
        result['warm_start_ts'] = time.time()
        deadline = time.time() + args.minutes * 60.0
        last_sample = 0.0; last_idle = time.time(); edits = 0
        while time.time() < deadline:
            owned()
            tap(0x11, 0x09)                      # switch to next page
            tap(0x20); tap(0x20)                 # space x2
            tap(0x11, 0x5A); tap(0x11, 0x5A)     # undo x2
            edits += 1
            now = time.time()
            if now - last_sample >= args.sample_seconds:
                last_sample = now
                c = counters(); handles, _ = threads_handles()
                result['samples'].append(dict(t=now - result['warm_start_ts'], pid=proc.pid,
                    ws_mib=c.ws/1048576, private_mib=c.private/1048576, handles=handles, edits=edits))
            if now - last_idle >= 300.0:         # 5-minute idle window
                last_idle = now; time.sleep(20.0)
        owned(); cad.user32.PostMessageW(hwnd, 0x10, 0, 0); time.sleep(0.5)
        # 编辑后撤销会让 revision != savedRevision（内容相同也算脏）：关窗会走未保存
        # 确认。这里显式选"丢弃"（左移到丢弃项 + 回车）直到进程退出，而不是干等超时。
        for _ in range(40):
            if proc.poll() is not None:
                break
            owned()
            vk(0x25, True); vk(0x25, False); time.sleep(0.05)
            vk(0x0D, True); vk(0x0D, False); time.sleep(0.4)
        proc.wait(timeout=20)
        result['exit_code'] = proc.returncode
        result['closed_normally'] = True
    finally:
        if proc is not None and proc.poll() is None and hwnd:
            try:
                cad.ensure_foreground(hwnd); cad.user32.PostMessageW(hwnd, 0x10, 0, 0); proc.wait(timeout=10)
            except Exception:
                proc.kill()
        samples = result['samples']
        if samples:
            warm_cut = min(120.0, args.minutes * 60.0 * 0.1)
            post = [s for s in samples if s['t'] >= warm_cut]
            privates = [s['private_mib'] for s in post]
            result['summary'] = dict(
                total_samples=len(samples), post_warm_samples=len(post),
                private_min=min(privates), private_max=max(privates),
                private_last=privates[-1], handles_min=min(s['handles'] for s in samples if s['handles']),
                handles_max=max(s['handles'] for s in samples if s['handles']))
            buckets = {}
            for s in post:
                buckets.setdefault(int(s['t'] // 300), []).append(s['private_mib'])
            result['window_medians'] = {str(k * 5): statistics.median(v) for k, v in sorted(buckets.items())}
        (out/'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result.get('summary', {}), ensure_ascii=False))


if __name__ == '__main__':
    main()

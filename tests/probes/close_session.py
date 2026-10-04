"""Own-PID normal-exit latency, blank restart, cancellation and recovery checks.

Uses private APPDATA/TEMP, explicit input paths, no foreign instance and no kill.
--baseline records old behavior without enforcing the new session policy.
"""
import argparse, ctypes, hashlib, json, os, shutil, subprocess, time
from pathlib import Path
import win_capture as cad
from capture_markdown import window_for_pid
from memory_save import Counters


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--baseline', action='store_true')
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--body-kib', type=int, default=1)
    args = ap.parse_args()
    exe = Path(args.exe).resolve(); out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware(); cad.assert_unlocked('exit/session probe')
    cfg = out/'appdata/EUI-Edits'; cfg.mkdir(parents=True)
    temp = out/'temp'; temp.mkdir()
    doc = out/'document.md'
    content = '# CLOSE-MARK\n\n' + ('A deterministic document line.\n' *
                ((max(0, args.body_kib)*1024)//31 + 1))
    doc.write_text(content, encoding='utf-8')
    (cfg/'settings.ini').write_text(f'ui_language=en\nui_scale={args.scale}\n'
        f'theme={args.theme}\nmode=0\nanimations=0\n', encoding='utf-8')
    env = dict(os.environ, APPDATA=str(cfg.parent), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1',
               NEO_TABS_TRACE=str(out/'trace.csv'))
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  baseline=args.baseline, scale=args.scale, theme=args.theme,
                  body_kib=args.body_kib,
                  gray_atlas_initial_size=env.get('NEO_GRAY_ATLAS_INITIAL_SIZE', '512'),
                  checks=[], closes=[], memory=[], captures=[])
    proc = None; hwnd = None
    manifest = cfg/'session/manifest.json'

    def check(name, value):
        result['checks'].append(dict(name=name, passed=bool(value)))
        print(name, bool(value), flush=True)
        assert value, name

    def owned():
        cad.assert_unlocked('exit/session probe')
        pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        assert proc.poll() is None and pid.value == proc.pid and cad.user32.GetForegroundWindow() == hwnd

    def tap(*keys):
        owned()
        for k in keys:
            cad.user32.keybd_event(k, cad.user32.MapVirtualKeyW(k, 0), 0, 0); time.sleep(.03)
        for k in reversed(keys):
            cad.user32.keybd_event(k, cad.user32.MapVirtualKeyW(k, 0), 2, 0); time.sleep(.03)
        time.sleep(.1)

    def capture(name):
        owned(); w, h, raw = cad.capture_client(hwnd)
        cad.write_png(str(out/(name+'.png')), w, h, raw)
        result['captures'].append(name)

    def memory(stage):
        owned()
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        p = ctypes.WinDLL('psapi', use_last_error=True)
        k.OpenProcess.argtypes = [cad.wintypes.DWORD, cad.wintypes.BOOL, cad.wintypes.DWORD]
        k.OpenProcess.restype = cad.wintypes.HANDLE
        k.CloseHandle.argtypes = [cad.wintypes.HANDLE]
        p.GetProcessMemoryInfo.argtypes = [cad.wintypes.HANDLE, ctypes.POINTER(Counters), cad.wintypes.DWORD]
        h = k.OpenProcess(0x410, False, proc.pid); assert h
        try:
            c = Counters(); c.cb = ctypes.sizeof(c)
            assert p.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb)
            result['memory'].append(dict(stage=stage, pid=proc.pid, private_mib=c.private/1048576,
                ws_mib=c.ws/1048576, private_ws_mib=c.private_ws/1048576))
        finally: k.CloseHandle(h)

    def start(path=None, label='start'):
        nonlocal proc, hwnd
        proc = subprocess.Popen([str(exe)]+([str(path)] if path else []), cwd=exe.parent, env=env)
        hwnd = None
        for _ in range(150):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None: break
            time.sleep(.05)
        assert hwnd, 'owned main window missing'
        assert cad.ensure_foreground(hwnd), 'cannot foreground launched main window'
        owned(); cad.user32.SetWindowPos(hwnd, None, 30, 30, 1100, 760, 4)
        time.sleep(.75); owned()
        result['effective_scale'] = cad.user32.GetDpiForWindow(hwnd)/96*args.scale
        # New blank startup has no automatic editor focus. Use a real owned-client
        # click before injecting WM_CHAR rather than assuming a previous focused tab.
        cad.click(hwnd, round(180*result['effective_scale']), round(85*result['effective_scale']))
        time.sleep(.1); capture(label); memory(label)

    def records():
        try: return json.loads(manifest.read_text('utf-8'))['records']
        except (FileNotFoundError, PermissionError, json.JSONDecodeError): return []

    def wait_for(predicate, description, seconds=5):
        end = time.perf_counter()+seconds
        while time.perf_counter() < end:
            owned()
            if predicate(): return
            time.sleep(.02)
        raise AssertionError(description)

    def measure_close(label, already_confirming=False):
        owned()
        start_at = time.perf_counter()
        if already_confirming:
            tap(0x0D)
        else:
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
        blocked = 0; longest = 0
        cad.user32.SendMessageTimeoutW.argtypes = [cad.wintypes.HWND, cad.wintypes.UINT,
            cad.wintypes.WPARAM, cad.wintypes.LPARAM, cad.wintypes.UINT, cad.wintypes.UINT,
            ctypes.POINTER(ctypes.c_size_t)]
        cad.user32.SendMessageTimeoutW.restype = cad.wintypes.LPARAM
        while proc.poll() is None:
            assert time.perf_counter()-start_at < 12, 'exit exceeded 12 seconds'
            pid = ctypes.c_ulong(); cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == proc.pid:
                answer = ctypes.c_size_t(); q = time.perf_counter()
                ok = cad.user32.SendMessageTimeoutW(hwnd, 0, 0, 0, 2, 50, ctypes.byref(answer))
                elapsed = (time.perf_counter()-q)*1000
                longest = max(longest, elapsed)
                if not ok: blocked += 1
            time.sleep(.01)
        latency = (time.perf_counter()-start_at)*1000
        result['closes'].append(dict(label=label, pid=proc.pid, elapsed_ms=latency,
            wm_null_timeouts=blocked, longest_wm_null_ms=longest, exit_code=proc.returncode))
        check(label+' normal exit', proc.returncode == 0)
        if not args.baseline:
            check(label+' no owned session files', not manifest.exists() and
                  not list((cfg/'session').glob('body-*.utf8')))
            check(label+' no 5-second exit stall', latency < 1500)

    try:
        start(label='initial-blank'); measure_close('blank')
        start(doc, 'explicit-clean')
        wait_for(lambda: len(records()) == 1, 'clean document registered')
        clean_snapshot = out/'clean-snapshot'; shutil.copytree(cfg/'session', clean_snapshot)
        measure_close('clean-document')
        if args.baseline:
            result['clean_manifest_after_close'] = manifest.exists()
            start(label='baseline-reopen'); measure_close('baseline-reopen')
        else:
            check('last_file cleared', 'last_file='+str(doc) not in (cfg/'settings.ini').read_text('utf-8'))
            start(label='normal-reopen-blank')
            check('normal reopen has no document records', not records())
            measure_close('normal-reopen')
            # Previous versions could leave a valid clean manifest and last_file.
            # Replay only our own archived data; none of the real user config is touched.
            shutil.copytree(clean_snapshot, cfg/'session', dirs_exist_ok=True)
            with (cfg/'settings.ini').open('a', encoding='utf-8') as f: f.write(f'last_file={doc}\n')
            start(label='legacy-clean-ignored')
            for ch in 'BLANK-PROBE': cad.user32.PostMessageW(hwnd, 0x102, ord(ch), 1)
            wait_for(lambda: len(records()) == 1 and records()[0].get('dirty') and not records()[0]['path'],
                     'legacy clean record ignored by live editor')
            record = records()[0]
            check('legacy clean record and last_file are ignored',
                  (cfg/'session'/record['body']).read_text('utf-8') == 'BLANK-PROBE')
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0); time.sleep(.25)
            tap(0x25); measure_close('legacy-clean', already_confirming=True)
            start(doc, 'dirty-cancel')
            tap(0x11, 0x23)
            for ch in 'CACHE-PROBE': cad.user32.PostMessageW(hwnd, 0x102, ord(ch), 1)
            wait_for(lambda: any(r.get('dirty') for r in records()), 'recovery cache owns draft')
            memory('dirty-before-save'); tap(0x11, 0x53)
            wait_for(lambda: len(records()) == 1 and not records()[0].get('dirty') and
                     not list((cfg/'session').glob('body-*.utf8')), 'saved page released recovery body')
            memory('saved-clean'); content += 'CACHE-PROBE'
            check('Ctrl S saved only the intended edit', doc.read_text('utf-8') == content)
            for ch in 'UNSAVED-CLOSE': cad.user32.PostMessageW(hwnd, 0x102, ord(ch), 1)
            wait_for(lambda: any(r.get('dirty') for r in records()), 'dirty recovery committed')
            recovery_snapshot = out/'dirty-snapshot'; shutil.copytree(cfg/'session', recovery_snapshot)
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0); time.sleep(.25)
            capture('dirty-confirm'); tap(0x1B); time.sleep(.25)
            check('cancel close retains dirty recovery', proc.poll() is None and any(r.get('dirty') for r in records()))
            capture('cancel-keeps-window')
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0); time.sleep(.25)
            tap(0x25); measure_close('discard-dirty', already_confirming=True)
            check('discard leaves source file unchanged', doc.read_text('utf-8') == content)
            # Trace batches flush at shutdown. Locate the dirty->clean transition
            # in this process before a new process truncates the same trace path.
            cache_sizes = [int(line.split('retained_body_bytes=')[1].split()[0])
                           for line in (out/'trace.csv').read_text('utf-8').splitlines()
                           if 'retained_body_bytes=' in line]
            result['recovery_cache_bytes'] = cache_sizes
            check('clean submission releases cached recovery document',
                  any(before > 0 and after == 0 for before, after in zip(cache_sizes, cache_sizes[1:])))
            # This simulates a durable crash checkpoint without killing a process.
            shutil.copytree(recovery_snapshot, cfg/'session', dirs_exist_ok=True)
            start(label='dirty-crash-checkpoint-restored')
            wait_for(lambda: any(r.get('dirty') for r in records()), 'durable draft still available')
            capture('restored-draft'); check('abnormal-exit checkpoint remains recoverable', any(r.get('dirty') for r in records()))
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0); time.sleep(.25)
            tap(0x25); measure_close('discard-restored', already_confirming=True)
    finally:
        if proc is not None and proc.poll() is None and hwnd:
            # Normal cleanup only, limited to our owned process.
            try:
                owned(); cad.user32.PostMessageW(hwnd, 0x10, 0, 0); time.sleep(.25)
                for _ in range(8):
                    if proc.poll() is not None: break
                    tap(0x25); tap(0x0D); time.sleep(.25)
                proc.wait(timeout=12)
            except Exception as error: result['cleanup_error'] = str(error)
        (out/'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(sha256=result['sha256'], closes=result['closes'], memory=result['memory']), ensure_ascii=False))


if __name__ == '__main__': main()

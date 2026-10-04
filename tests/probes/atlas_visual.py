"""Same-EXE gray-atlas size comparison using real owned Win32 windows.

No editing, registry changes, foreign-process input, or force-kill cleanup.
Excludes DWM rounded corners from client-pixel comparisons.
"""
import argparse, ctypes, hashlib, json, os, subprocess, time
from pathlib import Path
import win_capture as cad
from capture_markdown import window_for_pid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--scale', type=float, default=1)
    args = ap.parse_args()
    exe = Path(args.exe).resolve(); out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware(); cad.assert_unlocked('atlas visual comparison')
    fixture = out/'fixture'; fixture.mkdir()
    doc = fixture/'atlas.md'
    lines = ['# Atlas 中文字号与字形缓存', '', 'English 0123456789 😀 🐱 🌸', '']
    for row in range(64):
        lines.append(''.join(chr(0x4e00 + row*22 + col) for col in range(22)))
    doc.write_text('\n'.join(lines), encoding='utf-8')
    report = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  scale=args.scale, theme=args.theme, exits=[], comparisons=[])
    references = {}
    strategy_top = {}
    # Keep captures/reports outside the document library: outputs created between
    # paired processes must not change the visible file list or watcher workload.
    for strategy in ('2048', '512'):
        config = out/strategy/'appdata/EUI-Edits'; config.mkdir(parents=True)
        temp = out/strategy/'temp'; temp.mkdir()
        (config/'settings.ini').write_text(f'ui_language=zh-CN\nui_scale={args.scale}\n'
            f'theme={args.theme}\nmode=1\neditor_font_size=32\nui_font_size=14\n'
            'animations=0\nline_numbers=0\n', encoding='utf-8')
        env = dict(os.environ, APPDATA=str(config.parent), TEMP=str(temp), TMP=str(temp),
                   NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1',
                   NEO_GRAY_ATLAS_INITIAL_SIZE=strategy)
        proc = subprocess.Popen([str(exe), str(doc)], cwd=exe.parent, env=env)
        hwnd = None
        try:
            for _ in range(160):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None: break
                time.sleep(.05)
            assert hwnd, 'owned window missing'
            def owned():
                cad.assert_unlocked('atlas visual comparison')
                pid = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                assert proc.poll() is None and pid.value == proc.pid and cad.ensure_foreground(hwnd)
            def resize(width, height):
                owned(); cad.user32.SetWindowPos(hwnd, None, 30, 30, width, height, 4)
                # Keep hover/tooltips outside the client for deterministic captures.
                cad.user32.SetCursorPos(20, 20); time.sleep(.8)
            def wheel(delta):
                owned()
                w, h, _ = cad.capture_client(hwnd)
                point = cad.wintypes.POINT(w-120, h//2)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                param = ((point.y & 0xffff) << 16) | (point.x & 0xffff)
                # The input router uses the current pointer hit target. Move to
                # the owned editor before injecting an actual wheel event.
                cad.user32.SetCursorPos(point.x, point.y); time.sleep(.1)
                cad.user32.mouse_event(0x800, 0, 0, delta, 0); time.sleep(.3)
                cad.user32.SetCursorPos(20, 20); time.sleep(.3)
            def capture(label):
                owned(); w, h, raw = cad.capture_client(hwnd)
                cad.write_png(str(out/strategy/(label+'.png')), w, h, raw)
                # DWM corner margins differ according to the underlying desktop.
                crop = b''.join(raw[(y*w+10)*4:(y*w+w-10)*4] for y in range(10, h-10))
                if label == 'top': strategy_top[strategy] = crop
                if label == 'scroll-0':
                    assert crop != strategy_top[strategy], 'wheel did not scroll the visible editor'
                if label == 'return-top':
                    assert crop == strategy_top[strategy], 'scroll up did not restore the original viewport'
                if strategy == '2048':
                    references[label] = (w, h, crop)
                else:
                    rw, rh, before = references[label]
                    assert (w, h) == (rw, rh), 'client geometry differs'
                    different = sum(before[i:i+4] != crop[i:i+4] for i in range(0, len(crop), 4))
                    report['comparisons'].append(dict(label=label, differing_pixels=different,
                        compared_pixels=len(crop)//4, passed=different == 0))
                    assert different == 0, f'{label}: atlas strategies rendered differently ({different} pixels)'
            resize(1200, 820); capture('top')
            # New visible codepoints fill and grow the page; returning to earlier
            # text checks that cached normalized UVs recover after that growth.
            for step in range(5):
                wheel(-1200); capture(f'scroll-{step}')
            wheel(120*100); capture('return-top')
            resize(940, 700); capture('narrow')
            resize(1200, 820); capture('wide-again')
            owned(); started = time.perf_counter()
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0); proc.wait(timeout=8)
            report['exits'].append(dict(strategy=strategy, exit_code=proc.returncode,
                elapsed_ms=(time.perf_counter()-started)*1000))
            assert proc.returncode == 0
            assert not (config/'session/manifest.json').exists()
        finally:
            if proc.poll() is None and hwnd:
                try:
                    cad.user32.PostMessageW(hwnd, 0x10, 0, 0); proc.wait(timeout=8)
                except Exception as error: report['cleanup_error'] = str(error)
            (out/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__': main()

"""Owned-window code-background and short color-feedback evidence, plus CPU samples.

Screen sampling establishes intermediate colors, not monitor frame pacing. All
documents/settings are disposable, and the owned process is closed normally.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from ctypes import wintypes

import win_capture as cad
from capture_markdown import window_for_pid, write_settings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--baseline', action='store_true')
    ap.add_argument('--doc', help='Optional representative Markdown for CPU comparison')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('lightweight feedback')
    cad.assert_no_foreign_instance('lightweight feedback')
    result = dict(exe=str(exe), exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  theme=args.theme, scale=args.scale, baseline=args.baseline, checks=[])
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    with tempfile.TemporaryDirectory(prefix='neo-feedback-') as temp:
        doc = Path(temp) / 'feedback.md'
        doc.write_text(Path(args.doc).read_text(encoding='utf-8') if args.doc else
                       'Plain baseline\n\n```cpp\nint count = 1;\n\n    \ncount++;\n```\n\n'
                       '> Quote line\n> > Nested quote\n\n| Key | Value |\n| --- | --- |\n| a | b |\n',
                       encoding='utf-8')
        result['sample_sha256'] = hashlib.sha256(doc.read_bytes()).hexdigest()
        write_settings(str(Path(temp) / 'EUI-Edits/settings.ini'), str(doc), args.theme, 16, str(args.scale))
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='1')
        with (out / 'renderer.log').open('w', encoding='utf-8') as log:
            proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env, stderr=log)
            handle = kernel.OpenProcess(0x1000, False, proc.pid)
            hwnd = None

            def owned():
                cad.assert_unlocked('lightweight feedback')
                owner = wintypes.DWORD()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                assert proc.poll() is None and owner.value == proc.pid and cad.ensure_foreground(hwnd)

            def move(x, y):
                owned()
                p = wintypes.POINT(round(x * unit), round(y * unit))
                cad.user32.ClientToScreen(hwnd, ctypes.byref(p))
                cad.user32.SetCursorPos(p.x, p.y)

            def capture(label):
                owned()
                w, h, pixels = cad.capture_client(hwnd)
                cad.write_png(str(out / (label + '.png')), w, h, pixels)
                return w, h, pixels

            def rgb(pixels, w, x, y):
                pos = (round(y * unit) * w + round(x * unit)) * 4
                return tuple(pixels[pos:pos+3])

            def check(name, passed):
                result['checks'].append(dict(name=name, passed=bool(passed)))
                assert passed, name

            def cpu():
                times = [wintypes.FILETIME() for _ in range(4)]
                assert kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times))
                return sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in times[2:]) / 1e7

            def measure(action):
                start, before = time.perf_counter(), cpu()
                action()
                elapsed, seconds = time.perf_counter()-start, cpu()-before
                return dict(wall_seconds=elapsed, cpu_seconds=seconds,
                            one_core_percent=100*seconds/elapsed)

            try:
                for _ in range(150):
                    hwnd = window_for_pid(proc.pid)
                    if hwnd or proc.poll() is not None: break
                    time.sleep(.1)
                assert hwnd and handle
                owned()
                cad.user32.SetWindowPos(hwnd, None, 80, 50, 1120, 820, 0x0004)
                unit = cad.user32.GetDpiForWindow(hwnd) / 96 * args.scale
                result.update(pid=proc.pid, system_dpi=cad.user32.GetDpiForWindow(hwnd))
                move(450, 360)
                time.sleep(3)
                w, h, pixels = capture('document')
                if not args.doc:
                    # Default body text is 24 logical px, code rows 21. Blank
                    # and whitespace rows are sampled well away from text/caret.
                    column = w / unit - 80
                    colors = [rgb(pixels, w, column, y) for y in (130, 151, 172)]
                    distinct = colors[0] != rgb(pixels, w, column, 50)
                    result['code_background'] = dict(colors=colors, distinct=distinct)
                    if not args.baseline:
                        check('code blank/whitespace rows share distinct background', distinct and len(set(colors)) == 1)

                # Menu title background at its text-free top padding.
                menu_x, menu_y = 24, 3
                initial = rgb(pixels, w, menu_x, menu_y)
                move(24, 17)
                started, sequence, frames = time.perf_counter(), [], []
                point = wintypes.POINT(round(menu_x * unit), round(menu_y * unit))
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                pixel_user = ctypes.WinDLL('user32', use_last_error=True)
                pixel_gdi = ctypes.WinDLL('gdi32', use_last_error=True)
                pixel_user.GetDC.restype = wintypes.HDC
                pixel_user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
                pixel_gdi.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
                pixel_gdi.GetPixel.restype = wintypes.DWORD
                screen_dc = pixel_user.GetDC(None)
                try:
                    for n in range(64):
                        owned()
                        color = pixel_gdi.GetPixel(screen_dc, point.x, point.y)
                        assert color != 0xffffffff
                        sequence.append(dict(ms=(time.perf_counter()-started)*1000,
                                             color=((color >> 16) & 255, (color >> 8) & 255, color & 255)))
                        if n in (0, 10, 30, 63):
                            sw, sh, sample = cad.capture_client(hwnd)
                            frames.append((n, sw, sh, sample))
                        time.sleep(.003)
                finally:
                    pixel_user.ReleaseDC(None, screen_dc)
                for n, sw, sh, sample in frames:
                    cad.write_png(str(out / f'hover-{n:02}.png'), sw, sh, sample)
                time.sleep(.8)
                sw, _, settled = capture('hover-settled')
                target = rgb(settled, sw, menu_x, menu_y)
                result['hover'] = dict(initial=initial, target=target, sequence=sequence,
                                       intermediate_colors=len({tuple(s['color']) for s in sequence
                                                                if tuple(s['color']) not in (initial, target)}))
                check('menu hover feedback visible', target != initial)
                if not args.baseline:
                    check('hover traverses intermediate colors', result['hover']['intermediate_colors'] >= 2)
                    check('transparent hover does not flash darker than either endpoint',
                          all(min(initial[k], target[k])-1 <= s['color'][k] <= max(initial[k], target[k])+1
                              for s in sequence for k in range(3)))

                result['idle_before'] = measure(lambda: time.sleep(3))
                def sweep():
                    for _ in range(12):
                        for x in (24, 78, 132, 186, 240, 400):
                            move(x, 17)
                            time.sleep(.1)
                result['menu_sweep'] = measure(sweep)
                move(450, 360)
                time.sleep(2)
                result['idle_after'] = measure(lambda: time.sleep(3))
                # Press and drag cancellation must not open a menu after release.
                move(24, 17)
                time.sleep(.6)
                cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
                time.sleep(.12)
                capture('pressed')
                move(450, 360)
                cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                time.sleep(1)
                capture('cancelled')
            finally:
                cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                kernel.CloseHandle(handle)
                if hwnd and proc.poll() is None:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                result['exit_code'] = proc.wait(timeout=15)
                (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    check('normal process exit', result['exit_code'] == 0)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()

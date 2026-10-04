"""Dump the full pixel timeline at a code-block point while focus toggles.

Pinpoints when the white flash happens relative to clicks and what the color
sequence looks like (single flash vs sustained), on a disposable document.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from ctypes import wintypes

import win_capture as cad
from capture_markdown import window_for_pid, write_settings

DOC = """# 闪烁复现样本

正文段落，用来区分编辑器表面与代码块底色。

```cpp
int count = 1;
count++;
return count;
```

> 引用行

收尾段落。
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=0)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('focus flash')
    cad.assert_no_foreign_instance('focus flash')
    with tempfile.TemporaryDirectory(prefix='neo-focus-') as temp:
        doc = Path(temp) / 'focus.md'
        doc.write_text(DOC, encoding='utf-8')
        write_settings(str(Path(temp) / 'EUI-Edits/settings.ini'), str(doc), args.theme, 16, '1.0')
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='1')
        with (out / 'renderer.log').open('w', encoding='utf-8') as log:
            proc = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stderr=log)
            hwnd = None
            for _ in range(150):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            assert hwnd and proc.poll() is None
            cad.ensure_foreground(hwnd)
            cad.user32.SetWindowPos(hwnd, None, 80, 50, 1120, 820, 0x0004)
            unit = cad.user32.GetDpiForWindow(hwnd) / 96
            time.sleep(2.0)
            w, h, pixels = cad.capture_client(hwnd)
            cad.write_png(str(out / 'doc.png'), w, h, pixels)

            def rgb(x, y):
                pos = (round(y * unit) * w + round(x * unit)) * 4
                return tuple(pixels[pos:pos + 3])

            # 代码块底色点：先在截图上扫描一条与编辑器表面不同的连续带。
            surface = rgb(520, 120)
            band_y = None
            for y in range(140, 500):
                if rgb(520, y) != surface and rgb(520, y + 6) != surface and rgb(520, y + 12) != surface:
                    band_y = y + 6
                    break
            assert band_y, 'no band'
            block_color = rgb(520, band_y)
            sample_x, sample_y = 640, band_y  # 块底色右段，避开文字
            block_color = rgb(sample_x, sample_y)

            user = ctypes.WinDLL('user32', use_last_error=True)
            gdi = ctypes.WinDLL('gdi32', use_last_error=True)
            user.GetDC.restype = wintypes.HDC
            user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
            gdi.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
            gdi.GetPixel.restype = wintypes.DWORD
            pt = wintypes.POINT(round(sample_x * unit), round(sample_y * unit))
            user.ClientToScreen(hwnd, ctypes.byref(pt))
            dc = user.GetDC(None)

            timeline = []
            marks = []

            def sample_loop(stop, label):
                started = time.perf_counter()
                while time.perf_counter() - started < stop:
                    color = gdi.GetPixel(dc, pt.x, pt.y)
                    timeline.append(dict(ms=round((time.perf_counter() - started) * 1000, 1),
                                         label=label,
                                         color=((color >> 16) & 255, (color >> 8) & 255, color & 255)))

            import threading
            stop_flag = []

            def loop():
                started = time.perf_counter()
                while not stop_flag:
                    color = gdi.GetPixel(dc, pt.x, pt.y)
                    timeline.append(dict(ms=round((time.perf_counter() - started) * 1000, 1),
                                         color=((color >> 16) & 255, (color >> 8) & 255, color & 255)))
                stop_flag.clear()

            def mark(text):
                marks.append(dict(ms=round(time.perf_counter() * 1000 - t0, 1), text=text))

            sampler = threading.Thread(target=loop, daemon=True)
            result = dict(sample=dict(x=sample_x, y=sample_y, color=block_color),
                          surface=surface, marks=marks)
            try:
                time.sleep(0.5)
                t0 = time.perf_counter() * 1000
                sampler.start()
                mark('sampler start')
                for i in range(6):
                    # 点击编辑器下部空白 → 聚焦编辑器
                    p = wintypes.POINT(round(450 * unit), round(560 * unit))
                    user.ClientToScreen(hwnd, ctypes.byref(p))
                    user.SetCursorPos(p.x, p.y)
                    time.sleep(.15)
                    mark(f'click editor #{i}')
                    user.mouse_event(0x0002, 0, 0, 0, 0)
                    user.mouse_event(0x0004, 0, 0, 0, 0)
                    time.sleep(.5)
                    # 点设置按钮右边空白菜单栏 → 编辑器失焦
                    p2 = wintypes.POINT(round(560 * unit), round(8 * unit))
                    user.ClientToScreen(hwnd, ctypes.byref(p2))
                    user.SetCursorPos(p2.x, p2.y)
                    time.sleep(.15)
                    mark(f'click menubar #{i}')
                    user.mouse_event(0x0002, 0, 0, 0, 0)
                    user.mouse_event(0x0004, 0, 0, 0, 0)
                    time.sleep(.5)
                time.sleep(1.0)
            finally:
                stop_flag.append(1)
                sampler.join()
                user.ReleaseDC(None, dc)
                result['exit_code'] = proc.wait(timeout=15) if (user.PostMessageW(hwnd, 0x0010, 0, 0) and True) else None
                if proc.poll() is None:
                    proc.wait(timeout=15)
                result['exit_code'] = proc.poll()
            result['samples'] = timeline
            (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2),
                                                 encoding='utf-8')
            # 摘要：非 {block,surface} 颜色的片段
            odd = []
            prev = None
            for s in timeline:
                c = s['color']
                if c in (block_color, surface):
                    prev = None
                    continue
                if prev is None:
                    odd.append([])
                    prev = []
                odd[-1].append(s)
            result['flashes'] = [dict(ms=f[0]['ms'], colors=[x['color'] for x in f][:8], n=len(f))
                                 for f in odd]
            print(json.dumps(dict(sample=result['sample'], surface=surface,
                                  flashes=result['flashes'], marks=marks,
                                  exit_code=result['exit_code']), ensure_ascii=False, indent=1))


if __name__ == '__main__':
    main()

"""Hunt background-block flicker on the editor content while animations run.

One app session per phase, so every phase starts from a known layout and its
sampling point is located live on the current frame. Sampling is screen-point
GetPixel at ~3ms: it proves per-frame pixel correctness, not scan-out pacing.
All documents/settings are disposable; the owned process closes normally.
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

DOC = """# 闪烁复现样本

正文段落，用来区分编辑器表面与代码块底色。

```cpp
int count = 1;

    \s
count++;
return count;
```

> 引用行
> > 嵌套引用

| Key | Value |
| --- | --- |
| a | b |

- [ ] 待办一
- [ ] 待办二

```python
def hello():
    return "world"

x = hello()
```

收尾段落。
"""
DOC += "".join("\n\n补充段落 {}：让文档足够长，能够滚动查看更多内容，包含一些较长的文字行。\n".format(i)
               for i in range(30))

WHEEL = 0x0800
LDOWN, LUP = 0x0002, 0x0004
RDOWN, RUP = 0x0008, 0x0010
KEYDOWN, KEYUP = 0, 2
VK_CTRL, VK_ESC, VK_V = 0x11, 0x1b, 0x56


class Session:
    def __init__(self, exe, temp, theme, scale, log):
        settings = Path(temp) / 'EUI-Edits/settings.ini'
        doc = Path(temp) / 'flicker.md'
        doc.write_text(DOC, encoding='utf-8')
        values = {
            "vault": "",
            "last_file": str(doc),
            "mode": "1",
            "line_numbers": "1",
            "readable_width": "1",
            "show_status_bar": "1",
            "editor_font_size": "16",
            "editor_font_file": "",
            "ui_font_size": "14",
            "ui_scale": str(scale),
            "theme": str(theme),
        }
        settings.parent.mkdir(parents=True, exist_ok=True)
        lines = "".join(f"{key}={value}\n" for key, value in values.items())
        settings.write_text(lines, encoding='utf-8')
        self.doc = doc
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='1')
        self.proc = subprocess.Popen([str(exe)], cwd=str(Path(exe).parent), env=env, stderr=log)
        self.hwnd = None
        self.unit = 1.0
        self.w = self.h = 0

    def wait_window(self):
        for _ in range(150):
            self.hwnd = window_for_pid(self.proc.pid)
            if self.hwnd or self.proc.poll() is not None:
                break
            time.sleep(.1)
        assert self.hwnd and self.proc.poll() is None
        cad.ensure_foreground(self.hwnd)
        cad.user32.SetWindowPos(self.hwnd, None, 80, 50, 1120, 820, 0x0004)
        self.unit = cad.user32.GetDpiForWindow(self.hwnd) / 96
        return self.hwnd

    def owned(self, where):
        cad.assert_unlocked(where)
        owner = wintypes.DWORD()
        cad.user32.GetWindowThreadProcessId(self.hwnd, ctypes.byref(owner))
        assert self.proc.poll() is None and owner.value == self.proc.pid and cad.ensure_foreground(self.hwnd)

    def move(self, x, y):
        self.owned('move')
        p = wintypes.POINT(round(x * self.unit), round(y * self.unit))
        cad.user32.ClientToScreen(self.hwnd, ctypes.byref(p))
        cad.user32.SetCursorPos(p.x, p.y)

    def click(self, x, y):
        self.move(x, y)
        time.sleep(.15)
        cad.user32.mouse_event(LDOWN, 0, 0, 0, 0)
        cad.user32.mouse_event(LUP, 0, 0, 0, 0)
        time.sleep(.3)

    def key(self, vk):
        cad.user32.keybd_event(vk, 0, KEYDOWN, 0)
        cad.user32.keybd_event(vk, 0, KEYUP, 0)

    def capture(self, out, label):
        self.owned('capture')
        w, h, pixels = cad.capture_client(self.hwnd)
        cad.write_png(str(out / (label + '.png')), w, h, pixels)
        self.w, self.h = w, h
        return w, h, pixels

    def rgb(self, pixels, x, y):
        pos = (round(y * self.unit) * self.w + round(x * self.unit)) * 4
        return tuple(pixels[pos:pos + 3])

    def close(self):
        if self.hwnd and self.proc.poll() is None:
            cad.user32.PostMessageW(self.hwnd, 0x0010, 0, 0)
        return self.proc.wait(timeout=15)


class Sampler:
    """Continuous screen-point sampling at one client position, in a thread
    so the main thread can inject interaction while samples are taken."""

    def __init__(self, session, client_x, client_y):
        self.session = session
        p = wintypes.POINT(round(client_x * session.unit), round(client_y * session.unit))
        cad.user32.ClientToScreen(session.hwnd, ctypes.byref(p))
        self.point = p
        self.user = ctypes.WinDLL('user32', use_last_error=True)
        self.gdi = ctypes.WinDLL('gdi32', use_last_error=True)
        self.user.GetDC.restype = wintypes.HDC
        self.user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
        self.gdi.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
        self.gdi.GetPixel.restype = wintypes.DWORD
        self.dc = self.user.GetDC(None)
        self.samples = []
        self._thread = None

    def run(self, seconds):
        started = time.perf_counter()
        deadline = started + seconds
        while time.perf_counter() < deadline:
            color = self.gdi.GetPixel(self.dc, self.point.x, self.point.y)
            assert color != 0xffffffff, 'sampling point off screen'
            self.samples.append(dict(ms=(time.perf_counter() - started) * 1000,
                                     color=((color >> 16) & 255, (color >> 8) & 255, color & 255)))

    def start(self, seconds):
        import threading
        self._thread = threading.Thread(target=self.run, args=(seconds,), daemon=True)
        self._thread.start()

    def join(self):
        if self._thread:
            self._thread.join()
            self._thread = None

    def close(self):
        if self.dc:
            self.user.ReleaseDC(None, self.dc)
            self.dc = None


def find_band(session, pixels, column_x=520, y_lo=60, y_hi=420):
    """Locate the code-block band on the CURRENT frame; returns (y0, y1, color)."""
    surface = session.rgb(pixels, column_x, 30)
    run_start, run_len = None, 0
    for y in range(y_lo, y_hi):
        c = session.rgb(pixels, column_x, y)
        if c != surface:
            if run_start is None:
                run_start, run_len = y, 1
            else:
                run_len += 1
        else:
            if run_start is not None and run_len >= 8:
                return run_start, run_start + run_len, session.rgb(pixels, column_x, run_start + run_len // 2)
            run_start, run_len = None, 0
    if run_start is not None and run_len >= 8:
        return run_start, run_start + run_len, session.rgb(pixels, column_x, run_start + run_len // 2)
    return None


def analyze_within(sequence, lo, hi, tol=2):
    bad = []
    for sample in sequence:
        for k in range(3):
            if not (min(lo[k], hi[k]) - tol <= sample['color'][k] <= max(lo[k], hi[k]) + tol):
                bad.append(sample)
                break
    return bad


def spikes(sequence, jump=18):
    found = []
    for i in range(1, len(sequence) - 1):
        prev, cur, nxt = sequence[i - 1]['color'], sequence[i]['color'], sequence[i + 1]['color']
        if all(abs(cur[k] - prev[k]) > jump and abs(cur[k] - nxt[k]) > jump for k in range(3)):
            found.append(sequence[i])
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=0)
    ap.add_argument('--scale', type=float, default=1.0)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('block flicker')
    cad.assert_no_foreign_instance('block flicker')
    result = dict(exe=str(exe), exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  theme=args.theme, scale=args.scale, phases=[])
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]

    def record(tag, sampler, extra=None, bad=None):
        entry = dict(phase=tag, samples=len(sampler.samples),
                     distinct=len({tuple(s['color']) for s in sampler.samples}),
                     spikes=len(spikes(sampler.samples)),
                     spike_first=spikes(sampler.samples)[:4])
        if bad is not None:
            entry['excursions'] = len(bad)
            entry['excursion_first'] = bad[:4]
        if extra:
            entry.update(extra)
        result['phases'].append(entry)
        return entry

    # ---- Phase 1: 点击聚焦/失焦编辑器（hit rect 边框渐变),采样块背景点。
    def phase_focus(session, temp):
        _, _, pixels = session.capture(out, 'focus-initial')
        band = find_band(session, pixels)
        assert band, 'no code block band'
        y0, y1, color = band
        session.move(450, 560)
        time.sleep(1.2)
        sampler = Sampler(session, 520, (y0 + y1) // 2)
        try:
            sampler.start(14)
            time.sleep(1.0)
            base = sampler.samples[-1]['color']
            for _ in range(6):
                session.click(450, 560)   # 聚焦编辑器
                session.click(560, 8)     # 点菜单栏让它失焦（不触发菜单则再点编辑器）
                session.key(VK_ESC)
                time.sleep(.2)
            time.sleep(1.5)
            sampler.join()
        finally:
            sampler.close()
        bad = analyze_within(sampler.samples, base, base, tol=3)
        record('focus_toggle', sampler, dict(base=base), bad)

    # ---- Phase 2: 复选框快速悬停进出，采样点在复选框命中区。
    def phase_checkbox(session, temp):
        _, _, pixels = session.capture(out, 'checkbox-doc')
        session.key(0x70)  # F7 等无关键不做；直接 Ctrl+Home 回顶部
        cad.user32.keybd_event(0x11, 0, 0, 0)
        cad.user32.keybd_event(0x24, 0, 0, 0)
        cad.user32.keybd_event(0x24, 0, 2, 0)
        cad.user32.keybd_event(0x11, 0, 2, 0)
        time.sleep(.8)
        _, _, pixels = session.capture(out, 'checkbox-doc')
        def dark(a, b):
            return sum(abs(a[i] - b[i]) for i in range(3)) > 60
        pos = None
        for y in range(120, 740, 2):
            for x in range(26, 70, 2):
                c00 = session.rgb(pixels, x, y)
                c10 = session.rgb(pixels, x + 16, y)
                c01 = session.rgb(pixels, x, y + 16)
                c11 = session.rgb(pixels, x + 16, y + 16)
                mid = session.rgb(pixels, x + 8, y + 8)
                bg = session.rgb(pixels, x + 40, y + 8)
                if dark(c00, bg) and dark(c10, bg) and dark(c01, bg) and dark(c11, bg) and not dark(mid, bg):
                    pos = (x + 8, y + 8)
                    break
            if pos:
                break
        result.setdefault('checkbox_pos', pos)
        assert pos, 'checkbox not found'
        session.move(450, 560)
        time.sleep(1.2)
        sampler = Sampler(session, pos[0], pos[1])
        try:
            sampler.start(10)
            time.sleep(1.0)
            lo = sampler.samples[-1]['color']
            for _ in range(12):
                session.move(pos[0], pos[1])
                time.sleep(.08)
                session.move(450, 300)
                time.sleep(.08)
            time.sleep(1.0)
            hi = sampler.samples[-1]['color']
            sampler.join()
        finally:
            sampler.close()
        bad = analyze_within(sampler.samples, lo, hi)
        record('checkbox_hover_cycle', sampler, dict(lo=lo, hi=hi), bad)

    # ---- Phase 3: 在代码块内拖动做跨行选择，采样块背景点。
    def phase_drag(session, temp):
        _, _, pixels = session.capture(out, 'drag-doc')
        band = find_band(session, pixels)
        assert band, 'no code block band'
        y0, y1, color = band
        sampler = Sampler(session, 900, (y0 + y1) // 2)
        try:
            sampler.start(6)
            time.sleep(0.8)
            p0 = wintypes.POINT(round(40 * session.unit), round((y0 + 8) * session.unit))
            cad.user32.ClientToScreen(session.hwnd, ctypes.byref(p0))
            p1 = wintypes.POINT(round(1000 * session.unit), round((y1 - 8) * session.unit))
            cad.user32.ClientToScreen(session.hwnd, ctypes.byref(p1))
            cad.user32.SetCursorPos(p0.x, p0.y)
            time.sleep(.2)
            cad.user32.mouse_event(LDOWN, 0, 0, 0, 0)
            steps = 24
            for i in range(1, steps + 1):
                x = p0.x + (p1.x - p0.x) * i // steps
                y = p0.y + (p1.y - p0.y) * i // steps
                cad.user32.SetCursorPos(x, y)
                time.sleep(.03)
            time.sleep(.5)
            session.capture(out, 'drag-selected')
            cad.user32.mouse_event(LUP, 0, 0, 0, 0)
            time.sleep(1.2)
            session.key(VK_ESC)
            time.sleep(.5)
            sampler.join()
        finally:
            sampler.close()
        record('drag_select', sampler)

    # ---- Phase 4: 粘贴多行文本进代码块，排版逐帧变化。
    def phase_paste(session, temp):
        _, _, pixels = session.capture(out, 'paste-doc')
        band = find_band(session, pixels)
        assert band, 'no code block band'
        y0, y1, color = band
        sampler = Sampler(session, 520, (y0 + y1) // 2)
        try:
            sampler.start(8)
            time.sleep(0.8)
            session.click(60, y0 + 8)
            text = "paste_a\n\npaste_b\n\npaste_c"
            k32 = ctypes.WinDLL('kernel32', use_last_error=True)
            k32.GlobalAlloc.restype = ctypes.c_void_p
            k32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
            k32.GlobalLock.restype = ctypes.c_void_p
            k32.GlobalLock.argtypes = [ctypes.c_void_p]
            k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
            if not cad.user32.OpenClipboard(session.hwnd):
                raise AssertionError('clipboard busy')
            cad.user32.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
            cad.user32.EmptyClipboard(session.hwnd)
            h = k32.GlobalAlloc(0x2002, (len(text) + 1) * 2)
            p = k32.GlobalLock(h)
            ctypes.memmove(p, text.encode('utf-16-le'), len(text) * 2)
            k32.GlobalUnlock(h)
            cad.user32.SetClipboardData(13, h)
            cad.user32.CloseClipboard()
            for _ in range(4):
                cad.user32.keybd_event(VK_CTRL, 0, KEYDOWN, 0)
                cad.user32.keybd_event(VK_V, 0, KEYDOWN, 0)
                cad.user32.keybd_event(VK_V, 0, KEYUP, 0)
                cad.user32.keybd_event(VK_CTRL, 0, KEYUP, 0)
                time.sleep(.35)
            time.sleep(1.5)
            sampler.join()
        finally:
            sampler.close()
        record('paste_into_block', sampler)

    # ---- Phase 5: 滚轮滚动，采样点定位在视口中部代码块带上。
    def phase_scroll(session, temp):
        _, _, pixels = session.capture(out, 'scroll-doc')
        band = find_band(session, pixels)
        assert band, 'no code block band'
        y0, y1, color = band
        mid = (y0 + y1) // 2
        sampler = Sampler(session, 520, mid)
        try:
            sampler.start(7)
            time.sleep(1.0)
            for _ in range(8):
                for _ in range(3):
                    cad.user32.mouse_event(WHEEL, 0, 0, -120, 0)
                    time.sleep(.05)
                time.sleep(.3)
            time.sleep(1.5)
            sampler.join()
        finally:
            sampler.close()
        record('wheel_scroll', sampler)

    # ---- Phase 6: 主菜单标题快速扫过（动画压力），同时采样菜单按钮本身。
    def phase_menu_sweep(session, temp):
        _, _, pixels = session.capture(out, 'menu-doc')
        session.move(450, 400)
        time.sleep(1.2)
        sampler = Sampler(session, 24, 3)
        try:
            sampler.start(9)
            time.sleep(1.0)
            initial = sampler.samples[-1]['color']
            for _ in range(3):
                for x in (24, 78, 132, 186, 240, 400):
                    session.move(x, 17)
                    time.sleep(.08)
            time.sleep(1.2)
            target = sampler.samples[-1]['color']
            sampler.join()
        finally:
            sampler.close()
        bad = analyze_within(sampler.samples, initial, target)
        record('menu_title_sweep', sampler, dict(initial=initial, target=target), bad)

    phases = [('focus_toggle', phase_focus),
              ('checkbox', phase_checkbox),
              ('drag', phase_drag),
              ('paste', phase_paste),
              ('scroll', phase_scroll),
              ('menu_sweep', phase_menu_sweep)]

    for tag, fn in phases:
        if hasattr(fn, 'phase_tag'):
            tag = fn.phase_tag
        with tempfile.TemporaryDirectory(prefix='neo-flicker-') as temp:
            with (out / (tag + '-renderer.log')).open('w', encoding='utf-8') as log:
                session = Session(exe, temp, args.theme, args.scale, log)
                handle = kernel.OpenProcess(0x1000, False, session.proc.pid)
                try:
                    session.wait_window()
                    time.sleep(1.5)
                    fn(session, temp)
                    session.capture(out, tag + '-final')
                finally:
                    cad.user32.mouse_event(LUP, 0, 0, 0, 0)
                    if handle:
                        kernel.CloseHandle(handle)
                    code = session.close()
                    if result['phases'] and 'exit_code' not in result['phases'][-1]:
                        result['phases'][-1]['exit_code'] = code

    (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()

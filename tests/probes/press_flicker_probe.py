"""Press/release flicker probe: pixel timeline on a clickable control.

Samples one screen point on the control while pressing and releasing, then
checks every frame stays between the two settled endpoint colors (rest vs
held). Overshoot beyond either endpoint = the reported mid-press flash.
"""
import argparse
import ctypes
import hashlib
import threading
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from ctypes import wintypes

import win_capture as cad
from capture_markdown import window_for_pid

DOC = """# 按压闪动复现

- [ ] 待办一
- [ ] 待办二

正文段落。
"""
DOC += "".join("\n\n补段 {}：一些文字。\n".format(i) for i in range(20))


class Session:
    def __init__(self, exe, temp, theme, log, animations=1):
        doc = Path(temp) / 'press.md'
        doc.write_text(DOC, encoding='utf-8')
        values = {
            "vault": "", "last_file": str(doc), "mode": "1", "line_numbers": "1",
            "readable_width": "1", "show_status_bar": "1", "editor_font_size": "16",
            "ui_scale": "1", "theme": str(theme),
        }
        # animations < 0 表示故意不写这个键：验证首启是否跟随系统"动画效果"。
        if animations >= 0:
            values["animations"] = str(animations)
        settings = Path(temp) / 'EUI-Edits/settings.ini'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text("".join(f"{k}={v}\n" for k, v in values.items()), encoding='utf-8')
        self.doc = doc
        os.environ.setdefault('NEO_PROBE_ALLOW_FOREIGN', '1')
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1',
                   NEO_LIVE_RESIZE='1', NEO_SINGLE_INSTANCE='0', NEO_ANIM_DEBUG='1')
        self.proc = subprocess.Popen([str(exe)], cwd=str(Path(exe).parent), env=env, stderr=log)
        self.hwnd = None
        self.unit = 1.0
        self.w = 0

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
        time.sleep(2.0)

    def owned(self):
        cad.assert_unlocked('press flicker')
        owner = wintypes.DWORD()
        cad.user32.GetWindowThreadProcessId(self.hwnd, ctypes.byref(owner))
        assert self.proc.poll() is None and owner.value == self.proc.pid and cad.ensure_foreground(self.hwnd)

    def move(self, x, y):
        self.owned()
        p = wintypes.POINT(round(x * self.unit), round(y * self.unit))
        cad.user32.ClientToScreen(self.hwnd, ctypes.byref(p))
        cad.user32.SetCursorPos(p.x, p.y)

    def capture(self, out, label):
        self.owned()
        w, h, pixels = cad.capture_client(self.hwnd)
        cad.write_png(str(out / (label + '.png')), w, h, pixels)
        self.w = w
        return pixels


class Sampler:
    def __init__(self, session, cx, cy):
        p = wintypes.POINT(round(cx * session.unit), round(cy * session.unit))
        cad.user32.ClientToScreen(session.hwnd, ctypes.byref(p))
        self.pt = p
        self.user = ctypes.WinDLL('user32', use_last_error=True)
        self.gdi = ctypes.WinDLL('gdi32', use_last_error=True)
        self.user.GetDC.restype = wintypes.HDC
        self.user.ReleaseDC.argtypes = [wintypes.HDC]
        self.gdi.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
        self.gdi.GetPixel.restype = wintypes.DWORD
        self.dc = self.user.GetDC(None)
        self.samples = []
        self.thread = None
        self.lock = threading.Lock()

    def mark(self, text):
        with self.lock:
            if self.samples:
                self.samples.append(dict(ms=self.samples[-1]['ms'], color='MARK', text=text))

    def _loop(self, stop):
        started = time.perf_counter()
        while not stop:
            c = self.gdi.GetPixel(self.dc, self.pt.x, self.pt.y)
            with self.lock:
                self.samples.append(dict(ms=round((time.perf_counter() - started) * 1000, 1),
                                         color=((c >> 16) & 255, (c >> 8) & 255, c & 255)))

    def start(self):
        import threading
        self.stop = []
        self.thread = threading.Thread(target=self._loop, args=(self.stop,), daemon=True)
        self.thread.start()

    def join(self):
        self.stop.append(1)
        self.thread.join()


def analyze(samples, lo, hi, tol=2):
    bad = []
    for s in samples:
        if not isinstance(s['color'], tuple):
            continue
        for k in range(3):
            if not (min(lo[k], hi[k]) - tol <= s['color'][k] <= max(lo[k], hi[k]) + tol):
                bad.append(s)
                break
    return bad


def press_cycle(session, sampler, cx, cy, out, tag, cycles=3):
    for i in range(cycles):
        session.move(cx, cy)
        time.sleep(.45)
        sampler.mark('press')
        cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(.40)
        sampler.mark('release')
        cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(.7)
        if i == 0:
            session.capture(out, tag + '-held-again')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--animations', type=int, default=1, choices=(-1, 0, 1))
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('press flicker')
    cad.assert_no_foreign_instance('press flicker')
    result = dict(exe=str(exe), exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  theme=args.theme, animations=args.animations, targets=[])
    with tempfile.TemporaryDirectory(prefix='neo-press-') as temp:
        with (out / 'renderer.log').open('w', encoding='utf-8') as log:
            session = Session(exe, temp, args.theme, log, args.animations)
            try:
                session.wait_window()
                pixels = session.capture(out, 'doc')

                def rgb(x, y):
                    pos = (round(y * session.unit) * session.w + round(x * session.unit)) * 4
                    return tuple(pixels[pos:pos + 3])

                # 目标 1：菜单栏"文件"标题（可点击、有按压态）。
                # 目标 2：文档库侧栏文件行。
                # 目标 3：复选框（找方形四角）。
                targets = [('menu_file', 24, 3), ('vault_row', 130, 200), ('settings_btn', 845, 13)]
                def dark(a, b):
                    return sum(abs(a[i] - b[i]) for i in range(3)) > 60
                bg = rgb(400, 300)
                for y in range(120, 500, 2):
                    found = None
                    for x in range(26, 70, 2):
                        c00, c10 = rgb(x, y), rgb(x + 16, y)
                        c01, c11 = rgb(x, y + 16), rgb(x + 16, y + 16)
                        mid = rgb(x + 8, y + 8)
                        if dark(c00, bg) and dark(c10, bg) and dark(c01, bg) and dark(c11, bg) and not dark(mid, bg):
                            found = (x + 8, y + 8)
                            break
                    if found:
                        targets.append(('checkbox', found[0], found[1]))
                        break

                for tag, cx, cy in targets:
                    session.move(cx, cy)
                    time.sleep(.6)
                    pixels = session.capture(out, tag + '-rest')
                    pos = (round(cx * session.unit), round(cy * session.unit))
                    base = tuple(pixels[(pos[1] * session.w + pos[0]) * 4:
                                        (pos[1] * session.w + pos[0]) * 4 + 3])
                    sampler = Sampler(session, cx, cy)
                    try:
                        sampler.start()
                        time.sleep(.5)
                        press_cycle(session, sampler, cx, cy, out, tag)
                        time.sleep(.6)
                        sampler.join()
                    finally:
                        pass
                    # 按住端点：取按压期间（每周期 0.45~0.85s 处）的众数色。
                    held_colors = []
                    for i in range(3):
                        window = [s['color'] for s in sampler.samples
                                  if isinstance(s['color'], tuple) and 500 + i * 1550 < s['ms'] < 750 + i * 1550]
                        if window:
                            held_colors.append(window[len(window) // 2])
                    held = held_colors[0] if held_colors else base
                    bad = analyze(sampler.samples, base, held)
                    result['targets'].append(dict(target=tag, point=[cx, cy], base=base, held=held,
                                                  samples=len(sampler.samples),
                                                  distinct=len({tuple(s['color']) for s in sampler.samples}),
                                                  excursions=len(bad), first=bad[:6],
                                                  timeline=sampler.samples))
                    print(tag, 'base', base, 'held', held, 'excursions', len(bad))
                    for s in bad[:6]:
                        print('   ', s)
            finally:
                cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                for _ in range(3):
                    cad.user32.keybd_event(0x1b, 0, 0, 0)
                    cad.user32.keybd_event(0x1b, 0, 2, 0)
                    time.sleep(.15)
                if session.hwnd and session.proc.poll() is None:
                    cad.user32.PostMessageW(session.hwnd, 0x0010, 0, 0)
                try:
                    session.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    session.proc.terminate()  # 只终止探针自己启动的进程
                    session.proc.wait(timeout=10)
                result['exit_code'] = session.proc.poll()
    (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('exit', result['exit_code'])


if __name__ == '__main__':
    main()

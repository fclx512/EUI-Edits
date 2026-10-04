"""Focused capture probe for the association list scroll alignment.

Launches the candidate with a disposable APPDATA, opens the file-system
settings page and captures the association list while wheeling up and down.
Read-only with respect to associations: nothing is clicked that applies
registration changes.
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

import win_capture as cad
from capture_markdown import window_for_pid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--scale', type=float, default=1)
    parser.add_argument('--font', type=int, default=14)
    parser.add_argument('--language', default='en')
    args = parser.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('association scroll probe')
    cad.assert_no_foreign_instance('association scroll probe')
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), steps=[])
    with tempfile.TemporaryDirectory(prefix='neo-assoc-scroll-') as temp:
        doc = Path(temp) / 'scroll.md'
        doc.write_text('# Scroll probe\n\nNo document changes.\n', encoding='utf-8')
        settings = Path(temp) / 'EUI-Edits/settings.ini'
        settings.parent.mkdir()
        settings.write_text(f'vault=\nlast_file={doc}\nmode=0\nui_scale={args.scale}\n'
                            f'ui_font_size={args.font}\ntheme=1\nanimations=0\nui_language={args.language}\n',
                            encoding='utf-8')
        env = dict(os.environ, APPDATA=temp, NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
        proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
        hwnd = None
        state = dict(unit=1.0)

        def owned():
            cad.assert_unlocked('association scroll probe')
            assert proc.poll() is None
            pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            assert pid.value == proc.pid and cad.ensure_foreground(hwnd), 'owned foreground lost'

        def click(x, y):
            owned()
            pt = cad.wintypes.POINT(round(x * state['unit']), round(y * state['unit']))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.SetCursorPos(pt.x, pt.y)
            time.sleep(.08)
            cad.user32.mouse_event(2, 0, 0, 0, 0)
            time.sleep(.06)
            cad.user32.mouse_event(4, 0, 0, 0, 0)
            time.sleep(.5)

        def capture(name):
            owned()
            pt = cad.wintypes.POINT(5, 8)
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.SetCursorPos(pt.x, pt.y)
            time.sleep(.5)
            pixels = cad.capture_client(hwnd)
            cad.write_png(str(out / f'{name}.png'), *pixels)
            result['steps'].append(name)
            return pixels

        def wheel(x, y, delta, repeats=1, settle=.45):
            owned()
            pt = cad.wintypes.POINT(round(x * state['unit']), round(y * state['unit']))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.SetCursorPos(pt.x, pt.y)
            time.sleep(.15)
            for _ in range(repeats):
                cad.user32.mouse_event(0x0800, 0, 0, ctypes.c_ulong(delta & 0xFFFFFFFF).value, 0)
                time.sleep(.12)
            if settle:
                time.sleep(settle)

        try:
            for _ in range(120):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            assert hwnd, 'editor window did not appear'
            owned()
            cad.user32.SetWindowPos(hwnd, None, 40, 40, 1400, 1080, 4)
            time.sleep(1)
            dpi = cad.user32.GetDpiForWindow(hwnd)
            state['unit'] = dpi / 96 * args.scale
            for vk, down in ((0x11, True), (0xBC, True), (0xBC, False), (0x11, False)):
                cad.user32.keybd_event(vk, 0, 0 if down else 2, 0)
                time.sleep(.05)
            time.sleep(.8)
            capture('settings-open')
            click(85, 208)  # file/system nav in the wide sidebar
            capture('filesystem-page')
            # 列表内部下滚建立偏移，再向上滚抓瞬时与稳态帧（坐标均为逻辑像素，
            # 列表视口约 (220,520)-(1061,736)）。
            wheel(400, 600, -120, 4)
            capture('down4-settled')
            wheel(400, 600, -120, 4)
            capture('down8-settled')
            # 触控板式上划：连续小增量 + 中途连拍（不加 settle 等待）。
            owned()
            pt = cad.wintypes.POINT(round(400 * state['unit']), round(600 * state['unit']))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.SetCursorPos(pt.x, pt.y)
            time.sleep(.15)
            for i in range(24):
                cad.user32.mouse_event(0x0800, 0, 0, ctypes.c_ulong(30 & 0xFFFFFFFF).value, 0)
                time.sleep(.016)
                if i in (6, 12, 18, 23):
                    pixels = cad.capture_client(hwnd)
                    cad.write_png(str(out / f'swipe-mid{i}.png'), *pixels)
            time.sleep(.6)
            capture('swipe-settled')
            # 回到列表顶部再截一帧整页：看每页可见行数与搜索框文字的纵向位置。
            wheel(400, 600, 120, 12)
            capture('page-top')
            # 风险确认弹窗（默认 title/message 路径）：只弹出不确认，验证自适应
            # 宽高与正文断词后按 Escape 取消。
            click(957, 603)  # "Clear all associations"（页面顶部的按钮行）
            time.sleep(.6)
            capture('risk-dialog')
            for vk, down in ((0x1B, True), (0x1B, False)):
                cad.user32.keybd_event(vk, 0, 0 if down else 2, 0)
                time.sleep(.05)
            time.sleep(.5)
            capture('risk-dismissed')
        finally:
            if hwnd and proc.poll() is None:
                cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                try:
                    proc.wait(timeout=10)
                except Exception:
                    proc.kill()
            result['exit'] = proc.returncode
            (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2),
                                                 encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()

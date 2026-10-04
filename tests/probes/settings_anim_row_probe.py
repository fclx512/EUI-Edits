"""Capture the settings panel (Appearance) to confirm the new 动画效果 row renders.

Also clicks the 关闭 segment of that row and reads settings.ini back, so the whole
UI -> persist chain is covered, not just the file-write path the other probes use.
"""
import argparse
import ctypes
import hashlib
import json
import subprocess
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

import win_capture as cad
from capture_markdown import window_for_pid

DOC = "# 设置面板\n\n正文。\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--click', type=int, default=1, help='1 = 点"动画效果"行的关闭段')
    ap.add_argument('--click-x', type=float, default=0.0)
    ap.add_argument('--click-y', type=float, default=0.0)
    ap.add_argument('--scroll', type=int, default=0, help='面板内容区滚轮下滚的档数')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('settings anim row')
    cad.assert_no_foreign_instance('settings anim row')
    import os
    os.environ.setdefault('NEO_PROBE_ALLOW_FOREIGN', '1')

    with tempfile.TemporaryDirectory(prefix='neo-setrow-') as temp:
        doc = Path(temp) / 'setrow.md'
        doc.write_text(DOC, encoding='utf-8')
        values = {"vault": "", "last_file": str(doc), "mode": "0", "line_numbers": "1",
                  "readable_width": "1", "show_status_bar": "1", "ui_scale": "1",
                  "theme": str(args.theme), "animations": "1"}
        settings = Path(temp) / 'EUI-Edits/settings.ini'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text("".join(f"{k}={v}\n" for k, v in values.items()), encoding='utf-8')
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1',
                   NEO_LIVE_RESIZE='1', NEO_SINGLE_INSTANCE='0')
        result = dict(exe=str(exe), exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                      theme=args.theme)
        with (out / 'renderer.log').open('w', encoding='utf-8') as log:
            proc = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stderr=log)
            hwnd = None
            try:
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

                # Ctrl+, 打开设置面板（VisualPointer 复核过这个 chord）。
                for vk, down in ((0x11, True), (0xBC, True), (0xBC, False), (0x11, False)):
                    cad.user32.keybd_event(vk, 0, 0 if down else 2, 0)
                    time.sleep(.05)
                time.sleep(1.5)

                # 设置面板是滚动区：外观页第 5 行在折叠之下，先滚下去再截图。
                if args.scroll:
                    p = wintypes.POINT(round(550 * unit), round(430 * unit))
                    cad.user32.ClientToScreen(hwnd, ctypes.byref(p))
                    cad.user32.SetCursorPos(p.x, p.y)
                    time.sleep(.4)
                    for _ in range(args.scroll):
                        cad.user32.mouse_event(0x0800, 0, 0, ctypes.c_ulong(0xFFFFFF88).value, 0)
                        time.sleep(.18)
                    time.sleep(1.0)

                w, h, pixels = cad.capture_client(hwnd)
                cad.write_png(str(out / 'settings-appearance.png'), w, h, pixels)
                result['client'] = [w, h]

                if args.click:
                    def click(x, y):
                        p = wintypes.POINT(round(x * unit), round(y * unit))
                        cad.user32.ClientToScreen(hwnd, ctypes.byref(p))
                        cad.user32.SetCursorPos(p.x, p.y)
                        time.sleep(.35)
                        cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
                        time.sleep(.10)
                        cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                    # 逻辑坐标：面板居中，行高 84；外观分类第 5 行右侧段位 = "关闭"。
                    click(args.click_x, args.click_y)
                    time.sleep(1.2)
                    w2, h2, px2 = cad.capture_client(hwnd)
                    cad.write_png(str(out / 'settings-after-click.png'), w2, h2, px2)
                    time.sleep(0.8)
                    result['settings_after'] = settings.read_text(encoding='utf-8')
            finally:
                for _ in range(3):
                    cad.user32.keybd_event(0x1b, 0, 0, 0)
                    cad.user32.keybd_event(0x1b, 0, 2, 0)
                    time.sleep(.15)
                if hwnd and proc.poll() is None:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    proc.wait(timeout=10)
        (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2),
                                             encoding='utf-8')
        print(json.dumps({k: v for k, v in result.items() if k != 'settings_after'},
                         ensure_ascii=False))


if __name__ == '__main__':
    main()

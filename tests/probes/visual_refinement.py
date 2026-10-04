"""Real-window menu-leave pixels and responsive settings review, with isolated data."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from PIL import Image
import win_capture as cad
from capture_markdown import window_for_pid, write_settings


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--theme', type=int, default=1)
    parser.add_argument('--scale', type=float, default=1)
    parser.add_argument('--ui-font', type=int, default=14)
    args = parser.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('visual refinement')
    cad.assert_no_foreign_instance('visual refinement')
    results = []
    with tempfile.TemporaryDirectory(prefix='neo-refinement-') as runtime:
        doc = Path(runtime)/'review.md'
        source = '# 视觉复核\n\n## 菜单与设置\n\n用于检查图标、悬停和设置布局。\n'
        doc.write_text(source, encoding='utf-8')
        (Path(runtime)/'notes.txt').write_text('Text', encoding='utf-8')
        settings = Path(runtime)/'EUI-Edits/settings.ini'
        write_settings(str(settings), str(doc), args.theme, 16, str(args.scale))
        with settings.open('a', encoding='utf-8') as target:
            target.write(f'mode=1\nshow_status_bar=1\nui_font_size={args.ui_font}\n')
        env = dict(os.environ, APPDATA=runtime, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
        process = subprocess.Popen([str(exe)], env=env, cwd=exe.parent)
        hwnd = None

        def owned():
            cad.assert_unlocked('visual refinement')
            owner = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            assert process.poll() is None and owner.value == process.pid and cad.ensure_foreground(hwnd)

        def move(x, y):
            owned()
            point = cad.wintypes.POINT(round(x*unit), round(y*unit))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
            cad.user32.SetCursorPos(point.x, point.y)

        def click(x, y):
            owned()
            w, h, _ = cad.capture_client(hwnd)
            assert 0 <= x*unit < w and 0 <= y*unit < h
            cad.click(hwnd, round(x*unit), round(y*unit))

        def key(*keys):
            owned()
            for k in keys: cad.user32.keybd_event(k, 0, 0, 0)
            for k in reversed(keys): cad.user32.keybd_event(k, 0, 2, 0)
            time.sleep(.3)

        def capture(name, delay=.4):
            owned()
            time.sleep(delay)
            w, h, pixels = cad.capture_client(hwnd)
            path = out/(name+'.png')
            cad.write_png(str(path), w, h, pixels)
            return Image.open(path).convert('RGB')

        def check(name, passed):
            results.append(dict(name=name, passed=bool(passed)))
            assert passed, name

        def nav(index):
            w, _, _ = cad.capture_client(hwnd)
            lw = w/unit
            if lw < 840: click(12+(index+.5)*(lw-24)/3, 72+20)
            else: click(80, 72+20+index*48+20)

        def scroll(direction, count=20):
            w, h, _ = cad.capture_client(hwnd)
            move(w/unit*.75, h/unit*.65)
            time.sleep(.4)
            for _ in range(count):
                cad.user32.mouse_event(0x0800, 0, 0, (direction*120)&0xffffffff, 0)
                time.sleep(.12)

        try:
            for _ in range(100):
                hwnd = window_for_pid(process.pid)
                if hwnd or process.poll() is not None: break
                time.sleep(.1)
            assert hwnd
            owned()
            cad.user32.SetWindowPos(hwnd, None, 0, 0, 1400, 900, 0x0002|0x0004)
            time.sleep(.8)
            unit = cad.user32.GetDpiForWindow(hwnd)/96*args.scale
            initial = capture('files-arrow')
            if args.ui_font == 14:
                move(initial.width/unit-30, 110)
                tabs_base = capture('tabs-no-hover')
                move(230, 58)
                tabs_hover = capture('tabs-hover', .03)
                if args.theme == 1:
                    pixel = tabs_hover.getpixel((round(244*unit), round(58*unit)))
                    check('tab hover is immediate and uses requested HSL',
                          max(abs(a-b) for a,b in zip(pixel,(233,234,235))) <= 1)
                move(initial.width/unit-30, 110)
                tabs_left = capture('tabs-left', .03)
                tabs_bounds = tuple(round(v*unit) for v in (135, 45, 249, 71))
                check('tab hover clears immediately',
                      tabs_base.crop(tabs_bounds).tobytes() == tabs_left.crop(tabs_bounds).tobytes())
                move(106, 216)
                time.sleep(.2)
                cad.user32.mouse_event(0x0008, 0, 0, 0, 0)
                cad.user32.mouse_event(0x0010, 0, 0, 0, 0)
                move(initial.width/unit-30, 110)
                base = capture('menu-no-hover')
                bounds = tuple(round(v*unit) for v in (110, 220, 310, 390))
                move(150, 374)
                hovered = capture('menu-last-hover', .03)
                if args.theme == 1:
                    pixel = hovered.getpixel((round(294*unit), round(374*unit)))
                    check('light hover matches requested HSL', max(abs(a-b) for a,b in zip(pixel,(233,234,235))) <= 1)
                move(initial.width/unit-30, 110)
                for frame in range(4):
                    left = capture(f'menu-left-{frame}', .03)
                    check(f'leave frame {frame} has no moving or residual highlight',
                          base.crop(bounds).tobytes() == left.crop(bounds).tobytes())
                key(0x1B)
                capture('menu-dismissed')
            key(0x11, 0xBC)
            appearance = capture('settings-appearance-wide')
            scroll(-1)
            bottom = capture('settings-appearance-bottom-wide')
            check('settings scroll changes the visible content',
                  appearance.crop((appearance.width//2, 160, appearance.width-30, appearance.height-25)).tobytes() !=
                  bottom.crop((bottom.width//2, 160, bottom.width-30, bottom.height-25)).tobytes())
            # At the scroll end the font card sits directly above the reset card.
            # Derive its control center from the same row geometry as the main page.
            lw, lh = bottom.width/unit, bottom.height/unit
            page_width = min(952, lw-(0 if lw < 840 else 176))
            content_width = page_width-52
            control_width = min(max(0, content_width-36), max(224, args.ui_font*16))
            stacked = content_width < control_width+args.ui_font*21+60
            label_font, hint_font = args.ui_font+3, max(10, args.ui_font-2)
            text_bottom = 18+label_font+7+6+(hint_font+5)*2
            card_height = text_bottom+18+(48 if stacked else 0)
            control_top = text_bottom+14 if stacked else (card_height-34)*.5
            # Main content ends with a 26-DIP bottom padding, then one reset row.
            font_control_y = lh-26-card_height-(card_height+12)+control_top+17
            click(lw-156 if not stacked else lw*.55, font_control_y)
            capture('settings-font-picker-wide', 1)
            cad.user32.SetWindowPos(hwnd, None, 0, 0, 950, 720, 0x0002|0x0004)
            capture('settings-font-picker-narrow', .6)
            key(0x1B)
            cad.user32.SetWindowPos(hwnd, None, 0, 0, 1400, 900, 0x0002|0x0004)
            key(0x11, 0xBC)
            nav(1)
            capture('settings-editor-wide')
            nav(2)
            capture('settings-system-wide')
            scroll(-1)
            capture('settings-system-bottom-wide')
            cad.user32.SetWindowPos(hwnd, None, 0, 0, 950, 720, 0x0002|0x0004)
            nav(0)
            capture('settings-appearance-narrow')
            scroll(-1)
            capture('settings-appearance-bottom-narrow')
            nav(2)
            capture('settings-system-narrow')
            scroll(-1)
            capture('settings-system-bottom-narrow')
            key(0x1B)
            capture('returned-editor')
            check('visual review preserves document bytes', doc.read_text(encoding='utf-8') == source)
        finally:
            if hwnd and process.poll() is None: cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
            process.wait(timeout=10)
            (out/'results.json').write_text(json.dumps(dict(exe=str(exe),
                sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), theme=args.theme,
                scale=args.scale, ui_font=args.ui_font, tests=results,
                exit_code=process.returncode), indent=2), encoding='utf-8')


if __name__ == '__main__': main()

"""Owned-window UI acceptance on temporary documents; never operates a user's instance."""
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
from capture_markdown import window_for_pid, write_settings


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--theme', type=int, default=1)
    parser.add_argument('--scale', type=float, default=1)
    args = parser.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('visual UI review')
    cad.assert_no_foreign_instance('visual UI review')
    source = ('# 视觉设计与交互\n\nAlpha alpha ALPHA alphabet\n\n'
              '## 清晰的视觉层级\n\n中文搜索结果，搜索！（搜索）\n\n'
              '### 减少视觉噪声\n\n段落示例。\n\n#### 四级标题\n\n'
              '##### 五级标题\n\n###### 六级标题\n\n# 另一个章节\n\n结尾。\n')
    results = []
    with tempfile.TemporaryDirectory(prefix='neo-visual-review-') as temp:
        doc = Path(temp) / 'visual-review.md'
        doc.write_text(source, encoding='utf-8')
        (Path(temp) / 'notes.txt').write_text('Text document', encoding='utf-8')
        settings = Path(temp) / 'EUI-Edits/settings.ini'
        write_settings(str(settings), str(doc), args.theme, 16, str(args.scale))
        with settings.open('a', encoding='utf-8') as f:
            f.write('mode=1\nshow_status_bar=1\nreadable_width=1\n')
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='0')
        process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
        hwnd = None
        def owned():
            cad.assert_unlocked('visual UI review')
            assert process.poll() is None and cad.ensure_foreground(hwnd), 'Owned foreground lost'
            pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            assert pid.value == process.pid
        def chord(*keys):
            owned()
            for key in keys: cad.user32.keybd_event(key, 0, 0, 0)
            for key in reversed(keys): cad.user32.keybd_event(key, 0, 2, 0)
            time.sleep(.25)
        def click(x, y):
            owned()
            w, h, _ = cad.capture_client(hwnd)
            assert 0 <= x*unit < w and 0 <= y*unit < h, 'Click outside owned client'
            cad.click(hwnd, round(x*unit), round(y*unit))
            time.sleep(.25)
        def move(x, y):
            owned()
            point = cad.wintypes.POINT(round(x*unit), round(y*unit))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
            cad.user32.SetCursorPos(point.x, point.y)
        def text(value):
            owned()
            for c in value: cad.user32.SendMessageW(hwnd, 0x0102, ord(c), 0)
            time.sleep(.35)
        def capture(label, delay=.4):
            owned()
            time.sleep(delay)
            w, h, pixels = cad.capture_client(hwnd)
            cad.write_png(str(out / (label + '.png')), w, h, pixels)
        def saved():
            chord(0x11, 0x53)
            return doc.read_text(encoding='utf-8')
        def check(name, condition):
            results.append({'name': name, 'passed': bool(condition)})
            assert condition, name
        try:
            for _ in range(100):
                hwnd = window_for_pid(process.pid)
                if hwnd or process.poll() is not None: break
                time.sleep(.1)
            assert hwnd, 'Window missing'
            owned()
            cad.user32.SetWindowPos(hwnd, None, 0, 0, 1400, 900, 0x0002 | 0x0004)
            time.sleep(1)
            unit = cad.user32.GetDpiForWindow(hwnd) / 96 * args.scale
            w, h, _ = cad.capture_client(hwnd)
            lw = w / unit
            capture('files')
            click(35, 17)
            capture('menu-file')
            chord(0x1B)
            click(84, 17)
            capture('menu-edit')
            chord(0x1B)
            click(185, 58)
            capture('outline')
            click(180, 183)
            capture('outline-jump')
            chord(0x11, 0x46)
            text('Alpha')
            capture('find')
            fw = min(550, max(280, lw - min(264, lw*.38) - 1 - 28))
            fx = lw - fw - 14
            find_y, find_top, field_height = 46, 8, 32
            replacement_top = 48
            options_button_x = fx + fw - 56
            options_menu_x = fx + fw - 240
            options_menu_y, options_menu_width = find_y + find_top + field_height + 6, 210
            options_inset, options_row_height = 6, 32
            def choose_find_option(index, capture_menu=False):
                # Case, whole-word, and wrap live in a transient menu; each choice
                # closes it, so reopen the options button for every setting.
                click(options_button_x, find_y + find_top + field_height*.5)
                if capture_menu:
                    capture('find-options')
                click(options_menu_x + options_menu_width*.5,
                      options_menu_y + options_inset + index*options_row_height + options_row_height*.5)

            # Expanded replacement via Ctrl+H, then case and whole-word toggles.
            chord(0x11, 0x48)
            capture('replace')
            choose_find_option(0, capture_menu=True)
            choose_find_option(1)
            click(fx + 80, find_y + replacement_top + field_height*.5)
            text('Beta')
            click(fx + fw - 48, find_y + replacement_top + field_height*.5)
            capture('replace-sensitive-word')
            check('case-sensitive whole-word replacement', saved() == source.replace('Alpha', 'Beta'))
            chord(0x1B)
            chord(0x11, 0x5A)
            check('replace all is one undo step', saved() == source)
            chord(0x11, 0x48)
            # Turn case sensitivity and whole-word off, replace all four spans.
            choose_find_option(0)
            choose_find_option(1)
            click(fx + fw - 48, find_y + replacement_top + field_height*.5)
            check('case-insensitive replace all', saved() == source.replace('Alpha', 'Beta').replace('alpha', 'Beta').replace('ALPHA', 'Beta'))
            chord(0x1B)
            chord(0x11, 0x5A)
            check('second replacement undo restores document', saved() == source)
            chord(0x11, 0x24)  # move the document caret to its start
            chord(0x11, 0x48)
            choose_find_option(2)  # disable wrap
            for _ in range(6): click(fx + fw - 120, find_y + find_top + field_height*.5)  # next, past final match
            capture('find-no-wrap-end')
            click(fx + fw - 122, find_y + replacement_top + field_height*.5)  # replace one, not all
            check('non-wrapping navigation remains at the last match', saved() == source.replace('alphabet', 'Betabet'))
            chord(0x1B)
            chord(0x11, 0x5A)
            check('single replacement undo', saved() == source)
            chord(0x11, 0x24)
            chord(0x11, 0x48)
            click(fx + 80, find_y + find_top + field_height*.5)  # focus query
            chord(0x11, 0x41)
            text('Alpha')
            chord(0x0D)
            chord(0x10, 0x0D)  # return to the first match
            click(fx + fw - 122, find_y + replacement_top + field_height*.5)
            check('Enter and Shift+Enter navigate both directions', saved() == source.replace('Alpha', 'Beta'))
            chord(0x1B)
            chord(0x11, 0x5A)
            check('keyboard navigation replacement undo', saved() == source)
            capture('outline-after-undo')
            click(62, 58)
            # Right-click the inactive text document; capture live highlight motion.
            move(106, 216)
            cad.user32.mouse_event(0x0008, 0, 0, 0, 0)
            cad.user32.mouse_event(0x0010, 0, 0, 0, 0)
            capture('context-menu')
            move(150, 264)
            capture('context-hover-first')
            move(150, 296)
            for frame in range(5): capture(f'context-transition-{frame}', .025)
            click(lw - 30, min(490, h/unit - 80))
            move(lw - 40, min(490, h/unit - 80))
            capture('context-dismissed')
            # A second document refresh is deliberately unnecessary here.
            chord(0x11, 0x46)
            text('搜索')
            capture('find-chinese')
            chord(0x10, 0x0D)
            capture('find-previous')
            cad.user32.SetWindowPos(hwnd, None, 0, 0, 950, 720, 0x0002 | 0x0004)
            capture('find-narrow')
            chord(0x1B)
            check('UI-only operations preserve document', saved() == source)
            w, h, _ = cad.capture_client(hwnd)
            click(min(264, w/unit*.38)*.75, 58)
            capture('outline-narrow', 3)
            chord(0x11, 0xBC)
            # Compact settings navigation: file/system is the third segment.
            w, h, _ = cad.capture_client(hwnd)
            click(w/unit*.83, 85)
            capture('settings-document-icons')
            move(w/unit*.75, h/unit*.65)
            time.sleep(.5)
            for _ in range(8):
                cad.user32.mouse_event(0x0800, 0, 0, (-120) & 0xffffffff, 0)
                time.sleep(.12)
            capture('settings-document-icons-scrolled')
        finally:
            if hwnd and process.poll() is None:
                cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired: pass
            (out / 'results.json').write_text(json.dumps({
                'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                'theme': args.theme, 'scale': args.scale, 'tests': results,
                'exit_code': process.poll()}, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__': main()

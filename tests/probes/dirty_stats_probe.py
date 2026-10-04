"""Read the debug title stats (Dirty area%, Full%) around interactions.

Quantifies how much area each interaction repaints: idle vs click-to-focus vs
menu hover vs typing. Disposable document; owned process closes normally.
"""
import argparse
import ctypes
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import time
from ctypes import wintypes

import win_capture as cad
from capture_markdown import window_for_pid

DOC = """# 脏区测量

```cpp
int count = 1;
count++;
return count;
```

> 引用行

- [ ] 待办

收尾段落。
"""
DOC += "".join("\n\n补段 {}：一些文字让排版稳定，包含较长的文字行内容。\n".format(i) for i in range(40))

TITLE_RE = re.compile(r"Dirty ([\d.]+)/([\d.]+)%.*?Full ([\d.]+)%.*?Cache ([\d.]+)%.*?Re ([\d.]+)%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('dirty stats')
    cad.assert_no_foreign_instance('dirty stats')
    with tempfile.TemporaryDirectory(prefix='neo-dirty-') as temp:
        doc = Path(temp) / 'dirty.md'
        doc.write_text(DOC, encoding='utf-8')
        values = {
            "vault": "", "last_file": str(doc), "mode": "1", "line_numbers": "1",
            "readable_width": "1", "show_status_bar": "1", "editor_font_size": "16",
            "ui_scale": "1", "theme": str(args.theme),
        }
        settings = Path(temp) / 'EUI-Edits/settings.ini'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text("".join(f"{k}={v}\n" for k, v in values.items()), encoding='utf-8')
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
            time.sleep(2.5)

            def title():
                buf = ctypes.create_unicode_buffer(2048)
                cad.user32.GetWindowTextW(hwnd, buf, 2048)
                return buf.value

            def move(x, y):
                p = wintypes.POINT(round(x * unit), round(y * unit))
                cad.user32.ClientToScreen(hwnd, ctypes.byref(p))
                cad.user32.SetCursorPos(p.x, p.y)

            def click(x, y):
                move(x, y)
                time.sleep(.1)
                cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
                cad.user32.mouse_event(0x0004, 0, 0, 0, 0)

            phases = []
            current_title = {'text': ''}

            def collect(seconds, label, action=None):
                samples = []
                end = time.perf_counter() + seconds
                fired = False
                while time.perf_counter() < end:
                    t = title()
                    if t != current_title['text']:
                        current_title['text'] = t
                        m = TITLE_RE.search(t.replace(' | ', ' '))
                        entry = dict(ms=round((time.perf_counter() - end + seconds) * 1000), text=t[:220])
                        if m:
                            entry.update(dirty_rects=m.group(1), dirty_area_pct=m.group(2),
                                         full_pct=m.group(3), cache_pct=m.group(4))
                        samples.append(entry)
                    if action and not fired and time.perf_counter() > end - seconds + 0.6:
                        action()
                        fired = True
                    time.sleep(.05)
                phases.append(dict(phase=label, titles=samples))
                print(f"--- {label}: {len(samples)} title updates")
                for s in samples:
                    print('   ', s.get('dirty_rects'), s.get('dirty_area_pct'), 'full', s.get('full_pct'),
                          '|', s['text'][:150])

            collect(3.0, 'idle')
            collect(2.5, 'click_focus_editor', action=lambda: click(450, 500))
            collect(2.5, 'click_menubar_defocus', action=lambda: click(560, 8))
            collect(3.0, 'hover_menu_file', action=lambda: move(24, 17))
            collect(3.0, 'hover_vault_row', action=lambda: move(130, 200))

            result = dict(phases=phases, exit_code=proc.wait(timeout=15) if
                          (cad.user32.PostMessageW(hwnd, 0x0010, 0, 0) or True) else None)
            (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2),
                                                 encoding='utf-8')
            print('exit', result['exit_code'])


if __name__ == '__main__':
    main()

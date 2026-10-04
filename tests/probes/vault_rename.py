"""Real F2 rename workflow. Isolated files/settings; never touches user windows.

Run from the repository root. The stem-selection check is behavioral: typing
without Ctrl+A must replace the stem while retaining .md. Captures supplement
the real filename/content checks; they do not assert pixel-perfect selection.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import win_capture as cad
from capture_markdown import window_for_pid
from text_files import registry_snapshot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, choices=(0, 1), default=1)
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--language', choices=('en', 'zh-CN'), default='en')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    docs, temp, appdata = out / 'documents', out / 'temp', out / 'appdata'
    docs.mkdir(); temp.mkdir(); (appdata / 'EUI-Edits').mkdir(parents=True)
    original = b'# Rename fixture\n\nText stays in the editor.\n'
    first = docs / '01-stem.md'
    first.write_bytes(original)
    taken = docs / '03-taken.md'
    taken.write_bytes(b'# Other document\n')
    (appdata / 'EUI-Edits' / 'settings.ini').write_text(
        f'last_file={first}\nmode=1\ntheme={args.theme}\nui_scale={args.scale}\n'
        f'ui_language={args.language}\nui_font_size=14\neditor_font_size=16\n'
        'animations=0\nreadable_width=0\nline_numbers=0\nshow_status_bar=1\nvault_width=264\n',
        encoding='utf-8')
    cad.make_dpi_aware(); cad.assert_unlocked('vault rename')
    before_registry = registry_snapshot()
    env = dict(os.environ, APPDATA=str(appdata), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
    report = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  theme=args.theme, scale=args.scale, language=args.language,
                  checks=[], captures=[], normal_exit=False)
    proc = subprocess.Popen([str(exe), str(first)], cwd=exe.parent, env=env)
    hwnd = None
    cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
    cad.user32.WindowFromPoint.restype = ctypes.c_void_p
    cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    cad.user32.GetAncestor.restype = ctypes.c_void_p

    def owned():
        cad.assert_unlocked('vault rename')
        pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        assert proc.poll() is None and pid.value == proc.pid and cad.ensure_foreground(hwnd), 'PID/foreground lost'

    def check(name, passed):
        report['checks'].append(dict(name=name, passed=bool(passed)))
        print(name, bool(passed), flush=True)
        assert passed, name

    def point(x, y):
        owned()
        pt = cad.wintypes.POINT(round(x), round(y))
        cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
        assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(pt), 2) == hwnd, 'client obscured'
        return pt

    def click(x, y):
        point(x * unit, y * unit)
        cad.click(hwnd, round(x * unit), round(y * unit))
        time.sleep(.3)

    def key(*codes):
        owned()
        for code in codes:
            cad.user32.keybd_event(code, 0, 0, 0); time.sleep(.025)
        for code in reversed(codes):
            cad.user32.keybd_event(code, 0, 2, 0); time.sleep(.025)
        time.sleep(.35)

    def text(value):
        owned()
        for c in value:
            # BMP-only fixtures: WM_CHAR exercises the real custom input model.
            cad.user32.PostMessageW(hwnd, 0x0102, ord(c), 1)
        time.sleep(.35)

    def capture(label):
        owned(); time.sleep(.25)
        w, h, pixels = cad.capture_client(hwnd)
        for x in (14, w * .25, w * .5, w * .75, w - 24):
            for y in (14, h * .25, h * .5, h * .75, h - 24):
                point(x, y)
        cad.write_png(str(out / (label + '.png')), w, h, pixels)
        report['captures'].append(dict(label=label, width=w, height=h))
        return w / unit, h / unit

    def row(name):
        # This fixture contains only two files and no folders/filters. The source
        # sorts file names by ASCII case; ui_font_size=14 fixes rowHeight at 26.
        names = sorted((p.name for p in docs.iterdir()), key=lambda n: (n.lower(), n))
        index = names.index(name)
        # Menu 34 + panel inset 8 + tabs 33/header26/address24/filter28 + four gaps6.
        click(110, 34 + 8 + 33 + 26 + 24 + 28 + 24 + 13 + index * 26)

    def start_rename(name):
        row(name); key(0x71)  # F2: only the genuinely focused row handles it.
        capture('prompt-' + str(len(report['captures'])))

    try:
        for _ in range(150):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None: break
            time.sleep(.1)
        assert hwnd, f'window missing, exit={proc.poll()}'
        cad.user32.SetWindowPos(hwnd, None, 25, 25, 1280, 900, 4)
        time.sleep(1)
        unit = cad.user32.GetDpiForWindow(hwnd) / 96 * args.scale
        report.update(pid=proc.pid, dpi=cad.user32.GetDpiForWindow(hwnd))
        width, height = capture('initial')

        start_rename('01-stem.md')
        capture('stem-selected')
        text('02-renamed'); key(0x0D)
        renamed = docs / '02-renamed.md'
        check('F2 selects filename stem while preserving extension', renamed.exists() and not first.exists() and renamed.read_bytes() == original)

        # F2 after completion also checks that focus returned to the renamed row.
        key(0x71); key(0x11, 0x41); text('03-taken.md'); key(0x0D)
        capture('conflict-suggestion')
        check('conflict retains both original names and bytes', renamed.exists() and taken.read_bytes() == b'# Other document\n' and not (docs / '03-taken (2).md').exists())
        panel_width = min(520, max(280, width - 48))
        panel_height = min(280, max(184, height - 48))
        panel_x, panel_y = (width - panel_width) / 2, (height - panel_height) / 2
        button_width = (panel_width - 40 - 16) / 3
        click(panel_x + 20 + button_width / 2, panel_y + panel_height - 54 + 16)
        numbered = docs / '03-taken (2).md'
        check('explicit numbered button renames without overwrite', numbered.exists() and not renamed.exists() and numbered.read_bytes() == original and taken.read_bytes() == b'# Other document\n')
        capture('numbered-complete')

        key(0x71); text('cancel-this'); key(0x1B)
        check('Escape cancels instead of confirming input', numbered.exists() and not (docs / 'cancel-this.md').exists())
        capture('escape-canceled')

        start_rename(numbered.name)
        key(0x11, 0x41); text('04-changed.txt'); key(0x0D)
        changed = docs / '04-changed.txt'
        check('extension change waits for confirmation', numbered.exists() and not changed.exists())
        capture('extension-warning')
        key(0x0D)
        check('confirmed extension change completes', changed.exists() and not numbered.exists() and changed.read_bytes() == original)

        click(360, 110); key(0x11, 0x23); text('BODY_F2_'); key(0x71); text('STILL_EDITOR'); key(0x11, 0x53)
        check('F2 with document focus never renames stale selected row', changed.exists() and b'BODY_F2_STILL_EDITOR' in changed.read_bytes())
        text('DIRTY_BEFORE_F2')
        old_disk = changed.read_bytes()
        start_rename(changed.name)  # Current dirty row must not open discard dialog.
        text('05-dirty'); key(0x0D)
        dirty = docs / '05-dirty.txt'
        check('dirty current document can gain row focus and rename', dirty.exists() and not changed.exists() and dirty.read_bytes() == old_disk)
        click(360, 110); key(0x11, 0x53)
        check('save follows renamed dirty document path', b'DIRTY_BEFORE_F2' in dirty.read_bytes() and not changed.exists())
        capture('dirty-renamed-and-saved')

        # An invalid name must leave the prompt editable so Enter can correct it.
        start_rename(dirty.name); key(0x11, 0x41); text('CON.txt'); key(0x0D)
        capture('invalid-name-retained')
        check('reserved name leaves source untouched', dirty.exists())
        key(0x11, 0x41); text('06-corrected.txt'); key(0x0D)
        corrected = docs / '06-corrected.txt'
        check('error prompt accepts corrected input without reopening', corrected.exists() and not dirty.exists())
        capture('corrected')
        cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
        proc.wait(timeout=12)
        report['normal_exit'] = proc.returncode == 0
        check('normal exit', report['normal_exit'])
    finally:
        if hwnd and proc.poll() is None:
            # Only our owned isolated window. Cancel any prompt, save its test
            # draft, then ask for normal close; never terminate foreign processes.
            try:
                key(0x1B); key(0x11, 0x53)
                cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                proc.wait(timeout=12)
                report['normal_exit'] = proc.returncode == 0
            except Exception as error:
                report['cleanup_error'] = str(error)
        report['registry_unchanged'] = before_registry == registry_snapshot()
        report['executable_unchanged'] = report['sha256'] == hashlib.sha256(exe.read_bytes()).hexdigest()
        report['exit_code'] = proc.poll()
        (out / 'conditions.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    check('registry unchanged', report['registry_unchanged'])
    check('executable unchanged', report['executable_unchanged'])


if __name__ == '__main__':
    main()

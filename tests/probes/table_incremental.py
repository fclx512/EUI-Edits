"""Owned-window table/ordinary edit/undo and incremental-vs-reopen pixel regression."""
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
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--animations', type=int, choices=(0, 1), default=0)
    ap.add_argument('--drag', action='store_true')
    ap.add_argument('--ordinary', action='store_true')
    ap.add_argument('--stable-offset', action='store_true',
                    help='Equal-byte replacement/undo/redo, then compare with a cold reopen')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('table incremental')
    cad.assert_no_foreign_instance('table incremental')
    source = ('Intro plain text\n\n| Name | Count |\n| :--- | ---: |\n'
              '| CELL_ALPHA | 123 |\n| Second cell | 456 |\n\n'
              'Paragraph between tables\n\n| Left | Right |\n| --- | :---: |\n'
              '| Other table | untouched |\n\nTail\n')
    target = 'CELL_ALPHA'
    replacement = 'X'
    if args.ordinary:
        target = 'EDIT_ALPHA'
        replacement = '换'
        source = ('Intro plain 中文 😀 text\n\n'
                  'Before *MULTI_OPEN 中文 text\nMULTI_CLOSE text* suffix\n\n'
                  '- [x] task **done**\n\n> quote first\n> quote second\n\n'
                  'Paragraph EDIT_ALPHA ordinary **strong** `code` suffix\n\n'
                  'Tail long ' + '中文 😀 ordinary wrapped text ' * 14 + '\n')
    checks = []
    if args.stable_offset:
        # A single WM_CHAR is one edit/undo record. Multiple typed characters
        # are separate records, so use one 3-byte Chinese replacement here.
        source = source.replace(target, '替')
        target, replacement = '替', '换'
        assert len(target.encode('utf-8')) == len(replacement.encode('utf-8'))
    pictures = {}
    exits = []
    unit = 1
    with tempfile.TemporaryDirectory(prefix='neo-table-incremental-') as temp:
        doc = Path(temp) / 'table.md'
        doc.write_text(source, encoding='utf-8')
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='1')
        for phase in ('incremental', 'reopened'):
            write_settings(str(Path(temp) / 'EUI-Edits/settings.ini'), str(doc), args.theme, 16, str(args.scale))
            with (Path(temp) / 'EUI-Edits/settings.ini').open('a', encoding='utf-8') as settings:
                settings.write(f'animations={args.animations}\n')
            proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
            hwnd = None

            def owned():
                cad.assert_unlocked('table incremental')
                pid = ctypes.c_ulong()
                target_thread = cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                assert pid.value == proc.pid
                foreground = cad.ensure_foreground(hwnd) if proc.poll() is None else False
                if not foreground and proc.poll() is None:
                    own_thread = cad.kernel32.GetCurrentThreadId()
                    attached = cad.user32.AttachThreadInput(own_thread, target_thread, True)
                    try:
                        cad.user32.ShowWindow(hwnd, 9)
                        cad.user32.BringWindowToTop(hwnd)
                        cad.user32.SetForegroundWindow(hwnd)
                    finally:
                        if attached: cad.user32.AttachThreadInput(own_thread, target_thread, False)
                    time.sleep(.4)
                    foreground = cad.user32.GetForegroundWindow() == hwnd
                if not foreground:
                    title = ctypes.create_unicode_buffer(256)
                    cad.user32.GetWindowTextW(hwnd, title, 256)
                    print(dict(owned_title=title.value,
                               enabled=bool(cad.user32.IsWindowEnabled(hwnd)),
                               visible=bool(cad.user32.IsWindowVisible(hwnd))), flush=True)
                assert proc.poll() is None and foreground, (
                    f'owned foreground lost: pid={proc.pid}, exit={proc.poll()}, '
                    f'hwnd={hwnd}, foreground={cad.user32.GetForegroundWindow()}')

            def chord(*keys):
                owned()
                for key in keys: cad.user32.keybd_event(key, 0, 0, 0)
                for key in reversed(keys): cad.user32.keybd_event(key, 0, 2, 0)
                time.sleep(.25)

            def type_text(text):
                owned()
                for ch in text:
                    if ch == '\n': chord(0x0D)
                    else:
                        encoded = ch.encode('utf-16-le')
                        for offset in range(0, len(encoded), 2):
                            cad.user32.PostMessageW(hwnd, 0x0102,
                                int.from_bytes(encoded[offset:offset + 2], 'little'), 0)
                    time.sleep(.01)
                time.sleep(.35)

            def check(name, actual, expected):
                passed = actual == expected
                checks.append(dict(name=name, passed=passed))
                if not passed:
                    (out / 'failure.json').write_text(json.dumps(dict(name=name, actual=actual, expected=expected),
                        indent=2), encoding='utf-8')
                assert passed, name

            def saved():
                chord(0x11, 0x53)
                return doc.read_text(encoding='utf-8')

            def find(text):
                chord(0x11, 0x46)
                chord(0x11, 0x41)
                type_text(text)
                chord(0x0D)
                chord(0x1B)

            def go_start():
                # Collapse the whole-document selection to its left endpoint.
                chord(0x11, 0x41)
                chord(0x25)

            def capture(label):
                owned()
                point = cad.wintypes.POINT(5, 8)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                cad.user32.SetCursorPos(point.x, point.y)
                time.sleep(.6)
                result = cad.capture_client(hwnd)
                cad.write_png(str(out / f'{phase}-{label}.png'), *result)
                return result

            try:
                for _ in range(120):
                    hwnd = window_for_pid(proc.pid)
                    if hwnd or proc.poll() is not None: break
                    time.sleep(.1)
                assert hwnd, 'owned window missing'
                owned()
                cad.user32.SetWindowPos(hwnd, None, 0, 0, 1200, 850, 0x0002 | 0x0004)
                time.sleep(.8)
                dpi = cad.user32.GetDpiForWindow(hwnd)
                unit = dpi / 96 * args.scale
                if phase == 'incremental':
                    capture('initial')
                    find(target)
                    type_text(replacement)
                    replaced = source.replace(target, replacement)
                    check('target edit saves exact document', saved(), replaced)
                    chord(0x11, 0x5A)
                    check('target edit undo', saved(), source)
                    chord(0x11, 0x59)
                    check('target edit redo', saved(), replaced)
                    if not args.stable_offset:
                        find(replacement)
                        long_cell = 'LongCell_' * 10
                        if args.ordinary: long_cell = '**长文本 😀 ordinary edit** ' * 8
                        type_text(long_cell)
                        expected = source.replace(target, long_cell)
                        check('grow target preserving unrelated content', saved(), expected)
                        go_start()
                        type_text('PREFIX\n')
                        check('prefix edit preserves complete document', saved(), 'PREFIX\n' + expected)
                    # Save notifications are transient UI, not table-layout differences.
                    # A timer callback can reach its steady-clock deadline on
                    # the next interval; wait through two save-toast intervals.
                    time.sleep(6.5)
                # Same caret/source/geometry for both incremental and fresh full layout.
                go_start()
                if args.drag:
                    owned()
                    rect = cad.wintypes.RECT()
                    cad.user32.GetWindowRect(hwnd, ctypes.byref(rect))
                    x, y = rect.right - 2, (rect.top + rect.bottom) // 2
                    cad.user32.SetCursorPos(x, y)
                    cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
                    try:
                        for step in range(60):
                            cad.assert_unlocked('table drag')
                            assert proc.poll() is None and cad.user32.GetForegroundWindow() == hwnd
                            cad.user32.SetCursorPos(x - int(220 * abs(step % 30 - 15) / 15), y)
                            time.sleep(.05)
                    finally:
                        cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                    time.sleep(.6)
                    owned()
                    check(phase + ' drag process alive', proc.poll(), None)
                    capture('dragged')
                pictures[phase] = {}
                for label, width in (('wide', 1200), ('narrow', 760)):
                    cad.user32.SetWindowPos(hwnd, None, 0, 0, width, 850, 0x0002 | 0x0004)
                    time.sleep(.8)
                    pictures[phase][label] = capture(label)
            finally:
                if proc.poll() is None and hwnd:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                    proc.wait(timeout=10)
                exits.append(proc.returncode)
                assert proc.returncode == 0, 'abnormal probe process exit'
        for label in ('wide', 'narrow'):
            aw, ah, a = pictures['incremental'][label]
            bw, bh, b = pictures['reopened'][label]
            check(label + ' viewport size', (aw, ah), (bw, bh))
            # Exclude the first-line caret and the status bar; keep document content.
            top, bottom = round(115 * unit), ah - round(40 * unit)
            differences = sum(a[y*aw*4:(y+1)*aw*4] != b[y*bw*4:(y+1)*bw*4] for y in range(top, bottom))
            check(label + ' document pixels equal fresh full layout', differences, 0)
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  theme=args.theme, ui_scale=args.scale, dpi=dpi, animations=args.animations, drag=args.drag,
                  ordinary=args.ordinary,
                  stable_offset=args.stable_offset,
                  checks=checks, exits=exits)
    (out / 'conditions.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

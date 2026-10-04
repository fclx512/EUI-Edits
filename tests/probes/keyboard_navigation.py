"""Owned native-window navigation, selection, edit/undo and viewport regression."""
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
    ap.add_argument('--theme', type=int, default=0)
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--animations', type=int, default=0)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('navigation')
    cad.assert_no_foreign_instance('navigation')
    cad.user32.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_int, ctypes.c_int, ctypes.c_uint]
    cad.user32.SetWindowPos.restype = ctypes.c_bool
    source = ''.join(f'ROW{i:03d} 中文 😀 ' + 'ordinary paragraph alpha beta gamma delta epsilon zeta eta theta iota kappa lambda ' * 4 + '\n' for i in range(180))
    checks = []
    initial_hash = hashlib.sha256(exe.read_bytes()).hexdigest()

    def check(name, result):
        checks.append(dict(name=name, passed=bool(result)))
        assert result, name

    with tempfile.TemporaryDirectory(prefix='neo-navigation-') as temp:
        doc = Path(temp) / 'navigation.md'
        doc.write_text(source, encoding='utf-8')
        write_settings(str(Path(temp) / 'EUI-Edits/settings.ini'), str(doc), args.theme, 16, str(args.scale))
        with (Path(temp) / 'EUI-Edits/settings.ini').open('a', encoding='utf-8') as f:
            f.write(f'animations={args.animations}\n')
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
        proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
        hwnd = None

        def owned():
            cad.assert_unlocked('navigation')
            pid = ctypes.c_ulong()
            thread = cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            assert proc.poll() is None and pid.value == proc.pid
            if not cad.ensure_foreground(hwnd):
                attached = cad.user32.AttachThreadInput(cad.kernel32.GetCurrentThreadId(), thread, True)
                try:
                    cad.user32.ShowWindow(hwnd, 9)
                    cad.user32.BringWindowToTop(hwnd)
                    cad.user32.SetForegroundWindow(hwnd)
                finally:
                    if attached: cad.user32.AttachThreadInput(cad.kernel32.GetCurrentThreadId(), thread, False)
            assert cad.user32.GetForegroundWindow() == hwnd

        def chord(*keys):
            owned()
            for k in keys:
                cad.user32.keybd_event(k, cad.user32.MapVirtualKeyW(k, 0), 1 if k in (0x21, 0x22, 0x23, 0x24) else 0, 0)
                time.sleep(.025)
            time.sleep(.04)
            for k in reversed(keys):
                cad.user32.keybd_event(k, cad.user32.MapVirtualKeyW(k, 0), 2 | (1 if k in (0x21, 0x22, 0x23, 0x24) else 0), 0)
                time.sleep(.025)
            time.sleep(.15)

        def copy():
            chord(0x11, 0x43)
            cad.user32.GetClipboardData.restype = ctypes.c_void_p
            cad.kernel32.GlobalLock.restype = ctypes.c_void_p
            cad.kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
            cad.kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
            assert cad.user32.OpenClipboard(hwnd)
            try:
                handle = cad.user32.GetClipboardData(13)
                ptr = cad.kernel32.GlobalLock(handle)
                assert ptr
                try: return ctypes.wstring_at(ptr).replace('\r\n', '\n')
                finally: cad.kernel32.GlobalUnlock(handle)
            finally: cad.user32.CloseClipboard()

        def capture(label):
            owned()
            point = cad.wintypes.POINT(5, 8)
            cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
            cad.user32.SetCursorPos(point.x, point.y)
            time.sleep(.2)
            cad.write_png(str(out / f'{label}.png'), *cad.capture_client(hwnd))

        def insert_save(char):
            owned()
            cad.user32.PostMessageW(hwnd, 0x0102, ord(char), 0)
            time.sleep(.2)
            chord(0x11, 0x53)
            time.sleep(.3)
            actual = doc.read_text(encoding='utf-8')
            (out / 'last-saved.md').write_text(actual, encoding='utf-8')
            return actual

        def undo():
            chord(0x11, 0x5A)
            chord(0x11, 0x53)
            time.sleep(.3)
            check('undo restores exact source', doc.read_text(encoding='utf-8') == source)

        try:
            for _ in range(100):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None: break
                time.sleep(.1)
            assert hwnd
            owned()
            assert cad.user32.SetWindowPos(hwnd, None, 0, 0, 1800, 760, 0x0002 | 0x0004)
            time.sleep(1.5)  # Let the first document layout and native paint finish.
            wide_rect = cad.wintypes.RECT()
            cad.user32.GetClientRect(hwnd, ctypes.byref(wide_rect))
            chord(0x11, 0x46)
            time.sleep(.4)  # Find focus is applied by the next UI build.
            chord(0x1B)  # Return from the find input to the editor explicitly.
            time.sleep(.4)
            capture('ready-document')
            chord(0x11, 0x24)
            check('Ctrl Home inserts at document start', insert_save('X') == 'X' + source)
            undo()
            chord(0x11, 0x23)
            check('Ctrl End inserts at document end', insert_save('X') == source + 'X')
            undo()
            chord(0x11, 0x24)
            capture('document-start')
            chord(0x10, 0x22)
            first = copy()
            (out / 'first-page.txt').write_text(first, encoding='utf-8')
            (out / 'first-page-expected.txt').write_text(source[:len(first)], encoding='utf-8')
            capture('page-selection')
            check('Shift PageDown selects source prefix', len(first) > 20 and first == source[:len(first)])
            check('paging is bounded by viewport', 0 < first.count('\n') < 60)
            capture('page-selection')
            chord(0x10, 0x21)
            check('Shift PageUp returns to anchor', insert_save('Z') == 'Z' + source)
            undo()
            chord(0x11, 0x24)
            chord(0x22)
            chord(0x11, 0x10, 0x24)
            check('unselected paging reaches same first page', copy() == first)
            chord(0x11, 0x24)
            chord(0x22)
            chord(0x22)
            chord(0x11, 0x10, 0x24)
            second = copy()
            check('second page advances and preserves UTF-8', len(second) > len(first) and second == source[:len(second)])
            chord(0x11, 0x23)
            chord(0x11, 0x10, 0x24)
            check('Ctrl Shift Home selects full document backwards', copy() == source)
            chord(0x11, 0x24)
            chord(0x11, 0x10, 0x23)
            check('Ctrl Shift End selects full document forwards', copy() == source)
            chord(0x11, 0x24)
            resized = cad.user32.SetWindowPos(hwnd, None, 0, 0, 760, 760, 0x0002 | 0x0004)
            check('native resize succeeds', resized)
            time.sleep(.5)
            chord(0x10, 0x22)
            narrow = copy()
            rect = cad.wintypes.RECT()
            cad.user32.GetClientRect(hwnd, ctypes.byref(rect))
            (out / 'viewport.json').write_text(json.dumps(dict(wide_client_width=wide_rect.right,
                client_width=rect.right, client_height=rect.bottom,
                first_characters=len(first), narrow_characters=len(narrow))), encoding='utf-8')
            check('narrow viewport actually shrinks', rect.right < wide_rect.right - 150)
            (out / 'narrow-page.txt').write_text(narrow, encoding='utf-8')
            capture('narrow-page-selection')
            check('narrow paging follows wrapped geometry', narrow == source[:len(narrow)] and
                  0 < len(narrow) < len(first) and narrow.count('\n') < first.count('\n'))
            capture('narrow-page-selection')
            chord(0x11, 0x24)
            chord(0x11, 0x53)
            time.sleep(.3)
            check('navigation preserves document bytes', doc.read_text(encoding='utf-8') == source)
            dpi = cad.user32.GetDpiForWindow(hwnd)
        finally:
            if proc.poll() is None and hwnd:
                cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                proc.wait(timeout=10)
        check('normal exit and stable executable', proc.returncode == 0 and initial_hash == hashlib.sha256(exe.read_bytes()).hexdigest())
    result = dict(sha256=initial_hash, theme=args.theme, scale=args.scale, animations=args.animations,
                  dpi=dpi, exit_code=proc.returncode, checks=checks)
    (out / 'conditions.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

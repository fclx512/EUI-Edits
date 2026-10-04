"""Owned settings-window captures and refresh interaction; leaves associations unchanged."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import winreg

import win_capture as cad
from capture_markdown import window_for_pid


def association_snapshot():
    paths = ['Software\\EUI-Edits\\Capabilities', 'Software\\RegisteredApplications',
             'Software\\Classes\\EUIEdits.Document', 'Software\\Classes\\EUIEdits.TextDocument',
             'Software\\Classes\\EUIEdits.MarkdownDocument']
    for ext in ('.txt', '.md'):
        paths += [f'Software\\Classes\\{ext}\\OpenWithProgids',
                  f'Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\{ext}\\UserChoice']
    result = {}
    def read(path):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
                children, values, _ = winreg.QueryInfoKey(key)
                result[path] = [winreg.EnumValue(key, i) for i in range(values)]
                for i in range(children): read(path + '\\' + winreg.EnumKey(key, i))
        except FileNotFoundError:
            result[path] = None
    for path in paths: read(path)
    return result


def protected_associations(snapshot):
    # Repair may normalize owned metadata. Default choices and other applications must survive.
    result = {}
    owned = {'EUIEdits.Document', 'EUIEdits.TextDocument', 'EUIEdits.MarkdownDocument'}
    for path, values in snapshot.items():
        if path.endswith('UserChoice'):
            result[path] = sorted(values) if values is not None else None
        elif path == 'Software\\RegisteredApplications':
            result[path] = sorted(v for v in values or [] if v[0] != 'EUI-Edits')
        elif path.endswith('OpenWithProgids'):
            result[path] = sorted(v for v in values or [] if v[0] not in owned)
    return result


def normalized_snapshot(snapshot):
    return {path: sorted(values) if values is not None else None for path, values in snapshot.items()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--theme', type=int, default=1)
    parser.add_argument('--scale', type=float, default=1)
    parser.add_argument('--font', type=int, default=14)
    parser.add_argument('--refresh-x', type=float, default=0)
    parser.add_argument('--refresh-y', type=float, default=0)
    parser.add_argument('--register-x', type=float, default=0)
    parser.add_argument('--register-y', type=float, default=0)
    args = parser.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('file association settings')
    cad.assert_no_foreign_instance('file association settings')
    before = association_snapshot()
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  theme=args.theme, scale=args.scale, font=args.font, checks=[])
    with tempfile.TemporaryDirectory(prefix='neo-assoc-settings-') as temp:
        doc = Path(temp) / 'settings.md'
        doc.write_text('# Settings\n\nNo production document changes.\n', encoding='utf-8')
        settings = Path(temp) / 'EUI-Edits/settings.ini'
        settings.parent.mkdir()
        settings.write_text(f'vault=\nlast_file={doc}\nmode=0\nui_scale={args.scale}\n'
                            f'ui_font_size={args.font}\ntheme={args.theme}\nanimations=0\n', encoding='utf-8')
        env = dict(os.environ, APPDATA=temp, NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
        proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
        hwnd = None
        def owned():
            cad.assert_unlocked('association settings')
            assert proc.poll() is None
            pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            assert pid.value == proc.pid and cad.ensure_foreground(hwnd), 'owned foreground lost'
        def click(x, y):
            owned()
            pt = cad.wintypes.POINT(round(x * unit), round(y * unit))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.SetCursorPos(pt.x, pt.y)
            cad.user32.mouse_event(2, 0, 0, 0, 0)
            time.sleep(.08)
            cad.user32.mouse_event(4, 0, 0, 0, 0)
            time.sleep(.6)
        def capture(name):
            owned()
            pt = cad.wintypes.POINT(5, 8)
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.SetCursorPos(pt.x, pt.y)
            time.sleep(.5)
            pixels = cad.capture_client(hwnd)
            cad.write_png(str(out / f'{name}.png'), *pixels)
            return pixels
        def scroll(delta, repeats=1):
            owned()
            pt = cad.wintypes.POINT(round(400 * unit), round(400 * unit))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.SetCursorPos(pt.x, pt.y)
            for _ in range(repeats):
                cad.user32.mouse_event(0x0800, 0, 0, ctypes.c_ulong(delta).value, 0)
                time.sleep(.15)
            time.sleep(.5)
        try:
            for _ in range(120):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None: break
                time.sleep(.1)
            assert hwnd
            owned()
            cad.user32.SetWindowPos(hwnd, None, 40, 40, 1400, 1080, 4)
            time.sleep(1)
            dpi = cad.user32.GetDpiForWindow(hwnd)
            unit = dpi / 96 * args.scale
            result['dpi'] = dpi
            for vk, down in ((0x11, True), (0xBC, True), (0xBC, False), (0x11, False)):
                cad.user32.keybd_event(vk, 0, 0 if down else 2, 0)
                time.sleep(.05)
            time.sleep(.8)
            # File/System nav: sidebar in wide mode, top navigation at higher UI scales.
            client_w = cad.capture_client(hwnd)[0] / unit
            if client_w >= 840: click(85, 208)
            else: click(client_w * 5 / 6, 99)
            scroll(-120, 7)
            wide = capture('wide')
            if args.register_x and args.register_y:
                click(args.register_x, args.register_y)
                capture('register-feedback')
                time.sleep(4)
                after_register = capture('registered')
                first = association_snapshot()
                assert protected_associations(before) == protected_associations(first), 'repair changed default choices or other applications'
                assert after_register[:2] == wide[:2]
                # The success toast uses a message-length-based lifetime; compare the full settings card above it.
                card_bytes = wide[0] * max(0, wide[1] - 160) * 4
                assert after_register[2][:card_bytes] == wide[2][:card_bytes], 'repair changed settings card geometry/status'
                click(args.register_x, args.register_y)
                time.sleep(13) # Existing toast caps its message-length lifetime at 12 seconds.
                assert normalized_snapshot(first) == normalized_snapshot(association_snapshot()), 'repeat registration must be idempotent'
                capture('registered-twice')
                result['checks'].append('clicked register/repair twice; defaults and other entries preserved; second registration idempotent')
            if args.refresh_x and args.refresh_y:
                click(args.refresh_x, args.refresh_y)
                refreshed = capture('wide-refreshed')
                assert refreshed == wide, 'refresh must retain geometry and display confirmed unchanged status'
                result['checks'].append('clicked refresh; stable pixels and unchanged association entries')
            result['checks'].append('file/system navigation and wheel scrolling completed')
            cad.user32.SetWindowPos(hwnd, None, 40, 40, 800, 950, 4)
            time.sleep(.8)
            scroll(-120, 12)
            capture('narrow-bottom')
            scroll(120, 4)
            capture('narrow-guide')
            scroll(120, 12)
            capture('narrow-top')
            scroll(-120, 3)
            capture('narrow-middle')
            result['checks'].append('narrow resize and scrolling completed')
        finally:
            if hwnd and proc.poll() is None:
                cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                proc.wait(timeout=10)
            result['exit'] = proc.returncode
            result['associations_unchanged'] = normalized_snapshot(before) == normalized_snapshot(association_snapshot())
            result['protected_associations_unchanged'] = protected_associations(before) == protected_associations(association_snapshot())
            (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        assert result['exit'] == 0 and result['protected_associations_unchanged']
        if not args.register_x: assert result['associations_unchanged']
        print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()

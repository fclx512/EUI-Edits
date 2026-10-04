"""Owned-window same-byte Markdown edit/undo and cold-layout pixel comparison."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import win_capture as cad
from capture_markdown import window_for_pid, write_settings

SOURCE = ('# Edit page review\n\n- [x] TASK_TARGET 中文 😀\n\n'
          '正文 **bold** and *italic* 中文 😀\n\n```cpp\nint value = 42;\n```\n\n'
          '> quote\n> second line\n\n| A | B |\n| --- | --- |\n| one | 42 |\n\nTail\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--offset', action='store_true', help='also insert/delete bytes without changing source rows')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('edited pages')
    cad.assert_no_foreign_instance('edited pages')
    result = dict(sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), theme=args.theme,
                  scale=args.scale, offset=args.offset, checks=[], runs=[], comparisons=[])
    pictures = {}

    def check(name, ok):
        result['checks'].append(dict(name=name, passed=bool(ok)))
        print(name, bool(ok), flush=True)
        assert ok, name

    def run(phase, source):
        runtime = out / phase / 'runtime'
        runtime.mkdir(parents=True, exist_ok=True)
        doc = out / phase / 'sample.md'
        doc.write_bytes(source.encode())
        write_settings(str(runtime/'EUI-Edits/settings.ini'), str(doc), args.theme, 16, str(args.scale))
        with (runtime/'EUI-Edits/settings.ini').open('a', encoding='utf-8') as target:
            target.write('animations=0\nmode=0\nshow_status_bar=1\n')
        env = dict(os.environ, APPDATA=str(runtime), NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
        proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
        hwnd = None
        images = {}

        def owned():
            cad.assert_unlocked('edited pages')
            pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            assert proc.poll() is None and pid.value == proc.pid and cad.ensure_foreground(hwnd)

        def clear_surface(width, height):
            cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
            cad.user32.WindowFromPoint.restype = ctypes.c_void_p
            cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            cad.user32.GetAncestor.restype = ctypes.c_void_p
            for x in (20, width*.25, width*.5, width*.75, width-24):
                for y in (20, height*.25, height*.5, height*.75, height-24):
                    point = cad.wintypes.POINT(round(x), round(y))
                    cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                    target = cad.user32.GetAncestor(cad.user32.WindowFromPoint(point), 2)
                    if target != hwnd:
                        title = ctypes.create_unicode_buffer(256)
                        name = ctypes.create_unicode_buffer(128)
                        pid = ctypes.c_ulong()
                        cad.user32.GetWindowTextW(target, title, 256)
                        cad.user32.GetClassNameW(target, name, 128)
                        cad.user32.GetWindowThreadProcessId(target, ctypes.byref(pid))
                        result['obscured'] = dict(point=[point.x, point.y], title=title.value,
                            window_class=name.value, pid=pid.value, root=target)
                    assert target == hwnd, f'client obscured at {point.x},{point.y}: root={target}, expected={hwnd}'

        def chord(*keys):
            owned()
            for key in keys:
                cad.user32.keybd_event(key, cad.user32.MapVirtualKeyW(key, 0), 0, 0)
                time.sleep(.035)
            time.sleep(.06)
            for key in reversed(keys):
                cad.user32.keybd_event(key, cad.user32.MapVirtualKeyW(key, 0), 2, 0)
                time.sleep(.035)
            time.sleep(.23)

        def type_text(value):
            owned()
            for char in value:
                cad.user32.PostMessageW(hwnd, 0x0102, ord(char), 1)
            time.sleep(.3)

        def paste(value):
            # One real clipboard event preserves this test's equal-byte edit boundary.
            payload = (value+'\0').encode('utf-16-le')
            cad.kernel32.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]
            cad.kernel32.GlobalAlloc.restype = ctypes.c_void_p
            cad.kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
            cad.kernel32.GlobalLock.restype = ctypes.c_void_p
            cad.kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
            cad.user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
            cad.user32.SetClipboardData.restype = ctypes.c_void_p
            handle = cad.kernel32.GlobalAlloc(0x42, len(payload))
            pointer = cad.kernel32.GlobalLock(handle)
            assert pointer
            ctypes.memmove(pointer, payload, len(payload))
            cad.kernel32.GlobalUnlock(handle)
            assert cad.user32.OpenClipboard(hwnd)
            try:
                assert cad.user32.EmptyClipboard()
                assert cad.user32.SetClipboardData(13, handle)
            finally: cad.user32.CloseClipboard()
            chord(0x11, 0x56)

        def find(value, collapse=True):
            chord(0x11, 0x46)
            chord(0x11, 0x41)
            type_text(value)
            chord(0x0D)
            chord(0x1B)
            if collapse: chord(0x25)

        def capture(label):
            owned()
            point = cad.wintypes.POINT(5, 8)
            cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
            cad.user32.SetCursorPos(point.x, point.y)
            frames = []
            for i in range(4):
                owned()
                frame = cad.capture_client(hwnd)
                cad.write_png(str(out/f'{phase}-{label}-{i}.png'), *frame)
                clear_surface(frame[0], frame[1])
                frames.append(frame)
                time.sleep(.4)
            images[label] = frames

        try:
            for _ in range(100):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None: break
                time.sleep(.1)
            assert hwnd
            owned()
            result['dpi'] = cad.user32.GetDpiForWindow(hwnd)
            cad.user32.SetWindowPos(hwnd, None, 40, 40, 1400, 1100, 0x0004)
            time.sleep(.8)
            if phase == 'edited':
                capture('before-task')
                # Task prefixes are concealed in Live Preview. Use the real checkbox,
                # located by its accent pixels rather than keyboarding through hidden text.
                width, height, pixels = images['before-task'][0]
                unit = result['dpi']/96*args.scale
                accent = []
                for yy in range(round(70*unit), round(150*unit)):
                    for xx in range(round(12*unit), round(40*unit)):
                        offset = (yy*width+xx)*4
                        blue, green, red = pixels[offset:offset+3]
                        if blue-red > 15 and red-green > 15: accent.append((xx, yy))
                assert accent, 'task checkbox pixels absent'
                xx = (min(p[0] for p in accent)+max(p[0] for p in accent))//2
                yy = (min(p[1] for p in accent)+max(p[1] for p in accent))//2
                point = cad.wintypes.POINT(xx, yy)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(point), 2) == hwnd
                cad.click(hwnd, xx, yy)
                time.sleep(.4)
                chord(0x11, 0x53)
                edited_task = SOURCE.replace('[x]', '[ ]')
                check('task style replacement saves exact source', doc.read_bytes() == edited_task.encode())
                capture('task-edited')
                chord(0x11, 0x5A)
                chord(0x11, 0x53)
                check('task undo restores exact source', doc.read_bytes() == SOURCE.encode())
                chord(0x11, 0x59)
                chord(0x11, 0x53)
                check('task redo restores exact source', doc.read_bytes() == edited_task.encode())
                find('int', collapse=False)
                paste('abc')
                chord(0x11, 0x53)
                final = edited_task.replace('int value', 'abc value')
                check('code style replacement saves exact source', doc.read_bytes() == final.encode())
                chord(0x11, 0x5A)
                chord(0x11, 0x53)
                check('code undo restores exact source', doc.read_bytes() == edited_task.encode())
                chord(0x11, 0x59)
                chord(0x11, 0x53)
                check('code redo restores exact source', doc.read_bytes() == final.encode())
                if args.offset:
                    original = final
                    find('value', collapse=False)
                    paste('long_value')
                    final = original.replace('abc value', 'abc long_value')
                    chord(0x11, 0x53)
                    check('byte insertion saves exact source', doc.read_bytes() == final.encode())
                    chord(0x11, 0x5A); chord(0x11, 0x53)
                    check('byte insertion undo restores source', doc.read_bytes() == original.encode())
                    chord(0x11, 0x59); chord(0x11, 0x53)
                    check('byte insertion redo restores source', doc.read_bytes() == final.encode())
                    inserted = final
                    find('long_value', collapse=False)
                    paste('v')
                    final = inserted.replace('abc long_value', 'abc v')
                    chord(0x11, 0x53)
                    check('byte deletion saves exact source', doc.read_bytes() == final.encode())
                    chord(0x11, 0x5A); chord(0x11, 0x53)
                    check('byte deletion undo restores source', doc.read_bytes() == inserted.encode())
                    chord(0x11, 0x59); chord(0x11, 0x53)
                    check('byte deletion redo restores source', doc.read_bytes() == final.encode())
                time.sleep(6.5)
            find(' = 42' if args.offset else 'value')
            chord(0x23)
            capture('wide')
            cad.user32.SetWindowPos(hwnd, None, 40, 40, 800, 1100, 0x0004)
            time.sleep(.6)
            capture('narrow')
            chord(0x11, 0x53)
            final_source = doc.read_text(encoding='utf-8')
            check(phase+' navigation preserves source', final_source == (source if phase == 'fresh' else final))
            pictures[phase] = images
            return final_source
        finally:
            if hwnd and proc.poll() is None:
                cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                proc.wait(timeout=10)
            result['runs'].append(dict(phase=phase, pid=proc.pid, exit_code=proc.poll()))
            check(phase+' normal exit and stable executable', proc.poll() == 0 and
                  result['sha256'] == hashlib.sha256(exe.read_bytes()).hexdigest())

    try:
        final = run('edited', SOURCE)
        run('fresh', final)
        for label in ('wide', 'narrow'):
            a, b = pictures['edited'][label], pictures['fresh'][label]
            width, height = a[0][:2]
            assert (width, height) == b[0][:2]
            # Exclude title/menu/status; compare exact editor pixels and mask only blink changes.
            differences, masked = 0, 0
            unit = result['dpi']/96*args.scale
            for y in range(round(60*unit), height-round(40*unit)):
                for x in range(16, width-16):
                    off = (y*width+x)*4
                    aa = [f[2][off:off+3] for f in a]
                    bb = [f[2][off:off+3] for f in b]
                    if len(set(aa)) > 1 or len(set(bb)) > 1: masked += 1
                    elif aa[0] != bb[0]: differences += 1
            result['comparisons'].append(dict(label=label, differences=differences, masked=masked))
            check(label+' edited/cold pixels match', differences == 0)
        result['status'] = 'passed'
    finally:
        (out/'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__': main()

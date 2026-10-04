"""Owned-window cursor/marker regression against the retained previous build.

Uses disposable APPDATA/documents, foreground ownership checks, normal WM_CLOSE.
Pixel comparison masks only pixels that change across a complete blink cycle.
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
from capture_markdown import window_for_pid, write_settings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--before', required=True)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--animations', type=int, choices=(0, 1), default=0)
    ap.add_argument('--extended', action='store_true')
    ap.add_argument('--fresh-reference', action='store_true',
                    help='Compare the new edited state against a new cold process; record old-build differences.')
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('cursor semantic')
    cad.assert_no_foreign_instance('cursor semantic')
    source = ('PLAIN_TARGET ordinary 中文 text\n\n'
              '# Heading **boldword** tail\n\n'
              'Before **strongword** after `codeword` and [[target|aliasword]]\n\n'
              '> quote paragraph\n> next quote line\n\n'
              '| Name | Value |\n| --- | ---: |\n| cell | 42 |\n\nTail\n')
    if args.extended:
        source += ('\nMULTI_BEFORE *MULTI_OPEN text\nMULTI_CLOSE text* suffix\n\n'
                   '```cpp\n/* CODE_COMMENT_OPEN\nCODE_COMMENT_CLOSE */ int value = 42;\n```\n\n'
                   'POST_CODE plain 中文 final paragraph\n')
    pictures, runs, checks = {}, [], []

    def check(name, condition):
        checks.append(dict(name=name, passed=bool(condition)))
        assert condition, name

    with tempfile.TemporaryDirectory(prefix='neo-cursor-semantic-') as temp:
        doc = Path(temp) / 'cursor.md'
        phases = [('before', args.before), ('after', args.exe)]
        if args.fresh_reference: phases.append(('fresh', args.exe))
        for phase, exe_arg in phases:
            exe = Path(exe_arg).resolve()
            doc.write_text(source, encoding='utf-8')
            write_settings(str(Path(temp) / 'EUI-Edits/settings.ini'), str(doc), args.theme, 16, str(args.scale))
            with (Path(temp) / 'EUI-Edits/settings.ini').open('a', encoding='utf-8') as f:
                f.write(f'animations={args.animations}\n')
            env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='1')
            before_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
            proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
            hwnd = None

            def owned():
                cad.assert_unlocked('cursor semantic')
                pid = ctypes.c_ulong()
                thread = cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                assert proc.poll() is None and pid.value == proc.pid
                if not cad.ensure_foreground(hwnd):
                    own_thread = cad.kernel32.GetCurrentThreadId()
                    attached = cad.user32.AttachThreadInput(own_thread, thread, True)
                    try:
                        cad.user32.ShowWindow(hwnd, 9)
                        cad.user32.BringWindowToTop(hwnd)
                        cad.user32.SetForegroundWindow(hwnd)
                    finally:
                        if attached:
                            cad.user32.AttachThreadInput(own_thread, thread, False)
                assert cad.user32.GetForegroundWindow() == hwnd

            def chord(*keys):
                owned()
                for key in keys:
                    cad.user32.keybd_event(key, 0, 0, 0)
                for key in reversed(keys):
                    cad.user32.keybd_event(key, 0, 2, 0)
                time.sleep(.12)

            def find(text):
                chord(0x11, 0x46)
                chord(0x11, 0x41)
                for char in text:
                    cad.user32.PostMessageW(hwnd, 0x0102, ord(char), 0)
                time.sleep(.2)
                chord(0x0D)
                chord(0x1B)
                chord(0x25)  # Collapse selection to its source start.

            def capture(label):
                owned()
                point = cad.wintypes.POINT(5, 8)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                cad.user32.SetCursorPos(point.x, point.y)
                time.sleep(.35)
                frames = []
                # Four samples span 1.2 seconds, exceeding the caret's full
                # 2 * 0.53s cycle. Shorter windows can freeze opposite blink phases.
                for sample in range(4):
                    owned()
                    frame = cad.capture_client(hwnd)
                    cad.write_png(str(out / f'{phase}-{label}-{sample}.png'), *frame)
                    frames.append(frame)
                    time.sleep(.4)
                pictures[phase][label] = frames

            try:
                for _ in range(100):
                    hwnd = window_for_pid(proc.pid)
                    if hwnd or proc.poll() is not None:
                        break
                    time.sleep(.1)
                assert hwnd
                owned()
                cad.user32.SetWindowPos(hwnd, None, 0, 0, 1200, 1000, 0x0002 | 0x0004)
                time.sleep(.8)
                unit = cad.user32.GetDpiForWindow(hwnd) / 96 * args.scale
                pictures[phase] = {}
                find('PLAIN_TARGET')
                for _ in range(5):
                    chord(0x27)
                capture('plain-region')
                if phase != 'fresh':
                    cad.user32.PostMessageW(hwnd, 0x0102, ord('X'), 0)
                    time.sleep(.2)
                    chord(0x11, 0x53)
                    time.sleep(.5)
                    actual = doc.read_text(encoding='utf-8')
                    (out / f'{phase}-inserted.md').write_text(actual, encoding='utf-8')
                    check(phase + ' insert after cursor movement', actual == source.replace('PLAIN_TARGET', 'PLAINX_TARGET'))
                    chord(0x11, 0x5A)
                    chord(0x11, 0x53)
                    time.sleep(.5)
                    check(phase + ' undo exact source', doc.read_text(encoding='utf-8') == source)
                    time.sleep(6.5)  # allow two save-toast timer intervals before pixel comparison
                find('strongword')
                for _ in range(3):
                    chord(0x27)
                capture('span-inside')
                chord(0x23)  # End: exit all spans to the same line's tail.
                capture('span-outside')
                find('codeword')
                for _ in range(2):
                    chord(0x27)
                capture('code-inside')
                find('aliasword')
                for _ in range(2):
                    chord(0x27)
                capture('wiki-inside')
                chord(0x28)
                chord(0x28)
                capture('block-change')
                if args.extended:
                    find('MULTI_OPEN')
                    capture('multiline-inside')
                    find('MULTI_CLOSE')
                    chord(0x23)
                    capture('multiline-outside')
                    find('CODE_COMMENT_CLOSE')
                    capture('code-comment-state')
                    chord(0x28)
                    chord(0x28)
                    capture('code-block-exit')
                    find('strongword')
                    cad.user32.SetWindowPos(hwnd, None, 0, 0, 760, 1000, 0x0002 | 0x0004)
                    time.sleep(.6)
                    capture('narrow-marker-wrap')
                    cad.user32.SetWindowPos(hwnd, None, 0, 0, 1200, 1000, 0x0002 | 0x0004)
                    time.sleep(.6)
                    find('PLAIN_TARGET')
                    capture('wide-return')
                chord(0x11, 0x53)
                time.sleep(.5)
                check(phase + ' navigation preserves source', doc.read_text(encoding='utf-8') == source)
            finally:
                if proc.poll() is None and hwnd:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                    proc.wait(timeout=10)
                runs.append(dict(phase=phase, exe=str(exe), sha256=before_hash, exit_code=proc.returncode))
                check(phase + ' normal exit and stable executable', proc.returncode == 0 and before_hash == hashlib.sha256(exe.read_bytes()).hexdigest())
        reference_phase = 'fresh' if args.fresh_reference else 'before'
        for label, a_frames in pictures[reference_phase].items():
            b_frames = pictures['after'][label]
            w, h, _ = a_frames[0]
            check(label + ' dimensions', all(f[:2] == (w, h) for f in a_frames + b_frames))
            differences, masked = 0, 0
            # Keep the complete sample text; exclude menu and bottom DWM corners.
            # The scaled ROI must not extend into the rounded window boundary.
            bottom = min(h - round(16 * unit), round(620 * unit))
            for y in range(round(35 * unit), bottom):
                for x in range(w):
                    pos = (y * w + x) * 4
                    a = [f[2][pos:pos+4] for f in a_frames]
                    b = [f[2][pos:pos+4] for f in b_frames]
                    if len(set(a)) != 1 or len(set(b)) != 1:
                        masked += 1
                    elif a[0] != b[0]:
                        differences += 1
            check(label + ' stable pixels match ' + reference_phase + ' build', differences == 0)
            check(label + ' blink mask bounded', masked < 1000)
            checks.append(dict(name=label + ' pixel measurements', differences=differences, masked=masked, passed=True))
        if args.fresh_reference:
            baseline = []
            for label, a_frames in pictures['before'].items():
                b_frames = pictures['after'][label]
                w, h, _ = a_frames[0]
                points = []
                for y in range(round(35 * unit), bottom):
                    for x in range(w):
                        pos = (y * w + x) * 4
                        a = [f[2][pos:pos+4] for f in a_frames]
                        b = [f[2][pos:pos+4] for f in b_frames]
                        if len(set(a)) == 1 and len(set(b)) == 1 and a[0] != b[0]: points.append((x, y))
                box = [min(x for x, y in points), min(y for x, y in points),
                       max(x for x, y in points), max(y for x, y in points)] if points else None
                baseline.append(dict(label=label, differences=len(points), bounds=box))
            (out / 'previous-build-differences.json').write_text(json.dumps(baseline, indent=2), encoding='utf-8')
    result = dict(theme=args.theme, scale=args.scale, animations=args.animations, extended=args.extended,
                  dpi=unit / args.scale * 96, fresh_reference=args.fresh_reference,
                  blink_samples=4, editor_roi_bottom=bottom, runs=runs, checks=checks)
    (out / 'conditions.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

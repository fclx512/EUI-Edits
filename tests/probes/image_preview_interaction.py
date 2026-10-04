"""Real image wheel/pan/exit checks with isolated settings and a synthetic fixture."""
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
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--theme', type=int, default=1)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware()
    cad.assert_unlocked('image preview')
    cad.assert_no_foreign_instance('image preview')
    registry = registry_snapshot()
    # An aspect ratio and four flat colors allow image geometry to be measured
    # from the actual window rather than assuming fixed thumbnail coordinates.
    colors = [(230, 65, 70), (45, 180, 95), (40, 90, 220), (230, 170, 30)]
    raw = bytearray()
    for y in range(300):
        for x in range(600):
            r, g, b = colors[(y >= 150) * 2 + (x >= 300)]
            raw.extend((b, g, r, 255))
    image = out / 'fixture.png'
    cad.write_png(str(image), 600, 300, raw)
    doc = out / 'image.md'
    original = '# Image preview\n\nA local image.\n\n![fixture](fixture.png)\n\nEnd of document.\n'
    doc.write_text(original, encoding='utf-8')
    settings = out / 'appdata/EUI-Edits/settings.ini'
    settings.parent.mkdir(parents=True)
    settings.write_text(f'last_file={doc}\nmode=0\ntheme={args.theme}\nui_scale={args.scale}\n'
                        'animations=0\nreadable_width=0\nline_numbers=0\nui_language=en\n', encoding='utf-8')
    temp = out / 'temp'
    temp.mkdir()
    env = dict(os.environ, APPDATA=str(settings.parent.parent), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
    report = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  scale=args.scale, theme=args.theme, checks=[], captures=[], normal_exit=False)
    proc = subprocess.Popen([str(exe), str(doc)], cwd=exe.parent, env=env)
    hwnd = None
    try:
        for _ in range(150):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None:
                break
            time.sleep(.1)
        assert hwnd, f'window missing, exit={proc.poll()}'
        cad.user32.SetWindowPos(hwnd, None, 25, 25, 1160, 830, 4)
        time.sleep(1.5)

        def check(name, ok):
            report['checks'].append(dict(name=name, passed=bool(ok)))
            print(name, bool(ok), flush=True)
            assert ok, name

        def owned():
            cad.assert_unlocked('image preview')
            owner = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            assert proc.poll() is None and owner.value == proc.pid and cad.ensure_foreground(hwnd), 'ownership/foreground lost'

        def point(x, y):
            owned()
            pt = cad.wintypes.POINT(round(x), round(y))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
            cad.user32.WindowFromPoint.restype = ctypes.c_void_p
            cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            cad.user32.GetAncestor.restype = ctypes.c_void_p
            assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(pt), 2) == hwnd, 'client obscured'
            return pt

        def move(x, y):
            pt = point(x, y)
            cad.user32.SetCursorPos(pt.x, pt.y)
            time.sleep(.1)

        def click(x, y, middle=False):
            move(x, y)
            cad.user32.mouse_event(0x20 if middle else 2, 0, 0, 0, 0)
            time.sleep(.08)
            cad.user32.mouse_event(0x40 if middle else 4, 0, 0, 0, 0)
            time.sleep(.4)

        def key(*keys):
            owned()
            for k in keys:
                cad.user32.keybd_event(k, 0, 0, 0)
                time.sleep(.025)
            for k in reversed(keys):
                cad.user32.keybd_event(k, 0, 2, 0)
            time.sleep(.35)

        def capture(label):
            owned()
            time.sleep(.25)
            w, h, pixels = cad.capture_client(hwnd)
            for x, y in ((15, 15), (w / 2, h / 2), (w - 25, h - 25)):
                point(x, y)
            cad.write_png(str(out / (label + '.png')), w, h, pixels)
            # Match fixture colors with allowance for filtering at its edges.
            left, top, right, bottom = w, h, -1, -1
            for y in range(0, h, 2):
                for x in range(0, w, 2):
                    i = (y * w + x) * 4
                    rgb = (pixels[i + 2], pixels[i + 1], pixels[i])
                    if any(max(abs(a - b) for a, b in zip(rgb, c)) < 12 for c in colors):
                        left, right = min(left, x), max(right, x)
                        top, bottom = min(top, y), max(bottom, y)
            assert right >= left, 'fixture not visible'
            bounds = (left, top, right, bottom)
            report['captures'].append(dict(label=label, width=w, height=h, image_bounds=bounds))
            return w, h, bounds

        def span(bounds):
            return bounds[2] - bounds[0], bounds[3] - bounds[1]

        def center(bounds):
            return (bounds[0] + bounds[2]) / 2, (bounds[1] + bounds[3]) / 2

        def wheel(steps):
            owned()
            cad.user32.mouse_event(0x800, 0, 0, ctypes.c_uint32(steps * 120).value, 0)
            time.sleep(.4)

        _, _, thumbnail = capture('thumbnail')
        click(*center(thumbnail))
        w, h, fitted = capture('fitted')
        fitted_w, fitted_h = span(fitted)
        check('preview_fits_window_and_keeps_aspect', abs(fitted_w / fitted_h - 2) < .03 and abs(fitted_w / w - .92) < .03)
        move(w / 2, h / 2)
        wheel(-2)
        _, _, small = capture('zoomed-out')
        check('wheel_changes_image_size', abs(span(small)[0] / fitted_w - 1 / 1.2**2) < .025)
        click(w / 2, h / 2, middle=True)
        _, _, stationary = capture('middle-click')
        check('middle_click_keeps_preview_open', stationary == small)
        move(w / 2, h / 2)
        cad.user32.mouse_event(0x20, 0, 0, 0, 0)
        try:
            for step in range(1, 9):
                move(w / 2 + step * 10, h / 2 + step * 5)
        finally:
            cad.user32.mouse_event(0x40, 0, 0, 0, 0)
        _, _, panned = capture('panned')
        check('middle_drag_moves_image', abs(center(panned)[0] - center(small)[0] - 80) < 5 and abs(center(panned)[1] - center(small)[1] - 40) < 5)
        # Scroll about a point inside the image, not its center.
        anchor_x, anchor_y = w / 2 - 60, h / 2 - 30
        move(anchor_x, anchor_y)
        wheel(1)
        _, _, anchored = capture('zoomed-at-pointer')
        oldcx, oldcy = center(panned)
        check('zoom_retains_pointer_anchor', abs(center(anchored)[0] - (anchor_x + (oldcx - anchor_x) * 1.2)) < 5 and abs(center(anchored)[1] - (anchor_y + (oldcy - anchor_y) * 1.2)) < 5)
        key(0x11, 0x4E)  # Ctrl+N must not replace the document under the overlay.
        key(0x41)
        key(0x1B)
        _, _, restored = capture('escape-exit')
        check('escape_restores_document_and_keyboard_is_blocked', restored == thumbnail)
        click(*center(restored))
        _, _, reopened = capture('reopened')
        check('reopen_resets_pan_and_zoom', reopened == fitted)
        click(w / 2, h / 2)
        _, _, left_exit = capture('left-exit')
        check('left_click_still_exits', left_exit == thumbnail)
        key(0x11, 0x53)
        check('document_bytes_unchanged', doc.read_text(encoding='utf-8') == original)
        owned()
        cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
        proc.wait(timeout=8)
        check('normal_exit', proc.returncode == 0)
        report['normal_exit'] = True
        check('registry_unchanged', registry_snapshot() == registry)
    finally:
        if proc.poll() is None:
            if hwnd:
                cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait(timeout=5)
                report['forced_cleanup'] = True
        (out / 'conditions.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()

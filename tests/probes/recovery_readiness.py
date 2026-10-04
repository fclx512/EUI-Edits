"""Owned-window recovery checks. Does not apply file associations."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from PIL import Image

import win_capture as cad
from capture_markdown import window_for_pid
from text_files import registry_snapshot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--language', default='en')
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--modes', default='discard,save,conflict,deleted,failed,saveas_cancel,language,settings')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('recovery readiness')
    cad.assert_no_foreign_instance('recovery readiness')
    before = registry_snapshot()
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  language=args.language, theme=args.theme, scale=args.scale, checks=[], exits=[])

    def check(name, value):
        result['checks'].append(dict(name=name, passed=bool(value)))
        print(name, bool(value), flush=True)
        assert value, name

    def run(mode):
        folder = out / mode
        folder.mkdir(exist_ok=True)
        doc = folder / 'sample.txt'
        original = 'Original body line.\n'
        doc.write_text(original, encoding='utf-8')
        settings = folder / 'appdata/EUI-Edits/settings.ini'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text(f'vault=\nlast_file={doc}\nmode=0\nui_scale={args.scale}\nui_font_size=14\n'
                            f'theme={args.theme}\nanimations=0\nui_language={args.language}\n'
                            'show_status_bar=1\nline_numbers=1\n', encoding='utf-8')
        temp = folder / 'temp'
        temp.mkdir(exist_ok=True)
        env = dict(os.environ, APPDATA=str(settings.parent.parent), TEMP=str(temp), TMP=str(temp),
                   NEO_SINGLE_INSTANCE='1' if mode.startswith('forwarded') else '0',
                   NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
        recovery = settings.parent / 'recovery.txt'
        recovered_body = 'Recovered unsaved manuscript.\n'
        startup_recovery = mode.startswith('recovery_')
        if startup_recovery:
            metadata = dict(origin=str(folder/'crashed.txt'), encoding='utf8', codepage=0,
                            bom=False, crlf=False, language='text')
            recovery.write_text('#neo-recovery-v2 '+json.dumps(metadata)+'\n'+recovered_body, encoding='utf-8')
        proc = subprocess.Popen([str(exe)]+([str(doc)] if startup_recovery else []), cwd=exe.parent, env=env)
        hwnd = None

        def owned():
            cad.assert_unlocked('recovery readiness')
            pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            assert proc.poll() is None and pid.value == proc.pid and cad.ensure_foreground(hwnd)

        def point(x, y):
            pt = cad.wintypes.POINT(round(x * unit), round(y * unit))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
            cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
            cad.user32.WindowFromPoint.restype = ctypes.c_void_p
            cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            cad.user32.GetAncestor.restype = ctypes.c_void_p
            assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(pt), 2) == hwnd, 'client is obscured'
            return pt

        def click(x, y):
            owned()
            pt = point(x, y)
            cad.user32.SetCursorPos(pt.x, pt.y)
            time.sleep(.06)
            cad.user32.mouse_event(2, 0, 0, 0, 0)
            time.sleep(.06)
            cad.user32.mouse_event(4, 0, 0, 0, 0)
            time.sleep(.4)

        def key(*keys):
            owned()
            for k in keys:
                cad.user32.keybd_event(k, 0, 0, 0)
                time.sleep(.03)
            for k in reversed(keys):
                cad.user32.keybd_event(k, 0, 2, 0)
                time.sleep(.03)
            time.sleep(.35)

        def capture(name):
            owned()
            time.sleep(.3)
            pixels = cad.capture_client(hwnd)
            for x, y in ((15, 15), (pixels[0] / 2, pixels[1] / 2), (pixels[0] - 30, pixels[1] - 30)):
                point(x / unit, y / unit)
            cad.write_png(str(folder / (name + '.png')), *pixels)
            return pixels

        def action(index):
            w, h, raw = cad.capture_client(hwnd)
            img = Image.frombytes('RGBA', (w, h), raw, 'raw', 'BGRA')
            width = min(520, max(220, w/unit - 48))
            button = (width - 64) / 3
            cancel_x = (w/unit-width)/2 + 24 + 2*(button+8)
            # The default Cancel border and primary Save button identify the
            # real action row even when translated text changes dialog height.
            ys = []
            for y in range(round(h*.25), round(h*.8)):
                for x in range(round(cancel_x*unit), round((cancel_x+button)*unit)):
                    r, g, b, _ = img.getpixel((x, y))
                    if b > 130 and b > r*1.25 and b > g*1.08:
                        ys.append(y)
            assert ys, 'modal action row not visible'
            action_y = (min(ys)+max(ys))/2/unit
            w, h = w / unit, h / unit
            click((w-width)/2 + 24 + index*(button+8) + button/2, action_y)

        def cancel_native_save():
            matches = []
            callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
            @callback_type
            def visit(handle, _):
                pid = ctypes.c_ulong()
                name = ctypes.create_unicode_buffer(80)
                cad.user32.GetWindowThreadProcessId(handle, ctypes.byref(pid))
                cad.user32.GetClassNameW(handle, name, 80)
                if pid.value in {proc.pid, *cad.direct_child_process_ids(proc.pid)} and name.value == '#32770' and cad.user32.IsWindowVisible(handle):
                    matches.append(handle)
                return True
            for _ in range(50):
                matches.clear()
                cad.user32.EnumWindows(visit, 0)
                if matches:
                    break
                time.sleep(.1)
            assert len(matches) == 1 and cad.ensure_foreground(matches[0]), 'owned Save As dialog absent'
            cad.write_png(str(folder/'native-save-as.png'), *cad.capture_client(matches[0]))
            cad.user32.keybd_event(0x1B, 0, 0, 0)
            cad.user32.keybd_event(0x1B, 0, 2, 0)
            time.sleep(.6)
            owned()

        try:
            for _ in range(150):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            assert hwnd, 'window did not appear'
            owned()
            cad.user32.SetWindowPos(hwnd, None, 40, 40, 1120, 840, 4)
            time.sleep(.7)
            unit = cad.user32.GetDpiForWindow(hwnd) / 96 * args.scale
            capture('opened')
            if startup_recovery:
                capture('startup-recovery-confirm')
                check('explicit startup preserves recovery before consent', recovery.exists() and
                      recovered_body in recovery.read_text(encoding='utf-8'))
                check('explicit startup leaves requested file unchanged', doc.read_text(encoding='utf-8') == original)
                if mode == 'recovery_discard':
                    action(1)
                    check('explicit discard alone clears recovery', not recovery.exists())
                    capture('startup-requested-file')
                else:
                    if mode == 'recovery_saveas_cancel':
                        action(0)
                        cancel_native_save()
                        check('startup canceled Save As retains recovery', recovery.exists() and
                              recovered_body in recovery.read_text(encoding='utf-8'))
                    key(0x0D)
                    check('startup default Cancel retains recovery draft', recovery.exists() and
                          recovered_body in recovery.read_text(encoding='utf-8'))
                    capture('startup-recovered-editor')
                    cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    time.sleep(.5)
                    capture('recovered-close-confirm')
                    action(1)  # Explicitly discard only this isolated test draft.
                check('startup actions preserve requested file bytes', doc.read_text(encoding='utf-8') == original)
            elif mode == 'small':
                cad.user32.SetWindowPos(hwnd, None, 40, 40, 800, 450, 4)
                time.sleep(.6)
                click(140 if args.language == 'en' else 112, 17)
                capture('short-view-menu')
                pt = point(200, 180)
                cad.user32.SetCursorPos(pt.x, pt.y)
                for _ in range(5):
                    cad.user32.mouse_event(0x800, 0, 0, ctypes.c_ulong(-120).value, 0)
                    time.sleep(.1)
                time.sleep(.5)
                capture('short-menu-scrolled')
                # Windows enforces a minimum physical client size. At 100%
                # the menu already fits; at 150% scrolling reveals its last row.
                w, h, _ = cad.capture_client(hwnd)
                result['small_geometry'] = dict(width=w, height=h, unit=unit)
                click(360 if args.language == 'en' else 260, min(338, h/unit-32))
                capture('short-window-settings')
                key(0x1B)
                capture('editor-return')
            elif mode == 'about':
                cad.user32.SetWindowPos(hwnd, None, 40, 40, 800, 450, 4)
                time.sleep(.6)
                key(0x11, 0xBC)
                w, h, _ = cad.capture_client(hwnd)
                logical_w = w/unit
                compact = logical_w < 840
                # Four categories: Appearance, Editor, Files & System, About.
                click(10 + 7*(logical_w-24)/8, 99) if compact else click(85, 256)
                capture('about-top')
                pt = point(logical_w-42, 180)
                cad.user32.SetCursorPos(pt.x, pt.y)
                for _ in range(12):
                    cad.user32.mouse_event(0x800, 0, 0, ctypes.c_ulong(-120).value, 0)
                    time.sleep(.08)
                capture('about-bottom')
                key(0x1B)
                capture('editor-return')
            elif mode == 'settings':
                cad.user32.SetWindowPos(hwnd, None, 40, 40, 1400, 1080, 4)
                time.sleep(.7)
                key(0x11, 0xBC)
                capture('settings')
                w, h, _ = cad.capture_client(hwnd)
                compact = w/unit < 840
                click(10 + 5*(w/unit-24)/8, 99) if compact else click(85, 208)
                capture('associations')
                if compact:
                    # Outside the nested type list, the page itself must scroll
                    # so actions below the initial compact viewport are reachable.
                    pt = point(w/unit - 42, 180)
                    cad.user32.SetCursorPos(pt.x, pt.y)
                    for _ in range(10):
                        cad.user32.mouse_event(0x800, 0, 0, ctypes.c_ulong(-120).value, 0)
                        time.sleep(.08)
                    time.sleep(.4)
                    capture('compact-lower-page')
                    key(0x1B)
                    capture('editor-return')
                    cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    proc.wait(timeout=8)
                    check('compact settings normal exit', proc.returncode == 0)
                    return
                pt = point(400, 600)
                cad.user32.SetCursorPos(pt.x, pt.y)
                for delta in [-120] * 6 + [120] * 6:
                    cad.user32.mouse_event(0x800, 0, 0, ctypes.c_ulong(delta & 0xffffffff).value, 0)
                    time.sleep(.08)
                capture('scroll-return')
                # Click a selected row twice; only changes the draft selection.
                click(280, 548)
                key(0x20)
                capture('checkbox-keyboard')
                click(950, 462)
                capture('risk')
                key(0x1B)
                capture('risk-cancel')
                key(0x1B)
                capture('editor-return')
            else:
                if mode == 'saveas_cancel':
                    key(0x11, 0x4E)
                key(0x11, 0x46)
                key(0x1B)
                click(300, 110)
                key(0x11, 0x23)
                # Native character input avoids changing the user's IME layout.
                key(0x10)  # Clear any native focus/composition cancellation guard.
                cad.user32.PostMessageW(hwnd, 0x0102, ord('x'), 1)
                time.sleep(.4)
                capture('dirty')
                if mode.startswith('forwarded'):
                    cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    time.sleep(.5)
                    capture('close-before-forward')
                    forwarded_doc = folder/'forwarded.txt'
                    forwarded_body = 'Another file requested by the second process.\n'
                    forwarded_doc.write_text(forwarded_body, encoding='utf-8')
                    secondary = subprocess.run([str(exe), str(forwarded_doc)], cwd=exe.parent,
                                               env=env, timeout=10)
                    check('secondary instance forwarded and exited', secondary.returncode == 0)
                    forward_request = temp/'EUI-Edits.next-open'
                    check('modal preserves the unconsumed forward request', forward_request.exists())
                    capture('close-after-forward')
                    if mode in ('forwarded_cancel', 'forwarded_keys'):
                        if mode == 'forwarded_keys':
                            key(0x0D)
                        else:
                            action(2)  # Cancel close; now the deferred open gets its own gate.
                        check('forward request consumed after close cancellation', not forward_request.exists())
                        capture('deferred-open-confirm')
                        if mode == 'forwarded_keys':
                            key(0x0D)
                        else:
                            action(2)  # Cancel the deferred open too, preserving this draft.
                        key(0x11, 0x53)
                        check('deferred-open cancellation retains original buffer', doc.read_text(encoding='utf-8') == original+'x')
                        cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    else:
                        action(0)
                    proc.wait(timeout=8)
                    check('forwarding cannot replace pending close', proc.returncode == 0)
                    check('pending close saves original manuscript', doc.read_text(encoding='utf-8') == original+'x')
                    check('forwarding leaves second document unchanged', forwarded_doc.read_text(encoding='utf-8') == forwarded_body)
                    check('closed instance leaves no stale forward request', not forward_request.exists())
                elif mode in ('conflict', 'deleted'):
                    external = 'External program changed this file.\n'
                    if mode == 'deleted':
                        doc.unlink()
                    else:
                        doc.write_text(external, encoding='utf-8')
                    key(0x11, 0x53)
                    capture('conflict')
                    key(0x0D)  # Default choice is Cancel.
                    check(mode+' default Enter preserves disk', not doc.exists() if mode == 'deleted' else doc.read_text(encoding='utf-8') == external)
                    key(0x11, 0x53)
                    capture('conflict-again')
                    if mode == 'conflict':
                        action(1)
                        cancel_native_save()
                        check('conflict Save As cancellation preserves external bytes', doc.read_text(encoding='utf-8') == external)
                        key(0x11, 0x53)
                    action(0)
                    check('explicit overwrite saves editor buffer', doc.read_text(encoding='utf-8') == original + 'x')
                elif mode == 'language':
                    # View -> Interface language -> Chinese / English.
                    click(140 if args.language == 'en' else 112, 17)
                    capture('view-menu')
                    pt = point(200, 306)
                    cad.user32.SetCursorPos(pt.x, pt.y)
                    time.sleep(.5)
                    capture('language-menu')
                    screen_w = cad.capture_client(hwnd)[0]/unit
                    child_x = (460 if screen_w >= 724 else 100) if args.language == 'en' else 405
                    click(child_x, 346 if args.language == 'en' else 378)
                    target = 'zh-CN' if args.language == 'en' else 'en'
                    check('menu language choice persisted', 'ui_language='+target in settings.read_text(encoding='utf-8'))
                    check('language switch preserves dirty disk state', doc.read_text(encoding='utf-8') == original)
                    capture('language-switched')
                    key(0x10)
                    cad.user32.PostMessageW(hwnd, 0x0102, ord('y'), 1)
                    time.sleep(.4)
                    key(0x11, 0x53)
                    check('language switch preserves buffer and editor focus', doc.read_text(encoding='utf-8') == original+'xy')
                    cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    proc.wait(timeout=8)
                    check('language first normal exit', proc.returncode == 0)
                    proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
                    hwnd = None
                    for _ in range(150):
                        hwnd = window_for_pid(proc.pid)
                        if hwnd:
                            break
                        time.sleep(.1)
                    assert hwnd
                    owned()
                    time.sleep(.5)
                    capture('language-restarted')
                    check('language preference survives restart', 'ui_language='+target in settings.read_text(encoding='utf-8'))
                else:
                    cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    time.sleep(.5)
                    capture('close-confirm')
                    key(0x0D)
                    check('close default Enter keeps window', proc.poll() is None)
                    check('close Cancel preserves disk', doc.read_text(encoding='utf-8') == original)
                    cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                    time.sleep(.5)
                    capture('close-again')
                    if mode == 'failed':
                        cad.kernel32.SetFileAttributesW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong]
                        assert cad.kernel32.SetFileAttributesW(str(doc), 1)
                        action(0)
                        check('failed close-save retains window', proc.poll() is None)
                        check('failed close-save preserves disk', doc.read_text(encoding='utf-8') == original)
                        capture('save-failed')
                        assert cad.kernel32.SetFileAttributesW(str(doc), 128)
                        action(1)
                    elif mode == 'saveas_cancel':
                        action(0)
                        cancel_native_save()
                        check('close Save As cancellation retains window', proc.poll() is None)
                        capture('save-as-canceled')
                        action(1)
                    else:
                        action(0 if mode == 'save' else 1)
                    proc.wait(timeout=8)
                    check(mode + ' normal exit', proc.returncode == 0)
                    check(mode + ' disk result', doc.read_text(encoding='utf-8') == (original+'x' if mode == 'save' else original))
            if proc.poll() is None:
                cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                proc.wait(timeout=8)
            check(mode + ' exit code', proc.returncode == 0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
                result['forced_cleanup'] = mode
            result['exits'].append(dict(mode=mode, code=proc.returncode))

    try:
        for mode in args.modes.split(','):
            run(mode)
        check('production associations unchanged', before == registry_snapshot())
    finally:
        (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()

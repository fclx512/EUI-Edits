"""Attempt real installed Chinese IME input on an owned, disposable window.

This probe uses SendInput virtual keys, never WM_CHAR/preedit fabrication.
If native candidate/composition telemetry is unavailable, status is manual_needed,
even when a screenshot and a Chinese commit are observed. Candidate appearance,
page labels and near-caret placement still require visual review.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time

import win_capture as cad
from capture_markdown import window_for_pid, write_settings


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', wintypes.WORD), ('wScan', wintypes.WORD),
                ('dwFlags', wintypes.DWORD), ('time', wintypes.DWORD),
                ('dwExtraInfo', ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', wintypes.LONG), ('dy', wintypes.LONG),
                ('mouseData', wintypes.DWORD), ('dwFlags', wintypes.DWORD),
                ('time', wintypes.DWORD), ('dwExtraInfo', ctypes.c_size_t)]


class INPUTUNION(ctypes.Union):
    _fields_ = [('ki', KEYBDINPUT), ('mi', MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [('type', wintypes.DWORD), ('data', INPUTUNION)]


SOURCE = ('ANCHOR 普通正文 中文 😀\n\n# 标题\n\n'
          '**强调文字** 与 `inline code`\n\n> 引用中的中文\n\n'
          '- [ ] 任务中的中文\n\n'
          + ''.join(f'第 {i:02d} 行 中文 😀 普通段落 alpha beta gamma delta\n' for i in range(1, 61))
          + '\nEND_ANCHOR')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=0)
    ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--width', type=int, default=1250)
    ap.add_argument('--height', type=int, default=800)
    ap.add_argument('--prepare-manual', action='store_true')
    ap.add_argument('--focus-only', action='store_true',
                    help='Exercise client focus and real loss of foreground using an owned peer window')
    ap.add_argument('--continue-without-imm', action='store_true',
                    help='Run text assertions even when cross-process IMM is unavailable; candidate screenshots still need visual review')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    runtime = out / 'runtime'
    doc = out / 'ime-sample.md'
    doc.write_text(SOURCE, encoding='utf-8')
    write_settings(str(runtime / 'EUI-Edits/settings.ini'), str(doc), args.theme, 16, str(args.scale))
    with (runtime / 'EUI-Edits/settings.ini').open('a', encoding='utf-8') as target:
        target.write('animations=0\n')
    # A manual launcher changes environment only in this PowerShell invocation.
    def ps_quote(value):
        return "'" + str(value).replace("'", "''") + "'"
    (out / 'start-manual.ps1').write_text(
        '$previousAppData = $env:APPDATA\n'
        'try {\n'
        f'    $env:APPDATA = {ps_quote(runtime)}\n'
        f'    Push-Location -LiteralPath {ps_quote(exe.parent)}\n'
        f'    & {ps_quote(exe)}\n'
        '} finally { Pop-Location; $env:APPDATA = $previousAppData }\n', encoding='utf-8-sig')
    manifest = dict(executable=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                    document=str(doc), theme=args.theme, scale=args.scale,
                    status='prepared_manual', checks=[], native_observations=[],
                    continue_without_imm=args.continue_without_imm,
                    limitations=['Actual candidate page contents and near-caret positioning need visual review.',
                                 'Only the owned target thread input locale is requested; no global language configuration is written.',
                                 'Third party IME, physical multi-monitor DPI and candidate mouse selection need manual testing.'])
    report = out / 'conditions.json'
    if args.prepare_manual:
        report.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
        print(str(out / 'start-manual.ps1'))
        return

    cad.make_dpi_aware()
    cad.assert_unlocked('IME candidate')
    cad.assert_no_foreign_instance('IME candidate')
    user = cad.user32
    user.GetKeyboardLayoutList.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
    user.GetKeyboardLayoutList.restype = ctypes.c_int
    user.GetKeyboardLayout.argtypes = [wintypes.DWORD]
    user.GetKeyboardLayout.restype = ctypes.c_void_p
    user.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
    user.SendInput.restype = wintypes.UINT
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.PostMessageW.restype = wintypes.BOOL
    user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user.SetWindowPos.restype = wintypes.BOOL
    imm = ctypes.WinDLL('imm32', use_last_error=True)
    imm.ImmGetContext.argtypes = [wintypes.HWND]
    imm.ImmGetContext.restype = ctypes.c_void_p
    imm.ImmReleaseContext.argtypes = [wintypes.HWND, ctypes.c_void_p]
    imm.ImmGetCompositionStringW.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
    imm.ImmGetCompositionStringW.restype = wintypes.LONG
    imm.ImmGetCandidateListW.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
    imm.ImmGetCandidateListW.restype = wintypes.DWORD
    layout_count = user.GetKeyboardLayoutList(0, None)
    layouts = (ctypes.c_void_p * layout_count)()
    user.GetKeyboardLayoutList(layout_count, layouts)
    chinese_layouts = [int(item) for item in layouts if item and int(item) & 0xffff == 0x0804]
    manifest['installed_layouts'] = [hex(int(item)) for item in layouts if item]
    if not chinese_layouts:
        manifest.update(status='manual_needed', reason='No loaded Chinese 0804 layout; no system layout will be installed.')
        report.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(manifest, ensure_ascii=False, indent=2))
        return

    env = dict(os.environ, APPDATA=str(runtime), NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
    proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
    hwnd, old_layout, thread = None, None, None
    attempted_layout = False
    peer = None

    def owned():
        cad.assert_unlocked('IME candidate')
        owner = wintypes.DWORD()
        assert hwnd and user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner)) == thread
        assert owner.value == proc.pid and proc.poll() is None
        # Refuse to inject keys after focus is stolen. Re-focus only at startup.
        assert user.GetForegroundWindow() == hwnd, 'Foreground ownership lost; stopping IME keys'

    def check(name, condition):
        manifest['checks'].append(dict(name=name, passed=bool(condition)))
        if not condition:
            raise AssertionError(name)

    def key(vk, down=True):
        owned()
        flags = (0 if down else 2) | (1 if vk in (0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28) else 0)
        event = INPUT(1, INPUTUNION(ki=KEYBDINPUT(vk, user.MapVirtualKeyW(vk, 0), flags, 0, 0)))
        assert user.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT)) == 1

    def chord(*vks):
        pressed = []
        try:
            for vk in vks:
                key(vk)
                pressed.append(vk)
                time.sleep(.025)
        finally:
            for vk in reversed(pressed):
                # Release injected keys even when ownership is interrupted.
                flags = 2 | (1 if vk in (0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28) else 0)
                event = INPUT(1, INPUTUNION(ki=KEYBDINPUT(vk, user.MapVirtualKeyW(vk, 0), flags, 0, 0)))
                user.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT))
        time.sleep(.18)

    def type_pinyin(text):
        for char in text:
            chord(ord(char.upper()))
        time.sleep(.35)

    def click_client(x, y):
        owned()
        point = wintypes.POINT(int(x), int(y))
        assert user.ClientToScreen(hwnd, ctypes.byref(point))
        assert user.SetCursorPos(point.x, point.y)
        manifest.setdefault('client_clicks', []).append(dict(client=[x, y], screen=[point.x, point.y]))
        for flags in (2, 4):
            event = INPUT(0, INPUTUNION(mi=MOUSEINPUT(0, 0, 0, flags, 0, 0)))
            assert user.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT)) == 1
            time.sleep(.05)
        time.sleep(.4)

    def copied_field():
        chord(0x11, 0x41)
        chord(0x11, 0x43)
        user.GetClipboardData.argtypes = [wintypes.UINT]
        user.GetClipboardData.restype = ctypes.c_void_p
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GlobalLock.argtypes = [ctypes.c_void_p]
        kernel.GlobalLock.restype = ctypes.c_void_p
        kernel.GlobalUnlock.argtypes = [ctypes.c_void_p]
        assert user.OpenClipboard(hwnd)
        try:
            handle = user.GetClipboardData(13)
            assert handle
            pointer = kernel.GlobalLock(handle)
            assert pointer
            try:
                return ctypes.wstring_at(pointer)
            finally:
                kernel.GlobalUnlock(handle)
        finally:
            user.CloseClipboard()

    def focus_sequence():
        nonlocal peer
        unit = user.GetDpiForWindow(hwnd) / 96 * args.scale
        manifest['focus_dpi'] = user.GetDpiForWindow(hwnd)
        manifest['focus_unit'] = unit
        width, _, _ = cad.capture_client(hwnd)
        logical_width = width / unit
        find_width = min(550, max(280, logical_width - min(264, logical_width * .38) - 29))
        find_x = logical_width - find_width - 14
        chord(0x11, 0x48)
        click_client(360 * unit, 250 * unit)
        # Keep the native candidate surface away from the find panel. Some TSF
        # popups intercept clicks outside their visible ink rectangle.
        chord(0x11, 0x23)
        type_pinyin('nihao')
        observe('focus-editor-before')
        # The candidate popup below the first document line can cover the
        # query's center. Click inside its top padding, above that popup.
        click_client((find_x + 80) * unit, 57 * unit)
        observe('focus-query-after-transfer')
        chord(0x11, 0x41)
        chord(0x08)
        type_pinyin('zhongwen')
        observe('focus-query-preedit')
        chord(0x20)
        query = copied_field()
        manifest['focus_query_phrase'] = query
        check('Query commits one CJK phrase after editor cancellation', 0 < len(query) <= 8 and
              all('\u3400' <= char <= '\u9fff' for char in query))
        observe('focus-query-centered')
        click_client((find_x + 80) * unit, 110 * unit)
        type_pinyin('nihao')
        observe('focus-replacement-preedit')
        chord(0x20)
        replacement = copied_field()
        manifest['focus_replacement_phrase'] = replacement
        check('Replacement commits one CJK phrase without query carryover', 0 < len(replacement) <= 8 and
              all('\u3400' <= char <= '\u9fff' for char in replacement))
        observe('focus-replacement-centered')
        chord(0x1b)
        cancel_save(SOURCE, 'Cross-control composition does not enter the document')
        chord(0x11, 0x24)
        type_pinyin('nihao')
        observe('focus-relocation-before')
        click_client(400 * unit, 270 * unit)
        observe('focus-relocation-after')
        cancel_save(SOURCE, 'Client relocation cancels preedit before moving insertion point')
        chord(0x11, 0x24)
        type_pinyin('nihao')
        observe('focus-app-before')
        user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                       wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                       wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p]
        user.CreateWindowExW.restype = wintypes.HWND
        peer = user.CreateWindowExW(0, 'STATIC', 'Owned IME focus regression peer',
                                   0x10cf0000, 50, 50, 360, 180, None, None, None, None)
        assert peer
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(peer, ctypes.byref(owner))
        assert owner.value == os.getpid()
        assert user.SetForegroundWindow(peer)
        assert user.GetForegroundWindow() == peer
        # Pump only this probe's window thread while the editor actually loses
        # foreground. This is not a synthetic WM_KILLFOCUS injection.
        deadline = time.monotonic() + .7
        message = wintypes.MSG()
        while time.monotonic() < deadline:
            while user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
                user.TranslateMessage(ctypes.byref(message))
                user.DispatchMessageW(ctypes.byref(message))
            time.sleep(.02)
        check('Foreground really transferred to the owned peer', user.GetForegroundWindow() == peer)
        width, height, pixels = cad.capture_client(hwnd)
        cad.write_png(str(out / 'focus-app-inactive.png'), width, height, pixels)
        assert cad.ensure_foreground(hwnd)
        owned()
        time.sleep(.5)
        observe('focus-app-returned')
        chord(0x1b)
        chord(0x11, 0x53)
        time.sleep(.4)
        after_blur = doc.read_text(encoding='utf-8')
        (out / 'application-blur-saved.md').write_text(after_blur, encoding='utf-8')
        manifest['application_blur_policy'] = ('cancel' if after_blur == SOURCE else
                                              'commit_raw_pinyin' if after_blur == 'nihao' + SOURCE else 'unexpected')
        # The manual contract permits the IME's blur confirmation policy. Do not
        # retroactively undo a real result just because focus later changes.
        check('Application blur finishes at the old owner exactly once', after_blur in (SOURCE, 'nihao' + SOURCE))
        if after_blur != SOURCE:
            chord(0x11, 0x5a)
        cancel_save(SOURCE, 'Blur completion restores with one undo or cancellation')
        chord(0x11, 0x24)
        type_pinyin('nihao')
        observe('focus-app-fresh-preedit')
        chord(0x20)
        chord(0x11, 0x53)
        time.sleep(.4)
        committed = doc.read_text(encoding='utf-8')
        prefix = committed[:-len(SOURCE)] if committed.endswith(SOURCE) else ''
        check('Fresh composition after refocus commits once', 0 < len(prefix) <= 8 and
              all('\u3400' <= char <= '\u9fff' for char in prefix))
        chord(0x11, 0x5a)
        cancel_save(SOURCE, 'Refocus commit remains one undo step')
        manifest['status'] = 'passed_text_checks_manual_visual_pending'

    def observe(label):
        owned()
        context = imm.ImmGetContext(hwnd)
        observation = dict(label=label, context_available=bool(context), composition='', candidates=[],
                           candidate_count=0, candidate_selection=None, page_start=None, page_size=None)
        if context:
            try:
                size = imm.ImmGetCompositionStringW(context, 8, None, 0)
                observation['composition_bytes'] = size
                if 0 < size < 65536:
                    buffer = ctypes.create_string_buffer(size)
                    if imm.ImmGetCompositionStringW(context, 8, buffer, size) == size:
                        observation['composition'] = buffer.raw.decode('utf-16-le', errors='replace')
                size = imm.ImmGetCandidateListW(context, 0, None, 0)
                observation['candidate_list_bytes'] = size
                if 24 <= size < 1024 * 1024:
                    buffer = ctypes.create_string_buffer(size)
                    if imm.ImmGetCandidateListW(context, 0, buffer, size) == size:
                        data = buffer.raw
                        _, _, count, selection, page_start, page_size = struct.unpack_from('<6I', data)
                        if count <= (size - 24) // 4:
                            observation.update(candidate_count=count, candidate_selection=selection,
                                               page_start=page_start, page_size=page_size)
                            for offset in struct.unpack_from(f'<{count}I', data, 24):
                                if offset < size and offset % 2 == 0:
                                    observation['candidates'].append(data[offset:].decode('utf-16-le', errors='replace').split('\0', 1)[0])
            finally:
                imm.ImmReleaseContext(hwnd, context)
        manifest['native_observations'].append(observation)
        cad.write_png(str(out / f'{label}-client.png'), *cad.capture_client(hwnd))
        # Expanded window crop includes candidate popup even outside client bounds.
        from PIL import ImageGrab
        rect = wintypes.RECT()
        user.GetWindowRect(hwnd, ctypes.byref(rect))
        screen_width, screen_height = user.GetSystemMetrics(0), user.GetSystemMetrics(1)
        ImageGrab.grab(bbox=(max(0, rect.left - 20), max(0, rect.top - 20),
                            min(screen_width, rect.right + 300), min(screen_height, rect.bottom + 300))).save(out / f'{label}-candidate.png')
        return observation

    def cancel_save(expected, label):
        chord(0x1b)
        time.sleep(.25)
        chord(0x11, 0x53)
        time.sleep(.35)
        actual = doc.read_text(encoding='utf-8')
        (out / f'{label}-saved.md').write_text(actual, encoding='utf-8')
        check(label + ' exact document', actual == expected)

    try:
        for _ in range(100):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None:
                break
            time.sleep(.1)
        assert hwnd, 'Owned test window did not appear'
        owner = wintypes.DWORD()
        thread = user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        assert owner.value == proc.pid and cad.ensure_foreground(hwnd)
        owned()
        user.SetWindowPos(hwnd, None, 20, 20, args.width, args.height, 0x0004)
        time.sleep(1.5)
        # Existing app focus path: find then close explicitly focuses the document.
        chord(0x11, 0x46)
        time.sleep(.4)
        chord(0x1b)
        time.sleep(.4)
        old_layout = user.GetKeyboardLayout(thread)
        manifest['original_layout'] = hex(old_layout or 0)
        attempted_layout = True
        assert user.PostMessageW(hwnd, 0x0050, 0, chinese_layouts[0])
        time.sleep(.7)
        new_layout = user.GetKeyboardLayout(thread)
        manifest['requested_layout'] = hex(chinese_layouts[0])
        manifest['actual_layout'] = hex(new_layout or 0)
        if not new_layout or new_layout & 0xffff != 0x0804:
            raise RuntimeError('Target thread did not activate existing Chinese layout; manual selection required')
        chord(0x11, 0x24)
        observe('ready')
        if args.focus_only:
            focus_sequence()
        else:
            type_pinyin('nihao')
            preedit = observe('nihao-preedit')
            native_api_proven = bool(preedit['composition']) and preedit['candidate_count'] > 0
            manifest['candidate_native_api_proven'] = native_api_proven
            if not native_api_proven and not args.continue_without_imm:
                chord(0x1b)
                chord(0x11, 0x53)
                time.sleep(.35)
                actual = doc.read_text(encoding='utf-8')
                (out / 'unsupported-telemetry-saved.md').write_text(actual, encoding='utf-8')
                manifest['unsupported_telemetry_saved_matches_source'] = actual == SOURCE
                raise RuntimeError('Real pinyin keys sent, but candidate/composition cannot both be proven through IMM; inspect screenshot and test manually')
            if native_api_proven:
                check('native preedit and candidates observed', True)
            cancel_save(SOURCE, 'Escape cancels preedit')
            type_pinyin('shi')
            observe('shi-page-initial')
            chord(0x22)
            observe('shi-page-down')
            chord(0x21)
            observe('shi-page-up')
            for vk, label in [(0x25, 'left'), (0x27, 'right'), (0x26, 'up'), (0x28, 'down')]:
                chord(vk)
                observe('shi-' + label)
            cancel_save(SOURCE, 'Candidate navigation preserves document')
            type_pinyin('nihao')
            observe('commit-before-space')
            chord(0x20)
            time.sleep(.35)
            observe('commit-after-space')
            chord(0x11, 0x53)
            time.sleep(.35)
            committed = doc.read_text(encoding='utf-8')
            (out / 'chinese-committed.md').write_text(committed, encoding='utf-8')
            prefix = committed[:-len(SOURCE)] if committed.endswith(SOURCE) else ''
            # The selected wording may vary with the user's dictionary. Require CJK,
            # one inserted phrase and exact surrounding document; retain chosen words.
            check('Space commits one CJK phrase and preserves remainder', bool(prefix) and
                  '\n' not in prefix and 'nihao' not in prefix.lower() and
                  len(prefix) <= 8 and any('\u3400' <= char <= '\u9fff' for char in prefix))
            manifest['committed_phrase'] = prefix
            chord(0x11, 0x5a)
            cancel_save(SOURCE, 'Undo restores pre-IME document')
            type_pinyin('nihao')
            observe('enter-before')
            chord(0x0d)
            chord(0x11, 0x53)
            time.sleep(.35)
            check('Enter commits raw pinyin without document newline', doc.read_text(encoding='utf-8') == 'nihao' + SOURCE)
            chord(0x11, 0x5a)
            cancel_save(SOURCE, 'Undo raw pinyin commit')
            chord(0x11, 0x23)
            type_pinyin('zhongwen')
            observe('scrolled-end-preedit')
            cancel_save(SOURCE, 'Scrolled end cancellation')
            manifest['status'] = ('passed_automated_manual_visual_pending' if native_api_proven
                                  else 'passed_text_checks_manual_visual_pending')
    except Exception as error:
        manifest.update(status='manual_needed' if not any(not item['passed'] for item in manifest['checks']) else 'failed',
                        reason=str(error))
    finally:
        if peer:
            user.DestroyWindow(peer)
        if hwnd and proc.poll() is None:
            # No key injection on a foreign foreground. Restore only owned thread.
            if user.GetForegroundWindow() == hwnd:
                try:
                    chord(0x1b)
                except Exception:
                    pass
            if attempted_layout and old_layout:
                user.PostMessageW(hwnd, 0x0050, 0, old_layout)
                time.sleep(.35)
                manifest['restored_layout'] = hex(user.GetKeyboardLayout(thread) or 0)
                manifest['layout_restored'] = user.GetKeyboardLayout(thread) == old_layout
            user.PostMessageW(hwnd, 0x0010, 0, 0)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                manifest.update(status='failed', reason='Owned window did not exit normally; no termination performed')
        manifest['exit_code'] = proc.poll()
        manifest['normal_exit'] = proc.poll() == 0
        manifest['executable_unchanged'] = manifest['sha256'] == hashlib.sha256(exe.read_bytes()).hexdigest()
        if not manifest['normal_exit'] or not manifest['executable_unchanged'] or manifest.get('layout_restored') is False:
            manifest['status'] = 'failed'
        report.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    if manifest['status'] == 'failed':
        raise SystemExit(1)


if __name__ == '__main__':
    main()

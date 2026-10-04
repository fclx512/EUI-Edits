"""Own-process Win32 regression for dirty tab close, canceled Save As, and conflicts.

Run serially against an explicit final executable hash. All settings, session data,
documents, and temporary picker state are isolated under --out.
"""
import argparse
import ctypes
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import win_capture as cad
from capture_markdown import window_for_pid
import tab_menu_visual as visual


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--expected-sha256', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--scale', type=float, default=0.8)
    ap.add_argument('--target-effective-scale', type=float, default=1.0)
    ap.add_argument('--ui-font', type=int, default=14)
    ap.add_argument('--window', default='1600x1000', help='physical client WxH')
    args = ap.parse_args()

    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    if not exe.is_file():
        ap.error(f'executable does not exist: {exe}')
    try:
        requested_w, requested_h = (int(part) for part in args.window.lower().split('x', 1))
    except ValueError:
        ap.error('--window must be WxH')
    if not (700 <= requested_w <= 2400 and 500 <= requested_h <= 1400):
        ap.error('--window must be 700..2400 by 500..1400 physical pixels')
    actual_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    if actual_hash.lower() != args.expected_sha256.lower():
        ap.error(f'executable SHA256 mismatch: {actual_hash}')
    out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware()
    cad.assert_unlocked('tab close Save As cancellation')

    config = out / 'appdata' / 'EUI-Edits'
    temp = out / 'temp'
    fixtures = out / 'fixtures'
    config.mkdir(parents=True)
    temp.mkdir(parents=True)
    fixtures.mkdir(parents=True)
    first_file = fixtures / 'first-clean.md'
    second_file = fixtures / 'second-clean.md'
    first_file.write_text('# First clean document\n\nFirst sentinel.\n', encoding='utf-8')
    second_file.write_text('# Second clean document\n\nSecond sentinel.\n', encoding='utf-8')
    (config / 'settings.ini').write_text(
        f'mode=1\nui_scale={args.scale}\nui_font_size={args.ui_font}\n'
        'theme=1\nanimations=0\nui_language=en\nshow_status_bar=1\n', encoding='utf-8')
    env = dict(os.environ, APPDATA=str(config.parent), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')

    result = dict(exe=str(exe), sha256=actual_hash, expected_sha256=args.expected_sha256.lower(),
                  requested_client_px=[requested_w, requested_h], scale=args.scale,
                  target_effective_scale=args.target_effective_scale, checks=[], captures=[],
                  snapshots=[], picker_evidence=[], exits=[], exception=None,
                  normal_exit_verified=False)
    failures = []
    proc = None
    hwnd = None
    unit = 1.0
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.OpenProcess.argtypes = [cad.wintypes.DWORD, cad.wintypes.BOOL, cad.wintypes.DWORD]
    kernel32.OpenProcess.restype = cad.wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [cad.wintypes.HANDLE]
    kernel32.QueryFullProcessImageNameW.argtypes = [cad.wintypes.HANDLE, cad.wintypes.DWORD,
                                                     cad.wintypes.LPWSTR,
                                                     ctypes.POINTER(cad.wintypes.DWORD)]
    kernel32.QueryFullProcessImageNameW.restype = cad.wintypes.BOOL
    kernel32.GetExitCodeProcess.argtypes = [cad.wintypes.HANDLE, ctypes.POINTER(cad.wintypes.DWORD)]
    kernel32.GetExitCodeProcess.restype = cad.wintypes.BOOL
    kernel32.WaitForSingleObject.argtypes = [cad.wintypes.HANDLE, cad.wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = cad.wintypes.DWORD
    cad.user32.GetWindow.argtypes = [cad.wintypes.HWND, cad.wintypes.UINT]
    cad.user32.GetWindow.restype = cad.wintypes.HWND

    def check(name, condition, detail=None):
        passed = bool(condition)
        result['checks'].append(dict(name=name, passed=passed, detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name, detail if detail is not None else '', flush=True)
        if not passed:
            failures.append(name)
        return passed

    def require(name, condition, detail=None):
        if not check(name, condition, detail):
            raise AssertionError(name)

    def owned():
        cad.assert_unlocked('tab close Save As cancellation')
        pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if proc is None or proc.poll() is not None or pid.value != proc.pid or \
                cad.user32.GetForegroundWindow() != hwnd:
            raise RuntimeError(f'root ownership/foreground lost: hwnd pid={pid.value}, '
                               f'process={proc.pid if proc else None}')

    def client_rect():
        rect = cad.wintypes.RECT()
        cad.user32.GetClientRect(hwnd, ctypes.byref(rect))
        return rect.right, rect.bottom

    def resize_client():
        for _ in range(12):
            owned()
            cw, ch = client_rect()
            if abs(cw - requested_w) <= 2 and abs(ch - requested_h) <= 2:
                return cw, ch
            outer = cad.wintypes.RECT()
            cad.user32.GetWindowRect(hwnd, ctypes.byref(outer))
            dw, dh = (outer.right - outer.left - cw), (outer.bottom - outer.top - ch)
            cad.user32.SetWindowPos(hwnd, None, 20, 20, requested_w + dw, requested_h + dh, 0x0004)
            time.sleep(0.3)
        return client_rect()

    def key(*codes):
        owned()
        for code in codes:
            cad.user32.keybd_event(code, cad.user32.MapVirtualKeyW(code, 0), 0, 0)
            time.sleep(0.03)
        for code in reversed(codes):
            cad.user32.keybd_event(code, cad.user32.MapVirtualKeyW(code, 0), 2, 0)
            time.sleep(0.03)
        time.sleep(0.3)

    def type_text(value):
        owned()
        for char in value:
            cad.user32.PostMessageW(hwnd, 0x0102, ord(char), 1)
        time.sleep(0.35)

    def click(x, y):
        owned()
        point = cad.wintypes.POINT(round(x * unit), round(y * unit))
        cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
        cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
        cad.user32.WindowFromPoint.restype = ctypes.c_void_p
        cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        cad.user32.GetAncestor.restype = ctypes.c_void_p
        top = cad.user32.WindowFromPoint(point)
        if cad.user32.GetAncestor(top, 2) != hwnd:
            raise RuntimeError('click target is obscured by a foreign window')
        cad.click(hwnd, round(x * unit), round(y * unit))
        time.sleep(0.25)

    def capture(name, save=True):
        owned()
        width, height, raw = cad.capture_client(hwnd)
        if save:
            path = out / f'{name}.png'
            cad.write_png(str(path), width, height, raw)
            result['captures'].append(str(path.relative_to(out)))
        return width, height, raw

    def manifest():
        return json.loads((config / 'session' / 'manifest.json').read_text(encoding='utf-8'))

    def record_snapshot(label, snap=None):
        snap = manifest() if snap is None else snap
        result['snapshots'].append(dict(label=label, snapshot=snap))
        return snap

    def wait_manifest(predicate, label, timeout=10.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            owned()
            try:
                last = manifest()
                if predicate(last):
                    return record_snapshot(label, last)
            except (OSError, ValueError, KeyError, TypeError):
                pass
            time.sleep(0.06)
        raise AssertionError(f'{label} timed out; last={last}')

    def ids(snap=None):
        snap = snap or manifest()
        return [int(row['id']) for row in snap.get('records', [])]

    def body(snap, row):
        name = row.get('body') or f"body-{snap.get('generation', '')}-{row['id']}.utf8"
        return (config / 'session' / name).read_text(encoding='utf-8')

    def root_snapshot(label):
        snap = record_snapshot(label)
        rows = snap.get('records', [])
        require(label + ' active TabId resolves', int(snap.get('active', 0)) in ids(snap),
                dict(active=snap.get('active'), ids=ids(snap)))
        return snap, rows

    def open_file(path):
        owned()
        request = temp / 'EUI-Edits.next-open'
        staged = temp / 'owned-open.tmp'
        staged.write_text(str(path), encoding='utf-8')
        staged.replace(request)
        key(0x10)  # Shift consumes the request through this process's private TEMP.
        require('owned deferred-open request consumed', not request.exists(), str(path))

    def select_tab_id(target_id):
        snap = manifest()
        order = ids(snap)
        if target_id not in order:
            raise RuntimeError(f'target TabId {target_id} is absent: {order}')
        active = int(snap.get('active', 0))
        if active not in order:
            raise RuntimeError(f'active TabId {active} is absent: {order}')
        for _ in range((order.index(target_id) - order.index(active)) % len(order)):
            key(0x11, 0x09)
        wait_manifest(lambda value: int(value.get('active', 0)) == target_id,
                      f'active TabId {target_id}')

    def list_geometry(width):
        screen_dip = width / unit
        text_width = sum(args.ui_font if ord(ch) >= 0x2E80 else args.ui_font * 0.55
                         for ch in 'Settings')
        settings_width = 8.0 + 16.0 + 6.0 + text_width + 12.0
        list_center = screen_dip - settings_width - 10.0 - 22.0
        panel_width = min(440.0, max(300.0, screen_dip * 0.42), screen_dip - 16.0)
        panel_left = max(8.0, min(screen_dip - settings_width - 10.0 - panel_width,
                                  screen_dip - panel_width - 8.0))
        return list_center, panel_left, panel_width

    def detect_tab_list(frame, width, height):
        _, panel_left, panel_width = list_geometry(width)
        menu_bar = max(30.0, args.ui_font + 20.0)
        left, right, rows = visual.panel_body_columns(
            frame, width, unit, int((panel_left - 6.0) * unit),
            int((panel_left + panel_width + 6.0) * unit),
            int((menu_bar + 2.0) * unit), min(height - int(32.0 * unit), int((menu_bar + 410.0) * unit)),
            min_straight=panel_width - 8.0)
        if left is None or not rows:
            return None
        return left / unit, right / unit, rows[0] / unit, rows[-1] / unit

    def open_tab_list(label):
        width, height, _ = capture(label + '-list-closed')
        list_center, _, _ = list_geometry(width)
        require(label + ' tab-list center lies in client', 0.0 < list_center < width / unit,
                list_center)
        click(list_center, 17.0)
        time.sleep(0.3)
        width2, height2, frame = capture(label + '-list-open')
        panel = detect_tab_list(frame, width2, height2)
        require(label + ' expanded tab list detected', panel is not None, panel)
        # The chosen target is normally visible. Reset the independent list
        # scroll to the top before using manifest-order row geometry.
        move_x, move_y = panel[0] + 30.0, panel[2] + 30.0
        point = cad.wintypes.POINT(round(move_x * unit), round(move_y * unit))
        cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
        owned()
        cad.user32.SetCursorPos(point.x, point.y)
        for _ in range(6):
            owned()
            cad.user32.mouse_event(0x0800, 0, 0, 120 * 100, 0)
            time.sleep(0.06)
        return panel

    def click_row_close(panel, index):
        x0, _, y0, _ = panel
        card_width = (list_geometry(requested_w)[2] - 16.0)
        # Three actions occupy 80 DIP; close is the third 24-DIP button.
        x = x0 + 8.0 + card_width - 90.0 + 68.0
        y = y0 + 8.0 + index * 76.0 + 20.0
        click(x, y)

    def inspect_safety_dialog(label, save=True):
        width, height, frame = capture(label, save=save)
        screen_dip_w, screen_dip_h = width / unit, height / unit
        expected_width = min(520.0, max(220.0, screen_dip_w - 48.0))
        center_x = screen_dip_w * 0.5
        y0 = max(1, int((screen_dip_h * 0.5 - 190.0) * unit))
        y1 = min(height - 1, int((screen_dip_h * 0.5 + 190.0) * unit))
        left, right, rows = visual.panel_body_columns(
            frame, width, unit, int((center_x - expected_width * 0.5 - 16.0) * unit),
            int((center_x + expected_width * 0.5 + 16.0) * unit), y0, y1,
            min_straight=max(200.0, expected_width - 28.0))
        geometry = None if left is None or not rows else dict(
            left_dip=left / unit, right_dip=right / unit,
            width_dip=(right - left + 1) / unit,
            top_dip=rows[0] / unit, bottom_dip=rows[-1] / unit)
        if geometry:
            geometry['recognized'] = abs(geometry['width_dip'] - expected_width) <= 5.0 and \
                abs((geometry['left_dip'] + geometry['right_dip']) * 0.5 - center_x) <= 8.0
        return geometry

    def wait_safety_dialog(visible, label, timeout=4.0):
        deadline = time.time() + timeout
        latest = None
        while time.time() < deadline:
            latest = inspect_safety_dialog(label + '-sample', save=False)
            found = bool(latest and latest.get('recognized'))
            if found == visible:
                return latest
            time.sleep(0.12)
        return latest

    def process_image_path(pid):
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            raise OSError(ctypes.get_last_error(), 'OpenProcess for picker image path failed')
        try:
            buffer = ctypes.create_unicode_buffer(32768)
            size = cad.wintypes.DWORD(len(buffer))
            if not kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
                raise OSError(ctypes.get_last_error(), 'QueryFullProcessImageNameW failed')
            return Path(buffer.value).resolve()
        finally:
            kernel32.CloseHandle(handle)

    def process_command_line(pid):
        powershell = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'),
                                  'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')
        if not Path(powershell).is_file():
            raise RuntimeError('Windows PowerShell is unavailable for picker command-line proof')
        command = (f"$p=Get-CimInstance Win32_Process -Filter 'ProcessId = {int(pid)}'; "
                   "if($p){$p.CommandLine}")
        completed = subprocess.run([powershell, '-NoLogo', '-NoProfile', '-NonInteractive',
                                    '-Command', command], capture_output=True, text=True,
                                   timeout=8, creationflags=0x08000000)
        if completed.returncode != 0 or not completed.stdout.strip():
            raise RuntimeError(f'cannot read isolated picker command line: {completed.stderr.strip()}')
        return completed.stdout.strip()

    def find_owned_picker(timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            cad.assert_unlocked('tab close Save As picker')
            root_pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(root_pid))
            if proc.poll() is not None or root_pid.value != proc.pid:
                raise RuntimeError('root process/window ownership was lost while Save As picker was open')
            children = set(cad.direct_child_process_ids(proc.pid))
            foreground = cad.user32.GetForegroundWindow()
            found = []

            @ctypes.WINFUNCTYPE(cad.wintypes.BOOL, cad.wintypes.HWND, cad.wintypes.LPARAM)
            def visit(candidate, _):
                pid = cad.wintypes.DWORD()
                cad.user32.GetWindowThreadProcessId(candidate, ctypes.byref(pid))
                cls = ctypes.create_unicode_buffer(64)
                cad.user32.GetClassNameW(candidate, cls, 64)
                if pid.value in children and cad.user32.IsWindowVisible(candidate) and cls.value == '#32770':
                    found.append((candidate, pid.value))
                    return False
                return True

            cad.user32.EnumWindows(visit, 0)
            if found:
                dialog, child_pid = found[0]
                owner = cad.user32.GetWindow(dialog, 4)  # GW_OWNER
                foreground_pid = cad.wintypes.DWORD()
                cad.user32.GetWindowThreadProcessId(foreground, ctypes.byref(foreground_pid))
                foreground_class = ctypes.create_unicode_buffer(64)
                cad.user32.GetClassNameW(foreground, foreground_class, 64)
                foreground_owner = cad.user32.GetWindow(foreground, 4)
                require('picker foreground is the root or its direct owned dialog',
                        foreground == hwnd or
                        (foreground_pid.value in children and foreground_class.value == '#32770' and
                         foreground_owner == hwnd),
                        dict(foreground=int(foreground or 0), foreground_pid=foreground_pid.value,
                             foreground_class=foreground_class.value,
                             foreground_owner=int(foreground_owner or 0), root=hwnd))
                if foreground != dialog:
                    time.sleep(0.08)
                    continue
                command_line = process_command_line(child_pid)
                image = process_image_path(child_pid)
                tail_pid = re.search(r'\s(\d+)\s*$', command_line)
                evidence = dict(hwnd=int(dialog), pid=child_pid, root_pid=proc.pid,
                                owner=int(owner or 0), image=str(image), command_line=command_line)
                result['picker_evidence'].append(evidence)
                require('Save As helper is a direct child running the explicit executable',
                        child_pid in children and os.path.normcase(str(image)) ==
                        os.path.normcase(str(exe)), evidence)
                require('Save As helper command line carries its private picker protocol',
                        str(exe).casefold() in command_line.casefold() and
                        '--neo-save-picker' in command_line and tail_pid is not None and
                        int(tail_pid.group(1)) == proc.pid, evidence)
                require('native picker is visible, foreground, and owned by the root window',
                        cad.user32.GetForegroundWindow() == dialog and owner == hwnd and
                        not cad.user32.IsWindowEnabled(hwnd), evidence)
                return dialog, child_pid, evidence
            time.sleep(0.08)
        raise AssertionError('owned #32770 Save As picker did not appear')

    def send_picker_escape(dialog, child_pid):
        pid = cad.wintypes.DWORD()
        cad.user32.GetWindowThreadProcessId(dialog, ctypes.byref(pid))
        require('Escape is sent only to the verified owned picker', pid.value == child_pid and
                cad.user32.GetForegroundWindow() == dialog and not cad.user32.IsWindowEnabled(hwnd),
                dict(dialog_pid=pid.value, helper_pid=child_pid, root_pid=proc.pid))
        cad.user32.keybd_event(0x1B, cad.user32.MapVirtualKeyW(0x1B, 0), 0, 0)
        time.sleep(0.05)
        cad.user32.keybd_event(0x1B, cad.user32.MapVirtualKeyW(0x1B, 0), 2, 0)
        wait_deadline = time.time() + 10.0
        while time.time() < wait_deadline:
            children = cad.direct_child_process_ids(proc.pid)
            if child_pid not in children and cad.user32.GetForegroundWindow() == hwnd and \
                    cad.user32.IsWindowEnabled(hwnd):
                owned()
                return
            time.sleep(0.08)
        raise AssertionError('picker Escape did not return focus to the owned root window')

    def choose_confirmation(choice):
        if choice == 'save':
            key(0x27)  # Default focus is Cancel; move to Save.
            key(0x0D)
        elif choice == 'discard':
            key(0x25)  # Default focus is Cancel; move to Discard.
            key(0x0D)
        elif choice == 'cancel':
            key(0x1B)
        else:
            raise ValueError(choice)

    def close_dirty_from_list(target_id, label):
        snap = manifest()
        order = ids(snap)
        if target_id not in order:
            raise RuntimeError(f'{label}: target TabId {target_id} not in {order}')
        panel = open_tab_list(label)
        click_row_close(panel, order.index(target_id))
        snap = wait_manifest(lambda value: int(value.get('active', 0)) == target_id,
                             label + ' exact target activated for close')
        require(label + ' close request leaves exact manifest order intact', ids(snap) == order,
                dict(before=order, after=ids(snap)))
        geometry = wait_safety_dialog(True, label + '-confirmation')
        if geometry and geometry.get('recognized'):
            geometry = inspect_safety_dialog(label + '-confirmation-visible')
        require(label + ' actual unsaved confirmation panel is visible',
                bool(geometry and geometry.get('recognized')), geometry)
        return panel, geometry, snap

    def close_clean_tabs_to_blank():
        while True:
            snap = manifest()
            records = snap.get('records', [])
            if len(records) == 1 and not records[0].get('path'):
                require('cleanup blank tab is clean', not records[0].get('dirty'), records[0])
                return snap
            target_id = int(snap.get('active', 0))
            row = next((record for record in records if int(record['id']) == target_id), None)
            if row is None or row.get('dirty'):
                raise RuntimeError(f'cleanup refuses non-clean active page: {row}')
            key(0x11, 0x57)
            if len(records) == 1:
                wait_manifest(lambda value, old=target_id:
                    len(value.get('records', [])) == 1 and
                    int(value['records'][0]['id']) != old and
                    not value['records'][0].get('path') and not value['records'][0].get('dirty'),
                    'last clean file replaced by blank tab')
                return manifest()
            wait_manifest(lambda value, old_count=len(records), old=target_id:
                len(value.get('records', [])) == old_count - 1 and old not in ids(value),
                'clean tab removed during cleanup')

    def normal_close():
        owned()
        cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
        proc.wait(timeout=15)
        result['exits'].append(proc.returncode)
        result['normal_exit_verified'] = proc.returncode == 0
        require('owned app exits normally', proc.returncode == 0, proc.returncode)

    try:
        proc = subprocess.Popen([str(exe), str(first_file)], cwd=exe.parent, env=env)
        for _ in range(180):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None:
                break
            time.sleep(0.1)
        require('launched main window appears', bool(hwnd), proc.pid)
        root_pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(root_pid))
        require('main window belongs to launched PID', root_pid.value == proc.pid,
                [root_pid.value, proc.pid])
        require('owned root window acquires startup foreground', cad.ensure_foreground(hwnd) and
                cad.user32.GetForegroundWindow() == hwnd)
        cw, ch = resize_client()
        unit = cad.user32.GetDpiForWindow(hwnd) / 96.0 * args.scale
        result['effective_scale'] = unit
        require('requested physical client size reached', abs(cw-requested_w) <= 2 and
                abs(ch-requested_h) <= 2, [cw, ch])
        require('effective UI scale matches target', abs(unit-args.target_effective_scale) <= 0.01,
                dict(dpi=cad.user32.GetDpiForWindow(hwnd), effective=unit,
                     target=args.target_effective_scale))

        initial = wait_manifest(lambda value: len(value.get('records', [])) == 1 and
                                Path(value['records'][0].get('path', '')).resolve() == first_file,
                                'first explicit clean fixture opened')
        require('first fixture begins clean', not initial['records'][0].get('dirty'), initial['records'][0])
        open_file(second_file)
        two = wait_manifest(lambda value: len(value.get('records', [])) == 2 and
                            any(Path(row.get('path', '')).resolve() == second_file for row in value['records']),
                            'second explicit clean fixture opened')
        require('two explicit fixture files are both clean and retain distinct IDs',
                len(ids(two)) == 2 and all(not row.get('dirty') and row.get('path')
                                           for row in two['records']) and len(set(ids(two))) == 2,
                two['records'])
        neighbor_ids = ids(two)

        key(0x11, 0x4E)  # Ctrl+N creates an untitled page.
        draft = wait_manifest(lambda value: len(value.get('records', [])) == 3 and
                              int(value.get('active', 0)) not in neighbor_ids and
                              any(int(row['id']) == int(value['active']) and not row.get('path')
                                  for row in value['records']), 'Ctrl+N creates a pathless draft')
        draft_id = int(draft['active'])
        marker = 'UNIQUE-DRAFT-SAVEAS-CANCEL-7E3A'
        key(0x11, 0x23)  # Ctrl+End
        type_text('\n' + marker)
        draft_dirty = wait_manifest(lambda value: any(int(row['id']) == draft_id and
                                    row.get('dirty') and not row.get('path') and
                                    marker in body(value, row) for row in value['records']),
                                    'unique marker persisted in the dirty pathless draft')
        require('draft has exactly two stable clean neighbors',
                [ident for ident in ids(draft_dirty) if ident != draft_id] == neighbor_ids,
                dict(draft=draft_id, ids=ids(draft_dirty), neighbors=neighbor_ids))

        panel, _, before_close = close_dirty_from_list(draft_id, 'draft-close-saveas')
        # A second close hit while the confirmation is pending must not remove
        # the selected draft or either neighbor. The modal backdrop may consume
        # it as Cancel; either outcome is recorded and followed by a fresh action.
        order_before_repeat = ids(before_close)
        click_row_close(panel, order_before_repeat.index(draft_id))
        time.sleep(0.2)
        repeated = wait_manifest(lambda value: ids(value) == order_before_repeat,
                                 'repeated pending close preserves every TabId')
        require('pending repeated click leaves draft and neighbor IDs unchanged',
                ids(repeated) == order_before_repeat, ids(repeated))
        repeated_dialog = inspect_safety_dialog('draft-repeat-close-modal-state')
        repeated_pending = bool(repeated_dialog and repeated_dialog.get('recognized'))
        result['checks'].append(dict(name='pending repeated close modal state recorded', passed=True,
                                     detail=dict(still_open=repeated_pending,
                                                 geometry=repeated_dialog)))
        if not repeated_pending:
            # The second hit landed on the known modal backdrop and canceled it.
            # Reopen the exact same draft close action and prove a fresh prompt.
            panel, _, before_close = close_dirty_from_list(draft_id, 'draft-retry-close-saveas')
            require('fresh confirmation retains the original TabId order',
                    ids(before_close) == order_before_repeat, ids(before_close))
        else:
            panel = panel

        # Save from the verified confirmation; an untitled page must invoke the
        # isolated picker helper. Escape cancels that picker without saving.
        choose_confirmation('save')
        picker, helper_pid, _ = find_owned_picker()
        send_picker_escape(picker, helper_pid)
        after_picker_cancel = wait_manifest(lambda value:
            ids(value) == neighbor_ids + [draft_id] or
            ids(value) == [draft_id] + neighbor_ids or
            (len(ids(value)) == 3 and set(ids(value)) == set(neighbor_ids + [draft_id])),
            'Save As cancellation preserves all pages')
        draft_row = next(row for row in after_picker_cancel['records'] if int(row['id']) == draft_id)
        require('Save As cancellation keeps pathless dirty body and marker',
                not draft_row.get('path') and draft_row.get('dirty') and
                marker in body(after_picker_cancel, draft_row), draft_row)
        require('Save As cancellation keeps neighbor IDs and order',
                [int(row['id']) for row in after_picker_cancel['records'] if int(row['id']) != draft_id]
                == neighbor_ids, ids(after_picker_cancel))
        require('Save As cancellation leaves the draft active',
                int(after_picker_cancel.get('active', 0)) == draft_id, after_picker_cancel.get('active'))
        # saveDocument returns false when the picker is cancelled; the existing
        # unsaved confirmation remains so the user can choose another action.
        # Wait for its rendered return rather than reading a transient frame.
        remaining_confirm = wait_safety_dialog(True, 'draft-after-saveas-cancel')
        require('Save As cancellation returns to the original unsaved confirmation',
                bool(remaining_confirm and remaining_confirm.get('recognized')), remaining_confirm)
        key(0x1B)
        cancelled_confirm = wait_safety_dialog(False, 'draft-cancelled-confirmation-chain')
        require('explicit Cancel ends the original unsaved confirmation',
                not (cancelled_confirm and cancelled_confirm.get('recognized')), cancelled_confirm)
        require('explicit Cancel still preserves the exact original TabId order',
                ids() == order_before_repeat, ids())

        # A fresh close request on the same exact ID, followed by Discard, must
        # remove only that draft and leave both clean file tabs in their order.
        close_dirty_from_list(draft_id, 'draft-close-discard')
        choose_confirmation('discard')
        after_discard = wait_manifest(lambda value: draft_id not in ids(value),
                                      'Discard removes the pathless draft')
        require('Discard removes only the draft and keeps neighbor order',
                ids(after_discard) == neighbor_ids, ids(after_discard))
        require('Discard leaves exactly the two explicit clean files',
                len(after_discard['records']) == 2 and
                all(not row.get('dirty') and row.get('path') for row in after_discard['records']),
                after_discard['records'])

        # Exercise a real external-write conflict on an existing dirty file.
        first_row = next(row for row in after_discard['records']
                         if Path(row['path']).resolve() == first_file)
        conflict_id = int(first_row['id'])
        conflict_before_ids = ids(after_discard)
        select_tab_id(conflict_id)
        conflict_marker = 'DIRTY-EXTERNAL-CONFLICT-CANCEL-4C91'
        key(0x11, 0x23)
        type_text('\n' + conflict_marker)
        dirty_file = wait_manifest(lambda value: any(int(row['id']) == conflict_id and
                               row.get('dirty') and conflict_marker in body(value, row)
                               for row in value['records']), 'existing file becomes dirty')
        external = 'EXTERNAL-WRITER-MUST-SURVIVE-2F61\n'
        first_file.write_text(external, encoding='utf-8')
        require('external writer content is on disk before Save',
                first_file.read_text(encoding='utf-8') == external, first_file)
        close_dirty_from_list(conflict_id, 'external-conflict-close')
        choose_confirmation('save')  # Existing path is checked against its disk fingerprint.
        conflict_dialog = wait_safety_dialog(True, 'external-conflict-confirmation')
        if conflict_dialog and conflict_dialog.get('recognized'):
            conflict_dialog = inspect_safety_dialog('external-conflict-confirmation-visible')
        require('external Save conflict is visibly presented before Cancel',
                bool(conflict_dialog and conflict_dialog.get('recognized')), conflict_dialog)
        key(0x1B)
        after_conflict_cancel = wait_manifest(lambda value: ids(value) == conflict_before_ids,
                                              'conflict Cancel retains every tab')
        require('conflict Cancel keeps the dirty editor body',
                any(int(row['id']) == conflict_id and row.get('dirty') and
                    conflict_marker in body(after_conflict_cancel, row)
                    for row in after_conflict_cancel['records']), after_conflict_cancel['records'])
        require('conflict Cancel leaves external disk bytes untouched',
                first_file.read_text(encoding='utf-8') == external,
                first_file.read_text(encoding='utf-8'))
        require('conflict Cancel keeps all original TabIds in order',
                ids(after_conflict_cancel) == conflict_before_ids, ids(after_conflict_cancel))
        conflict_modal_after = wait_safety_dialog(False, 'external-conflict-after-cancel')
        if conflict_modal_after and conflict_modal_after.get('recognized'):
            check('conflict Cancel ends the close confirmation chain', False, conflict_modal_after)
            key(0x1B)
            wait_safety_dialog(False, 'conflict-cancelled-confirmation-chain')
        else:
            check('conflict Cancel ends the close confirmation chain', True,
                  'no centered safety dialog remains')

        # Dispose of the dirty fixture by an explicit, visually verified
        # Discard, then close the two clean tabs and exit normally.
        close_dirty_from_list(conflict_id, 'cleanup-conflict-dirty-page')
        choose_confirmation('discard')
        cleaned = wait_manifest(lambda value: conflict_id not in ids(value),
                                'cleanup Discard removes the dirty conflict page')
        require('cleanup Discard preserves the clean neighbor',
                len(cleaned['records']) == 1 and not cleaned['records'][0].get('dirty'),
                cleaned['records'])
        blank = close_clean_tabs_to_blank()
        require('probe exits from one clean blank page', len(blank.get('records', [])) == 1 and
                not blank['records'][0].get('path') and not blank['records'][0].get('dirty'), blank)
        normal_close()
    except Exception as exc:
        result['exception'] = f'{type(exc).__name__}: {exc}'
        result['checks'].append(dict(name='probe completed without exception', passed=False,
                                     detail=result['exception']))
        failures.append('probe completed without exception')
        print('ERROR', result['exception'], flush=True)
    finally:
        if proc is not None and proc.poll() is None and hwnd:
            try:
                pid = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value == proc.pid and cad.user32.GetForegroundWindow() == hwnd:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                proc.wait(timeout=5)
                result['cleanup_exit_code'] = proc.returncode
            except Exception as cleanup_error:
                result['cleanup_error'] = f'{type(cleanup_error).__name__}: {cleanup_error}'
        result['normal_exit_verified'] = bool(result['normal_exit_verified'] and
                                              result['exits'] and result['exits'][-1] == 0)
        result['failed_checks'] = failures
        (out / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2,
                                                   default=lambda value: str(value) if isinstance(value, Path)
                                                   else (_ for _ in ()).throw(TypeError(type(value).__name__))),
                                         encoding='utf-8')
    if failures or result['exception']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

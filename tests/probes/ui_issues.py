"""Real-window visual acceptance captures for the 2026 UI issue fixes.

Run only after building the candidate:
  python -B tests/probes/ui_issues.py --exe PATH --out DIR --language zh-CN --scale 1.25 --ui-font 18

Captures are evidence for human visual review. This probe records interaction
checkpoints and the executable hash; screenshots and UI text are intentionally
not treated as semantic pass/fail assertions.
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

VK_CONTROL, VK_ESCAPE, VK_RETURN, VK_LEFT = 0x11, 0x1B, 0x0D, 0x25


def estimated_width(label, font_size):
    """Mirror menu_bar.h/widgets.h glyphAdvance for a translated title."""
    width = 0.0
    for ch in label:
        code = ord(ch)
        width += font_size if code >= 0x800 else font_size * (0.62 if code >= 0x80 else 0.55)
    return width


def root_row_center(index, ui_font, menu_y, separator_rows):
    item_height = max(28.0, ui_font + 18.0)
    return menu_y + 6.0 + index * item_height + sum(
        8.0 for row in separator_rows if row <= index
    ) + item_height * 0.5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--language', choices=('zh-CN', 'en'), default='zh-CN')
    ap.add_argument('--scale', type=float, choices=(1.0, 1.25, 1.5), default=1.0)
    ap.add_argument('--ui-font', type=int, choices=(14, 18), default=14)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    if not exe.is_file(): raise SystemExit(f'exe not found: {exe}')
    out.mkdir(parents=True, exist_ok=False)
    profile, temp = out/'profile', out/'temp'
    (profile/'EUI-Edits').mkdir(parents=True)
    temp.mkdir()
    # Empty markdown document demonstrates the editor placeholder. A long-name
    # existing file is then dirtied for the unsaved confirmation overlay.
    library = out/'fixture'/'library'
    library.mkdir(parents=True)
    doc = library/('long-' + '界面字号窗口确认_' * 8 + '.md')
    doc.write_text('', encoding='utf-8')
    (library/'other.txt').write_text('Other file', encoding='utf-8')
    cad.make_dpi_aware()
    system_scale = cad.user32.GetDpiForSystem() / 96.0
    ui_scale = args.scale / system_scale
    if not 0.8 <= ui_scale <= 2.0:
        raise RuntimeError('requested effective scale is outside supported app scale limits')
    settings = profile/'EUI-Edits'/'settings.ini'
    settings.write_text(
        f'vault={library}\nmode=1\nui_language={args.language}\n'
        f'ui_font_size={args.ui_font}\nui_scale={ui_scale}\neditor_font_size=16\n'
        'show_status_bar=1\nanimations=0\ntheme=1\n', encoding='utf-8')
    env = dict(os.environ, APPDATA=str(profile), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1',
               NEO_LIVE_RESIZE='0')
    cad.assert_unlocked('ui issues')
    result = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'language': args.language, 'scale': args.scale, 'ui_font': args.ui_font,
              'steps': [], 'exit_code': None}
    process = subprocess.Popen([str(exe), str(doc)], cwd=exe.parent, env=env)
    hwnd = None
    try:
        for _ in range(150):
            hwnd = window_for_pid(process.pid)
            if hwnd or process.poll() is not None: break
            time.sleep(.1)
        if not hwnd: raise RuntimeError(f'owned window missing; exit={process.poll()}')
        cad.user32.SetWindowPos(hwnd, None, 0, 0, 1400, 900, 0x0002 | 0x0004)
        time.sleep(1)
        unit = cad.user32.GetDpiForWindow(hwnd) / 96.0 * ui_scale
        assert abs(unit - args.scale) < .01

        def owned():
            cad.assert_unlocked('ui issues interaction')
            pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if process.poll() is not None or pid.value != process.pid or not cad.ensure_foreground(hwnd):
                raise RuntimeError('owned process lost foreground or exited')

        def capture(name, note):
            owned(); time.sleep(.35)
            w, h, pixels = cad.capture_client(hwnd)
            cad.write_png(str(out/f'{name}.png'), w, h, pixels)
            result['steps'].append({'capture': name, 'note': note, 'client': [w, h],
                                    'effective_scale': unit})

        def key(*keys):
            owned()
            for k in keys: cad.user32.keybd_event(k, 0, 0, 0)
            for k in reversed(keys): cad.user32.keybd_event(k, 0, 2, 0)
            time.sleep(.35)

        def text(value):
            owned()
            for ch in value: cad.user32.SendMessageW(hwnd, 0x0102, ord(ch), 0)
            time.sleep(.4)

        def click(x, y):
            owned()
            cad.click(hwnd, round(x*unit), round(y*unit))
            time.sleep(.35)

        def move(x, y):
            owned(); p = cad.wintypes.POINT(round(x*unit), round(y*unit))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(p)); cad.user32.SetCursorPos(p.x, p.y)
            time.sleep(.4)

        # Match menu_bar.h's translated title widths, UiMetrics item height, and
        # contextmenu.h's inset and separator gaps to locate rows at both UI sizes.
        ui_font = float(args.ui_font)
        menu_y = max(30.0, ui_font + 20.0)
        item_height = max(28.0, ui_font + 18.0)
        separator_rows = (3, 5, 7)
        active_language = args.language

        def menu_geometry(language):
            file_label, edit_label = (('文件', '编辑') if language == 'zh-CN'
                                      else ('File', 'Edit'))
            title_x = (10.0 + estimated_width(file_label, ui_font) + 20.0 + 2.0 +
                       estimated_width(edit_label, ui_font) + 20.0 + 2.0)
            width = 300.0 if language == 'en' else 212.0
            return title_x, title_x + width + 4.0

        view_x, child_x = menu_geometry(active_language)
        tabs_h = max(11.0, ui_font - 1.0) + 20.0
        sidebar_head_y = menu_y + 8.0 + tabs_h + 6.0 + 13.0
        sidebar_field_y = menu_y + 8.0 + tabs_h + 6.0 + 26.0 + 6.0 + 15.0

        # Baseline, then switch language from View > UI language without changing
        # the selected document/scene.
        capture('01-initial', 'Initial editor and sidebar; visual inspection only.')
        click(24, sidebar_head_y)
        capture('01b-filter-before-language', 'Empty folder filter before switching language.')
        click(view_x + 10.0, 17); capture('02-view-menu', 'View menu root before language selection.')
        language_y = root_row_center(7, ui_font, menu_y, separator_rows)
        move(view_x + 100.0, language_y)
        capture('03-language-submenu', 'Language submenu; verify interaction stayed in current scene.')
        target = 1 if args.language == 'en' else 2
        language_top = language_y - item_height * 0.5
        click(child_x + 75.0, language_top + 6.0 + target * item_height + item_height * 0.5)
        active_language = 'zh-CN' if args.language == 'en' else 'en'
        view_x, child_x = menu_geometry(active_language)
        capture('04-placeholder-after-language', 'Editor empty placeholder after live language switch.')
        assert f'ui_language={active_language}\n' in settings.read_text(encoding='utf-8'), \
            'language menu interaction must actually switch the preference'

        # Sidebar controls: search/filter mode, type and clear; then path mode,
        # type a real folder path and Enter. Native folder chooser opens and Escape cancels.
        menu_bar_h = max(30.0, ui_font + 20.0)
        tabs_h = max(11.0, ui_font - 1.0) + 20.0
        sidebar_head_y = menu_bar_h + 8.0 + tabs_h + 6.0 + 13.0
        sidebar_field_y = menu_bar_h + 8.0 + tabs_h + 6.0 + 26.0 + 6.0 + 15.0
        capture('05-sidebar-filter', 'Search/filter mode and unified field after live translation.')
        move(24, sidebar_head_y)
        capture('05a-filter-tooltip', 'Filter mode explanation; review horizontal and vertical centering.')
        move(54, sidebar_head_y)
        capture('05b-parent-tooltip', 'Parent directory explanation; review horizontal and vertical centering.')
        move(205, sidebar_head_y)
        capture('05c-folder-tooltip', 'Open folder explanation; review horizontal and vertical centering.')
        move(600, 420)
        click(120, sidebar_field_y); text('界面'); capture('06-sidebar-filter-typed', 'Filter typed in unified field.')
        key(VK_CONTROL, 0x41); key(0x08)
        click(24, sidebar_head_y); capture('07-sidebar-path', 'Path mode and same unified field.')
        click(120, sidebar_field_y); key(VK_CONTROL, 0x41); text(str(library)); key(VK_RETURN)
        capture('08-sidebar-path-enter', 'Path navigation after Enter.')
        # Choose directory button lives at the right end of the sidebar header.
        click(205, sidebar_head_y); time.sleep(1.0)
        foreground = cad.user32.GetForegroundWindow()
        owner = cad.user32.GetWindow(foreground, 4)  # GW_OWNER
        root_owner = cad.user32.GetAncestor(foreground, 3)  # GA_ROOTOWNER
        fg_pid = ctypes.c_ulong()
        if foreground: cad.user32.GetWindowThreadProcessId(foreground, ctypes.byref(fg_pid))
        if foreground == hwnd or (owner != hwnd and root_owner != hwnd):
            raise RuntimeError('native folder chooser is not owned by the probed main window')
        result['steps'].append({'capture': 'native-folder-chooser', 'foreground_hwnd': int(foreground),
                                'foreground_pid': fg_pid.value, 'owner_hwnd': int(owner or 0),
                                'root_owner_hwnd': int(root_owner or 0),
                                'note': 'Owned native chooser foreground; Escape cancels it.'})
        cad.user32.keybd_event(VK_ESCAPE, 0, 0, 0); cad.user32.keybd_event(VK_ESCAPE, 0, 2, 0)
        for _ in range(60):
            time.sleep(.1)
            if cad.user32.GetForegroundWindow() == hwnd: break
        if cad.user32.GetForegroundWindow() != hwnd:
            raise RuntimeError('main window did not regain foreground after chooser cancellation')
        capture('09-sidebar-after-chooser-cancel', 'Folder chooser canceled.')
        # Parent navigation is exercised after entering the output directory.
        click(24, sidebar_head_y); click(56, sidebar_head_y); capture('10-sidebar-parent', 'Parent navigation affordance.')

        # Dirty existing long-filename document and close its tab to show the
        # wrapped safety body. Cancel with Escape to retain content.
        click(600, 420); text('Dirty confirmation visual case.')
        key(VK_CONTROL, 0x57); time.sleep(.6)
        capture('11-long-unsaved-dialog', 'Long filename plus wrapped unsaved-confirmation body.')
        key(VK_ESCAPE); capture('12-dialog-canceled', 'Escape canceled dialog and returned to editor.')
        assert doc.read_bytes() == b'', 'probe must preserve the original file bytes'

        # Font-size submenu maximum option (24pt) for clipped-label review.
        click(view_x + 10.0, 17)
        font_y = root_row_center(6, ui_font, menu_y, separator_rows)
        move(view_x + 100.0, font_y)
        capture('13-font-size-submenu', 'Font-size submenu before selecting maximum size.')
        font_top = font_y - item_height * 0.5
        click(child_x + 56.0, font_top + 6.0 + 5 * item_height + item_height * 0.5)
        capture('14-font-size-maximum', 'Maximum editor font applied; inspect menu/viewport layout.')
    finally:
        if hwnd and process.poll() is None:
            cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
            # The safety dialog starts on Cancel (2); Left selects Discard (1).
            time.sleep(.8)
            if process.poll() is None and cad.user32.GetForegroundWindow() == hwnd:
                cad.user32.keybd_event(VK_LEFT, 0, 0, 0); cad.user32.keybd_event(VK_LEFT, 0, 2, 0)
                time.sleep(.15)
                cad.user32.keybd_event(VK_RETURN, 0, 0, 0); cad.user32.keybd_event(VK_RETURN, 0, 2, 0)
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired: result['shutdown_timeout'] = True
        result['exit_code'] = process.poll()
        (out/'results.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__': main()

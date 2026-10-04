"""Isolated Win32 acceptance probe for tab root identity and palette rollover.

Run serially against the final executable, for example:
  python tests/probes/tab_menu_roots.py --exe build/Release/neo_editor.exe \
      --expected-sha256 <sha256> --out <new-private-output-directory>

The probe never starts or drives the user's instance. Every input/capture is
bound to the launched PID and its foreground window. Ordinal text is retained
as a screenshot for human review; this script does not claim OCR verification.
"""
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import win_capture as cad
from capture_markdown import window_for_pid
import tab_menu_visual as visual


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--expected-sha256', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--ui-scale', type=float, default=0.8)
    parser.add_argument('--target-effective-scale', type=float, default=1.0)
    parser.add_argument('--window', default='2200x1200')
    args = parser.parse_args()
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    if not exe.is_file():
        parser.error(f'executable not found: {exe}')
    out.mkdir(parents=True, exist_ok=False)
    actual_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    if actual_hash.lower() != args.expected_sha256.lower():
        raise SystemExit(f'expected SHA256 mismatch: {actual_hash}')
    cad.make_dpi_aware()
    cad.assert_unlocked('tab menu roots')
    required_visual_helpers = ('detect_bars', 'color_distance', 'panel_bounds')
    missing_helpers = [name for name in required_visual_helpers
                       if not callable(getattr(visual, name, None))]
    if missing_helpers:
        raise RuntimeError(f'tab_menu_visual helper API missing: {missing_helpers}')

    result = dict(exe=str(exe), sha256=actual_hash,
                  expected_sha256=args.expected_sha256.lower(),
                  ui_scale=args.ui_scale, target_effective_scale=args.target_effective_scale,
                  requested_window=args.window, checks=[], captures=[], roots=[], exits=[])
    failures = []
    processes = []
    width_px, height_px = (int(part) for part in args.window.lower().split('x'))
    unit = 1.0
    bar_height = 34.0
    root_bar_origin_px = None

    def check(name, ok, detail=''):
        passed = bool(ok)
        result['checks'].append(dict(name=name, passed=passed, detail=str(detail)))
        print(('PASS ' if passed else 'FAIL ') + name, detail, flush=True)
        if not passed:
            failures.append(name)
        return passed

    def old_preferred_slot(root):
        # Mirrors the documented old tab_bar lowerAscii + FNV-1a modulo five.
        key = str(root.resolve(strict=False)).replace('\\', '/').rstrip('/')
        key = ''.join(chr(ord(ch) + 32) if 'A' <= ch <= 'Z' else ch for ch in key)
        value = 2166136261
        for byte in key.encode('utf-8'):
            value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
        return value % 5

    def make_fixtures(base):
        collision_roots = []
        seen_slots = {}
        for index in range(10000):
            candidate = base / f'lib-old-hash-{index:05d}'
            slot = old_preferred_slot(candidate)
            if slot in seen_slots:
                collision_roots = [seen_slots[slot], candidate]
                break
            seen_slots[slot] = candidate
        if not collision_roots:
            raise RuntimeError('could not generate an old-hash modulo-five root collision')
        roots = {'A': base / 'A', 'B': base / 'B'}
        roots.update({f'lib{i}': base / f'lib{i}' for i in range(8)})
        roots['lib0'], roots['lib1'] = collision_roots
        for root in roots.values():
            root.mkdir(parents=True, exist_ok=True)
        (roots['A'] / 'docs').mkdir(parents=True, exist_ok=True)
        files = {'child': roots['A'] / 'docs' / 'child.md',
                 'parent': roots['A'] / 'top.md',
                 'B': roots['B'] / 'readme.md'}
        for i in range(8):
            files[f'lib{i}'] = roots[f'lib{i}'] / f'lib{i}.md'
        for key, path in files.items():
            path.write_text(f'# {key}\n\nroot identity fixture {key}\n', encoding='utf-8')
        return roots, files

    def new_context(name, roots, config_root=None, temp_root=None):
        home = out / name
        appdata = (config_root or home / 'appdata') / 'EUI-Edits'
        temp = temp_root or home / 'temp'
        appdata.mkdir(parents=True, exist_ok=True)
        temp.mkdir(parents=True, exist_ok=True)
        (appdata / 'settings.ini').write_text(
            f'mode=1\nui_scale={args.ui_scale}\nui_font_size=14\ntheme=1\n'
            'animations=0\nui_language=en\nshow_status_bar=1\n', encoding='utf-8')
        env = dict(os.environ, APPDATA=str(appdata.parent), TEMP=str(temp), TMP=str(temp),
                   NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
        return dict(name=name, appdata=appdata, temp=temp, env=env,
                    roots=roots, proc=None, hwnd=None)

    def own(ctx):
        cad.assert_unlocked('tab menu roots')
        proc, hwnd = ctx['proc'], ctx['hwnd']
        pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if proc.poll() is not None or pid.value != proc.pid or cad.user32.GetForegroundWindow() != hwnd:
            raise RuntimeError(f"{ctx['name']}: window ownership/foreground check failed")

    def client_rect(ctx):
        rect = cad.wintypes.RECT()
        cad.user32.GetClientRect(ctx['hwnd'], ctypes.byref(rect))
        outer = cad.wintypes.RECT()
        cad.user32.GetWindowRect(ctx['hwnd'], ctypes.byref(outer))
        return rect.right, rect.bottom, outer.right - outer.left - rect.right, outer.bottom - outer.top - rect.bottom

    def ensure_size(ctx):
        for _ in range(12):
            own(ctx)
            cw, ch, dw, dh = client_rect(ctx)
            if abs(cw - width_px) <= 2 and abs(ch - height_px) <= 2:
                return True
            cad.user32.SetWindowPos(ctx['hwnd'], None, 20, 20, width_px + dw, height_px + dh, 0x0004)
            time.sleep(0.35)
        return False

    def start(ctx, first_path=None):
        nonlocal unit
        command = [str(exe)] + ([str(first_path)] if first_path else [])
        ctx['proc'] = subprocess.Popen(command, cwd=exe.parent, env=ctx['env'])
        processes.append(ctx)
        for _ in range(180):
            ctx['hwnd'] = window_for_pid(ctx['proc'].pid)
            if ctx['hwnd'] or ctx['proc'].poll() is not None:
                break
            time.sleep(0.1)
        if not ctx['hwnd']:
            raise RuntimeError(f"{ctx['name']}: owned main window did not appear")
        if not cad.ensure_foreground(ctx['hwnd']):
            raise RuntimeError(f"{ctx['name']}: owned window did not become foreground")
        own(ctx)
        if not ensure_size(ctx):
            raise RuntimeError(f"{ctx['name']}: requested client size was not honored")
        dpi = cad.user32.GetDpiForWindow(ctx['hwnd'])
        unit = (dpi / 96.0) * args.ui_scale
        result.setdefault('scale_samples', []).append(dict(case=ctx['name'], dpi=dpi,
            system_scale=dpi / 96.0, ui_scale=args.ui_scale, effective_scale=unit))
        check(f'{ctx["name"]} effective scale', abs(unit - args.target_effective_scale) <= 0.01,
              f'{unit} target={args.target_effective_scale}')

    def keys(ctx, *codes):
        own(ctx)
        for code in codes:
            cad.user32.keybd_event(code, cad.user32.MapVirtualKeyW(code, 0), 0, 0)
            time.sleep(0.035)
        for code in reversed(codes):
            cad.user32.keybd_event(code, cad.user32.MapVirtualKeyW(code, 0), 2, 0)
            time.sleep(0.035)
        time.sleep(0.35)

    def click_dip(ctx, x, y):
        own(ctx)
        point = cad.wintypes.POINT(round(x * unit), round(y * unit))
        cad.user32.ClientToScreen(ctx['hwnd'], ctypes.byref(point))
        cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
        cad.user32.WindowFromPoint.restype = ctypes.c_void_p
        cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        cad.user32.GetAncestor.restype = ctypes.c_void_p
        top = cad.user32.WindowFromPoint(point)
        if cad.user32.GetAncestor(top, 2) != ctx['hwnd']:
            raise RuntimeError(f"{ctx['name']}: click target is obscured")
        cad.click(ctx['hwnd'], round(x * unit), round(y * unit))

    def open_path(ctx, path):
        own(ctx)
        request = ctx['temp'] / 'EUI-Edits.next-open'
        staged = ctx['temp'] / 'owned-open.tmp'
        staged.write_text(str(path), encoding='utf-8')
        staged.replace(request)
        keys(ctx, 0x10)  # Shift wakes the owned app's file-request poll path.
        deadline = time.time() + 8.0
        while time.time() < deadline and request.exists():
            time.sleep(0.05)
        check('deferred open consumed: ' + path.name, not request.exists())
        wait_manifest(ctx, lambda snap: any(path_key(r.get('path', '')) == path_key(path) for r in snap['records']),
                      'manifest contains ' + path.name)

    def manifest(ctx):
        path = ctx['appdata'] / 'session' / 'manifest.json'
        return json.loads(path.read_text(encoding='utf-8'))

    def wait_manifest(ctx, predicate, label, timeout=10.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            own(ctx)
            try:
                last = manifest(ctx)
                if predicate(last):
                    return last
            except (OSError, ValueError, KeyError):
                pass
            time.sleep(0.05)
        raise RuntimeError(f'{ctx["name"]}: {label} timed out; last={last}')

    def path_key(path):
        return os.path.normcase(os.path.normpath(str(path))) if path else ''

    def fixture_for(path, roots):
        if not path:
            return None
        key = path_key(path)
        for name, root in roots.items():
            root_key = path_key(root)
            if key == root_key or key.startswith(root_key + os.sep):
                return str(root)
        return None

    def root_snapshot(ctx):
        snap = manifest(ctx)
        rows = []
        for row in snap.get('records', []):
            actual_root = row.get('vaultRoot') or ''
            rows.append(dict(id=int(row['id']), path=row.get('path', ''),
                vaultRoot=actual_root or None,
                fixtureRoot=fixture_for(row.get('path', ''), ctx['roots'])))
        result['roots'].append(dict(case=ctx['name'], active=int(snap.get('active', 0)), records=rows))
        return snap, rows

    def known_groups(snap):
        roots = [path_key(row.get('vaultRoot', '')) for row in snap.get('records', [])]
        if not roots or any(not root for root in roots):
            return None
        return list(dict.fromkeys(roots))

    def capture(ctx, name, save=False):
        own(ctx)
        w, h, raw = cad.capture_client(ctx['hwnd'])
        if save:
            target = out / (name + '.png')
            cad.write_png(str(target), w, h, raw)
            result['captures'].append(str(target))
        return w, h, raw

    def scan(ctx, save_name=None):
        nonlocal root_bar_origin_px
        snap, rows = root_snapshot(ctx)
        w, h, raw = capture(ctx, save_name or 'unused', bool(save_name))
        y = int(round((bar_height * 0.5) * unit))
        bars = visual.detect_bars(raw, w, y, 0, int(w * 0.95), unit)
        if root_bar_origin_px is None and bars:
            root_bar_origin_px = bars[0][0]
            result.setdefault('measurements', {})['root_bar_origin_px'] = root_bar_origin_px
        indices = []
        for bar in bars:
            index = round((bar[0] - root_bar_origin_px) / (180.0 * unit)) if root_bar_origin_px is not None else -1
            indices.append(index)
        valid = all(0 <= index < len(rows) for index in indices) and len(set(indices)) == len(indices)
        result.setdefault('measurements', {}).setdefault('bar_coverage', []).append(
            dict(case=ctx['name'], tab_count=len(rows), detected_indices=indices,
                 missing_indices=[i for i in range(len(rows)) if i not in indices]))
        check(f'{ctx["name"]} detected bars map to manifest-order card geometry', valid,
              f'indices={indices} tabs={len(rows)} size={w}x{h}')
        return snap, rows, bars, raw, w, h

    def indexed_bar(bars, tab_index):
        if root_bar_origin_px is None:
            return None
        for bar in bars:
            index = round((bar[0] - root_bar_origin_px) / (180.0 * unit))
            if index == tab_index and abs((bar[0] - root_bar_origin_px) - index * 180.0 * unit) <= 3.0 * unit:
                return bar
        return None

    def select_tab_id(ctx, target_id):
        snap = manifest(ctx)
        ids = [int(record['id']) for record in snap.get('records', [])]
        if not ids or target_id not in ids:
            raise RuntimeError(f"{ctx['name']}: target TabId {target_id} absent from manifest {ids}")
        current = int(snap.get('active', 0))
        if current not in ids:
            raise RuntimeError(f"{ctx['name']}: active TabId {current} absent from manifest {ids}")
        steps = (ids.index(target_id) - ids.index(current)) % len(ids)
        for _ in range(steps):
            keys(ctx, 0x11, 0x09)  # Ctrl+Tab cycles in manifest order, including tabs beyond slot 8.
        wait_manifest(ctx, lambda value: int(value.get('active', 0)) == target_id,
                      f'active TabId {target_id}')

    def select_index(ctx, one_based):
        records = manifest(ctx).get('records', [])
        if one_based < 1 or one_based > len(records):
            raise RuntimeError(f'{ctx["name"]}: invalid tab index {one_based} of {len(records)}')
        select_tab_id(ctx, int(records[one_based - 1]['id']))

    def color_for_record(ctx, index, label, save=False):
        before = manifest(ctx)
        records = before.get('records', [])
        if index >= len(records):
            check(label + ' representative exists in manifest', False, index)
            return None
        target_id = int(records[index]['id'])
        select_tab_id(ctx, target_id)
        snap, rows, bars, _, _, _ = scan(ctx, label if save else None)
        bar = indexed_bar(bars, index)
        if index >= len(rows) or bar is None:
            return None
        active_id = int(snap.get('active', 0))
        check(label + ' selected TabId is active', active_id == rows[index]['id'],
              f'active={active_id}, wanted={rows[index]["id"]}')
        return bar[2]

    def close_window(ctx):
        if ctx['proc'] is None or ctx['proc'].poll() is not None:
            return
        own(ctx)
        cad.user32.PostMessageW(ctx['hwnd'], 0x0010, 0, 0)
        try:
            code = ctx['proc'].wait(timeout=12)
        except subprocess.TimeoutExpired:
            raise RuntimeError(f"{ctx['name']}: normal window close did not exit")
        result['exits'].append(dict(case=ctx['name'], code=code))
        check(ctx['name'] + ' normal exit code', code == 0, code)

    try:
        roots, files = make_fixtures(out / 'fixtures')
        main_ctx = new_context('primary', roots)
        start(main_ctx, files['child'])
        wait_manifest(main_ctx, lambda snap: len(snap.get('records', [])) == 1,
                      'startup child record')
        scan(main_ctx, 'one-tab-bar-origin')
        for key in ('parent', 'B', 'lib0', 'lib1', 'lib2', 'lib3', 'lib4'):
            open_path(main_ctx, files[key])
        snap, rows = root_snapshot(main_ctx)
        groups = known_groups(snap)
        check('manifest records fixture root identities from vaultRoot', groups is not None,
              'missing roots are recorded with fixtureRoot and left ungrouped' if groups is None else groups)
        result['measurements'] = {'after_lib0_to_lib4_tabs': len(rows),
                                  'after_lib0_to_lib4_known_roots': len(groups) if groups else None}
        collision_slot0 = old_preferred_slot(roots['lib0'])
        collision_slot1 = old_preferred_slot(roots['lib1'])
        result['measurements']['old_hash_collision_fixtures'] = dict(
            roots=[str(roots['lib0']), str(roots['lib1'])],
            preferred_mod_5=[collision_slot0, collision_slot1])
        check('private fixture roots collide under the old FNV modulo-five rule',
              collision_slot0 == collision_slot1 and path_key(roots['lib0']) != path_key(roots['lib1']),
              result['measurements']['old_hash_collision_fixtures'])

        # Continue with named, private fixtures until real manifest roots reach
        # eight and nine. Group membership is derived only from vaultRoot.
        next_lib = 5
        sampled_eight = False
        observed_eight_to_nine = False

        def sample_eight_roots(snap_value, row_value):
            nonlocal sampled_eight
            actual_groups = known_groups(snap_value)
            if actual_groups is None or len(actual_groups) != 8 or sampled_eight:
                return
            colors = []
            for root in actual_groups:
                representative = next(i for i, row in enumerate(row_value)
                                      if path_key(row.get('vaultRoot', '')) == root)
                colors.append(color_for_record(main_ctx, representative,
                                               f'palette8-{representative + 1}'))
            check('eight actual root slots have distinct tab-bar colors',
                  None not in colors and all(visual.color_distance(a, b) > 12
                      for i, a in enumerate(colors) for b in colors[i + 1:]), colors)
            index0 = next(i for i, row in enumerate(row_value)
                          if path_key(row.get('path', '')) == path_key(files['lib0']))
            index1 = next(i for i, row in enumerate(row_value)
                          if path_key(row.get('path', '')) == path_key(files['lib1']))
            key0 = path_key(row_value[index0].get('vaultRoot', ''))
            key1 = path_key(row_value[index1].get('vaultRoot', ''))
            color0 = color_for_record(main_ctx, index0, 'old-hash-collision-lib0')
            color1 = color_for_record(main_ctx, index1, 'old-hash-collision-lib1')
            check('distinct roots colliding under the old hash still receive distinct colors',
                  key0 and key1 and key0 != key1 and collision_slot0 == collision_slot1 and
                  color0 is not None and color1 is not None and
                  visual.color_distance(color0, color1) > 12,
                  dict(roots=(row_value[index0].get('vaultRoot'), row_value[index1].get('vaultRoot')),
                       old_preferred=(collision_slot0, collision_slot1), colors=(color0, color1)))
            sampled_eight = True

        if groups is not None and len(groups) == 8:
            sample_eight_roots(snap, rows)
        previous_count = len(groups) if groups is not None else None
        while groups is not None and len(groups) < 9 and next_lib < 8:
            open_path(main_ctx, files[f'lib{next_lib}'])
            next_lib += 1
            snap, rows = root_snapshot(main_ctx)
            groups = known_groups(snap)
            if groups is not None:
                result['measurements'][f'after_lib{next_lib - 1}_known_roots'] = len(groups)
            if groups is not None and len(groups) == 8:
                sample_eight_roots(snap, rows)
            if previous_count == 8 and groups is not None and len(groups) == 9:
                observed_eight_to_nine = True
                check('manifest root count advances from eight to nine', True,
                      f'added lib{next_lib - 1}; tabs={len(rows)}')
            previous_count = len(groups) if groups is not None else None

        # A/docs and A must resolve to the same actual vaultRoot and sampled color.
        snap, rows, bars, _, _, _ = scan(main_ctx, 'root-order-before-overflow')
        child_i = next(i for i, row in enumerate(rows) if path_key(row['path']) == path_key(files['child']))
        parent_i = next(i for i, row in enumerate(rows) if path_key(row['path']) == path_key(files['parent']))
        check('A/docs and A expose the same manifest vaultRoot',
              bool(rows[child_i]['vaultRoot']) and
              path_key(rows[child_i]['vaultRoot']) == path_key(rows[parent_i]['vaultRoot']),
              (rows[child_i]['vaultRoot'], rows[parent_i]['vaultRoot']))
        child_color = color_for_record(main_ctx, child_i, 'A-docs-color')
        parent_color = color_for_record(main_ctx, parent_i, 'A-parent-color')
        check('A/docs and A use one palette color', child_color is not None and parent_color is not None and
              visual.color_distance(child_color, parent_color) <= 12, (child_color, parent_color))

        snap, rows = root_snapshot(main_ctx)
        groups = known_groups(snap)
        check('eight-root palette stage sampled before overflow', sampled_eight)
        check('real manifest reached nine through an observed eight-to-nine step',
              observed_eight_to_nine,
              result['measurements'].get(f'after_lib{next_lib - 1}_known_roots'))
        if groups is not None and len(groups) != 9:
            check('nine root groups are reachable from private fixtures', False, groups)
        elif groups is None:
            check('nine root groups are verifiable from actual vaultRoot values', False,
                  'manifest omitted one or more vaultRoot values')
        else:
            check('ninth actual vault root is present', len(groups) == 9, groups)

        # Capture an expanded list with the last tab revealed. Keep the image
        # for human confirmation that the ninth group ordinal is drawn.
        select_index(main_ctx, len(rows))
        _, _, _, closed, w, h = scan(main_ctx)
        ybar = int(round((bar_height * 0.5) * unit))
        # Use the English menu's source geometry. The Settings gear and label
        # can form separate pixel clusters, so the last cluster is not a safe
        # anchor for the adjacent tab-list button.
        font = 14.0
        settings_width = sum(font if ord(ch) >= 0x2E80 else font * 0.55
                             for ch in 'Settings')
        screen_dip = w / unit
        list_x = screen_dip - (8.0 + 16.0 + 6.0 + settings_width + 12.0) - 10.0 - 22.0
        if 0.0 < list_x < screen_dip:
            click_dip(main_ctx, list_x, bar_height * 0.5)
            time.sleep(0.4)
            _, _, _, opened, _, _ = scan(main_ctx)
            bounds = visual.panel_bounds(closed, opened, w, h, unit, (bar_height + 2) * unit)
            if bounds[0] is not None:
                _, _, py0, py1 = bounds
                hover_x = (bounds[0] + 8 * unit + 70 * unit) / unit
                hover_y = py1 / unit - 40.0
                point = cad.wintypes.POINT(round(hover_x * unit), round(hover_y * unit))
                cad.user32.ClientToScreen(main_ctx['hwnd'], ctypes.byref(point))
                own(main_ctx)
                cad.user32.SetCursorPos(point.x, point.y)
                time.sleep(0.5)
                capture(main_ctx, 'ninth-root-list-ordinal-review', save=True)
                check('ninth root ordinal screenshot saved for human review', True,
                      str(out / 'ninth-root-list-ordinal-review.png'))
            else:
                check('expanded tab list panel detected for ordinal screenshot', False, bounds)
            keys(main_ctx, 0x1B)
        else:
            check('tab-list button source geometry falls inside client', False, list_x)

        # Close B, a single-document root. Use its current manifest order index,
        # then prove one exclusive root/slot disappeared (tab count is separate).
        snap, rows = root_snapshot(main_ctx)
        groups_before = known_groups(snap)
        b_index = next((i for i, row in enumerate(rows) if path_key(row['path']) == path_key(files['B'])), None)
        if b_index is None:
            check('exclusive B tab is present in the manifest order', False, (b_index, len(rows)))
        else:
            b_id = int(rows[b_index]['id'])
            select_tab_id(main_ctx, b_id)
            check('exclusive B TabId is active before close',
                  int(manifest(main_ctx).get('active', 0)) == b_id, b_id)
            keys(main_ctx, 0x11, 0x57)
            snap_after = wait_manifest(main_ctx,
                lambda value: len(value.get('records', [])) == len(rows) - 1 and
                    all(int(record['id']) != b_id for record in value.get('records', [])),
                'exclusive B tab closed')
            groups_after = known_groups(snap_after)
            check('closing exclusive B changes nine roots to eight',
                  groups_before is not None and groups_after is not None and
                  len(groups_before) == 9 and len(groups_after) == 8,
                  (groups_before, groups_after))
            rows_after = snap_after['records']
            colors = []
            roots_after = list(dict.fromkeys(path_key(r.get('vaultRoot', '')) for r in rows_after))
            if any(not root for root in roots_after):
                check('post-close manifest roots are complete for palette sampling', False, roots_after)
            else:
                for root in roots_after:
                    representative = next(i for i, row in enumerate(rows_after)
                                          if path_key(row.get('vaultRoot', '')) == root)
                    select_index(main_ctx, representative + 1)
                    colors.append(color_for_record(main_ctx, representative,
                                                   f'palette8-after-close-{representative + 1}'))
                check('all eight remaining root slots have distinct colors',
                      len(colors) == 8 and None not in colors and
                      all(visual.color_distance(a, b) > 12
                          for i, a in enumerate(colors) for b in colors[i + 1:]), colors)

        # Close clean tabs to the ordinary blank page, then normally exit and
        # reopen from the same private APPDATA to prove the workspace is clear.
        while True:
            snap = manifest(main_ctx)
            records = snap.get('records', [])
            if not records:
                check('cleanup manifest retains a tab until blank verification', False, snap)
                break
            if len(records) == 1 and not records[0].get('path'):
                check('final blank tab is clean', not records[0].get('dirty'), records[0])
                break
            active_id = int(snap.get('active', 0))
            active = next((record for record in records
                           if int(record['id']) == active_id), None)
            if active is None:
                check('cleanup active TabId resolves to a manifest record', False,
                      (active_id, [record.get('id') for record in records]))
                break
            if active.get('dirty'):
                check('cleanup target is a clean active document', False, active)
                break
            check('cleanup targets the real clean active TabId', True,
                  dict(id=active_id, path=active.get('path', '')))
            if len(records) == 1:
                if not active.get('path'):
                    check('single-tab cleanup target is a document', False, active)
                    break
                keys(main_ctx, 0x11, 0x57)
                replacement = wait_manifest(main_ctx,
                    lambda value, closed_id=active_id:
                        len(value.get('records', [])) == 1 and
                        int(value.get('records', [{}])[0].get('id', 0)) != closed_id and
                        not value.get('records', [{}])[0].get('path') and
                        not value.get('records', [{}])[0].get('dirty'),
                    'single document replaced by a clean blank tab')
                check('final document close creates a new clean blank TabId',
                      int(replacement['records'][0]['id']) != active_id and
                      not replacement['records'][0].get('path') and
                      not replacement['records'][0].get('dirty'), replacement['records'][0])
                break
            keys(main_ctx, 0x11, 0x57)
            wait_manifest(main_ctx,
                lambda value, count=len(records), closed_id=active_id:
                    len(value.get('records', [])) == count - 1 and
                    all(int(record['id']) != closed_id for record in value.get('records', [])),
                'clean active tab removed')
        blank = manifest(main_ctx)
        check('root probe closes to one clean blank tab', len(blank.get('records', [])) == 1 and
              not blank['records'][0].get('path') and not blank['records'][0].get('dirty'), blank)
        close_window(main_ctx)
        manifest_path = main_ctx['appdata'] / 'session' / 'manifest.json'
        cleared = not manifest_path.exists()
        if manifest_path.exists():
            try:
                cleared = not json.loads(manifest_path.read_text(encoding='utf-8')).get('records')
            except (OSError, ValueError):
                cleared = False
        check('normal close clears the committed workspace manifest', cleared, manifest_path)
        start(main_ctx)
        # An untouched clean startup intentionally has no session manifest.
        # Publish a known edit to prove it is exactly one new untitled page.
        check('blank startup has no persisted previous workspace', not manifest_path.exists())
        capture(main_ctx, 'blank-reopen-before-edit', save=True)
        click_dip(main_ctx, 500.0, 300.0)
        marker = 'ROOT-BLANK-REOPEN-PROBE'
        for character in marker:
            own(main_ctx)
            cad.user32.PostMessageW(main_ctx['hwnd'], 0x0102, ord(character), 1)
        restarted = wait_manifest(main_ctx, lambda value: len(value.get('records', [])) == 1,
                                  'blank restart manifest')
        check('blank reopen does not restore prior root tabs',
              len(restarted['records']) == 1 and not restarted['records'][0].get('path') and
              restarted['records'][0].get('dirty'), restarted)
        capture(main_ctx, 'blank-reopen', save=True)
        dirty_id = int(restarted['records'][0]['id'])
        keys(main_ctx, 0x11, 0x57)
        capture(main_ctx, 'blank-marker-discard-dialog', save=True)
        keys(main_ctx, 0x25)
        keys(main_ctx, 0x0D)
        wait_manifest(main_ctx, lambda value: len(value.get('records', [])) == 1 and
                      int(value['records'][0]['id']) != dirty_id and
                      not value['records'][0].get('path') and not value['records'][0].get('dirty'),
                      'blank marker explicitly discarded')
        close_window(main_ctx)

        # Separate-process reverse ancestor order: A first, then A/docs.
        reverse_roots, reverse_files = make_fixtures(out / 'reverse-fixtures')
        reverse_ctx = new_context('reverse-ancestor', reverse_roots)
        start(reverse_ctx, reverse_files['parent'])
        wait_manifest(reverse_ctx, lambda value: len(value.get('records', [])) == 1,
                      'reverse startup parent')
        open_path(reverse_ctx, reverse_files['child'])
        snap, rows, bars, _, _, _ = scan(reverse_ctx, 'reverse-ancestor-order')
        check('reverse order retains both parent and child tabs', len(rows) == 2 and len(bars) == 2,
              [(r['id'], r['path'], r['vaultRoot']) for r in rows])
        if len(rows) == 2 and len(bars) == 2:
            check('reverse order merges to the same real manifest root',
                  bool(rows[0].get('vaultRoot')) and
                  path_key(rows[0]['vaultRoot']) == path_key(rows[1].get('vaultRoot', '')),
                  (rows[0]['vaultRoot'], rows[1]['vaultRoot']))
            check('reverse order parent and child colors match',
                  visual.color_distance(bars[0][2], bars[1][2]) <= 12,
                  (bars[0][2], bars[1][2]))
        close_window(reverse_ctx)
    finally:
        for ctx in reversed(processes):
            proc = ctx['proc']
            if proc is not None and proc.poll() is None and ctx['hwnd']:
                try:
                    pid = ctypes.c_ulong()
                    cad.user32.GetWindowThreadProcessId(ctx['hwnd'], ctypes.byref(pid))
                    if pid.value == proc.pid and cad.ensure_foreground(ctx['hwnd']):
                        cad.user32.PostMessageW(ctx['hwnd'], 0x0010, 0, 0)
                    proc.wait(timeout=6)
                except Exception:
                    pass
        (out / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2),
                                         encoding='utf-8')
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()

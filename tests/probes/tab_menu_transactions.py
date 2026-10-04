"""Own-process transactions, full-path tooltip, and editor-font menu probe.

This is a real-window companion to tab_menu_visual.py. Run serially with the
final executable hash; it creates all documents, settings, screenshots, and
session state under a fresh output directory.
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tab_menu_visual as visual


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--expected-sha256', required=True)
    ap.add_argument('--scale', type=float, default=0.8)
    ap.add_argument('--target-effective-scale', type=float, default=1.0)
    ap.add_argument('--ui-font', type=int, default=14)
    ap.add_argument('--editor-font', type=int, choices=(17, 32), default=17,
                    help='custom current editor size whose submenu row is tested')
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--lang', default='zh-CN', choices=('zh-CN', 'en'))
    ap.add_argument('--window', default='2200x1300', help='physical client WxH; capped at 2500x1500')
    args = ap.parse_args()

    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    if not exe.is_file():
        ap.error(f'executable does not exist: {exe}')
    actual_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    if actual_hash.lower() != args.expected_sha256.lower():
        ap.error(f'executable SHA256 mismatch: {actual_hash}')
    try:
        requested_w, requested_h = (int(x) for x in args.window.lower().split('x', 1))
    except ValueError:
        ap.error('--window must be WxH')
    if requested_w > 2500 or requested_h > 1500 or requested_w < 440 or requested_h < 400:
        ap.error('--window physical client dimensions must be 440..2500 by 400..1500')
    out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware()
    cad.assert_unlocked('tab menu transactions')

    config = out / 'appdata' / 'EUI-Edits'
    config.mkdir(parents=True)
    temp = out / 'temp'
    temp.mkdir()
    fixtures = out / 'fixtures' / '长路径根目录' / ('路径段-' * 8)
    fixtures.mkdir(parents=True)
    paths = [fixtures / f'{i:02d}-事务探针-' / f'文档-{i:02d}.md' for i in range(4)]
    for i, path in enumerate(paths):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            f'fixture {i}\n\nink-row-measurement 0123456789 abcdefghijklmnopqrstuvwxyz\n\n'
            f'second fixed paragraph with the same rendered text width for each font size\n',
            encoding='utf-8')

    settings = config / 'settings.ini'
    settings.write_text(
        f'mode=1\nui_scale={args.scale}\nui_font_size={args.ui_font}\n'
        f'editor_font_size={args.editor_font}\ntheme={args.theme}\nanimations=0\n'
        f'ui_language={args.lang}\nshow_status_bar=1\n', encoding='utf-8')
    env = dict(os.environ, APPDATA=str(config.parent), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
    result = dict(exe=str(exe), sha256=actual_hash, expected_sha256=args.expected_sha256.lower(),
                  scale=args.scale, target_effective_scale=args.target_effective_scale,
                  ui_font=args.ui_font, editor_font=args.editor_font, theme=args.theme, lang=args.lang,
                  requested_client_px=[requested_w, requested_h], checks=[], captures=[],
                  measurements={}, manifests=[], exits=[], normal_exit_verified=False, exception=None)
    proc = None
    hwnd = None
    unit = 1.0
    forced_resize_cases=[]

    def check(name, condition, detail=None):
        passed = bool(condition)
        result['checks'].append(dict(name=name, passed=passed, detail=detail))
        print(('PASS ' if passed else 'FAIL ') + name, detail or '', flush=True)
        if not passed:
            raise AssertionError(name)

    def owned():
        cad.assert_unlocked('tab menu transactions')
        pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        check('live owned foreground window', proc is not None and proc.poll() is None and
              pid.value == proc.pid and cad.user32.GetForegroundWindow() == hwnd,
              dict(window_pid=pid.value, process_pid=proc.pid if proc else None))

    def client_rect():
        rect = cad.wintypes.RECT()
        cad.user32.GetClientRect(hwnd, ctypes.byref(rect))
        return rect.right, rect.bottom

    def resize_client(target_w,target_h,label):
        # SWP_NOSENDCHANGING is intentional for below-minimum forced-layout
        # stress cases. It suppresses app tracking-size negotiation but still
        # sends WM_SIZE so layout/rendering use the requested client geometry.
        flags=0x0004|0x0400
        applied=False
        for _ in range(12):
            cw,ch=client_rect()
            if applied and abs(cw-target_w)<=2 and abs(ch-target_h)<=2:
                result['measurements'].setdefault('window_sizes',[]).append(
                    dict(label=label,requested_client_px=[target_w,target_h],actual_client_px=[cw,ch],
                         setwindowpos_flags='0x0404',forced_below_tracking_minimum=target_w<950 or target_h<600))
                if target_w<950 or target_h<600:
                    forced_resize_cases.append(label)
                return cw,ch
            outer=cad.wintypes.RECT();cad.user32.GetWindowRect(hwnd,ctypes.byref(outer))
            dw=(outer.right-outer.left)-cw; dh=(outer.bottom-outer.top)-ch
            cad.user32.SetWindowPos(hwnd,None,0,0,target_w+dw,target_h+dh,flags)
            applied=True
            time.sleep(.35)
        cw,ch=client_rect()
        check(f'{label} exact client size reached',abs(cw-target_w)<=2 and abs(ch-target_h)<=2,
              dict(requested=[target_w,target_h],actual=[cw,ch]))
        raise AssertionError(f'{label} client size not honored')

    def start(path):
        nonlocal proc, hwnd, unit
        proc = subprocess.Popen([str(exe), str(path)], cwd=exe.parent, env=env)
        for _ in range(150):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None:
                break
            time.sleep(.1)
        check('main window appears', bool(hwnd), proc.pid)
        pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        check('window belongs to launched PID', pid.value == proc.pid, [pid.value, proc.pid])
        # The one explicit foreground acquisition is at startup. Every later
        # interaction checks GetForegroundWindow and never steals focus back.
        check('startup foreground acquired', cad.ensure_foreground(hwnd) and
              cad.user32.GetForegroundWindow() == hwnd)
        cw,ch=resize_client(requested_w,requested_h,'startup-client')
        system_scale = cad.user32.GetDpiForWindow(hwnd) / 96.0
        unit = system_scale * args.scale
        result['measurements'].setdefault('runs', []).append(
            dict(pid=proc.pid, dpi=cad.user32.GetDpiForWindow(hwnd), system_scale=system_scale,
                 effective_scale=unit, client_px=[cw, ch]))
        check('effective scale matches requested target', abs(unit-args.target_effective_scale) <= .01,
              dict(actual=unit, expected=args.target_effective_scale))
        check('physical client stays within safety bound', cw <= 2500 and ch <= 1500,
              [cw, ch])

    def capture(name):
        owned()
        w, h, raw = cad.capture_client(hwnd)
        target = out / f'{name}.png'
        cad.write_png(str(target), w, h, raw)
        result['captures'].append(str(target.relative_to(out)))
        return w, h, raw

    def detect_panel(frame, width, height, y_start=0, x0=0, x1=None, y_end=None):
        menu_bar=max(30.0,args.ui_font+20.0)
        if x1 is None:
            sw=width/unit
            label='Settings' if args.lang=='en' else '设置'
            settings_width=8+16+6+sum(args.ui_font if ord(c)>=0x2E80 else args.ui_font*.55 for c in label)+12
            panel_width=min(440.0,max(300.0,sw*.42),sw-16.0)
            panel_x=max(8.0,min(sw-settings_width-10-panel_width,sw-panel_width-8))
            x0=(panel_x-8)*unit; x1=(panel_x+panel_width+8)*unit
        bound_y=int(y_end) if y_end is not None else min(height-int(32*unit),int((menu_bar+400)*unit))
        left,right,rows=visual.panel_body_columns(frame,width,unit,int(x0),width if x1 is None else int(x1),
            max(int((menu_bar+2)*unit),int(y_start)),
            min(height,bound_y),min_straight=80.0)
        if left is None or not rows:
            return None
        return left,right,rows[0],rows[-1]

    def detect_below_tooltip(frame,width,height,list_bottom_px):
        logical_width=width/unit
        tooltip_width=min(480.0,max(220.0,logical_width-24.0))
        tooltip_left=max(8.0,logical_width-tooltip_width-8.0)
        # Restrict to the predicted tooltip x span so the full-width status-bar
        # separator cannot masquerade as a popup border. Test both candidate
        # lower bounds; the short-window box can legitimately reach screen-8.
        x0=int(round(tooltip_left*unit)); x1=int(round((tooltip_left+tooltip_width)*unit))
        expected_top=list_bottom_px+8*unit
        y0=min(height-1,int(list_bottom_px+4*unit))
        y_ends=sorted(set((max(y0+1,height-round(32*unit)),height)))
        candidates=[]
        for y_end in y_ends:
            left,right,rows=visual.panel_body_columns(frame,width,unit,x0,x1,y0,y_end,min_straight=100.0)
            if left is None or not rows:
                candidates.append(dict(y_end=y_end,geometry_pass=False,reason='no bordered body'))
                continue
            measured_width=(right-left+1)/unit
            top_error=abs(rows[0]-expected_top)/unit
            geometry_pass=abs(measured_width-tooltip_width)<=3.0 and top_error<=3.0
            candidates.append(dict(y_end=y_end,geometry_pass=geometry_pass,left=left,right=right,width_dip=measured_width,
                                   rows=[rows[0],rows[-1]],top_error_dip=top_error))
        chosen=None
        if height/unit<=432:
            full=candidates[-1]
            if full.get('geometry_pass') and abs((height-1-full['rows'][-1])/unit-8.0)<=4.0:
                chosen=full
        else:
            for candidate in candidates:
                if candidate.get('geometry_pass') and candidate['y_end']<height and \
                   candidate['rows'][-1] < candidate['y_end']-round(24*unit):
                    chosen=candidate
                    break
        if chosen is None:
            return dict(candidates=candidates,expected=[x0,x1,expected_top],chosen=None)
        return dict(left=chosen['left'],right=chosen['right'],rows=tuple(chosen['rows']),candidates=candidates,
                    chosen_y_end=chosen['y_end'],tooltip_width_dip=tooltip_width)

    def region_match_ratio(a,b,width,box,tolerance=18):
        x0,x1,y0,y1=(int(v) for v in box)
        same=total=0
        for y in range(max(0,y0),min(y1,len(a)//(width*4))):
            row=y*width*4
            for x in range(max(0,x0),min(x1,width)):
                i=row+x*4; total+=1
                if abs(a[i]-b[i])+abs(a[i+1]-b[i+1])+abs(a[i+2]-b[i+2])<=tolerance:
                    same+=1
        return same/total if total else 0.0

    def first_ink_band_height(frame,width,height):
        # Locate the first rendered editor text line with row ink counts in a
        # fixed content region; the region excludes the menu, gutter, and far
        # right blank canvas. This is a real glyph-height measure, not a hash.
        x0=int(width*0.52); x1=max(x0+1,width-int(30*unit))
        y0=int((max(30.0,args.ui_font+20.0)+8.0)*unit)
        y1=max(y0+1,height-int(35*unit))
        active=[]
        for y in range(y0,min(y1,height)):
            bg=visual.row_background(frame,width,y,max(x1-80,int(x0+1)),max(x1-1,x0+2))
            ink=sum(1 for x in range(x0,x1)
                    if visual.color_distance(visual.px(frame,width,x,y),bg)>48)
            active.append(ink>=3)
        start=None
        for i,on in enumerate(active+[False]):
            if on and start is None: start=i
            elif not on and start is not None:
                if i-start>=2:
                    return i-start
                start=None
        return 0

    def click(x, y):
        owned()
        cad.click(hwnd, round(x*unit), round(y*unit))
        time.sleep(.32)

    def move(x, y):
        owned()
        p = cad.wintypes.POINT(round(x*unit), round(y*unit))
        cad.user32.ClientToScreen(hwnd, ctypes.byref(p))
        cad.user32.SetCursorPos(p.x, p.y)
        time.sleep(.12)

    def path_move(points):
        for x, y in points:
            move(x, y)

    def key(*keys):
        owned()
        for vk in keys:
            cad.user32.keybd_event(vk, cad.user32.MapVirtualKeyW(vk, 0), 0, 0)
            time.sleep(.025)
        for vk in reversed(keys):
            cad.user32.keybd_event(vk, cad.user32.MapVirtualKeyW(vk, 0), 2, 0)
            time.sleep(.025)
        time.sleep(.35)

    def type_text(value):
        owned()
        for char in value:
            cad.user32.PostMessageW(hwnd, 0x0102, ord(char), 1)
        time.sleep(.4)

    def manifest():
        return json.loads((config/'session'/'manifest.json').read_text(encoding='utf-8'))

    def record_manifest(label, snap=None):
        snap=manifest() if snap is None else snap
        result['manifests'].append(dict(label=label,snapshot=snap))
        return snap

    def wait_manifest(predicate, label, timeout=8):
        deadline = time.time()+timeout
        latest = None
        while time.time() < deadline:
            try:
                latest = manifest()
                if predicate(latest):
                    return record_manifest(label,latest)
            except (OSError, ValueError, KeyError):
                pass
            time.sleep(.06)
        raise AssertionError(f'{label}: manifest did not reach expected state; last={latest}')

    def wait_file_text(path,marker,label,timeout=8):
        deadline=time.time()+timeout; last=''
        while time.time()<deadline:
            try:
                last=path.read_text(encoding='utf-8')
                if marker in last: return last
            except (OSError,UnicodeError):
                pass
            time.sleep(.05)
        raise AssertionError(f'{label}: expected text was not saved to {path}; last tail={last[-160:]!r}')

    def ids(snap=None):
        snap = snap or manifest()
        return [int(r['id']) for r in snap.get('records', [])]

    def open_file(path):
        owned()
        staged = temp/'owned-open.tmp'
        request = temp/'EUI-Edits.next-open'
        staged.write_text(str(path), encoding='utf-8')
        staged.replace(request)
        key(0x10)  # Shift consumes the private deferred-open request.
        check('deferred open request consumed', not request.exists(), str(path))

    def open_tab_list():
        w, h, closed = capture('list-closed')
        y = int(round(17*unit))
        bg = visual.row_background(closed, w, y, 0, w)
        clusters = visual.content_clusters(closed, w, y, 0, w, bg)
        check('top-bar settings control detected', bool(clusters), clusters)
        settings_label='Settings' if args.lang=='en' else '设置'
        settings_text_width=sum(args.ui_font if ord(c)>=0x2E80 else args.ui_font*.55 for c in settings_label)
        settings_width=8.0+min(16.0,max(16.0,args.ui_font+20.0)-14.0)+6.0+settings_text_width+12.0
        list_center=w/unit-settings_width-10.0-22.0
        click(list_center, 17)
        w2, h2, opened = capture('list-open')
        panel_bounds=detect_panel(opened,w2,h2,35*unit)
        check('tab-list panel detected by border geometry',panel_bounds is not None,panel_bounds)
        x0,x1,y0,y1=panel_bounds
        panel = (x0/unit, x1/unit, y0/unit, y1/unit)
        # Clear reveal-to-active scroll by repeatedly wheeling upward inside
        # the real panel, then verify the first visible accent run.
        move((panel[0]+25), panel[2]+30)
        for _ in range(8):
            owned(); cad.user32.mouse_event(0x0800, 0, 0, 120*100, 0); time.sleep(.1)
        time.sleep(.3)
        return list_center, panel

    def dismiss_confirmation(choice):
        # Dialog starts focused on Cancel (index 2); choose explicitly through
        # visible-button keyboard order: Left moves to Discard, Right to Save.
        capture(f'confirm-{choice}')
        if choice == 'cancel':
            key(0x1B)
        elif choice == 'discard':
            key(0x25, 0x0D)
        elif choice == 'save':
            key(0x27, 0x0D)
        else:
            raise ValueError(choice)

    def set_editor_end_marker(marker):
        key(0x11, 0x23)  # Ctrl+End
        type_text(marker)

    def click_row_action(panel, index, action='close'):
        x0, x1, y0, y1 = panel
        width = x1-x0+1
        card_width = width-16.0
        # List is wheeled to the top; first card is inset 8, then 76 DIP pitch.
        action_offset = {'up': 12.0, 'down': 40.0, 'close': 68.0}[action]
        x = x0+8.0+card_width-90.0+action_offset
        y = y0+8.0+index*76.0+20.0
        click(x, y)

    def normal_close():
        nonlocal proc
        owned()
        # All dirty decisions are completed in the tests. A single WM_CLOSE
        # must exit cleanly; no blind dismissal loop is allowed.
        cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
        proc.wait(timeout=15)
        result['exits'].append(proc.returncode)
        result['normal_exit_verified'] = proc.returncode == 0
        check('normal process exit code is zero', proc.returncode == 0, proc.returncode)

    def test_font_menu():
        # zh-CN/en title widths are estimated from the same documented glyph
        # advance rule used by widgets.h; menu item height is uiFont+18.
        def glyph_width(text, font):
            return sum(font if ord(c) >= 0x2E80 else .55*font for c in text)+20.0
        labels = ('File', 'Edit', 'View') if args.lang == 'en' else ('文件', '编辑', '视图')
        resize_client(380,400,'font-menu-right-lower-edge')
        cw,ch=client_rect()
        check('font-menu right/lower edge client reached',abs(cw-380)<=2 and abs(ch-400)<=2,[cw,ch])
        view_x = 10 + glyph_width(labels[0], args.ui_font)+2 + glyph_width(labels[1], args.ui_font)+2 + glyph_width(labels[2], args.ui_font)*.5
        row_h = max(28.0, args.ui_font+18.0)
        menu_bar=max(30.0,args.ui_font+20.0)

        def open_font_child(label):
            click(view_x, menu_bar*.5)
            w,h,root=capture('view-menu-'+label)
            root_edges=visual.panel_body_columns(root,w,unit,0,w,int((menu_bar+4)*unit),h-int(32*unit))
            check('View parent menu body detected',root_edges[0] is not None,root_edges)
            root_left,root_right,_=root_edges
            # A left-flipped Layout child covers the parent's left check column.
            # Hover/sample the parent's unobstructed right-hand part instead.
            hover_x=root_right/unit-30.0
            move(hover_x,menu_bar+6.0+row_h*.5)
            w2,h2,hover0=capture('view-row-zero-'+label)
            bands=visual.diff_bands_rows(root,hover0,w,int(root_right-45*unit),int(root_right-15*unit),
                                         int(menu_bar*unit),min(h,int((menu_bar+420)*unit)),
                                         tolerance=10,min_px=int(16*unit))
            check('View menu hover row detected',bool(bands),bands)
            measured_h=(bands[0][1]-bands[0][0]+1)/unit
            check('View row height follows UI metrics',abs(measured_h-row_h)<=2.0,[measured_h,row_h])
            font_top=bands[0][0]/unit+6*row_h+16.0
            move(hover_x,font_top+measured_h*.5)
            time.sleep(.35)
            wf,hf,child=capture('font-child-'+label)
            # Use the component's exact childMenuX rule with the measured root
            # body, since the flipped child may intentionally overlap the
            # parent's horizontal extent in a window too narrow for both.
            child_x=max(8.0,root_left/unit-112.0-4.0)
            current_size=read_editor_font()
            choices=6 if current_size in (12,14,16,18,20,24) else 7
            child_height=choices*row_h+12.0
            expected_child_top=min(font_top,hf/unit-child_height-8.0)
            child_edges=visual.panel_body_columns(child,wf,unit,int(child_x*unit),
                min(wf,int((child_x+112.0)*unit)),int((expected_child_top-3)*unit),
                min(hf,int((expected_child_top+child_height+2)*unit)))
            check('font submenu body detected',child_edges[0] is not None,child_edges)
            left,right,rows=child_edges
            width_dip=(right-left+1)/unit
            check('font submenu is 112 DIP wide',abs(width_dip-112.0)<=1.5,width_dip)
            check('font submenu fully inside client',left>=0 and right<wf and rows[0]>=0 and rows[-1]<hf,
                  [left,right,rows[0],rows[-1]])
            check('font submenu flips left at window edge',left<root_left,
                  dict(child_left=left,parent_left=root_left))
            child_top=rows[0]/unit
            check('font submenu vertically clamps at lower edge',
                  child_top<font_top-20 and hf-rows[-1]<=12*unit,
                  dict(child_top=child_top,parent_desired_top=font_top,bottom_margin_px=hf-rows[-1]))
            return left,right,child_top+6.0,row_h

        def read_editor_font():
            for line in settings.read_text(encoding='utf-8').splitlines():
                if line.startswith('editor_font_size='):
                    return int(float(line.split('=',1)[1]))
            raise AssertionError('settings.ini lost editor_font_size')

        def choose_font(index,value):
            left,right,first_top,item_h=open_font_child(str(value))
            click((left+right)/(2*unit),first_top+(index+.5)*item_h)
            time.sleep(.45)
            actual=read_editor_font()
            check(f'editor font menu persists {value}',actual==value,dict(actual=actual,expected=value))
            _,_,frame=capture(f'editor-body-font-{value}')
            return hashlib.sha256(frame).hexdigest()

        # A non-preset current size must appear and be selectable at row 0.
        sizes=[args.editor_font,12,16,24]
        # Selecting a custom value leaves it visible at row 0 on reopen; the
        # first preset follows it, so 12 is row 1 (not row 0).
        choose_font(0,args.editor_font)
        hashes=[]; ink_heights=[]
        for index,value in ((1,12),(2,16),(5,24)):
            left,right,first_top,item_h=open_font_child(f'choose-{value}')
            click((left+right)/(2*unit),first_top+(index+.5)*item_h)
            time.sleep(.45)
            actual=read_editor_font()
            check(f'editor font menu persists {value}',actual==value,dict(actual=actual,expected=value))
            w,h,frame=capture(f'editor-body-font-{value}')
            hashes.append(hashlib.sha256(frame).hexdigest())
            ink_heights.append(first_ink_band_height(frame,w,h))
        check('editor body first-line ink height increases at 12/16/24',
              0<ink_heights[0]<ink_heights[1]<ink_heights[2],ink_heights)
        result['measurements']['editor_font_body']={'sizes':sizes,'preset_capture_sha256':hashes,
            'first_line_ink_height_px':ink_heights}

    def test_tooltip(window_w, window_h, label):
        # Re-size in physical client pixels; dimensions are recorded as actual.
        cw,ch=resize_client(window_w,window_h,label)
        check(f'{label} client geometry reached',abs(cw-window_w)<=2 and abs(ch-window_h)<=2,[cw,ch])
        w, h, closed = capture(f'{label}-closed')
        y = int(round(17*unit)); bg = visual.row_background(closed,w,y,0,w)
        clusters = visual.content_clusters(closed,w,y,0,w,bg)
        check(f'{label} tab list button found', bool(clusters), clusters)
        settings_label='Settings' if args.lang=='en' else '设置'
        settings_text_width=sum(args.ui_font if ord(c)>=0x2E80 else args.ui_font*.55 for c in settings_label)
        settings_width=8.0+min(16.0,max(16.0,args.ui_font+20.0)-14.0)+6.0+settings_text_width+12.0
        list_x=w/unit-settings_width-10.0-22.0
        click(list_x,17)
        w, h, opened = capture(f'{label}-list-open')
        bounds=detect_panel(opened,w,h,35*unit,y_end=h-round(32*unit))
        check(f'{label} list panel found by border geometry',bounds is not None,bounds)
        px0,px1,py0,py1=(v/unit for v in bounds)
        panel_w=px1-px0+1; panel_h=py1-py0+1
        move(px0+24,py0+24)
        for _ in range(10):
            owned(); cad.user32.mouse_event(0x0800,0,0,120*100,0); time.sleep(.08)
        time.sleep(.25)
        w,h,opened=capture(f'{label}-list-at-top')
        # First row path line lies below title; hover the path itself, away from
        # all 3 action hit targets. In short layouts, the list may scroll and
        # viewport geometry is recorded so reviewers can inspect the captures.
        move(px0+38,py0+8+24+2+7)
        time.sleep(.6)
        tw,th,tip=capture(f'{label}-tooltip-first-row')
        # Bound list detection to the original panel bottom + 3px. The tooltip
        # starts 8 DIP below the list, so it cannot extend this scan into the
        # footer or be mistaken for the list's own bottom edge.
        list_after=detect_panel(tip,tw,th,int(py0*unit),int(px0*unit),int(px1*unit)+1,
                                y_end=int(py1*unit)+3)
        check(f'{label} list border measured in its isolated vertical range',list_after is not None,list_after)
        list_bottom=list_after[3] if list_after else None
        tooltip_body=detect_below_tooltip(tip,tw,th,list_bottom) if list_after else None
        check(f'{label} below-list tooltip has expected bordered geometry',
              tooltip_body is not None and tooltip_body.get('chosen_y_end') is not None,tooltip_body)
        tip_left,tip_right,tip_rows=tooltip_body['left'],tooltip_body['right'],tooltip_body['rows']
        tip_top,tip_bottom=tip_rows[0],tip_rows[-1]
        check(f'{label} tooltip does not cover row action region',tip_top>list_bottom,
              dict(list_bottom_px=list_bottom,tooltip_top_px=tip_top,
                   action_y_px=[list_after[2]+8*unit,list_bottom-8*unit]))
        text_crop=(tip_left+8,tip_right-8,tip_top+8,min(tip_bottom,tip_top+30))
        check(f'{label} tooltip contains visible path/root ink',
              any(visual.color_distance(visual.px(tip,tw,x,y),visual.px(tip,tw,x+1,y))>5
                  for y in range(max(0,text_crop[2]),min(th,text_crop[3]))
                  for x in range(max(0,text_crop[0]),min(tw-1,text_crop[1]))),text_crop)
        # Walk through the continuous right-edge corridor and then down into
        # the below-list tooltip. The bridge must retain the visible detail.
        list_left_dip=list_after[0]/unit; list_right_dip=list_after[1]/unit; list_bottom_dip=list_bottom/unit
        route=[(px1-10,py0+41),(min(tw/unit-4,px1+4),py0+41),
               (min(tw/unit-4,px1+4),min(th/unit-12,list_bottom_dip+4)),
               (min(tw/unit-20,px1+30),min(th/unit-20,list_bottom_dip+30))]
        path_move(route)
        bw,bh,bridge=capture(f'{label}-tooltip-bridge')
        bridge_list=detect_panel(bridge,bw,bh,int(py0*unit),int(px0*unit),int(px1*unit)+1,
                                 y_end=int(py1*unit)+3)
        check(f'{label} list border remains measured after bridge',bridge_list is not None,bridge_list)
        check(f'{label} same list boundary remains after bridge',
              bridge_list is not None and all(abs(a-b)<=2 for a,b in zip(bridge_list,list_after)),
              dict(before=list_after,after=bridge_list))
        bridge_tip=detect_below_tooltip(bridge,bw,bh,bridge_list[3]) if bridge_list else None
        check(f'{label} first-row tooltip border survives bridge',
              bridge_tip is not None and bridge_tip.get('chosen_y_end') is not None,bridge_tip)
        bridge_left,bridge_right,bridge_rows=bridge_tip['left'],bridge_tip['right'],bridge_tip['rows']
        same_geometry=(abs(bridge_left-tip_left)<=2 and abs(bridge_right-tip_right)<=2 and
                       abs(bridge_rows[0]-tip_top)<=2 and abs(bridge_rows[-1]-tip_bottom)<=2)
        check(f'{label} same tooltip remains after bridge',same_geometry,
              dict(before=[tip_left,tip_right,tip_top,tip_bottom],
                   after=[bridge_left,bridge_right,bridge_rows[0],bridge_rows[-1]]))
        stable=region_match_ratio(tip,bridge,tw,text_crop,tolerance=18)
        check(f'{label} tooltip path text remains stable through bridge',stable>=.90,stable)
        # A wheel over the tooltip scrolls wrapped path/root content when the
        # short client clips it; capture both ends for manual text review.
        move(min(tw/unit-20,list_right_dip+24),min(th/unit-20,list_bottom_dip+30))
        owned(); cad.user32.mouse_event(0x0800,0,0,120*100,0); time.sleep(.4)
        _,_,scroll_top=capture(f'{label}-tooltip-scroll-top')
        owned(); cad.user32.mouse_event(0x0800,0,0,-120*100,0); time.sleep(.4)
        _,_,scroll_bottom=capture(f'{label}-tooltip-scroll-bottom')
        if window_h<=400:
            changed=1.0-region_match_ratio(scroll_top,scroll_bottom,tw,text_crop,tolerance=18)
            check(f'{label} short-window tooltip scroll changes visible text',changed>.005,changed)
        result['measurements'].setdefault('tooltips',[]).append(dict(
            label=label,client_px=[w,h],list_panel_dip=[panel_w,panel_h],tooltip_bounds_px=[tip_left,tip_right,tip_top,tip_bottom],
            path_fixture=str(paths[0]),actions_region_dip=[px1-90,px1-10]))
        # Close popup without clicking document rows.
        key(0x1B)

    try:
        start(paths[0])
        open_file(paths[1]); open_file(paths[2])
        snap=wait_manifest(lambda s: len(s.get('records',[]))==3,'three explicit fixture tabs')
        check('fixture manifest paths exact',
              [Path(r['path']).resolve() for r in snap['records']] == [p.resolve() for p in paths[:3]],
              [r['path'] for r in snap['records']])
        capture('three-tabs-before')
        test_font_menu()
        # List geometry at both requested cramped dimensions. A large window
        # is restored for transaction hit targets after the tooltip checks.
        test_tooltip(440,640,'narrow-440x640')
        test_tooltip(440,400,'short-440x400')
        # Restore the wide client before row actions.
        resize_client(requested_w,requested_h,'transaction-client-restore')

        list_center,panel=open_tab_list()
        before=wait_manifest(lambda s: len(s.get('records',[]))==3,'before clean list close')
        target=int(before['records'][1]['id'])
        click_row_action(panel,1,'close')
        after=wait_manifest(lambda s: target not in ids(s),'clean list close removes target')
        check('clean close removed exactly its target', ids(after)==[int(before['records'][0]['id']),int(before['records'][2]['id'])],ids(after))
        # Popup remains open after action; Esc then closes it without changing order.
        rw,rh,retained=capture('clean-close-list-retained')
        check('clean close leaves tab list open',detect_panel(retained,rw,rh,35*unit) is not None)
        key(0x1B)
        check('Escape closes retained list only', ids()==ids(after),ids())
        ew,eh,escaped=capture('clean-close-list-escaped')
        check('Escape removes tab-list popup',detect_panel(escaped,ew,eh,35*unit) is None)

        # Dirty target B: Cancel retains it. Reopen list and Discard removes B,
        # with a manifest assertion guarding against a neighbor-close false pass.
        snap=wait_manifest(lambda s: len(s['records'])==2,'two tabs after clean transaction')
        dirty_id=int(snap['records'][0]['id'])
        key(0x11,0x31)  # select second remaining document by Ctrl+1
        check('dirty transaction targets first manifest TabId',int(manifest()['active'])==dirty_id,
              dict(expected=dirty_id,actual=manifest()['active']))
        set_editor_end_marker('\nDIRTY-CANCEL-DISCARD')
        dirty=wait_manifest(lambda s: any(int(r['id'])==dirty_id and r['dirty'] for r in s['records']), 'dirty target persisted')
        list_center,panel=open_tab_list()
        dirty_index=ids(dirty).index(dirty_id)
        click_row_action(panel,dirty_index,'close')
        dismiss_confirmation('cancel')
        kept=wait_manifest(lambda s: int(dirty_id) in ids(s),'Cancel retains dirty target')
        check('Cancel keeps exact dirty TabId and neighbor order',ids(kept)==ids(dirty),ids(kept))
        # Dirty-close routing closes the list before showing its modal. Reopen
        # from its control after Cancel, then repeat the exact same TabId action.
        cw,ch,cancel_frame=capture('dirty-cancel-list-retained')
        check('dirty Cancel closes tab list popup',detect_panel(cancel_frame,cw,ch,35*unit) is None,ids())
        list_center,panel=open_tab_list()
        click_row_action(panel,ids(kept).index(dirty_id),'close')
        dismiss_confirmation('discard')
        discarded=wait_manifest(lambda s: dirty_id not in ids(s),'Discard removes dirty target')
        check('Discard removes only dirty target',ids(discarded)==[int(r['id']) for r in kept['records'] if int(r['id'])!=dirty_id],ids(discarded))
        cw,ch,discard_frame=capture('dirty-discard-list-retained')
        check('dirty Discard closes tab list popup',detect_panel(discard_frame,cw,ch,35*unit) is None)

        # Third original path remains; mark it dirty and select Save from the
        # dirty-close confirmation. The path/body must be saved before its
        # exact TabId disappears from the manifest.
        remaining=wait_manifest(lambda s: len(s['records'])==1,'one original tab remains')
        save_id=int(remaining['records'][0]['id'])
        save_path=Path(remaining['records'][0]['path']).resolve()
        key(0x11,0x31)  # Ctrl+1 selects the only tab
        check('save transaction targets remaining TabId',int(manifest()['active'])==save_id,
              dict(expected=save_id,actual=manifest()['active']))
        set_editor_end_marker('\nDIRTY-SAVE-EXPECTED')
        wait_manifest(lambda s: any(int(r['id'])==save_id and r['dirty'] for r in s['records']),'save target dirty')
        list_center,panel=open_tab_list()
        click_row_action(panel,0,'close')
        dismiss_confirmation('save')
        closed=wait_manifest(lambda s: save_id not in ids(s),'Save choice saves and removes dirty target')
        wait_file_text(save_path,'DIRTY-SAVE-EXPECTED','Save choice wrote exact target body')
        check('Save choice wrote marker to exact fixture path',
              'DIRTY-SAVE-EXPECTED' in save_path.read_text(encoding='utf-8'),str(save_path))
        check('saved close leaves exactly the expected blank document',len(closed['records'])==1 and
              not closed['records'][0].get('path') and not closed['records'][0].get('dirty'),
              closed.get('records'))
        sw,sh,saved_frame=capture('dirty-save-list-closed')
        check('dirty Save closes tab list popup',detect_panel(saved_frame,sw,sh,35*unit) is None)
        normal_close()
    except Exception as exc:
        result['exception'] = f'{type(exc).__name__}: {exc}'
        result['checks'].append(dict(name='probe completed without exception',passed=False,detail=result['exception']))
        raise
    finally:
        if proc is not None and proc.poll() is None:
            # We do not steal foreground or dismiss unknown dialogs. WM_CLOSE
            # is attempted for owned cleanup; any abnormal process termination
            # is recorded as a failure, never represented as normal exit.
            pid=ctypes.c_ulong()
            if hwnd:
                cad.user32.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
                result['cleanup_owned_pid'] = pid.value
                if pid.value == proc.pid and cad.user32.GetForegroundWindow() == hwnd:
                    cad.user32.PostMessageW(hwnd,0x0010,0,0)
            try:
                proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                result['cleanup_still_running'] = True
        result['normal_exit_verified'] = bool(result['exits'] and result['exits'][-1] == 0)
        result['forced_below_tracking_minimum'] = bool(forced_resize_cases)
        result['forced_resize_cases'] = forced_resize_cases
        if result['exception'] or not result['normal_exit_verified']:
            result['checks'].append(dict(name='normal exit and exception status',passed=False,
                                         detail=dict(exception=result['exception'],exits=result['exits'])))
        (out/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        (out/'manifest.json').write_text(json.dumps(dict(
            exe=str(exe),sha256=actual_hash,expected_sha256=args.expected_sha256.lower(),
            effective_scale=result['measurements'].get('runs',[]),
            forced_below_tracking_minimum=bool(forced_resize_cases),
            forced_resize_cases=forced_resize_cases,snapshots=result['manifests']),
            ensure_ascii=False,indent=2),encoding='utf-8')


if __name__ == '__main__':
    main()

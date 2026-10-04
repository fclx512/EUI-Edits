"""Owned-window redraw comparison; captures before any restorative interaction.

Resize by one pixel and back to obtain a fresh rendering at identical geometry.
Compare viewport interiors only, excluding editor caret and window corner masks.
Never applies file associations. Every run uses private APPDATA/TEMP and a PID.
"""
import argparse, ctypes, hashlib, json, os, subprocess, time
from pathlib import Path
from PIL import Image, ImageChops
import win_capture as cad
from capture_markdown import window_for_pid
from file_assoc_settings import association_snapshot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1); ap.add_argument('--font', type=int, default=14)
    ap.add_argument('--lang', default='zh-CN'); ap.add_argument('--scale', type=float, default=1)
    ap.add_argument('--animations', type=int, choices=[0,1], default=0)
    ap.add_argument('--expect-fixed', action='store_true')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    profile, temp = out/'profile', out/'temp'
    (profile/'EUI-Edits').mkdir(parents=True); temp.mkdir()
    (profile/'EUI-Edits/settings.ini').write_text(
        f'ui_language={args.lang}\nui_font_size={args.font}\nui_scale={args.scale}\ntheme={args.theme}\nanimations={args.animations}\nmode=1\nreadable_width=0\n', encoding='utf-8')
    docs=[]
    for name, count in [('A', 140), ('B', 90)]:
        root=out/name;root.mkdir()
        for i in range(count): (root/f'{name}-{i:02d}.md').write_text(f'# {name} document {i}\n\nSample content.\n',encoding='utf-8')
        docs.append(root/f'{name}-00.md')
    wrap_doc=out/'wrap.md'
    wrap_doc.write_text('# Wrap redraw\n\n'+('A long plain text line with 中文文字 and **bold words**. '*18+'\n\n')*5+
        '> '+('Quoted long text 中文. '*35)+'\n\n```cpp\n'+('int long_code = 1; '*35)+'\n```\n\n'+
        '| Heading A | Heading B |\n| --- | --- |\n| '+('table long content '*25)+' | value |\n',encoding='utf-8')
    env=dict(os.environ,APPDATA=str(profile),TEMP=str(temp),TMP=str(temp),NEO_SINGLE_INSTANCE='0',
             NEO_D2D_SOFTWARE='1',NEO_WIN32_DC='1',NEO_LIVE_RESIZE='0')
    cad.make_dpi_aware();cad.assert_unlocked('refresh UI')
    registry_before=association_snapshot()
    result=dict(exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),checks=[],pairs=[])
    def check(name,value):
        result['checks'].append(dict(name=name,passed=bool(value)))
        assert value,name
    with (out/'renderer.log').open('w',encoding='utf-8') as log:
        proc=subprocess.Popen([str(exe)],cwd=exe.parent,env=env,stderr=log)
        hwnd=None
        try:
            for _ in range(100):
                hwnd=window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:break
                time.sleep(.1)
            check('owned window starts',hwnd and proc.poll() is None)
            check('foreground',cad.ensure_foreground(hwnd))
            unit=cad.user32.GetDpiForWindow(hwnd)/96*args.scale
            result['effective_scale']=unit
            def owned():
                cad.assert_unlocked('refresh UI')
                pid=ctypes.c_ulong();cad.user32.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
                assert pid.value==proc.pid and proc.poll() is None and cad.user32.GetForegroundWindow()==hwnd,'owned foreground lost'
            cr,wr=cad.wintypes.RECT(),cad.wintypes.RECT()
            cad.user32.GetClientRect(hwnd,ctypes.byref(cr));cad.user32.GetWindowRect(hwnd,ctypes.byref(wr))
            width,height=round(1100*unit)+(wr.right-wr.left-cr.right),round(820*unit)+(wr.bottom-wr.top-cr.bottom)
            check('fits desktop',width<cad.user32.GetSystemMetrics(0)-80 and height<cad.user32.GetSystemMetrics(1)-80)
            cad.user32.SetWindowPos(hwnd,None,40,40,width,height,4);time.sleep(.8)
            def move(x,y):
                owned();pt=cad.wintypes.POINT(round(x*unit),round(y*unit));cad.user32.ClientToScreen(hwnd,ctypes.byref(pt))
                cad.user32.SetCursorPos(pt.x,pt.y);time.sleep(.3 if args.animations else .18)
            def click(x,y):
                move(x,y);cad.click(hwnd,round(x*unit),round(y*unit));time.sleep(.35)
            def key(*keys):
                owned()
                for k in keys:cad.user32.keybd_event(k,0,0,0)
                for k in reversed(keys):cad.user32.keybd_event(k,0,2,0)
                time.sleep(.35)
            def text(value):
                owned()
                for ch in value:cad.user32.PostMessageW(hwnd,0x102,ord(ch),1)
                time.sleep(.35)
            def wheel(x,y,delta):
                move(x,y);cad.user32.mouse_event(0x800,0,0,ctypes.c_ulong(delta & 0xffffffff).value,0);time.sleep(1.25)
            def capture(name):
                owned();w,h,raw=cad.capture_client(hwnd)
                cad.write_png(str(out/f'{name}.png'),w,h,raw)
                return Image.frombytes('RGBA',(w,h),bytes(raw),'raw','BGRA').convert('RGB')
            def compare(name,rect):
                # Do not move the pointer or dispatch input between action and capture.
                before=capture(name+'-idle')
                cad.user32.SetWindowPos(hwnd,None,40,40,width+1,height,4);time.sleep(.15)
                cad.user32.SetWindowPos(hwnd,None,40,40,width,height,4);time.sleep(.4)
                after=capture(name+'-redraw')
                box=tuple(round(v*unit) for v in rect)
                a,b=before.crop(box),after.crop(box);diff=ImageChops.difference(a,b)
                count=sum(1 for p in diff.get_flattened_data() if max(p)>5)
                result['pairs'].append(dict(name=name,box=box,changed_pixels=count,area=a.width*a.height))
                if count:diff.save(out/f'{name}-difference.png')
                if args.expect_fixed:check(name+' matches fresh redraw',count==0)
            # Fresh unsaved document has short path and no inherited vault root.
            move(700,780);plain=capture('short-tooltip-none')
            move(335,18);tip=capture('short-tooltip-left')
            box=(0,round((args.font+22)*unit),round(800*unit),round(200*unit))
            bbox=ImageChops.difference(plain.crop(box),tip.crop(box)).getbbox()
            check('title hover shows tooltip',bbox is not None)
            result['short_tooltip_bounds']=bbox
            if args.expect_fixed:check('short tooltip adapts width',(bbox[2]-bbox[0])/unit<260)
            for name,x,y in [('close',427,18),('tail-gap',440,18),('top-gap',427,5),('bottom-gap',427,args.font+18)]:
                move(x,y);shot=capture('short-tooltip-'+name)
                if args.expect_fixed:check(name+' suppresses tooltip',ImageChops.difference(plain.crop(box),shot.crop(box)).getbbox() is None)
            move(700,780)
            request=temp/'EUI-Edits.next-open';request.write_text(str(docs[0]),encoding='utf-8')
            key(0x10);time.sleep(.6);check('first document consumed',not request.exists())
            compare('initial-vault',(8,175,275,770))
            request=temp/'EUI-Edits.next-open';request.write_text(str(docs[1]),encoding='utf-8')
            key(0x10);time.sleep(.6);check('deferred second document consumed',not request.exists())
            compare('opened-B',(8,175,275,770))
            for i in range(6):
                key(0x11,0x31+i%2);compare(f'switch-{i}',(8,175,275,770))
            move(700,780);b_top=capture('library-B-top-reference')
            key(0x11,0x31);wheel(130,350,-120*24);move(700,780);time.sleep(.5)
            a_scrolled=capture('library-A-scrolled-reference')
            key(0x11,0x32);time.sleep(.5);b_restored=capture('library-B-restored')
            list_box=tuple(round(v*unit) for v in (10,200,255,765))
            b_diff=ImageChops.difference(b_top.crop(list_box),b_restored.crop(list_box)).getbbox()
            result['library_B_restore_difference']=b_diff
            if args.expect_fixed:check('switch restores B visible rows',b_diff is None)
            key(0x11,0x31);time.sleep(.5);a_restored=capture('library-A-restored')
            a_diff=ImageChops.difference(a_scrolled.crop(list_box),a_restored.crop(list_box)).getbbox()
            result['library_A_restore_difference']=a_diff
            if args.expect_fixed:check('switch restores A scrolled rows',a_diff is None)
            key(0x11,0x32)
            for i,delta in enumerate([-120*3,-120*3,120*2,120*5]):
                wheel(130,350,delta);compare(f'vault-scroll-{i}',(8,175,275,770))
            # Filtering changes visible row contents and scroll bounds.
            click(100,args.font*2+129);key(0x11,0x41);text('00');compare('vault-filter',(8,175,275,770))
            key(0x11,0x41);key(8);compare('vault-filter-cleared',(8,175,275,770))
            move(700,780);capture('vault-before-external-change')
            added=out/'B/!external-added.md';added.write_text('# Added by owned test\n',encoding='utf-8')
            time.sleep(2);compare('vault-external-add',(8,175,275,770))
            added.unlink();time.sleep(2);compare('vault-external-remove',(8,175,275,770))
            request.write_text(str(wrap_doc),encoding='utf-8');key(0x10);time.sleep(.6)
            check('wrap document consumed',not request.exists())
            title_width=20+args.font*(2 if args.lang=='zh-CN' else 4*.55)
            view_x=10+2*(title_width+2)+title_width/2
            menu_y=args.font+20;item_h=args.font+18
            for i in range(4):
                click(view_x,17);click(view_x+40,menu_y+6+item_h*3.5+8)
                compare(f'wrap-toggle-{i}',(285,105,1060,750))
            key(0x11,0x57)
            key(0x11,0xBC)
            header=max(72,args.font*2+36)
            click(85,header+20+48*2+20)
            capture('associations-top')
            # Outer page scroll shifts the nested list viewport. The card starts
            # around y=250; wheel outside list to force nonzero outer translation.
            wheel(1050,210,-120*3)
            capture('associations-outer-scrolled')
            for i,delta in enumerate([-120*3,-120*2,120*2,120*5]):
                wheel(550,530,delta);compare(f'assoc-nested-scroll-{i}',(205,105,1060,765))
            wheel(1050,210,120*20)
            capture('associations-back-top')
            # Category changes and search update reused extension row IDs.
            card_y=header+24+(round(args.font*1.55)+26)+(14+(args.font+4)+4+(args.font-1+5)*2+6+12)
            click(740,card_y+136);compare('assoc-category',(205,105,1060,765))
            click(390,card_y+180);text('json')
            # Keep the blinking input caret outside the damage comparison region.
            compare('assoc-search',(205,card_y+250,1060,765))
            key(0x11,0x41);key(8);compare('assoc-search-cleared',(205,card_y+250,1060,765))
            key(0x1B)
            # A fresh unsaved tab gives a short tooltip independent of fixture path.
            key(0x11,0x4E);move(685,18);capture('tooltip-left')
            move(788,18);capture('tooltip-close')
            move(800,18);capture('tooltip-tail-gap')
            move(788,5);capture('tooltip-close-top-gap')
            move(720,70);capture('tooltip-dismissed')
            click(view_x,17);capture('view-menu')
            for name,index,seps in [('layout',0,0),('appearance',5,2),('font',6,2),('language',7,3)]:
                move(view_x+20,menu_y+6+item_h*(index+.5)+8*seps)
                capture('submenu-'+name)
            key(0x1B)
            # File/new and encoding children must also follow the common sizing.
            click(30,17);move(75,menu_y+6+item_h*.5);capture('submenu-new')
            move(75,menu_y+6+item_h*7.5+8*3);capture('submenu-encoding')
            key(0x1B)
            check('associations unchanged',association_snapshot()==registry_before)
            cad.user32.PostMessageW(hwnd,0x10,0,0);check('normal exit',proc.wait(timeout=10)==0)
        finally:
            if proc.poll() is None:
                if hwnd:cad.user32.PostMessageW(hwnd,0x10,0,0)
                try:proc.wait(timeout=10)
                except subprocess.TimeoutExpired:proc.terminate();proc.wait(timeout=10)
            result['exit']=proc.returncode
            (out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(dict(out=str(out),checks=len(result['checks']),pairs=len(result['pairs']),
                         changed=[p for p in result['pairs'] if p['changed_pixels']],exit=result['exit']),ensure_ascii=False))


if __name__=='__main__':main()

"""Owned Win32 text-file/settings acceptance. Never applies production associations."""
import argparse, ctypes, hashlib, json, os, subprocess, time, winreg
from pathlib import Path
from PIL import Image
import win_capture as cad
from capture_markdown import window_for_pid, write_settings
from file_assoc_settings import association_snapshot, normalized_snapshot


def registry_snapshot():
    snapshot = association_snapshot()
    paths = ['Software\\Classes\\EUIEdits.CodeDocument', 'Software\\Classes\\EUIEdits.DataDocument']
    # Read every extension defined by the same source registry, including foreign values.
    import re
    header = Path('apps/neo_editor/model/file_types.h').read_text(encoding='utf-8')
    extensions = re.findall(r'\{"([a-z0-9]+)",', header)
    for ext in extensions:
        paths += [f'Software\\Classes\\.{ext}\\OpenWithProgids',
                  f'Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.{ext}\\UserChoice']
    def read(path):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
                children, values, _ = winreg.QueryInfoKey(key)
                snapshot[path] = [winreg.EnumValue(key, i) for i in range(values)]
                for i in range(children): read(path + '\\' + winreg.EnumKey(key, i))
        except FileNotFoundError: snapshot[path] = None
    for path in paths: read(path)
    return normalized_snapshot(snapshot)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1); ap.add_argument('--scale', type=float, default=1)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    appdata, docs = out/'appdata', out/'documents'
    docs.mkdir(exist_ok=True); appdata.mkdir(exist_ok=True)
    source = '# source 中文\ndef hello(name):\n    flag = True\n    """first\n    second\n    """\n    return "hello" + name + '+repr('x'*230)+'\n\n# end\n'
    samples = {'sample.py': source.encode(), 'config.json': '{\n  "name": "中文",\n  "enabled": true,\n  "count": 123\n}\n'.encode(),
               '.env': b'KEY=123\n', '.gitignore': b'build/\n*.tmp\n', 'LICENSE': b'License plain text\n', 'unknown.odd': b'Unknown text\n',
               'notes.md': '# 标题\n\n**正文**\n'.encode(),
               'utf16.json': b'\xff\xfe'+'{\r\n  "名字": "测试"\r\n}\r\n'.encode('utf-16-le'),
               'legacy.py': '# 中文\r\nprint("你好")\r\n'.encode('gbk'),
               'mixed.txt': b'one\r\ntwo\nthree\r\nfour\n', 'binary.odd': b'Binary\0Data\0\x01\xff'}
    for name, value in samples.items(): (docs/name).write_bytes(value)
    settings = appdata/'EUI-Edits/settings.ini'
    write_settings(str(settings), str(docs/'sample.py'), args.theme, 18, str(args.scale))
    with settings.open('a', encoding='utf-8') as f:
        f.write('mode=1\nanimations=0\nshow_status_bar=1\nline_numbers=1\n')
    cad.make_dpi_aware(); cad.assert_unlocked('text files'); cad.assert_no_foreign_instance('text files')
    before_registry = registry_snapshot()
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  theme=args.theme, scale=args.scale, checks=[])
    env = dict(os.environ, APPDATA=str(appdata), NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
    proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env); hwnd = None
    def check(name, value):
        result['checks'].append(dict(name=name, passed=bool(value)))
        print(name, bool(value), flush=True)
        assert value, name
    def owned():
        cad.assert_unlocked('text files')
        pid = ctypes.c_ulong(); cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        assert proc.poll() is None and pid.value == proc.pid and cad.ensure_foreground(hwnd)
    def key(*keys):
        owned()
        for k in keys:
            cad.user32.keybd_event(k, cad.user32.MapVirtualKeyW(k, 0), 0, 0); time.sleep(.035)
        time.sleep(.06)
        for k in reversed(keys):
            cad.user32.keybd_event(k, cad.user32.MapVirtualKeyW(k, 0), 2, 0); time.sleep(.035)
        time.sleep(.25)
    def point_owned(x,y):
        cad.user32.WindowFromPoint.argtypes=[cad.wintypes.POINT]; cad.user32.WindowFromPoint.restype=ctypes.c_void_p
        cad.user32.GetAncestor.argtypes=[ctypes.c_void_p,ctypes.c_uint]; cad.user32.GetAncestor.restype=ctypes.c_void_p
        point=cad.wintypes.POINT(round(x),round(y)); cad.user32.ClientToScreen(hwnd,ctypes.byref(point))
        target=cad.user32.WindowFromPoint(point); root=cad.user32.GetAncestor(target,2)
        if root!=hwnd:
            pid=ctypes.c_ulong();cad.user32.GetWindowThreadProcessId(root,ctypes.byref(pid))
            name=ctypes.create_unicode_buffer(128);cad.user32.GetClassNameW(root,name,128)
            result['obscured']=dict(point=[point.x,point.y],root=root,pid=pid.value,window_class=name.value)
            raise AssertionError(f'owned client obscured: {result["obscured"]}, expected {hwnd}')
    def click(x, y):
        owned(); point_owned(x*unit,y*unit)
        cad.click(hwnd, round(x*unit), round(y*unit)); time.sleep(.35)
    def capture(name):
        owned(); time.sleep(.3)
        pixels = cad.capture_client(hwnd)
        for x in (12,pixels[0]*.25,pixels[0]*.5,pixels[0]*.75,pixels[0]-24):
            for y in (12,pixels[1]*.25,pixels[1]*.5,pixels[1]*.75,pixels[1]-24): point_owned(x,y)
        cad.write_png(str(out/(name+'.png')), *pixels)
        return pixels
    def copy_all():
        click(560, 160); key(0x11, 0x41); key(0x11, 0x43)
        cad.user32.GetClipboardData.restype=ctypes.c_void_p
        cad.kernel32.GlobalLock.restype=ctypes.c_void_p; cad.kernel32.GlobalLock.argtypes=[ctypes.c_void_p]
        cad.kernel32.GlobalUnlock.argtypes=[ctypes.c_void_p]
        assert cad.user32.OpenClipboard(hwnd)
        try:
            handle = cad.user32.GetClipboardData(13); ptr = cad.kernel32.GlobalLock(handle); assert ptr
            try: value = ctypes.wstring_at(ptr).replace('\r\n','\n')
            finally: cad.kernel32.GlobalUnlock(handle)
        finally: cad.user32.CloseClipboard()
        key(0x27)
        return value
    def open_file(name):
        owned()
        secondary = subprocess.run([str(exe), str(docs/name)], cwd=exe.parent, env=env, timeout=10)
        check('forward '+name, secondary.returncode == 0)
        time.sleep(.8); owned()
    def red_button(name):
        for attempt in range(4):
            capture(name)
            img = Image.open(out/(name+'.png')).convert('RGB')
            # Danger action is the only red text/button on this settings page.
            points = [(x,y) for y in range(round(img.height*.4),img.height-15) for x in range(round(img.width*.65),img.width-15)
                      if (lambda c:c[0]>c[1]*1.5 and c[0]>c[2]*1.3 and c[0]>130)(img.getpixel((x,y)))]
            if points and max(y for x,y in points)<img.height-80:break
            owned();point=cad.wintypes.POINT(round(img.width*.8),round(img.height*.6));cad.user32.ClientToScreen(hwnd,ctypes.byref(point))
            cad.user32.SetCursorPos(point.x,point.y)
            for _ in range(3):cad.user32.mouse_event(0x0800,0,0,ctypes.c_ulong(-120).value,0);time.sleep(.1)
            time.sleep(.25)
        check(name+' red visible', bool(points))
        return ((min(x for x,y in points)+max(x for x,y in points))/2/unit,
                (min(y for x,y in points)+max(y for x,y in points))/2/unit)
    def modal_buttons():
        w,h,_=cad.capture_client(hwnd)
        # Dialog is centered 420x220 with the action row 58 px from its bottom.
        left=(w/unit-420)/2; top=(h/unit-220)/2
        return (left+420-24-75, top+220-58+16), (left+420-24-150-12-75, top+220-58+16)
    def nav(page):
        w=cad.capture_client(hwnd)[0]/unit
        click(80,168 if page=='editor' else 208) if w>=840 else click(w*(.5 if page=='editor' else 5/6),99)
    def settings_open():
        click(cad.capture_client(hwnd)[0]/unit-42,17)
        time.sleep(.3)
    def modal_present():
        primary,_=modal_buttons()
        w,h,data=cad.capture_client(hwnd)
        x,y=round(primary[0]*unit),round(primary[1]*unit)
        b,g,r=data[(y*w+x)*4:(y*w+x)*4+3]
        return r>g*1.5 and r>b*1.3
    try:
        for _ in range(100):
            hwnd=window_for_pid(proc.pid)
            if hwnd: break
            assert proc.poll() is None; time.sleep(.1)
        assert hwnd
        screen_w,screen_h=cad.user32.GetSystemMetrics(0),cad.user32.GetSystemMetrics(1)
        cad.user32.SetWindowPos(hwnd,None,30,30,min(screen_w-60,1460 if args.scale==1 else 1800),min(screen_h-100,1000 if args.scale==1 else 1540),4)
        time.sleep(.8); dpi=cad.user32.GetDpiForWindow(hwnd); unit=dpi/96*args.scale
        result.update(pid=proc.pid,dpi=dpi,screen=[screen_w,screen_h],foreground_owned=True,unlocked=True)
        capture('source-unwrapped'); check('source text loaded',copy_all()==source)
        key(0x11,0x23); capture('source-horizontal-caret')
        # Find source-only long line and follow its end horizontally.
        key(0x26); key(0x26); key(0x23); capture('source-horizontal-end')
        key(0x11,0x53); check('source navigation preserves bytes',(docs/'sample.py').read_bytes()==samples['sample.py'])
        for name, codec in [('config.json','utf-8'),('.env','utf-8'),('.gitignore','utf-8'),('LICENSE','utf-8'),('unknown.odd','utf-8'),
                            ('utf16.json','utf-16'),('legacy.py','gbk'),('mixed.txt','utf-8')]:
            open_file(name)
            expected=samples[name].decode(codec).replace('\r\n','\n')
            check('decoded '+name,copy_all()==expected)
            capture('file-'+name.replace('.','-'))
            key(0x11,0x53); check('unmodified bytes '+name,(docs/name).read_bytes()==samples[name])
        # Editing and undo use the same document history, including non-UTF8 input.
        open_file('legacy.py'); click(550,160); key(0x11,0x23)
        cad.user32.PostMessageW(hwnd,0x0102,ord('X'),1); time.sleep(.25); key(0x11,0x53)
        check('GBK edit and encoding retained',(docs/'legacy.py').read_bytes()==samples['legacy.py']+b'X')
        key(0x11,0x5A); key(0x11,0x53)
        check('GBK undo restores bytes',(docs/'legacy.py').read_bytes()==samples['legacy.py'])
        open_file('binary.odd'); capture('binary-rejected')
        check('binary rejection retains current document',copy_all()==samples['legacy.py'].decode('gbk').replace('\r\n','\n'))
        w,h,_=cad.capture_client(hwnd);click(w/unit-46,h/unit-74)
        open_file('sample.py'); settings_open(); nav('editor')
        x,y=red_button('editor-settings'); settings_before=settings.read_bytes()
        click(x,y); capture('reset-confirm'); check('reset awaits confirmation',modal_present())
        # Ordinary typing and shortcuts must not change a covered search/editor or open a document.
        cad.user32.PostMessageW(hwnd,0x0102,ord('Q'),1); key(0x11,0x4E); key(0x1B)
        capture('reset-cancel-escape')
        check('Escape cancels reset',settings.read_bytes()==settings_before and not modal_present())
        click(x,y); _,cancel=modal_buttons(); click(*cancel); capture('reset-cancel-button')
        check('Cancel button preserves settings',settings.read_bytes()==settings_before and not modal_present())
        click(x,y); click(30,30); capture('reset-dismiss-backdrop')
        check('backdrop preserves settings',settings.read_bytes()==settings_before and not modal_present())
        nav('file'); x2,y2=red_button('association-settings')
        click(x2,y2); capture('association-cleanup-confirm'); check('cleanup awaits confirmation',modal_present()); key(0x1B)
        # Search and selection are drafts until Apply; cleanup cancellation cannot mutate registry.
        click(400,412)
        for c in 'bat': cad.user32.PostMessageW(hwnd,0x0102,ord(c),1)
        time.sleep(.3); capture('association-bat-candidate')
        key(0x11,0x41)
        for c in 'json': cad.user32.PostMessageW(hwnd,0x0102,ord(c),1)
        time.sleep(.3); capture('association-search'); click(350,506); capture('association-selection-draft')
        check('search and selection do not apply associations',registry_snapshot()==before_registry)
        key(0x1B)
        settings_open(); nav('editor'); x,y=red_button('editor-before-confirm-reset')
        click(x,y); primary,_=modal_buttons(); click(*primary); time.sleep(.5)
        # Reset may change theme/scale. Read saved values instead of continuing with stale coordinates.
        saved=settings.read_text(encoding='utf-8')
        values=dict(line.split('=',1) for line in saved.splitlines() if '=' in line)
        check('confirmed reset applies defaults',float(values['editor_font_size'])==16 and float(values['ui_scale'])==1 and values['animations']=='1')
        capture('reset-applied')
        key(0x1B); check('reset preserves document',copy_all()==source)
        check('covered shortcuts did not create another document',(docs/'sample.py').read_bytes()==samples['sample.py'])
        check('production associations unchanged',registry_snapshot()==before_registry)
        check('all sample bytes intact',all((docs/n).read_bytes()==v for n,v in samples.items()))
    finally:
        if hwnd and proc.poll() is None:
            cad.user32.PostMessageW(hwnd,0x0010,0,0); proc.wait(timeout=10)
        result['exit_code']=proc.returncode
        result['executable_unchanged']=result['sha256']==hashlib.sha256(exe.read_bytes()).hexdigest()
        result['registry_unchanged']=registry_snapshot()==before_registry
        (out/'conditions.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    check('normal exit and stable binary',proc.returncode==0 and result['executable_unchanged'])


if __name__=='__main__': main()

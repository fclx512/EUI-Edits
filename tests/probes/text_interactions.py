"""Native open/save, language, wrapping and explicit new-document interaction."""
import argparse,ctypes,hashlib,json,os,subprocess,time
from pathlib import Path
import win_capture as cad
from capture_markdown import window_for_pid,write_settings

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--exe',required=True); ap.add_argument('--out',required=True)
    args=ap.parse_args();exe=Path(args.exe).resolve();out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
    docs=out/'documents';docs.mkdir(exist_ok=True);appdata=out/'appdata';appdata.mkdir(exist_ok=True)
    source='def hello():\n    return True\n\n'+('# long '+ 'word '*150)+'\n'
    doc=docs/'sample.py';doc.write_bytes(source.encode());odd=docs/'中文.odd';odd.write_bytes(b'Unknown editable text\n')
    settings=appdata/'EUI-Edits/settings.ini';write_settings(str(settings),str(doc),1,16,'1')
    with settings.open('a',encoding='utf-8') as f:f.write('show_status_bar=1\nline_numbers=1\nanimations=0\nmode=0\n')
    cad.make_dpi_aware();cad.assert_unlocked('text interactions');cad.assert_no_foreign_instance('text interactions')
    env=dict(os.environ,APPDATA=str(appdata),NEO_D2D_SOFTWARE='1',NEO_WIN32_DC='1')
    proc=subprocess.Popen([str(exe)],cwd=exe.parent,env=env);hwnd=None;result=dict(sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),checks=[])
    cad.user32.WindowFromPoint.argtypes=[cad.wintypes.POINT];cad.user32.WindowFromPoint.restype=ctypes.c_void_p
    cad.user32.GetAncestor.argtypes=[ctypes.c_void_p,ctypes.c_uint];cad.user32.GetAncestor.restype=ctypes.c_void_p
    cad.user32.SendMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t]
    def check(name,passed):result['checks'].append(dict(name=name,passed=bool(passed)));print(name,passed,flush=True);assert passed,name
    def owned(target=None):
        target=target or hwnd;pid=ctypes.c_ulong();cad.user32.GetWindowThreadProcessId(target,ctypes.byref(pid))
        assert proc.poll() is None and pid.value==proc.pid and cad.ensure_foreground(target);cad.assert_unlocked('text interactions')
        return target
    def key(*keys,target=None):
        owned(target)
        for k in keys:cad.user32.keybd_event(k,0,0,0);time.sleep(.035)
        time.sleep(.06)
        for k in reversed(keys):cad.user32.keybd_event(k,0,2,0);time.sleep(.035)
        time.sleep(.3)
    def click(x,y):
        owned();point=cad.wintypes.POINT(round(x*unit),round(y*unit));cad.user32.ClientToScreen(hwnd,ctypes.byref(point))
        assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(point),2)==hwnd,'click obscured'
        cad.click(hwnd,round(x*unit),round(y*unit));time.sleep(.3)
    def capture(name,target=None):
        target=owned(target);time.sleep(.3);pixels=cad.capture_client(target)
        for x in (12,pixels[0]*.25,pixels[0]*.5,pixels[0]*.75,pixels[0]-24):
            for y in (12,pixels[1]*.25,pixels[1]*.5,pixels[1]*.75,pixels[1]-24):
                point=cad.wintypes.POINT(round(x),round(y));cad.user32.ClientToScreen(target,ctypes.byref(point))
                assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(point),2)==target,'capture obscured'
        cad.write_png(str(out/(name+'.png')),*pixels);return pixels
    def type_text(text):
        for c in text:
            if c=='\n':key(0x0D)
            else:cad.user32.PostMessageW(hwnd,0x0102,ord(c),1)
    def dialog():
        matches=[];callback=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
        def visit(h,_):
            pid=ctypes.c_ulong();cad.user32.GetWindowThreadProcessId(h,ctypes.byref(pid));name=ctypes.create_unicode_buffer(128);cad.user32.GetClassNameW(h,name,128)
            if pid.value==proc.pid and name.value=='#32770' and cad.user32.IsWindowVisible(h):matches.append(h)
            return True
        for _ in range(50):
            cad.user32.EnumWindows(callback(visit),0)
            if matches:return matches[0]
            time.sleep(.1)
        raise AssertionError('owned native dialog absent')
    def filename(d,path):
        edits=[];callback=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
        def visit(h,_):
            name=ctypes.create_unicode_buffer(128);cad.user32.GetClassNameW(h,name,128)
            if name.value=='Edit' and cad.user32.IsWindowVisible(h):edits.append((h,cad.user32.GetDlgCtrlID(h)))
            return True
        cad.user32.EnumChildWindows(d,callback(visit),0);result.setdefault('dialog_edits',[]).append(edits)
        edit=next((h for h,i in edits if i==1148),None) or (edits[-1][0] if edits else None);assert edit
        buf=ctypes.create_unicode_buffer(str(path));cad.user32.SendMessageW(edit,0x000C,0,ctypes.addressof(buf));key(0x0D,target=d);time.sleep(.5)
        check('native dialog accepted selected path',not cad.user32.IsWindow(d))
    try:
        for _ in range(100):
            hwnd=window_for_pid(proc.pid)
            if hwnd:break
            assert proc.poll() is None;time.sleep(.1)
        assert hwnd;cad.user32.SetWindowPos(hwnd,None,30,30,1460,1000,4);time.sleep(.6)
        dpi=cad.user32.GetDpiForWindow(hwnd);unit=dpi/96;result.update(pid=proc.pid,dpi=dpi,theme=1,scale=1)
        w,h,_=capture('source');w/=unit;h/=unit
        # Simplified mode gives language/wrap controls enough room in the status bar.
        click(w-370,h-13);capture('language-menu');click(w-160,h-275+8+48);capture('plain-view')
        key(0x11,0x53);check('language switch preserves source bytes',doc.read_bytes()==source.encode())
        click(w-370,h-13);click(w-160,h-275+8+16);capture('auto-source-view')
        click(w-286,h-13);capture('wrapped-source');click(w-286,h-13);capture('unwrapped-source')
        key(0x11,0x53);check('wrap switches preserve source bytes',doc.read_bytes()==source.encode())
        key(0x11,0x4F);d=dialog();capture('native-open',d);filename(d,odd)
        capture('unknown-opened');key(0x11,0x53);check('unknown extension native open/save',odd.read_bytes()==b'Unknown editable text\n')
        key(0x11,0x4E);click(500,130)
        type_text('# Plain text\n**kept literal**')
        time.sleep(.3);capture('new-text');key(0x11,0x53);d=dialog();capture('new-text-save',d);filename(d,docs/'new-text.txt')
        check('new text saved as text',(docs/'new-text.txt').read_text(encoding='utf-8')=='# Plain text\n**kept literal**')
        click(30,17);capture('new-submenu');click(80,51);capture('new-kind-submenu');click(295,83)
        click(500,130)
        type_text('# Markdown\n\n**bold**')
        time.sleep(.3);capture('new-markdown');key(0x11,0x53);d=dialog();capture('new-markdown-save',d);filename(d,docs/'new-markdown.md')
        check('new Markdown saves source',(docs/'new-markdown.md').read_text(encoding='utf-8')=='# Markdown\n\n**bold**')
    finally:
        if hwnd and proc.poll() is None:
            # Close only an owned outstanding file dialog, then the owned main window.
            try:
                d=dialog();key(0x1B,target=d)
            except AssertionError:pass
            cad.user32.PostMessageW(hwnd,0x0010,0,0);proc.wait(timeout=10)
        result.update(exit_code=proc.returncode,executable_unchanged=result['sha256']==hashlib.sha256(exe.read_bytes()).hexdigest())
        (out/'conditions.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    check('normal exit',proc.returncode==0 and result['executable_unchanged'])

if __name__=='__main__':main()

"""Own-process tab/vault/recovery GUI acceptance; never drives a foreign instance."""
import argparse, ctypes, hashlib, json, os, shutil, subprocess, time
from pathlib import Path
import win_capture as cad
from capture_markdown import window_for_pid
from text_files import registry_snapshot
from memory_save import Counters


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--scale', type=float, default=1); ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--font', type=int, default=14)
    ap.add_argument('--body-kib', type=int, default=0,
                    help='append this many deterministic ASCII bytes to each initial document')
    args = ap.parse_args(); exe = Path(args.exe).resolve(); out = Path(args.out).resolve()
    if args.body_kib < 0:
        ap.error('--body-kib must be non-negative')
    out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware(); cad.assert_unlocked('document tabs'); # NEO_SINGLE_INSTANCE=0 + private TEMP/APPDATA keeps any user instance untouched.
    before = registry_snapshot()
    config = out/'appdata/EUI-Edits'; config.mkdir(parents=True)
    temp = out/'temp'; temp.mkdir()
    project = out/'项目 A'; (project/'docs').mkdir(parents=True)
    other = out/'项目 B'; other.mkdir()
    child = project/'docs/README.md'; parent = project/'README.md'; outside = other/'README.md'
    filler_pattern = b'NeoEditor deterministic tab-probe filler 0123456789abcdef\n'
    filler_size = args.body_kib * 1024
    filler_bytes = (filler_pattern * ((filler_size + len(filler_pattern) - 1) // len(filler_pattern)))[:filler_size]
    filler = filler_bytes.decode('ascii')
    original = '# Child\n\nChild content.\n' + filler
    child.write_text(original, encoding='utf-8')
    parent.write_text('# Parent\n\nParent content.\n' + filler, encoding='utf-8')
    outside.write_text('Other content.\n' + filler, encoding='utf-8')
    (config/'settings.ini').write_text(f'last_file={child}\nmode=1\nui_scale={args.scale}\nui_font_size={args.font}\n'
        f'theme={args.theme}\nanimations=0\nui_language=zh-CN\n', encoding='utf-8')
    env = dict(os.environ, APPDATA=str(config.parent), TEMP=str(temp), TMP=str(temp),
        NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
        scale=args.scale, font=args.font, theme=args.theme, body_kib=args.body_kib,
        checks=[], captures=[], exits=[], memory=[])
    proc = None; hwnd = None; unit = 1
    def check(name, value):
        result['checks'].append(dict(name=name, passed=bool(value)))
        print(name, bool(value), flush=True); assert value, name
    def owned():
        cad.assert_unlocked('document tabs'); pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        assert proc.poll() is None and pid.value == proc.pid and cad.user32.GetForegroundWindow() == hwnd, 'owned foreground lost'
    def key(*keys):
        owned()
        for k in keys: cad.user32.keybd_event(k, cad.user32.MapVirtualKeyW(k, 0), 0, 0); time.sleep(.03)
        for k in reversed(keys): cad.user32.keybd_event(k, cad.user32.MapVirtualKeyW(k, 0), 2, 0); time.sleep(.03)
        time.sleep(.25)
    def click(x, y):
        owned(); point = cad.wintypes.POINT(round(x*unit), round(y*unit)); cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
        cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]; cad.user32.WindowFromPoint.restype = ctypes.c_void_p
        cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]; cad.user32.GetAncestor.restype = ctypes.c_void_p
        assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(point), 2) == hwnd, 'client obscured'
        cad.click(hwnd, round(x*unit), round(y*unit)); time.sleep(.3)
    def text(value):
        owned()
        for ch in value: cad.user32.PostMessageW(hwnd, 0x0102, ord(ch), 1)
        time.sleep(.35)
    def capture(name):
        owned(); w,h,raw = cad.capture_client(hwnd); target = out/(name+'.png')
        cad.write_png(str(target),w,h,raw); result['captures'].append(name)
    def memory(stage):
        owned()
        kernel=ctypes.WinDLL('kernel32',use_last_error=True);psapi=ctypes.WinDLL('psapi',use_last_error=True)
        kernel.OpenProcess.argtypes=[cad.wintypes.DWORD,cad.wintypes.BOOL,cad.wintypes.DWORD]
        kernel.OpenProcess.restype=cad.wintypes.HANDLE
        kernel.CloseHandle.argtypes=[cad.wintypes.HANDLE]
        psapi.GetProcessMemoryInfo.argtypes=[cad.wintypes.HANDLE,ctypes.POINTER(Counters),cad.wintypes.DWORD]
        handle=kernel.OpenProcess(0x410,False,proc.pid);assert handle
        try:
            counters=Counters();counters.cb=ctypes.sizeof(counters)
            assert psapi.GetProcessMemoryInfo(handle,ctypes.byref(counters),counters.cb)
            result['memory'].append(dict(stage=stage,pid=proc.pid,ws_mib=counters.ws/1048576,private_mib=counters.private/1048576))
        finally:kernel.CloseHandle(handle)
    def manifest():
        return json.loads((config/'session/manifest.json').read_text(encoding='utf-8'))
    def body(snapshot, record):
        # v2 清单：dirty 记录用内容寻址的 body 文件名（body-<contentid>.utf8）。
        name = record.get('body') or f"body-{snapshot.get('generation','')}-{record['id']}.utf8"
        return (config/'session'/name).read_text(encoding='utf-8')
    def wait_manifest(predicate, label, timeout=8.0):
        # 会话写现在是后台串行提交：不要固定 sleep 蒙混，显式等期望状态出现。
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                last = manifest()
                if predicate(last):
                    return last
            except Exception:
                last = None
            time.sleep(0.05)
        check(label, False)
        return last if last is not None else {'records': [], 'active': 0}
    def open_forward(path):
        owned()
        # Exercise the app's deferred-open consumption with its private TEMP
        # file; avoid the global singleton mutex/event owned by a user's instance.
        request = temp/'EUI-Edits.next-open'
        stage = temp/'owned-open.tmp';stage.write_text(str(path),encoding='utf-8');stage.replace(request)
        key(0x10);time.sleep(.65);owned()
        check('owned deferred open consumed',not request.exists())
    def start(path=None):
        nonlocal proc,hwnd,unit
        proc = subprocess.Popen([str(exe)]+([str(path)] if path else []),cwd=exe.parent,env=env)
        hwnd = None
        for _ in range(150):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None: break
            time.sleep(.1)
        assert hwnd, 'main window missing'
        assert cad.ensure_foreground(hwnd), 'cannot foreground the launched window'
        owned()
        cad.user32.SetWindowPos(hwnd,None,30,30,1320,880,4); time.sleep(.5)
        unit = cad.user32.GetDpiForWindow(hwnd)/96*args.scale; result['effective_scale']=unit
    def close_clean():
        owned(); cad.user32.PostMessageW(hwnd,0x10,0,0); proc.wait(timeout=10)
        result['exits'].append(proc.returncode); check('main exited normally', proc.returncode == 0)
    def select(index): key(0x11,0x30+index)
    def discard(): key(0x25); key(0x0D)
    def save_as(path):
        key(0x11,0x53)
        dialog = None
        @ctypes.WINFUNCTYPE(cad.wintypes.BOOL,cad.wintypes.HWND,cad.wintypes.LPARAM)
        def visit(candidate,_):
            nonlocal dialog
            pid=cad.wintypes.DWORD();cad.user32.GetWindowThreadProcessId(candidate,ctypes.byref(pid))
            cls=ctypes.create_unicode_buffer(64);cad.user32.GetClassNameW(candidate,cls,64)
            if pid.value in {proc.pid,*cad.direct_child_process_ids(proc.pid)} and cad.user32.IsWindowVisible(candidate) and cls.value=='#32770':
                dialog=candidate;return False
            return True
        for _ in range(80):
            cad.user32.EnumWindows(visit,0)
            if dialog:break
            time.sleep(.1)
        assert dialog and cad.user32.GetForegroundWindow()==dialog, 'owned save picker missing'
        edits=[]
        @ctypes.WINFUNCTYPE(cad.wintypes.BOOL,cad.wintypes.HWND,cad.wintypes.LPARAM)
        def visit_edit(child,_):
            cls=ctypes.create_unicode_buffer(64);cad.user32.GetClassNameW(child,cls,64)
            if cls.value=='Edit' and cad.user32.IsWindowVisible(child):edits.append(child)
            return True
        cad.user32.EnumChildWindows(dialog,visit_edit,0);assert len(edits)==1
        cad.user32.SendMessageW.argtypes=[cad.wintypes.HWND,cad.wintypes.UINT,cad.wintypes.WPARAM,cad.wintypes.LPARAM]
        buffer=ctypes.create_unicode_buffer(str(path));cad.user32.SendMessageW(edits[0],0xC,0,ctypes.cast(buffer,ctypes.c_void_p).value)
        cad.user32.keybd_event(0x0D,0,0,0);cad.user32.keybd_event(0x0D,0,2,0);time.sleep(.8);owned()
    try:
        start(child); capture('initial-child');memory('initial-single-page')
        click(400,130);key(0x11,0x23);text('CHILD-EDIT');select(1)
        first=wait_manifest(lambda s: s['records'] and s['records'][0].get('dirty') and body(s,s['records'][0]).endswith('CHILD-EDIT'), 'first dirty body recorded')
        open_forward(parent); snap=wait_manifest(lambda s: len(s['records'])==2, 'opening parent keeps dirty child')
        check('opening parent keeps dirty child',len(snap['records'])==2 and snap['records'][0]['dirty'])
        check('parent and child share shortest opened root',all(Path(r['vaultRoot'])==project for r in snap['records']))
        capture('merged-project-and-open-markers')
        open_forward(outside);snap=wait_manifest(lambda s: len(s['records'])==3, 'unrelated root remains separate')
        check('unrelated root remains separate',len(snap['records'])==3 and Path(snap['records'][2]['vaultRoot'])==other)
        open_forward(child);snap=wait_manifest(lambda s: s['active']==s['records'][0]['id'], 'duplicate open reuses dirty tab')
        check('duplicate open reuses dirty tab',len(snap['records'])==3 and snap['active']==snap['records'][0]['id'])
        capture('same-name-distinct-libraries')
        # Undo after returning to the first page, then save, proves history survived.
        for _ in 'CHILD-EDIT':key(0x11,0x5A)
        key(0x11,0x53);check('undo survives page switch',child.read_text(encoding='utf-8')==original)
        select(2);key(0x11,0x23);text('PARENT-EDIT');select(2)
        select(3);key(0x11,0x23);text('OTHER-EDIT');select(3)
        snap=wait_manifest(lambda s: sum(r['dirty'] for r in s['records'])==2 and
            body(s,s['records'][1]).endswith('PARENT-EDIT') and body(s,s['records'][2]).endswith('OTHER-EDIT'),
            'two independent dirty bodies')
        capture('independent-dirty-before-check');(out/'dirty-debug.json').write_text(json.dumps(dict(snapshot=snap,bodies={str(r['id']):body(snap,r) for r in snap['records'] if r['dirty']}),ensure_ascii=False,indent=2),encoding='utf-8');check('two independent dirty bodies',sum(r['dirty'] for r in snap['records'])==2 and
            body(snap,snap['records'][1]).endswith('PARENT-EDIT') and body(snap,snap['records'][2]).endswith('OTHER-EDIT'))
        key(0x11,0x57);capture('close-dirty-cancel');key(0x1B)
        check('cancel close preserves all tabs',len(wait_manifest(lambda s: True, 'cancel close read')['records'])==3)
        # External edits to a background dirty file still require conflict consent.
        outside.write_text('External writer.\n',encoding='utf-8');select(2);select(3);key(0x11,0x53)
        capture('background-external-conflict');key(0x1B)
        check('external conflict cancel preserves disk',outside.read_text(encoding='utf-8')=='External writer.\n')
        owned();cad.user32.PostMessageW(hwnd,0x10,0,0);time.sleep(.4);capture('exit-first-dirty')
        key(0x1B);check('exit cancel retains all pages',proc.poll() is None and len(wait_manifest(lambda s: True, 'exit cancel read')['records'])==3)
        # Preserve a real committed multi-draft snapshot, then exit normally and
        # restore that snapshot in an isolated new session (no forced process kill).
        archived=out/'recovery-snapshot';shutil.copytree(config/'session',archived)
        owned();cad.user32.PostMessageW(hwnd,0x10,0,0);time.sleep(.4);discard();discard()
        proc.wait(timeout=10);result['exits'].append(proc.returncode);check('multi-dirty discard exit normal',proc.returncode==0)
        session_target=(config/'session').resolve()
        assert session_target.is_relative_to(out.resolve()), 'session cleanup escaped owned output'
        shutil.rmtree(session_target);shutil.copytree(archived,session_target)
        start(child);snap=wait_manifest(lambda s: len(s['records'])==3 and sum(r['dirty'] for r in s['records'])==2, 'restored all dirty drafts')
        check('restored all dirty drafts beside startup request',len(snap['records'])==3 and sum(r['dirty'] for r in snap['records'])==2)
        capture('restored-multiple-drafts')
        select(2);save_as(project/'恢复父文稿.txt');check('first recovered draft saves independently',(project/'恢复父文稿.txt').read_text(encoding='utf-8').endswith('PARENT-EDIT'))
        select(3);save_as(other/'恢复另一文稿.txt');check('second recovered draft saves independently',(other/'恢复另一文稿.txt').read_text(encoding='utf-8').endswith('OTHER-EDIT'))
        # Build overflow with clean tabs; verify keyboard reachability and list UI.
        for i in range(7):key(0x11,0x4E)
        snap=wait_manifest(lambda s: len(s['records'])==10, 'new creates independent clean tabs')
        check('new creates independent clean tabs',len(snap['records'])==10)
        memory('ten-small-pages')
        key(0x11,0x09);snap=wait_manifest(lambda s: s['active']==s['records'][0]['id'], 'Ctrl Tab cycles to first page')
        check('Ctrl Tab cycles to first page',snap['active']==snap['records'][0]['id'])
        key(0x11,0x10,0x09);snap=wait_manifest(lambda s: s['active']==s['records'][-1]['id'], 'Ctrl Shift Tab cycles to last page')
        check('Ctrl Shift Tab cycles to last page',snap['active']==snap['records'][-1]['id'])
        key(0x11,0x39);capture('overflow-tabs')
        cad.user32.SetWindowPos(hwnd,None,30,30,820,640,4);time.sleep(.5);capture('narrow-tabs')
        w,h,_=cad.capture_client(hwnd);logical=w/unit
        settings_width=8+16+6+2*args.font+12
        list_x=logical-settings_width-10-8-14
        click(list_x,18);capture('narrow-tab-list')
        # Hover and scroll within the list, then select the first page. This
        # catches both overlapping hover tooltips and reversed wheel direction.
        point=cad.wintypes.POINT(round((list_x-100)*unit),round(110*unit));cad.user32.ClientToScreen(hwnd,ctypes.byref(point))
        owned();cad.user32.SetCursorPos(point.x,point.y);time.sleep(.3)
        cad.user32.mouse_event(0x800,0,0,120*100,0);time.sleep(.5);capture('list-scrolled-top')
        click(list_x-100,max(30,args.font+20)+18)
        snap=wait_manifest(lambda s: s['active']==s['records'][0]['id'], 'wheel up and list click reach first tab')
        check('wheel up and list click reach first tab',snap['active']==snap['records'][0]['id'])
        key(0x11,0x39);click(list_x,18);key(0x1B)
        click(list_x,18);capture('list-reopened');click(350,230);capture('list-click-outside')
        # Close all clean pages back to a fresh blank page without closing window.
        for _ in range(10):key(0x11,0x57)
        snap=wait_manifest(lambda s: len(s['records'])==1 and not s['records'][0]['path'] and not s['records'][0]['dirty'], 'last close leaves one blank page')
        check('last close leaves one blank page',len(snap['records'])==1 and not snap['records'][0]['path'] and not snap['records'][0]['dirty'])
        capture('last-tab-blank');memory('closed-to-one-blank');close_clean()
        check('registry unchanged',registry_snapshot()==before)
    finally:
        if proc is not None and proc.poll() is None and hwnd:
            # Best effort normal cleanup limited to the owned test window.
            cad.ensure_foreground(hwnd);cad.user32.PostMessageW(hwnd,0x10,0,0);time.sleep(.3)
            for _ in range(16):
                if proc.poll() is not None:break
                try:discard()
                except Exception:break
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired: pass
        (out/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')


if __name__=='__main__':main()

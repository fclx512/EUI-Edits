"""Record an owned, isolated promotional trial; retain raw footage and evidence."""
import argparse, ctypes, hashlib, json, os, statistics, subprocess, sys, threading, time, traceback
from pathlib import Path
from mcp_client import Client

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests/probes'))
import win_capture as cad
from capture_markdown import window_for_pid
from memory_save import Counters

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--exe',required=True); ap.add_argument('--run',required=True)
    args=ap.parse_args(); exe=Path(args.exe).resolve(); run=Path(args.run).resolve()
    assert exe.is_file(); run.mkdir(parents=True,exist_ok=False)
    profile=run/'profile/EUI-Edits'; profile.mkdir(parents=True)
    temp=run/'temp';temp.mkdir(); docs=run/'demo';docs.mkdir()
    first=docs/'实时写作.md';first.write_text('',encoding='utf-8')
    (docs/'今日计划.md').write_text('# 今日计划\n\n让文字工作更简单。\n\n- [x] 整理想法\n- [x] 写下一段 Markdown\n- [ ] 继续下一项工作\n\n> 在几个文稿间自由切换。\n',encoding='utf-8')
    (docs/'代码笔记.md').write_text('# 代码笔记\n\n把说明和代码放在一起。\n\n```cpp\nint main() {\n    return 0;\n}\n```\n\n## 常用操作\n\n| 操作 | 用途 |\n| --- | --- |\n| 标签切换 | 继续写作 |\n| 查找 | 定位文字 |\n',encoding='utf-8')
    (profile/'settings.ini').write_text('ui_language=zh-CN\nui_font_size=14\neditor_font_size=22\nui_scale=1\ntheme=1\nanimations=1\nmode=1\nreadable_width=0\nline_numbers=0\n',encoding='utf-8')
    env=dict(os.environ,APPDATA=str(profile.parent),TEMP=str(temp),TMP=str(temp),NEO_SINGLE_INSTANCE='0',NEO_D2D_SOFTWARE='1',NEO_WIN32_DC='1',NEO_LIVE_RESIZE='0')
    report=dict(exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),scenes=[],events=[],memory_samples=[],render_environment=dict(software_rendering=True,NEO_D2D_SOFTWARE='1',NEO_WIN32_DC='1',NEO_LIVE_RESIZE='0',isolated_profile=True),checks={})
    proc=None;hwnd=None;client=None;rid=None;started=None;stop=threading.Event();worker=None;error=None
    cad.make_dpi_aware();cad.assert_unlocked('promo trial')
    def owned():
        cad.assert_unlocked('promo trial');owner=ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        assert proc.poll() is None and owner.value==proc.pid and cad.user32.IsWindow(hwnd),'demo ownership lost'
        assert cad.user32.GetForegroundWindow()==hwnd,'foreground lost; recording aborted'
    def key(*codes):
        owned()
        for code in codes:cad.user32.keybd_event(code,0,0,0)
        time.sleep(.025)
        for code in reversed(codes):cad.user32.keybd_event(code,0,2,0)
        time.sleep(.055)
    def text(value,pace=.028):
        for ch in value:
            owned()
            if ch=='\n':key(0x0d)
            elif ch=='\t':key(0x09)
            else:cad.user32.PostMessageW(hwnd,0x0102,ord(ch),1);time.sleep(pace)
    def capture(name):
        owned();w,h,raw=cad.capture_client(hwnd);cad.write_png(str(run/(name+'.png')),w,h,raw)
    def state():return client.tool('snow_shot_recording_state')['result']
    def wait(predicate,limit=30):
        until=time.perf_counter()+limit
        while time.perf_counter()<until:
            s=state()
            if s.get('error'):raise RuntimeError(s['error'])
            if predicate(s):return s
            time.sleep(.15)
        raise RuntimeError('recording state timeout')
    def control(action):
        s=state();assert s['recording_id']==rid
        return client.tool('snow_shot_recording_control',dict(recording_id=rid,expected_revision=s['revision'],action=action))
    def stamp(name):
        at=(state()['duration_ms']/1000) if rid else 0
        report['events'].append(dict(name=name,at_s=at)); print(name,at,flush=True);return at
    def sample():
        kernel=ctypes.WinDLL('kernel32',use_last_error=True);psapi=ctypes.WinDLL('psapi',use_last_error=True)
        kernel.OpenProcess.argtypes=[ctypes.c_uint32,ctypes.c_bool,ctypes.c_uint32];kernel.OpenProcess.restype=ctypes.c_void_p
        kernel.CloseHandle.argtypes=[ctypes.c_void_p]
        psapi.GetProcessMemoryInfo.argtypes=[ctypes.c_void_p,ctypes.POINTER(Counters),ctypes.c_uint32]
        handle=kernel.OpenProcess(0x410,False,proc.pid)
        try:
            while not stop.is_set():
                c=Counters();c.cb=ctypes.sizeof(c)
                if not psapi.GetProcessMemoryInfo(handle,ctypes.byref(c),c.cb):raise ctypes.WinError(ctypes.get_last_error())
                report['memory_samples'].append(dict(t_s=time.perf_counter()-started,pid=proc.pid,private_ws_mib=c.private_ws/1048576,working_set_mib=c.ws/1048576,private_commit_mib=c.private/1048576))
                stop.wait(.2)
        except Exception:report['memory_error']=traceback.format_exc()
        finally:kernel.CloseHandle(handle)
    def open_doc(path):
        owned();request=temp/'EUI-Edits.next-open'; stage=temp/'request.tmp';stage.write_text(str(path),encoding='utf-8');stage.replace(request)
        key(0x10)
        deadline=time.perf_counter()+5
        while request.exists() and time.perf_counter()<deadline:owned();time.sleep(.1)
        assert not request.exists(),'deferred open not consumed';time.sleep(.5)
    try:
        log=(run/'app.log').open('w',encoding='utf-8')
        proc=subprocess.Popen([str(exe),str(first)],cwd=exe.parent,env=env,stdout=log,stderr=log)
        for _ in range(120):
            hwnd=window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None:break
            time.sleep(.1)
        assert hwnd and proc.poll() is None
        cad.user32.SetWindowPos(hwnd,None,200,120,1600,1000,4)
        assert cad.ensure_foreground(hwnd)
        time.sleep(1);capture('prepared')
        rect=cad.wintypes.RECT();cad.user32.GetWindowRect(hwnd,ctypes.byref(rect))
        report.update(pid=proc.pid,hwnd=hwnd,region=[rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top])
        (run/'ready.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print('READY',str(run),flush=True)
        deadline=time.perf_counter()+120
        while not (run/'GO').exists():
            if time.perf_counter()>deadline:raise RuntimeError('preview approval cue timed out')
            time.sleep(.15)
        owned();client=Client();s=state();assert not s['open'] and not s['busy'] and s['state']=='idle','foreign recorder active'
        rid=client.tool('snow_shot_recording_start',dict(region=report['region'],options=dict(format='mp4',path=str(run/'raw.mp4'),clarity='1080p',frame_rate=30,encoder='h264',encoding_preset='veryfast',quality=90,capture_toolbar=False,show_cursor=True,show_keyboard=False,mouse_highlight=False,record_mouse_clicks=False,system_audio=False,microphone=False,start_delay_seconds=0,post_processing=False)))['result']['recording_id']
        wait(lambda s:s['state']=='recording' and not s['busy']);started=time.perf_counter()
        worker=threading.Thread(target=sample,daemon=True);worker.start()
        assert cad.ensure_foreground(hwnd), 'recorder did not restore demo foreground'
        owned()
        # Client-space editor position is checked in prepared.png before GO.
        cad.click(hwnd,600,200);key(0x11,0x24)
        start=stamp('typing_start')
        blocks=['# 从文字开始\n\n','一边写 Markdown，一边看见效果。\n\n','## 今天的灵感\n\n','- 写下一个想法\n','- [x] 完成第一段文字\n\n','> 让工具轻一点，让思路走远一点。\n\n','```cpp\n','int main() { return 0; }\n','```\n\n']
        for block in blocks:text(block);time.sleep(.65)
        key(0x11,0x24);time.sleep(2);capture('typed');end=stamp('typing_end')
        report['scenes'].append(dict(name='typing',start_s=start,end_s=end))
        start=stamp('editing_start');key(0x11,0x23);text('## 继续写作\n\n');text('编辑和预览，在同一个地方。',.09);time.sleep(1)
        key(0x11,0x53);time.sleep(.5);key(0x11,0x24);time.sleep(1);capture('edited');end=stamp('editing_end')
        report['scenes'].append(dict(name='editing',start_s=start,end_s=end))
        # Opening scenes are retained raw, but omit setup from selected tabs segment.
        open_doc(docs/'今日计划.md');open_doc(docs/'代码笔记.md');key(0x11,0x31);time.sleep(.4)
        start=stamp('tabs_start')
        for index in [2,3,1,3,2,1]:key(0x11,0x30+index);time.sleep(1.45)
        capture('tabs');end=stamp('tabs_end');report['scenes'].append(dict(name='tabs',start_s=start,end_s=end))
        stamp('memory_hold_start');baseline=time.perf_counter()-started;time.sleep(8)
        owned();capture('memory-sample');steady=[x for x in report['memory_samples'] if x['t_s']>=baseline+1]
        assert len(steady)>=20 and 'memory_error' not in report
        summary=dict(scenario='3 small Markdown documents, idle after typing/editing/tab switching',pid=proc.pid,samples=len(steady),document_bytes=sum(p.stat().st_size for p in docs.glob('*.md')),document_lines=sum(len(p.read_text(encoding='utf-8').splitlines()) for p in docs.glob('*.md')),process_count=1+len(cad.direct_child_process_ids(proc.pid)))
        for field in ['private_ws_mib','working_set_mib','private_commit_mib']:
            values=[x[field] for x in steady];summary[field]=dict(median=statistics.median(values),min=min(values),max=max(values))
        report['memory_summary']=summary
        control('stop');final=wait(lambda s:s['finalized'] and not s['busy']);report['recording_final']=final
        report['checks']['recording_finalized']=(run/'raw.mp4').is_file() and (run/'raw.mp4').stat().st_size>0
        control('close');report['checks']['recorder_closed']=not state()['open']
        saved=first.read_text(encoding='utf-8');report['saved_demo']=saved
        report['checks']['saved_content']=all(x in saved for x in ['# 从文字开始','## 今天的灵感','```cpp','## 继续写作','编辑和预览'])
        report['checks']['memory_samples_valid']=len(steady)>=20
    except Exception:
        error=traceback.format_exc();report['error']=error;print(error,flush=True)
    finally:
        stop.set()
        if worker:worker.join(timeout=3)
        if client:
            if rid:
                try:
                    s=state()
                    if s['open'] and s['recording_id']==rid:
                        if not s['finalized']:control('stop');wait(lambda x:x['finalized'] and not x['busy'])
                        control('close')
                except Exception:report['cleanup_error']=traceback.format_exc()
            report['mcp_calls']=client.messages;report['bridge_exit']=client.close()
        if proc and proc.poll() is None and hwnd:
            owner=ctypes.c_ulong();cad.user32.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
            if owner.value==proc.pid:
                cad.user32.PostMessageW(hwnd,0x0010,0,0)
                try:proc.wait(timeout=8)
                except subprocess.TimeoutExpired:report['exit_error']='owned demo did not close normally'
        if proc:report['demo_exit']=proc.poll();report['checks']['normal_exit']=proc.poll()==0
        (run/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print('RESULT',json.dumps({k:v for k,v in report.items() if k in ['checks','error','demo_exit','memory_summary','scenes']},ensure_ascii=False),flush=True)
    if error or not all(report['checks'].values()):raise SystemExit(1)

if __name__=='__main__':main()

import ctypes, hashlib, json, os, subprocess, sys, time, traceback
from pathlib import Path
from mcp_client import Client
HERE=Path(__file__).resolve().parent
REPO=HERE.parent.parent
sys.path.insert(0,str(REPO/'tests/probes'))
import win_capture as cad
OUT=Path(os.environ.get('NEO_RECORDING_OUT', str(REPO/'build-promo-recording'))).resolve()
info=json.loads((OUT/'active.json').read_text(encoding='utf-8'))
run=Path(info['run']);hwnd=info['hwnd'];pid=info['pid']
cad.make_dpi_aware()
result={'demo':info,'events':[],'checks':{}}
c=Client();rid=None;completed=False
def state():
    return c.tool('snow_shot_recording_state')['result']
def mark(name, **values):
    result['events'].append(dict(event=name, wall=time.perf_counter(), **values))
    print(name,json.dumps(values,ensure_ascii=False),flush=True)
def owned():
    cad.assert_unlocked('recording smoke')
    owner=ctypes.c_ulong()
    cad.user32.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
    assert owner.value==pid and cad.user32.IsWindow(hwnd), 'demo ownership lost'
    assert cad.ensure_foreground(hwnd), 'demo foreground lost'
def key(*keys):
    owned()
    for k in keys:cad.user32.keybd_event(k,0,0,0)
    for k in reversed(keys):cad.user32.keybd_event(k,0,2,0)
def capture(name):
    owned(); w,h,raw=cad.capture_client(hwnd)
    cad.write_png(str(run/(name+'.png')),w,h,raw)
def wait_for(predicate, label, limit=20):
    deadline=time.perf_counter()+limit
    while time.perf_counter()<deadline:
        s=state()
        if s.get('error'):raise RuntimeError(s)
        if predicate(s):return s
        time.sleep(.15)
    raise RuntimeError('Timed out: '+label)
def control(action):
    s=state()
    assert s['recording_id']==rid, 'recording ownership changed'
    r=c.tool('snow_shot_recording_control',dict(recording_id=rid,expected_revision=s['revision'],action=action))
    mark('control_'+action, state=r['result'].get('state'), revision=r['result'].get('revision'))
    return r['result']
try:
    s=state()
    assert not s['open'] and not s['busy'] and s['state']=='idle', 'foreign recording active'
    owned();capture('before-recording')
    path=run/'EUI-Edits-SnowShot-smoke.mp4'
    assert not path.exists()
    r=c.tool('snow_shot_recording_start',dict(region=info['region'],options=dict(
        format='mp4',path=str(path),clarity='1080p',frame_rate=30,encoder='h264',
        encoding_preset='veryfast',quality=90,capture_toolbar=False,show_cursor=True,
        show_keyboard=False,mouse_highlight=False,record_mouse_clicks=False,
        system_audio=False,microphone=False,start_delay_seconds=0,post_processing=False)))
    rid=r['result']['recording_id'];mark('start_requested',recording_id=rid)
    s=wait_for(lambda s:s['state']=='recording' and not s['busy'],'start')
    result['checks']['start']=True;mark('recording_started',duration_ms=s['duration_ms'])
    time.sleep(2)
    owned()
    origin=cad.wintypes.POINT(700,650);cad.user32.ClientToScreen(hwnd,ctypes.byref(origin))
    cad.user32.SetCursorPos(origin.x,origin.y)
    for _ in range(3):
        owned();cad.user32.mouse_event(0x800,0,0,ctypes.c_ulong((-120)&0xffffffff).value,0);time.sleep(.6)
    time.sleep(1.2)
    control('pause');s=wait_for(lambda s:s['state']=='paused' and not s['busy'],'pause')
    result['checks']['pause']=True;mark('paused_confirmed',duration_ms=s['duration_ms'])
    cad.click(hwnd,700,650);key(0x11,0x23);time.sleep(.5)
    capture('paused-only-bottom')
    time.sleep(3)
    key(0x11,0x24);time.sleep(.5)
    paused_duration=state()['duration_ms']
    mark('pause_ended',duration_ms=paused_duration)
    result['checks']['paused_duration_frozen']=abs(paused_duration-s['duration_ms'])<300
    control('resume');s=wait_for(lambda s:s['state']=='recording' and not s['busy'],'resume')
    result['checks']['resume']=True;mark('resumed_confirmed',duration_ms=s['duration_ms'])
    time.sleep(1.8)
    owned();cad.user32.SetCursorPos(origin.x,origin.y)
    for _ in range(2):
        owned();cad.user32.mouse_event(0x800,0,0,ctypes.c_ulong((-120)&0xffffffff).value,0);time.sleep(.6)
    time.sleep(1.7)
    capture('before-stop')
    mark('stop_requested');control('stop')
    s=wait_for(lambda s:s['finalized'] and not s['busy'],'finalization',30)
    completed=True;result['final_state']=s
    result['checks']['stop_finalized']=s['path']==str(path) or Path(s['path'])==path
    assert path.exists() and path.stat().st_size>0,'recording file missing'
    result['video']={'path':str(path),'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    mark('file_finalized',path=str(path),duration_ms=s['duration_ms'],bytes=path.stat().st_size)
    control('close')
    final=state();result['checks']['recording_closed']=not final['open'] and not final['busy']
except Exception:
    result['error']=traceback.format_exc();print(result['error'],flush=True)
finally:
    if rid and not completed:
        try:control('stop');wait_for(lambda s:s['finalized'] and not s['busy'],'cleanup finalization',15)
        except Exception as e:result['cleanup_error']=str(e)
    result['mcp_calls']=c.messages
    result['bridge_exit']=c.close()
    # Only the isolated owned demonstration process is closed.
    owner=ctypes.c_ulong();cad.user32.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
    if owner.value==pid:
        k=ctypes.windll.kernel32
        k.OpenProcess.restype=ctypes.c_void_p
        k.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32]
        k.GetExitCodeProcess.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
        k.CloseHandle.argtypes=[ctypes.c_void_p]
        handle=k.OpenProcess(0x100000|0x1000,False,pid)
        cad.user32.PostMessageW(hwnd,0x0010,0,0)
        if handle:
            k.WaitForSingleObject(handle,5000)
            exit_code=ctypes.c_ulong();k.GetExitCodeProcess(handle,ctypes.byref(exit_code))
            result['demo_exit']=exit_code.value;k.CloseHandle(handle)
    (run/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print('RESULT',json.dumps({k:v for k,v in result.items() if k not in ['mcp_calls','events','demo']},ensure_ascii=False),flush=True)
if result.get('error') or not all(result['checks'].values()) or result.get('demo_exit')!=0:
    raise SystemExit(1)

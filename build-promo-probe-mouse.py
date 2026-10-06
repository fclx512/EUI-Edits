import ctypes, json, os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'tests/probes'))
import win_capture as cad
from capture_markdown import window_for_pid

exe = ROOT/'out/latest/EUI-Edits-0.1.0-windows-x64.exe'
run = ROOT/'build-promo-probe-layout'
profile = run/'profile/EUI-Edits'; temp = run/'temp'; docs = run/'demo'
first = docs/'实时写作.md'
env = dict(os.environ, APPDATA=str(profile.parent), TEMP=str(temp), TMP=str(temp), NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='0')
cad.make_dpi_aware(); cad.assert_unlocked('mouse probe')
log = (run/'app2.log').open('w', encoding='utf-8')
proc = subprocess.Popen([str(exe), str(first)], cwd=str(exe.parent), env=env, stdout=log, stderr=log)
hwnd = None
for _ in range(120):
    hwnd = window_for_pid(proc.pid)
    if hwnd or proc.poll() is not None: break
    time.sleep(0.1)
assert hwnd and proc.poll() is None
cad.user32.SetWindowPos(hwnd, None, 280, 140, 2000, 1250, 4)
assert cad.ensure_foreground(hwnd)
time.sleep(1.5)
for name in ['今日计划.md', '代码笔记.md']:
    request = temp/'EUI-Edits.next-open'; stage = temp/'request.tmp'
    stage.write_text(str(docs/name), encoding='utf-8'); stage.replace(request)
    cad.user32.keybd_event(0x10, 0, 0, 0); time.sleep(0.025)
    cad.user32.keybd_event(0x10, 0, 2, 0)
    deadline = time.perf_counter()+5
    while request.exists() and time.perf_counter() < deadline: time.sleep(0.1)
    assert not request.exists()
    time.sleep(0.6)

origin = ctypes.wintypes.POINT() if hasattr(ctypes, 'wintypes') else None
def client_to_screen():
    pt = cad.wintypes.POINT(0, 0)
    cad.user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y

def owner_ok():
    o = ctypes.c_ulong(); cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(o))
    return proc.poll() is None and o.value == proc.pid and cad.user32.IsWindow(hwnd) and cad.user32.GetForegroundWindow() == hwnd

def glide(x, y, dur=0.7):
    assert owner_ok(), 'foreground lost'
    ox, oy = client_to_screen()
    sx, sy = ox + x, oy + y
    pt = cad.wintypes.POINT(); cad.user32.GetCursorPos(ctypes.byref(pt))
    steps = max(2, int(dur * 72))
    for i in range(1, steps + 1):
        t = i / steps
        e = 3*t*t - 2*t*t*t
        cad.user32.SetCursorPos(int(pt.x + (sx - pt.x) * e), int(pt.y + (sy - pt.y) * e))
        time.sleep(dur / steps)
    time.sleep(0.05)

def click(x, y, dur=0.7):
    glide(x, y, dur)
    assert owner_ok(), 'foreground lost before click'
    cad.user32.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.03)
    cad.user32.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.15)

def shot(name):
    assert owner_ok()
    w, h, raw = cad.capture_client(hwnd); cad.write_png(str(run/f'mouse-{name}.png'), w, h, raw)

shot('0-start')
click(237, 74); time.sleep(0.8); shot('1-outline')
click(90, 74); time.sleep(0.8); shot('2-files-back')
click(103, 268); time.sleep(1.0); shot('3-vault-open')
click(630, 24, 0.6); time.sleep(0.8); shot('4-tab2')
click(900, 600, 0.6)
for _ in range(6):
    assert owner_ok()
    cad.user32.mouse_event(0x0800, 0, 0, -120, 0); time.sleep(0.09)
time.sleep(0.6); shot('5-wheel-down')
for _ in range(6):
    assert owner_ok()
    cad.user32.mouse_event(0x0800, 0, 0, 120, 0); time.sleep(0.09)
time.sleep(0.5)
cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
try: proc.wait(timeout=8)
except subprocess.TimeoutExpired: proc.kill(); print('killed')
print('demo_exit', proc.poll())

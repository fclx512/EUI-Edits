import ctypes, hashlib, json, os, subprocess, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO/'tests/probes'))
import win_capture as cad
from capture_markdown import window_for_pid
cad.make_dpi_aware()
cad.assert_unlocked('recording smoke prepare')
cad.assert_no_foreign_instance('recording smoke prepare')
OUT = Path(os.environ.get('NEO_RECORDING_OUT', str(REPO/'build-promo-recording'))).resolve()
OUT.mkdir(parents=True, exist_ok=True)
run = OUT / ('run-' + time.strftime('%Y%m%d-%H%M%S'))
run.mkdir()
profile = run/'profile'; temp = run/'temp'
(profile/'EUI-Edits').mkdir(parents=True); temp.mkdir()
(profile/'EUI-Edits/settings.ini').write_text(
    'ui_language=zh-CN\nui_font_size=14\neditor_font_size=18\nui_scale=1\ntheme=1\n'
    'animations=1\nmode=1\nreadable_width=0\nline_numbers=0\n', encoding='utf-8')
doc = run/'EUI-Edits-demo.md'
doc.write_text('# EUI-Edits\n\n轻量文本编辑器，常用 Markdown。\n\n'
    '## 从文字开始\n\n保留真实操作，看见编辑过程。\n\n'
    '- [x] 标题与列表\n- [x] 引用与代码\n- [ ] 写下下一段想法\n\n'
    '> 专注文字，让界面保持简单。\n\n'
    '```cpp\nint main() {\n    return 0;\n}\n```\n\n'
    '## 清晰展示\n\n| 操作 | 效果 |\n| --- | --- |\n| 打开文稿 | 阅读与编辑 |\n| 滚动正文 | 连续浏览 |\n\n'
    + '\n\n'.join(f'### 写作片段 {i}\n\n真实窗口，脚本控制操作节奏。' for i in range(1,12))+'\n', encoding='utf-8')
exe = Path(os.environ.get('NEO_RECORDING_EXE', str(REPO/'build/Release/neo_editor.exe'))).resolve()
assert exe.is_file(), 'demo executable missing'
env = dict(os.environ, APPDATA=str(profile), TEMP=str(temp), TMP=str(temp),
           NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='0')
log = (run/'euiedits.log').open('w', encoding='utf-8')
proc = subprocess.Popen([str(exe),str(doc)], cwd=exe.parent, env=env, stdout=log, stderr=log)
hwnd = None
for _ in range(100):
    hwnd = window_for_pid(proc.pid)
    if hwnd or proc.poll() is not None: break
    time.sleep(.1)
assert hwnd and proc.poll() is None, 'demo window missing'
cad.user32.SetWindowPos(hwnd,None,100,100,1440,1080,4)
assert cad.ensure_foreground(hwnd), 'demo foreground unavailable'
time.sleep(1)
w,h,raw=cad.capture_client(hwnd)
cad.write_png(str(run/'prepared.png'),w,h,raw)
rect=cad.wintypes.RECT();cad.user32.GetWindowRect(hwnd,ctypes.byref(rect))
origin=cad.wintypes.POINT(0,0);cad.user32.ClientToScreen(hwnd,ctypes.byref(origin))
info=dict(run=str(run),exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
          pid=proc.pid,hwnd=hwnd,client_origin=[origin.x,origin.y],
          region=[rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top])
(OUT/'active.json').write_text(json.dumps(info,indent=2),encoding='utf-8')
print(json.dumps(info))

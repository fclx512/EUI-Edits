import ctypes, json, os, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'tests/probes'))
import win_capture as cad
from capture_markdown import window_for_pid

exe = ROOT/'out/latest/EUI-Edits-0.1.0-windows-x64.exe'
run = ROOT/'build-promo-probe-layout'
assert not run.exists()
profile = run/'profile/EUI-Edits'; profile.mkdir(parents=True)
temp = run/'temp'; temp.mkdir(); docs = run/'demo'; docs.mkdir()
first = docs/'实时写作.md'; first.write_text('', encoding='utf-8')
(docs/'今日计划.md').write_text('# 今日计划\n\n让文字工作更简单。\n\n- [x] 整理想法\n- [x] 写下一段 Markdown\n- [ ] 继续下一项工作\n\n> 在几个文稿间自由切换。\n', encoding='utf-8')
(docs/'代码笔记.md').write_text('# 代码笔记\n\n把说明和代码放在一起。\n\n```cpp\nint main() {\n    return 0;\n}\n```\n\n## 常用操作\n\n| 操作 | 用途 |\n| --- | --- |\n| 标签切换 | 继续写作 |\n| 查找 | 定位文字 |\n', encoding='utf-8')
(profile/'settings.ini').write_text('ui_language=zh-CN\nui_font_size=14\neditor_font_size=22\nui_scale=1\ntheme=1\nanimations=1\nmode=1\nreadable_width=0\nline_numbers=0\n', encoding='utf-8')
env = dict(os.environ, APPDATA=str(profile.parent), TEMP=str(temp), TMP=str(temp), NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1', NEO_LIVE_RESIZE='0')
cad.make_dpi_aware(); cad.assert_unlocked('layout probe')
log = (run/'app.log').open('w', encoding='utf-8')
proc = subprocess.Popen([str(exe), str(first)], cwd=str(exe.parent), env=env, stdout=log, stderr=log)
hwnd = None
for _ in range(120):
    hwnd = window_for_pid(proc.pid)
    if hwnd or proc.poll() is not None: break
    time.sleep(0.1)
assert hwnd and proc.poll() is None, 'window not found'
cad.user32.SetWindowPos(hwnd, None, 280, 140, 2000, 1250, 4)
assert cad.ensure_foreground(hwnd)
time.sleep(1.5)
w, h, raw = cad.capture_client(hwnd); cad.write_png(str(run/'probe-1tab.png'), w, h, raw)
print('client', w, h)

def open_doc(path):
    request = temp/'EUI-Edits.next-open'; stage = temp/'request.tmp'
    stage.write_text(str(path), encoding='utf-8'); stage.replace(request)
    cad.user32.keybd_event(0x10, 0, 0, 0); time.sleep(0.025)
    cad.user32.keybd_event(0x10, 0, 2, 0)
    deadline = time.perf_counter()+5
    while request.exists() and time.perf_counter() < deadline: time.sleep(0.1)
    assert not request.exists(), 'open not consumed'
    time.sleep(0.6)

open_doc(docs/'今日计划.md'); open_doc(docs/'代码笔记.md')
w, h, raw = cad.capture_client(hwnd); cad.write_png(str(run/'probe-3tabs.png'), w, h, raw)
rect = cad.wintypes.RECT(); cad.user32.GetWindowRect(hwnd, ctypes.byref(rect))
print('rect', rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top)
saved = {'pid': proc.pid, 'hwnd': hwnd, 'client': [w, h], 'region': [rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top]}
(run/'probe.json').write_text(json.dumps(saved), encoding='utf-8')
print(json.dumps(saved))
cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
try: proc.wait(timeout=8)
except subprocess.TimeoutExpired:
    proc.kill(); print('killed')
print('demo_exit', proc.poll())

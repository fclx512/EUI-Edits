"""Owned-window appearance regression, with private APPDATA and normal exit.

--switch-system briefly changes AppsUseLightTheme and notifies only the owned
window. The original value/type is restored in finally; no global broadcast.
"""
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import time
import winreg
from pathlib import Path
from PIL import Image, ImageChops

import win_capture as cad
from capture_markdown import window_for_pid

cad.user32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
cad.user32.SendMessageW.restype = ctypes.c_ssize_t

PERSONALIZE = r'Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--switch-system', action='store_true')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    profile, temp = out/'profile', out/'temp'
    config = profile/'EUI-Edits'
    config.mkdir(parents=True)
    temp.mkdir()
    doc = out/'appearance.md'
    source = '# 系统外观回归\n\n亮色、暗色、菜单和设置同步。\n\n> 引用内容\n\n- [ ] 检查项\n'
    doc.write_text(source, encoding='utf-8')
    settings = config/'settings.ini'
    settings.write_text('theme=2\nui_language=zh-CN\nui_font_size=14\nanimations=0\nmode=1\n', encoding='utf-8')
    env = dict(os.environ, APPDATA=str(profile), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
    cad.make_dpi_aware()
    cad.assert_unlocked('system appearance')
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  checks=[], exits=[], registry_restored=False, system_switch=args.switch_system)
    process, hwnd = None, None
    unit = 1.0
    original = None
    changed = False
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, PERSONALIZE, 0,
                        winreg.KEY_READ | (winreg.KEY_SET_VALUE if args.switch_system else 0)) as registry:
        try:
            original = winreg.QueryValueEx(registry, 'AppsUseLightTheme')
        except FileNotFoundError:
            pass

        def check(name, value):
            result['checks'].append(dict(name=name, passed=bool(value)))
            assert value, name

        def owned():
            cad.assert_unlocked('system appearance')
            pid = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            assert process.poll() is None and pid.value == process.pid and cad.ensure_foreground(hwnd)

        def capture(label):
            owned()
            w, h, raw = cad.capture_client(hwnd)
            cad.write_png(str(out/f'{label}.png'), w, h, raw)
            return Image.frombytes('RGBA', (w,h), raw, 'raw', 'BGRA').convert('RGB')

        def background(label, light):
            time.sleep(.4)
            image = capture(label)
            pixel = image.getpixel((round(750*unit), round(420*unit)))
            check(label+' editor background', (min(pixel)>220) if light else (max(pixel)<80))
            return image

        def key(*keys):
            owned()
            for k in keys: cad.user32.keybd_event(k, 0, 0, 0)
            for k in reversed(keys): cad.user32.keybd_event(k, 0, 2, 0)
            time.sleep(.22)

        def move(x,y):
            owned()
            p = cad.wintypes.POINT(round(x*unit),round(y*unit))
            cad.user32.ClientToScreen(hwnd,ctypes.byref(p))
            cad.user32.SetCursorPos(p.x,p.y)
            time.sleep(.3)

        def click(x,y):
            move(x,y)
            cad.click(hwnd,round(x*unit),round(y*unit))
            time.sleep(.3)

        def preference():
            time.sleep(.3)
            for line in settings.read_text(encoding='utf-8').splitlines():
                if line.startswith('theme='): return int(line.split('=',1)[1])

        def switch(light):
            nonlocal changed
            owned()
            winreg.SetValueEx(registry,'AppsUseLightTheme',0,winreg.REG_DWORD,int(light))
            changed = True
            setting = ctypes.create_unicode_buffer('ImmersiveColorSet')
            cad.user32.SendMessageW(hwnd,0x1A,0,ctypes.cast(setting,ctypes.c_void_p).value)
            # No keyboard/mouse/resize between notification and pixel check.
            time.sleep(.35)

        def appearance_menu(label):
            click(134,17)
            root = capture(label+'-root')
            move(155,232)
            child = capture(label)
            # Locate submenu from its newly drawn area to the right of root.
            region = (round(335*unit),round(180*unit),round(620*unit),round(380*unit))
            bbox = ImageChops.difference(root.crop(region),child.crop(region)).getbbox()
            check(label+' submenu visible', bbox is not None)
            x = (region[0]+bbox[0])/unit+50
            # Child starts aligned to the appearance parent row (y=216 DIP).
            return x, 216+6+16

        def select_menu(index, expected, label):
            x,y=appearance_menu(label)
            click(x,y+32*index)
            check(label+' preference stored',preference()==expected)

        def start():
            nonlocal process, hwnd, unit
            process=subprocess.Popen([str(exe),str(doc)],cwd=exe.parent,env=env)
            hwnd=None
            for _ in range(100):
                hwnd=window_for_pid(process.pid)
                if hwnd or process.poll() is not None: break
                time.sleep(.1)
            check('owned window starts',bool(hwnd) and process.poll() is None)
            owned()
            unit=cad.user32.GetDpiForWindow(hwnd)/96
            result['effective_scale']=unit
            cad.user32.SetWindowPos(hwnd,None,40,40,round(1100*unit),round(760*unit),4)
            time.sleep(.65)
            move(850,450)

        def close():
            owned()
            cad.user32.PostMessageW(hwnd,0x10,0,0)
            code=process.wait(timeout=10)
            result['exits'].append(code)
            check('normal exit',code==0)

        try:
            start()
            current_light = True if original is None else bool(original[0])
            background('startup-follow',current_light)
            appearance_menu('menu-follow-startup')
            key(0x1B)
            if args.switch_system:
                for i,light in enumerate([False,True,False,True]):
                    switch(light)
                    background(f'follow-switch-{i}',light)
                    check(f'follow-switch-{i} retains automatic preference',preference()==2)
                select_menu(1,0,'menu-fixed-dark')
                switch(True)
                background('fixed-dark-ignores-system-light',False)
                select_menu(2,1,'menu-fixed-light')
                switch(False)
                background('fixed-light-ignores-system-dark',True)
                select_menu(0,2,'menu-follow-restored')
                background('follow-restored-dark',False)
            key(0x11,0xBC)
            capture('settings-follow-wide')
            if args.switch_system:
                w,_,_=cad.capture_client(hwnd)
                click(w/unit-81,274)
                check('settings selects fixed light',preference()==1)
                key(0x1B)
                background('settings-fixed-light-editor',True)
                appearance_menu('menu-after-settings-light')
                key(0x1B)
                key(0x11,0xBC)
                click(w/unit-231,274)
                check('settings restores follow',preference()==2)
                switch(True)
                background('settings-live-follow-light',True)
                switch(False)
                background('settings-live-follow-dark',False)
                check('settings live changes retain follow',preference()==2)
                capture('settings-follow-wide-final')
            cad.user32.SetWindowPos(hwnd,None,40,40,950,720,4)
            time.sleep(.3)
            capture('settings-follow-narrow')
            key(0x1B)
            check('document bytes preserved',doc.read_text(encoding='utf-8')==source)
            close()
            check('follow preference survives close',preference()==2)
            start()
            background('restart-follow',False if args.switch_system else current_light)
            close()
        finally:
            if changed:
                if original is None:
                    winreg.DeleteValue(registry,'AppsUseLightTheme')
                else:
                    winreg.SetValueEx(registry,'AppsUseLightTheme',0,original[1],original[0])
            try:
                restored=winreg.QueryValueEx(registry,'AppsUseLightTheme')
            except FileNotFoundError:
                restored=None
            result['registry_restored']=restored==original
            if process and process.poll() is None:
                if hwnd: cad.user32.PostMessageW(hwnd,0x10,0,0)
                result['exits'].append(process.wait(timeout=10))
            (out/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(dict(checks=len(result['checks']),exits=result['exits'],registry_restored=result['registry_restored'])))


if __name__=='__main__': main()

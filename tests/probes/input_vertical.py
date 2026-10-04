"""Owned-window captures of single-line ink, caret and selection; no document edits."""
import argparse, ctypes, hashlib, json, os, subprocess, time
from pathlib import Path
from PIL import Image
import win_capture as cad
from capture_markdown import window_for_pid
from text_files import registry_snapshot


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--exe',required=True); ap.add_argument('--out',required=True)
    ap.add_argument('--scale',type=float,default=1); ap.add_argument('--font',type=int,default=18)
    ap.add_argument('--theme',type=int,default=1)
    args=ap.parse_args(); exe=Path(args.exe).resolve(); out=Path(args.out).resolve()
    out.mkdir(parents=True,exist_ok=False)
    cad.make_dpi_aware(); cad.assert_unlocked('input vertical'); cad.assert_no_foreign_instance('input vertical')
    before=registry_snapshot(); doc=out/'文稿.md'; original='# 标题\n\n正文保持原样。\n'; doc.write_text(original,encoding='utf-8')
    settings=out/'appdata/EUI-Edits/settings.ini'; settings.parent.mkdir(parents=True)
    settings.write_text(f'last_file={doc}\nvault={out}\nmode=1\nui_scale={args.scale}\nui_font_size={args.font}\n'
                        f'theme={args.theme}\nanimations=0\nui_language=zh-CN\n',encoding='utf-8')
    env=dict(os.environ,APPDATA=str(settings.parent.parent),NEO_SINGLE_INSTANCE='0',NEO_D2D_SOFTWARE='1',NEO_WIN32_DC='1')
    report=dict(exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),ui_scale=args.scale,font=args.font,theme=args.theme,captures=[])
    p=subprocess.Popen([str(exe)],cwd=exe.parent,env=env); hwnd=None
    def owned():
        cad.assert_unlocked('input vertical'); pid=ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
        assert p.poll() is None and pid.value==p.pid and cad.ensure_foreground(hwnd)
    def key(*keys):
        owned()
        for k in keys: cad.user32.keybd_event(k,0,0,0);time.sleep(.03)
        for k in reversed(keys):cad.user32.keybd_event(k,0,2,0);time.sleep(.03)
        time.sleep(.3)
    def click(x,y):
        owned(); pt=cad.wintypes.POINT(round(x*unit),round(y*unit));cad.user32.ClientToScreen(hwnd,ctypes.byref(pt))
        cad.user32.WindowFromPoint.argtypes=[cad.wintypes.POINT];cad.user32.WindowFromPoint.restype=ctypes.c_void_p
        cad.user32.GetAncestor.argtypes=[ctypes.c_void_p,ctypes.c_uint];cad.user32.GetAncestor.restype=ctypes.c_void_p
        assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(pt),2)==hwnd,'point obscured'
        cad.click(hwnd,round(x*unit),round(y*unit));time.sleep(.35)
    def capture(name,rect=None):
        owned();time.sleep(.3);w,h,raw=cad.capture_client(hwnd);cad.write_png(str(out/(name+'.png')),w,h,raw)
        if rect:
            physical=tuple(round(v*unit) for v in rect)
            image=Image.frombytes('RGBA',(w,h),raw,'raw','BGRA').convert('RGB')
            image.crop(physical).save(out/(name+'-field.png'))
            report['captures'].append(dict(name=name,field_pixels=physical))
        else:report['captures'].append(dict(name=name))
    try:
        for _ in range(150):
            hwnd=window_for_pid(p.pid)
            if hwnd:break
            time.sleep(.1)
        assert hwnd
        owned();cad.user32.SetWindowPos(hwnd,None,25,25,1600,1000,4);time.sleep(.8)
        unit=cad.user32.GetDpiForWindow(hwnd)/96*args.scale;report['effective_scale']=unit
        capture('vault-address-filter')
        key(0x11,0x48);capture('find-replace-placeholder')
        key(0x1B); key(0x11,0xBC)
        w,h,_=cad.capture_client(hwnd); logical=w/unit;assert logical>=840,'wide geometry required'
        click(85,208)
        ui=args.font;header=max(72,ui*2+36);section=round(ui*1.55)+26
        page_width=min(952,logical-176);page_x=176+(logical-176-page_width)/2;content=page_width-52
        control=min(content-36,max(224,ui*16));stacked=content<control+ui*21+60
        row=14+(ui+4)+4+(max(11,ui-1)+5)*2+6+(14+34 if stacked else 0)+12
        x=page_x+26+18;y=header+24+section+row+164
        rect=(x,y,x+content-36,y+32)
        capture('association-placeholder',rect)
        click(x+80,y+16);key(0x10)
        for ch in '中文 Hg ÅÉgj':cad.user32.PostMessageW(hwnd,0x0102,ord(ch),1)
        time.sleep(.5);capture('association-text-caret',rect)
        key(0x11,0x41);capture('association-selection',rect)
        key(0x1B);capture('return-editor')
        assert doc.read_text(encoding='utf-8')==original
        cad.user32.PostMessageW(hwnd,0x10,0,0);p.wait(timeout=10);assert p.returncode==0
        report['normal_exit']=True;report['document_unchanged']=True
        report['registry_unchanged']=registry_snapshot()==before;assert report['registry_unchanged']
    finally:
        if hwnd and p.poll() is None:cad.user32.PostMessageW(hwnd,0x10,0,0);p.wait(timeout=10)
        (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))


if __name__=='__main__':main()

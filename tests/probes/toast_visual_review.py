"""Owned-window rendering of the production ToastBuilder with the reported message."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import win_capture as cad
from capture_markdown import window_for_pid

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--exe',required=True)
    parser.add_argument('--out',required=True)
    parser.add_argument('--scale',type=float,default=1)
    parser.add_argument('--font',type=int,default=14)
    parser.add_argument('--dark',action='store_true')
    args=parser.parse_args()
    exe,out=Path(args.exe).resolve(),Path(args.out).resolve()
    out.mkdir(parents=True,exist_ok=True)
    cad.make_dpi_aware();cad.assert_unlocked('Notification review');cad.assert_no_foreign_instance('Notification review')
    process=subprocess.Popen([str(exe),'--frames','-1','--scale',str(args.scale),'--font',str(args.font)]+(['--dark'] if args.dark else []),
        cwd=exe.parent,env=dict(os.environ,NEO_D2D_SOFTWARE='1',NEO_WIN32_DC='1'))
    hwnd=None
    def owned():
        cad.assert_unlocked('Notification review')
        pid=ctypes.c_ulong();cad.user32.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
        assert process.poll() is None and pid.value==process.pid and cad.ensure_foreground(hwnd)
    def capture(name):
        owned();time.sleep(.5)
        w,h,pixels=cad.capture_client(hwnd);cad.write_png(str(out/(name+'.png')),w,h,pixels)
        return w,h,pixels
    try:
        for _ in range(100):
            hwnd=window_for_pid(process.pid)
            if hwnd or process.poll() is not None:break
            time.sleep(.1)
        assert hwnd;owned()
        cad.user32.SetWindowPos(hwnd,None,0,0,1000,650,0x0002|0x0004)
        capture('notification-wide')
        cad.user32.SetWindowPos(hwnd,None,0,0,650,460,0x0002|0x0004)
        w,h,before=capture('notification-narrow')
    finally:
        if hwnd and process.poll() is None:cad.user32.PostMessageW(hwnd,0x0010,0,0)
        process.wait(timeout=10)
        (out/'results.json').write_text(json.dumps(dict(exe=str(exe),sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
            scale=args.scale,font=args.font,dark=args.dark,exit_code=process.returncode,fixture=True),indent=2),encoding='utf-8')

if __name__=='__main__':main()

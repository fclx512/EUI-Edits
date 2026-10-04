"""CPU cost of single click interactions (menu open/close cycles)."""
import argparse
import ctypes
import json
import os
import subprocess
import tempfile
import time
from ctypes import wintypes

import win_capture as cad
from capture_markdown import window_for_pid

DOC = "# 点击开销\n\n正文。\n" + "".join("\n\n段落 {}。\n".format(i) for i in range(10))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--theme', type=int, default=1)
    ap.add_argument('--animations', type=int, default=1, choices=(0, 1))
    args = ap.parse_args()
    exe, out = __import__('pathlib').Path(args.exe).resolve(), __import__('pathlib').Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('click cpu')
    os.environ.setdefault('NEO_PROBE_ALLOW_FOREIGN', '1')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    with tempfile.TemporaryDirectory(prefix='neo-click-') as temp:
        doc = __import__('pathlib').Path(temp) / 'click.md'
        doc.write_text(DOC, encoding='utf-8')
        values = {"vault": "", "last_file": str(doc), "mode": "1", "line_numbers": "1",
                  "readable_width": "1", "show_status_bar": "1", "ui_scale": "1",
                  "theme": str(args.theme), "animations": str(args.animations)}
        settings = __import__('pathlib').Path(temp) / 'EUI-Edits/settings.ini'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text("".join(f"{k}={v}\n" for k, v in values.items()), encoding='utf-8')
        env = dict(os.environ, APPDATA=temp, NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1',
                   NEO_LIVE_RESIZE='1', NEO_SINGLE_INSTANCE='0')
        with (out / 'renderer.log').open('w', encoding='utf-8') as log:
            proc = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stderr=log)
            hwnd = None
            for _ in range(150):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            assert hwnd
            cad.ensure_foreground(hwnd)
            cad.user32.SetWindowPos(hwnd, None, 80, 50, 1120, 820, 0x0004)
            time.sleep(2.0)
            handle = kernel.OpenProcess(0x1000, False, proc.pid)
            unit = cad.user32.GetDpiForWindow(hwnd) / 96

            def move(x, y):
                p = wintypes.POINT(round(x * unit), round(y * unit))
                cad.user32.ClientToScreen(hwnd, ctypes.byref(p))
                cad.user32.SetCursorPos(p.x, p.y)

            def cpu():
                times = [wintypes.FILETIME() for _ in range(4)]
                assert kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times))
                return sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in times[2:]) / 1e7

            def measure(label, action):
                start, before = time.perf_counter(), cpu()
                action()
                elapsed = time.perf_counter() - start
                seconds = cpu() - before
                r = dict(phase=label, wall_seconds=elapsed, cpu_seconds=seconds,
                         one_core_percent=100 * seconds / elapsed)
                print(label, round(seconds, 4), 'CPU s /', round(elapsed, 2), 's =',
                      round(r['one_core_percent'], 1), '% one core')
                return r

            result = dict(exe=str(exe), animations=args.animations, samples=[])
            try:
                # 静止 3 秒
                result['samples'].append(measure('idle', lambda: time.sleep(3)))
                # 连续点击 文件 菜单开/关 10 次（每次点击 = 按压+释放+弹层重建）
                def click_menu():
                    for _ in range(10):
                        move(24, 17)
                        time.sleep(.12)
                        cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
                        time.sleep(.06)
                        cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                        time.sleep(.32)
                result['samples'].append(measure('menu_open_close_x10', click_menu))
                # 连续悬停扫过侧栏文件行 20 行
                def sweep_vault():
                    for _ in range(3):
                        for y in (150, 180, 210, 240, 270, 300):
                            move(130, y)
                            time.sleep(.08)
                result['samples'].append(measure('vault_rows_sweep', sweep_vault))
                result['samples'].append(measure('idle_after', lambda: time.sleep(3)))
            finally:
                cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                for _ in range(2):
                    cad.user32.keybd_event(0x1b, 0, 0, 0)
                    cad.user32.keybd_event(0x1b, 0, 2, 0)
                    time.sleep(.1)
                if proc.poll() is None:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.terminate()
            result['exit_code'] = proc.poll()
            (out / 'conditions.json').write_text(json.dumps(result, ensure_ascii=False, indent=2),
                                                 encoding='utf-8')
            print('exit', result['exit_code'])


if __name__ == '__main__':
    main()

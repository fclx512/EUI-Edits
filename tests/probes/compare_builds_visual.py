"""临时诊断：同一份 Markdown 标本下对比多个 neo_editor.exe 的首屏渲染。

隔离 APPDATA，不碰用户设置；截图写入临时目录后由调用方查看。
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import win_capture as cad
from win_capture import assert_unlocked, capture_client, ensure_foreground

REPO = r"D:\ruanjian\NeoEditor"
DOC = os.path.join(REPO, "tests", "assets", "markdown", "live_preview_coverage.md")

EXES = [
    ("gpu-resize-verify-0927", r"D:\ruanjian\NeoEditor-gpu-resize-verify-20260927\Release\neo_editor.exe"),
    ("gpu-safe-0928", os.path.join(REPO, "build", "gpu-safe-20260928", "Release", "neo_editor.exe")),
    ("main-build-0928", os.path.join(REPO, "build", "Release", "neo_editor.exe")),
]

OUT = os.path.join(tempfile.gettempdir(), "neo-style-compare")


def window_for_pid(pid):
    import ctypes
    from ctypes import wintypes
    matches = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def callback(hwnd, unused):
        owner = wintypes.DWORD()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and cad.user32.IsWindowVisible(hwnd):
            matches.append(hwnd)
        return True

    cad.user32.EnumWindows(callback, 0)
    return matches[0] if len(matches) == 1 else None


def main():
    os.makedirs(OUT, exist_ok=True)
    cad.make_dpi_aware()
    assert_unlocked("style compare")
    previous_foreground = cad.user32.GetForegroundWindow()

    for label, exe in EXES:
        if not os.path.isfile(exe):
            print(f"{label}: exe missing, skipped")
            continue
        runtime = tempfile.mkdtemp(prefix=f"neo-{label}-")
        settings_dir = os.path.join(runtime, "EUI-Edits")
        os.makedirs(settings_dir, exist_ok=True)
        settings = os.path.join(settings_dir, "settings.ini")
        with open(settings, "w", encoding="utf-8", newline="\n") as target:
            target.write(f"vault={os.path.dirname(DOC)}\n")
            target.write(f"last_file={DOC}\n")
            target.write("mode=0\nline_numbers=0\nreadable_width=0\nshow_status_bar=0\n")
            target.write("editor_font_size=16\neditor_font_file=\nui_scale=1\ntheme=0\n")
        env = os.environ.copy()
        env["APPDATA"] = runtime
        process = None
        try:
            process = __import__("subprocess").Popen([exe], cwd=os.path.dirname(exe), env=env)
            hwnd = None
            for _ in range(100):
                time.sleep(0.1)
                hwnd = window_for_pid(process.pid)
                if hwnd:
                    break
            if not hwnd:
                print(f"{label}: window did not appear")
                continue
            time.sleep(2.0)
            if not ensure_foreground(hwnd):
                print(f"{label}: not foreground; capture may be stale")
            time.sleep(0.8)
            width, height, pixels = capture_client(hwnd)
            cad.write_png(os.path.join(OUT, f"{label}.png"), width, height, pixels)
            print(f"{label}: captured -> {os.path.join(OUT, label + '.png')}")
        except Exception as error:
            print(f"{label}: FAILED {error}")
        finally:
            if process is not None and hwnd is not None:
                try:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                    process.wait(timeout=5)
                except Exception:
                    process.kill()
                    process.wait()
            elif process is not None:
                process.kill()
                process.wait()
    if previous_foreground:
        cad.user32.SetForegroundWindow(previous_foreground)
    print("done")


if __name__ == "__main__":
    main()

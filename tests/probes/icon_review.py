"""Real-window review of the redrawn document icons, menu icons and checkmark.

Launches EUI-Edits against an isolated APPDATA + temp vault, captures the vault
file list and each top-bar menu in light and dark themes, and writes the PNGs to
the requested output directory.

    python tests/probes/icon_review.py --exe build-win32/visual/neo_editor.exe \
        --out build-win32/evidence/icon-review --scale 1.0
    python tests/probes/icon_review.py --exe build-win32/visual/neo_editor.exe \
        --out build-win32/evidence/icon-review --scale 1.5
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from PIL import Image
import win_capture as cad
from capture_markdown import window_for_pid, write_settings

# 与被测进程的 NEO_SINGLE_INSTANCE=0 配套：用户自己开着的 EUI-Edits 并存即可，
# 不抢互斥、不转发、不置前，探针只驱动自己启动的那一个窗口。
os.environ.setdefault("NEO_PROBE_ALLOW_FOREIGN", "1")

# 菜单栏标题：x 从 10 起、宽 = estimateTextWidth(label, ui) + 20、间隔 2。
# ui_font_size=14 时三个标题中心分别是 34 / 84 / 134，y = menuBarHeight/2 = 17。
TITLES = {"file": 34, "edit": 84, "view": 134}
TITLE_Y = 17

VAULT_FILES = {
    "笔记.md": "# 标题\n\n正文。\n",
    "readme.md": "# Readme\n",
    "说明.txt": "纯文本\n",
    "notes.txt": "text\n",
    "log.csv": "a,b\n",
    "源码.cpp": "int value = 42;\n",
    "配置.json": '{"enabled": true}\n',
    ".gitignore": "build/\n*.tmp\n",
    "LICENSE": "License plain text\n",
    "运行.bat": "@echo off\necho test\n",
}


def run_theme(exe, out, theme, tag, scratch, ui_scale):
    # 文档库与隔离 APPDATA 都落在临时目录里，证据目录只留 PNG。
    vault = scratch / f"vault-{tag}"
    vault.mkdir(parents=True, exist_ok=True)
    for name, body in VAULT_FILES.items():
        sample = vault / name
        sample.parent.mkdir(parents=True, exist_ok=True)
        sample.write_text(body, encoding="utf-8")
    runtime = scratch / f"runtime-{tag}"
    settings = runtime / "EUI-Edits/settings.ini"
    write_settings(str(settings), str(vault / "笔记.md"), theme, 16, str(ui_scale))
    with settings.open("a", encoding="utf-8") as target:
        target.write("mode=1\nshow_status_bar=1\nui_font_size=14\n")
    env = dict(os.environ, APPDATA=str(runtime), NEO_D2D_SOFTWARE="1",
               NEO_WIN32_DC="1", NEO_LIVE_RESIZE="1", NEO_SINGLE_INSTANCE="0")
    exe_hash_before = hashlib.sha256(exe.read_bytes()).hexdigest()
    process = subprocess.Popen([str(exe)], env=env, cwd=exe.parent)
    hwnd = None
    try:
        for _ in range(100):
            hwnd = window_for_pid(process.pid)
            if hwnd or process.poll() is not None:
                break
            time.sleep(0.1)
        assert hwnd, "window not found"
        assert cad.ensure_foreground(hwnd), "window not foreground"
        cad.user32.SetWindowPos(hwnd, None, 40, 40, 1400,
                               1300 if ui_scale == 1.5 else 900, 0x0004)
        time.sleep(1.0)
        unit = cad.user32.GetDpiForWindow(hwnd) / 96
        coord_scale = unit * ui_scale
        logical_width = 1400 / coord_scale

        def owned():
            cad.assert_unlocked("icon review")
            owner = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            assert process.poll() is None and owner.value == process.pid
            assert cad.ensure_foreground(hwnd), "lost foreground"

        def move(x, y):
            owned()
            point = cad.wintypes.POINT(round(x * coord_scale), round(y * coord_scale))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
            cad.user32.SetCursorPos(point.x, point.y)

        def click(x, y):
            owned()
            point = cad.wintypes.POINT(round(x * coord_scale), round(y * coord_scale))
            cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
            cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
            cad.user32.WindowFromPoint.restype = ctypes.c_void_p
            cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            cad.user32.GetAncestor.restype = ctypes.c_void_p
            assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(point), 2) == hwnd, "click point obscured"
            cad.click(hwnd, round(x * coord_scale), round(y * coord_scale))

        def escape():
            owned()
            cad.user32.keybd_event(0x1B, 0, 0, 0)
            cad.user32.keybd_event(0x1B, 0, 2, 0)
            time.sleep(0.4)

        def capture(name, delay=0.5):
            owned()
            time.sleep(delay)
            width, height, pixels = cad.capture_client(hwnd)
            path = out / f"{tag}-{name}.png"
            cad.write_png(str(path), width, height, pixels)
            print(f"wrote {path} ({width}x{height}) dpi_unit={unit} ui_scale={ui_scale}")
            return Image.open(path).convert("RGB")

        def probe_region(origin_x, origin_y=40):
            """Sample one pixel of the popup area; used to tell open from closed."""
            width, _, pixels = cad.capture_client(hwnd)
            x = round(origin_x * coord_scale)
            y = round(origin_y * coord_scale)
            offset = (y * width + x) * 4
            return tuple(pixels[offset:offset + 3])

        def open_menu(x):
            """点击菜单栏标题并确认弹层真的展开（点击偶尔会被启动期吃掉）。"""
            for attempt in range(6):
                move(logical_width - 40, 120)
                time.sleep(0.2)
                closed = probe_region(x + 40)
                click(x, TITLE_Y)
                time.sleep(0.6)
                if probe_region(x + 40) != closed:
                    return True
                print(f"  menu at x={x} did not open, retry {attempt + 1}")
            return False

        def zoom(name, source, box, scale=4):
            pixels = tuple(round(v * coord_scale) for v in box)
            region = source.crop(pixels)
            region = region.resize((region.width * scale, region.height * scale),
                                   Image.NEAREST)
            path = out / f"{tag}-{name}-zoom.png"
            region.save(path)
            print(f"wrote {path}")

        shot = capture("01-vault")
        zoom("02-vault-rows", shot, (8, 110, 335, 500))
        # 顶栏整条 + 右侧齿轮按钮放大。
        zoom("03-menubar", shot, (0, 0, logical_width, 40), 3)
        zoom("04-menu-button", shot, (logical_width - 120, 0, logical_width, 40), 8)

        for key, x in TITLES.items():
            assert open_menu(x), f"{key} menu never opened"
            shot = capture(f"10-menu-{key}")
            zoom(f"11-menu-{key}", shot, (x - 6, 30, x + 230, 310))
            if key == "view":
                # 悬停"外观"展开子菜单，顺带复核勾选粗细。
                move(x + 60, TITLE_Y + 32 + 16 + 32)
                shot = capture("12-menu-submenu")
                zoom("13-menu-submenu", shot, (x - 6, 30, x + 330, 300))
            escape()
    finally:
        hwnd = hwnd or window_for_pid(process.pid)
        owner = ctypes.c_ulong()
        if hwnd:
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        owned_hwnd = bool(hwnd and owner.value == process.pid)
        report = {
            "tag": tag,
            "pid": process.pid,
            "exe": str(exe),
            "sha256_before": exe_hash_before,
            "sha256_after": hashlib.sha256(exe.read_bytes()).hexdigest(),
            "hwnd": int(hwnd) if hwnd else None,
            "hwnd_owner_pid": owner.value if hwnd else None,
            "wm_close_posted": False,
            "ui_scale": ui_scale,
            "dpi": cad.user32.GetDpiForWindow(hwnd) if owned_hwnd else None,
            "exit_code": process.poll(),
        }
        if process.poll() is None and owned_hwnd:
            report["wm_close_posted"] = bool(cad.user32.PostMessageW(hwnd, 0x0010, 0, 0))
            if report["wm_close_posted"]:
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    report["cleanup"] = "WM_CLOSE timeout; process left running"
            else:
                report["cleanup"] = "WM_CLOSE failed; process left running"
        elif process.poll() is None:
            report["cleanup"] = "no verified owned window; process left running"
        report["exit_code"] = process.poll()
        if report["exit_code"] is None and "cleanup" not in report:
            report["cleanup"] = "process remained running after WM_CLOSE; left untouched"
        report_path = out / f"{tag}-process.json"
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
        print(f"process report: {report_path} {json.dumps(report, ensure_ascii=False)}")
        if report["sha256_before"] != report["sha256_after"]:
            raise RuntimeError(f"executable changed during {tag}; see {report_path}")
        if report["exit_code"] is None:
            raise RuntimeError(f"EUI-Edits PID {process.pid} did not exit; process left running")
        if report["exit_code"] != 0:
            raise RuntimeError(f"EUI-Edits PID {process.pid} exited with code {report['exit_code']}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--scale", type=float, choices=(1.0, 1.5), default=1.0,
                        help="UI scale: 1.0 (100%%) or 1.5 (150%%)")
    parser.add_argument("--complete-vault", action="store_true")
    args = parser.parse_args()
    if args.complete_vault:
        VAULT_FILES.update({".hidden/hidden.md": "# hidden\n", "build/generated.txt": "generated\n",
                            "node_modules/package.json": "{}\n", "unknown.custom": "unknown text\n"})
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked("icon review")
    cad.assert_no_foreign_instance("icon review")
    with tempfile.TemporaryDirectory(prefix="neo-icons-") as scratch:
        root = Path(scratch)
        for theme, tag in ((1, "light"), (0, "dark")):
            scaled_tag = tag if args.scale == 1.0 else f"{tag}-{int(args.scale * 100)}"
            run_theme(exe, out, theme, scaled_tag, root, args.scale)


if __name__ == "__main__":
    main()

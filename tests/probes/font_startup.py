"""Real owned-window startup checks under disposable APPDATA.

Only the explicitly owned test process is forcibly stopped, once, to exercise
the abnormal-exit font journal. No user process or user configuration is touched.
"""
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

import win_capture as cad
from capture_markdown import window_for_pid, write_settings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked("font recovery regression")
    cad.assert_no_foreign_instance("font recovery regression")
    checks = []
    with tempfile.TemporaryDirectory(prefix="neo-font-startup-") as temp:
        root = Path(temp)
        config = root / "EUI-Edits/settings.ini"
        marker = config.with_name("font-session.txt")
        doc = root / "font-review.md"
        doc.write_text("字体启动与恢复\n\n```cpp\nconst int i = 10;\n\nreturn i;\n```\n", encoding="utf-8")
        original = hashlib.sha256(doc.read_bytes()).hexdigest()
        fake = root / "损坏字体.ttf"
        fake.write_bytes(b"broken-font" * 10)
        mono = root / "代码字体.ttf"
        mono.write_bytes(Path("C:/Windows/Fonts/consola.ttf").read_bytes())
        env = dict(os.environ, APPDATA=str(root), NEO_D2D_SOFTWARE="1", NEO_WIN32_DC="1", NEO_LIVE_RESIZE="0")

        def configure(ui="", editor="", code=""):
            write_settings(str(config), str(doc), 1, 16, "1")
            with config.open("a", encoding="utf-8") as f:
                f.write(f"ui_font_file={ui}\neditor_font_file={editor}\ncode_font_file={code}\n")

        def read_config():
            return dict(line.split("=", 1) for line in config.read_text(encoding="utf-8").splitlines() if "=" in line)

        def launch(label, abnormal=False):
            proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
            hwnd = None
            try:
                for _ in range(200):
                    hwnd = window_for_pid(proc.pid)
                    if hwnd or proc.poll() is not None:
                        break
                    time.sleep(.05)
                assert hwnd and proc.poll() is None, f"{label}: startup failed {proc.returncode}"
                owner = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                assert owner.value == proc.pid and cad.ensure_foreground(hwnd)
                dpi = cad.user32.GetDpiForWindow(hwnd)
                time.sleep(1)
                w, h, px = cad.capture_client(hwnd)
                cad.write_png(str(out / f"{label}.png"), w, h, px)
                if abnormal:
                    assert marker.exists(), "Custom-font session was not armed"
                    # Deliberate crash-recovery fixture, only this owned PID.
                    proc.kill()
                    proc.wait(timeout=5)
                    assert marker.exists(), "Abnormal exit incorrectly cleared journal"
                else:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                    proc.wait(timeout=8)
                    assert proc.returncode == 0, f"{label}: orderly exit failed"
                checks.append(dict(name=label, passed=True, exit_code=proc.returncode,
                                   marker_after=marker.exists(), dpi=dpi))
            finally:
                if proc.poll() is None and hwnd:
                    cad.user32.PostMessageW(hwnd, 0x0010, 0, 0)
                    proc.wait(timeout=8)

        configure(str(fake), str(fake), str(fake))
        launch("01-invalid-fonts-start-safely")
        data = read_config()
        assert all(not data.get(key) for key in ("ui_font_file", "editor_font_file", "code_font_file"))
        assert not marker.exists()
        checks.append(dict(name="invalid-paths-cleared-on-disk", passed=True))

        configure(code=str(mono))
        launch("02-valid-unicode-monospace")
        assert read_config().get("code_font_file") == str(mono) and not marker.exists()
        checks.append(dict(name="normal-close-preserves-valid-font-and-clears-journal", passed=True))

        configure(ui=str(mono), code=str(mono))
        launch("03-deliberate-owned-abnormal-exit", abnormal=True)
        launch("04-restart-recovers-defaults")
        data = read_config()
        assert not data.get("ui_font_file") and not data.get("code_font_file") and not marker.exists()
        checks.append(dict(name="abnormal-restart-clears-matching-fonts", passed=True))

        configure(code="C:/Windows/Fonts/arial.ttf")
        launch("05-proportional-code-font-falls-back")
        assert not read_config().get("code_font_file") and not marker.exists()
        checks.append(dict(name="code-font-remains-monospace", passed=True))
        assert hashlib.sha256(doc.read_bytes()).hexdigest() == original
        checks.append(dict(name="test-document-preserved", passed=True))
    result = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), checks=checks)
    (out / "conditions.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

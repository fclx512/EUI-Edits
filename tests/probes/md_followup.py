"""Owned-window visual/interaction probe for Markdown follow-up review.

The script creates its Markdown vault and APPDATA under one unique temporary
directory. It never opens a user document, never attaches to a foreign window,
and closes only the process it started with WM_CLOSE. Screenshots and JSON
measurements go to --out. This is evidence collection; pixel heuristics are
reported as measurements and are not OCR or semantic rendering assertions.

Usage (run only when no EUI-Edits instance is open):
  python -B tests/probes/md_followup.py --exe build/Release/neo_editor.exe --out <dir>
"""
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import win_capture as cad
from capture_markdown import window_for_pid, write_settings

WM_CLOSE = 0x0010
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101


def sample_markdown():
    lines = [
        "Plain 中文 English 123 baseline",
        "",
        "```text",
        "code with text",
        "",
        "   ",
        "code after blank and whitespace-only lines",
        "```",
        "",
        "- first unordered item",
        "  - nested unordered item",
        "    - third level item",
        "",
        "[missing root link](missing-root.md)",
        "[missing nested link](sub/missing-child.md)",
        "",
        "---",
        "title: follow-up visual probe",
        "",
        "tags:",
        "  - markdown",
        "---",
        "",
        "## Long code block (scroll to inspect)",
        "```cpp",
    ]
    lines.extend(f"const int sample_{i:03d} = {i}; // long block viewport probe" for i in range(1, 151))
    lines.extend(["```", "", "Tail after the long code block."])
    return "\n".join(lines) + "\n"


def bgra_at(pixels, width, x, y):
    i = (int(y) * width + int(x)) * 4
    return tuple(pixels[i + j] for j in (2, 1, 0))


def crop_delta(a, b, width, height, rect, stride=3):
    """Fraction and average RGB difference in a bounded client-pixel rectangle."""
    x0, y0, x1, y1 = [int(v) for v in rect]
    x0, x1 = max(0, x0), min(width, x1)
    y0, y1 = max(0, y0), min(height, y1)
    changed = total = delta = 0
    for y in range(y0, y1, stride):
        for x in range(x0, x1, stride):
            ca, cb = bgra_at(a, width, x, y), bgra_at(b, width, x, y)
            d = sum(abs(ca[k] - cb[k]) for k in range(3))
            total += 1
            delta += d
            changed += d >= 24
    return {"sampled_pixels": total, "changed_fraction": changed / total if total else 0.0,
            "mean_rgb_delta": delta / (total * 3) if total else 0.0}


def analyze_code_blank_column(pixels, width, height, unit):
    """Find likely code-background bands, then test one inset-free x column.

    The selection is deliberately heuristic: among x columns in the editor's
    central area, choose the one with the fewest high-contrast pixels over the
    tall-code screenshot. Rounded block ends are excluded from continuity stats.
    """
    xlo, xhi = int(width * .48), int(width * .92)
    ylo, yhi = int(35 * unit), max(int(36 * unit), height - int(30 * unit))
    best_x, best_score = None, None
    for x in range(xlo, xhi, max(2, int(2 * unit))):
        colors = [bgra_at(pixels, width, x, y) for y in range(ylo, yhi, max(1, int(unit)))]
        # Text glyphs differ sharply from the prevailing background. A stable
        # column has few neighboring color jumps, including at its blank lines.
        jumps = sum(sum(abs(colors[i][k] - colors[i - 1][k]) for k in range(3)) > 100
                    for i in range(1, len(colors)))
        if best_score is None or jumps < best_score:
            best_x, best_score = x, jumps
    if best_x is None:
        return {"available": False}
    values = [bgra_at(pixels, width, best_x, y) for y in range(ylo, yhi)]
    # Longest exact-color run estimates continuity without assuming a theme RGB.
    longest = current = 0
    previous = None
    for c in values:
        current = current + 1 if c == previous else 1
        longest = max(longest, current)
        previous = c
    return {"available": True, "sample_x": best_x, "scan_y": [ylo, yhi],
            "longest_exact_rgb_run_px": longest, "run_fraction": longest / len(values),
            "note": "heuristic column selection; inspect code-block screenshot for placement"}


def connected_small_components(mask, width, height, max_size=8, max_area=48):
    """Count compact foreground components, for approximate list-marker evidence."""
    seen = bytearray(width * height)
    comps = []
    for start, value in enumerate(mask):
        if not value or seen[start]:
            continue
        stack = [start]
        seen[start] = 1
        xs, ys = [], []
        while stack:
            i = stack.pop()
            x, y = i % width, i // width
            xs.append(x); ys.append(y)
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if 0 <= nx < width and 0 <= ny < height:
                    ni = ny * width + nx
                    if mask[ni] and not seen[ni]:
                        seen[ni] = 1; stack.append(ni)
        w, h = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
        if 1 <= w <= max_size and 1 <= h <= max_size and 2 <= len(xs) <= max_area:
            comps.append({"x": min(xs), "y": min(ys), "width": w, "height": h,
                          "area": len(xs), "edge_distance": min(min(xs), min(ys), width - 1 - max(xs), height - 1 - max(ys))})
    return comps


def send_key(hwnd, key):
    cad.user32.PostMessageW(hwnd, WM_KEYDOWN, key, 0)
    cad.user32.PostMessageW(hwnd, WM_KEYUP, key, 0)
    time.sleep(.25)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(REPO / "build" / "Release" / "neo_editor.exe"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--theme", type=int, choices=(0, 1), default=1)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--width", type=int, default=1320, help="desired client width in physical pixels")
    ap.add_argument("--height", type=int, default=900, help="desired client height in physical pixels")
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not exe.is_file():
        raise SystemExit(f"EUI-Edits executable not found: {exe}")
    if args.scale <= 0:
        raise SystemExit("--scale must be greater than zero")

    cad.make_dpi_aware()
    cad.assert_unlocked("Markdown follow-up")
    cad.assert_no_foreign_instance("Markdown follow-up")
    results = {"exe": str(exe), "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
               "theme": args.theme, "ui_scale": args.scale, "captures": [], "measurements": {},
               "checks": [], "limitations": "Calibrated native input and pixel checks; captures do not measure animation frame pacing."}
    proc = hwnd = None

    with tempfile.TemporaryDirectory(prefix="neo-md-followup-") as runtime:
        runtime_path = Path(runtime)
        vault = runtime_path / "vault"
        (vault / "sub").mkdir(parents=True)
        doc = vault / "followup.md"
        doc.write_text(sample_markdown(), encoding="utf-8", newline="\n")
        (vault / "other.md").write_text("Other root note\n", encoding="utf-8")
        (vault / "sub" / "child.md").write_text("Child note\n", encoding="utf-8")
        write_settings(str(runtime_path / "EUI-Edits" / "settings.ini"), str(doc), args.theme, 16, str(args.scale))
        with open(runtime_path / "EUI-Edits" / "settings.ini", "a", encoding="utf-8") as cfg:
            cfg.write("mode=1\nshow_status_bar=0\nshow_line_numbers=0\n")
        env = os.environ.copy()
        env.update(APPDATA=runtime, NEO_LIVE_RESIZE="0", NEO_WIN32_DC="1", NEO_D2D_SOFTWARE="1")
        log_path = out / "neo_editor.log"
        log = open(log_path, "w", encoding="utf-8")
        try:
            proc = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stderr=log)

            def owned():
                cad.assert_unlocked("Markdown follow-up")
                if proc.poll() is not None:
                    raise RuntimeError(f"probe process exited early: {proc.returncode}")
                if not hwnd:
                    raise RuntimeError("probe HWND is unavailable")
                owner = ctypes.c_ulong()
                cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                if owner.value != proc.pid or not cad.ensure_foreground(hwnd):
                    raise RuntimeError("probe no longer owns the foreground window")

            def capture(label):
                owned()
                w, h, px = cad.capture_client(hwnd)
                cad.write_png(str(out / f"{label}.png"), w, h, px)
                results["captures"].append({"state": label, "width": w, "height": h})
                return w, h, px

            def move(x, y, settle=.25):
                owned()
                p = cad.wintypes.POINT(round(x), round(y))
                cad.user32.ClientToScreen(hwnd, ctypes.byref(p))
                cad.user32.SetCursorPos(p.x, p.y)
                time.sleep(settle)

            def click(x, y, right=False):
                owned()
                move(x, y, .10)
                cad.user32.mouse_event(0x0008 if right else 0x0002, 0, 0, 0, 0)
                time.sleep(.06)
                cad.user32.mouse_event(0x0010 if right else 0x0004, 0, 0, 0, 0)
                time.sleep(.35)

            def drag(x0, y0, x1, y1):
                owned(); move(x0, y0, .12)
                cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
                move(x1, y1, .25)
                cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
                time.sleep(.35)

            for _ in range(150):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            if not hwnd:
                raise RuntimeError("owned EUI-Edits window did not appear")
            owned()
            cad.user32.SetWindowPos(hwnd, None, 0, 0, args.width, args.height,
                                    0x0002 | 0x0004 | 0x0010)
            dpi = cad.user32.GetDpiForWindow(hwnd) / 96.0
            unit = dpi * args.scale
            results.update(pid=proc.pid, system_dpi=96 * dpi, effective_scale=unit,
                           vault=str(vault), sample_sha256=hashlib.sha256(doc.read_bytes()).hexdigest())
            time.sleep(2)
            width, height, initial = capture("01-first-page")

            # Editor coordinates use logical metrics; the vault panel is 38% of
            # window width (clamped to configured width). These anchors are the
            # only geometry assumptions; inspect 01-first-page.png before tuning.
            panel = min(264.0, width / unit * .38)
            editor_left = int((panel + 4) * unit)
            menu_h = 34.0  # default UI font 14 -> max(30, ui + 20)
            body_top = int(menu_h * unit)
            row_h = 24.0
            text_x = round((panel + 18) * unit)
            text_top = (menu_h + 5.2) * unit
            code_top = text_top + 2 * row_h * unit
            list_top = code_top + (6 * 21 + row_h) * unit
            first_link_y = round(list_top + (4 * row_h + 12) * unit)
            second_link_y = first_link_y + round(row_h * unit)
            column = round(width - 80 * unit)
            code_values = [bgra_at(initial, width, column, y) for y in range(
                round(code_top + 22 * unit), round(code_top + 5 * 21 * unit))]
            continuous = len(set(code_values)) == 1 and code_values[0] != bgra_at(initial, width, column, round(text_top))
            results["checks"].append({"name": "code_blank_and_space_rows_have_continuous_background", "passed": continuous})

            # Link hover: capture three settled frames; the color comparison is
            # restricted to the text neighborhood and does not decode glyphs.
            link_rect = (text_x, first_link_y - int(10 * unit), text_x + int(300 * unit), first_link_y + int(12 * unit))
            move(text_x + int(90 * unit), first_link_y, settle=0)
            time.sleep(0)
            hover0 = capture("02-link-hover-000ms")
            time.sleep(.05)
            hover50 = capture("03-link-hover-050ms")
            time.sleep(.10)
            hover150 = capture("04-link-hover-150ms")
            results["measurements"]["link_hover_0_vs_150ms"] = crop_delta(hover0[2], hover150[2], width, height, link_rect)
            hover_delta = crop_delta(initial, hover150[2], width, height, link_rect)
            results["checks"].append({"name": "link_hover_changes_its_visible_background", "passed": hover_delta["changed_fraction"] > .02})
            before_drag_hash = hashlib.sha256(doc.read_bytes()).hexdigest()
            drag(text_x + int(95 * unit), first_link_y, text_x + int(250 * unit), first_link_y + int(3 * row_h * unit))
            after_drag_hash = hashlib.sha256(doc.read_bytes()).hexdigest()
            results["checks"].append({"name": "link_press_drag_out_preserves_sample", "passed": before_drag_hash == after_drag_hash})
            capture("05-link-drag-cancel")

            # Right click near the editor's right edge to exercise menu clamping.
            edge_x = width - int(12 * unit)
            menu_y = body_top + int(4 * row_h * unit)
            click(edge_x, menu_y, right=True)
            menu0 = capture("06-editor-menu-edge")
            # Generic context menus place first item near the clamped anchor;
            # if this theme/window changes, screenshots remain the source of truth.
            item_h = 32 * unit
            menu_x = width - 220 * unit
            menu_top = min(menu_y, height - (348 + 8) * unit)
            hover_item = (menu_x + 80 * unit, menu_top + 22 * unit)
            disabled_item = (menu_x + 80 * unit, menu_top + 190 * unit)
            move(*hover_item, settle=0)
            menu_hover0 = capture("07-menu-hover-000ms")
            time.sleep(.05); menu_hover50 = capture("08-menu-hover-050ms")
            time.sleep(.10); menu_hover150 = capture("09-menu-hover-150ms")
            hover_box = (menu_x, max(0, menu_y), min(width, menu_x + int(230 * unit)), min(height, menu_y + int(8 * item_h)))
            results["measurements"]["menu_hover_0_vs_150ms"] = crop_delta(menu_hover0[2], menu_hover150[2], width, height, hover_box)
            hover_pixel = (round(menu_x + 194 * unit), round(menu_top + 22 * unit))
            results["checks"].append({"name": "menu_hover_has_visible_feedback", "passed":
                bgra_at(menu0[2], width, *hover_pixel) != bgra_at(menu_hover150[2], width, *hover_pixel)})
            cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(.15)
            pressed = capture("09b-menu-pressed")
            results["checks"].append({"name": "menu_press_has_visible_feedback", "passed":
                bgra_at(pressed[2], width, *hover_pixel) != bgra_at(menu_hover150[2], width, *hover_pixel)})
            move(editor_left + 20 * unit, body_top + 20 * unit, .10)
            cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
            original_hash = hashlib.sha256(doc.read_bytes()).hexdigest()
            click(*disabled_item)
            still_open = capture("10-menu-disabled-click")
            disabled_pixel = (round(menu_x + 194 * unit), round(menu_top + 190 * unit))
            results["checks"].append({"name": "disabled_cut_click_keeps_context_menu", "passed":
                bgra_at(menu0[2], width, *disabled_pixel) == bgra_at(still_open[2], width, *disabled_pixel) and
                crop_delta(menu0[2], still_open[2], width, height, hover_box)["changed_fraction"] < .10})
            results["checks"].append({"name": "disabled_cut_copy_click_preserves_sample", "passed": hashlib.sha256(doc.read_bytes()).hexdigest() == original_hash})
            send_key(hwnd, 0x1B)
            capture("11-menu-escape-dismiss")
            click(edge_x, menu_y, right=True)
            click(editor_left + int(300 * unit), body_top + int(2 * row_h * unit))
            capture("12-menu-outside-dismiss")

            code_rgb = code_values[0]
            # Code view: wheel to move the tall fence through the viewport. Keep
            # every page capture; the exact wheel-to-line mapping varies by DPI.
            for i, ticks in enumerate((round(5 + 14 * max(0, args.scale - 1)), 8, 10), 1):
                owned()
                point = cad.wintypes.POINT(editor_left + int(260 * unit), height // 2)
                cad.user32.ClientToScreen(hwnd, ctypes.byref(point)); cad.user32.SetCursorPos(point.x, point.y)
                for _ in range(ticks):
                    cad.user32.mouse_event(0x0800, 0, 0, (-120) & 0xffffffff, 0)
                    time.sleep(.045)
                time.sleep(.4)
                w, h, px = capture(f"13-code-scroll-{i}")
                results["measurements"][f"code_scroll_{i}"] = analyze_code_blank_column(px, w, h, unit)
                # Early scroll pages still contain ordinary text above the long
                # fence. Inspect only its continuous trailing background band.
                trailing = 0
                for sample_x in range(round(w - 45 * unit), round(w - 31 * unit)):
                    run = 0
                    for y in range(h - round(10 * unit) - 1, round((34 + 10) * unit), -1):
                        if bgra_at(px, w, sample_x, y) != code_rgb:
                            break
                        run += 1
                    trailing = max(trailing, run)
                results["measurements"][f"code_scroll_{i}"]["trailing_code_rgb_run_px"] = trailing
                results["checks"].append({"name": f"long_code_background_continuous_scroll_{i}", "passed": trailing >= min(round(200 * unit), round(h * .25))})

            # Approximate compact dark components in a sample-image gutter. The
            # captured list markers are outside the viewport area by construction.
            marker_left, marker_right = max(0, text_x), min(width, text_x + int(28 * unit))
            mask_w = marker_right - marker_left
            marker_top, marker_bottom = round(list_top), min(height - 1, round(list_top + 3 * row_h * unit))
            mask_h = marker_bottom - marker_top
            mask = bytearray(mask_w * mask_h)
            for yy in range(mask_h):
                for xx in range(mask_w):
                    r, g, b = bgra_at(initial, width, marker_left + xx, marker_top + yy)
                    background = bgra_at(initial, width, width - round(80 * unit), marker_top + yy)
                    mask[yy * mask_w + xx] = int(sum(abs(c - bc) for c, bc in zip((r,g,b),background)) > 50)
            dots = connected_small_components(mask, mask_w, mask_h, round(8 * unit), round(48 * unit * unit))
            results["measurements"]["compact_dark_marker_components"] = dots[:80]
            results["measurements"]["marker_components_clear_of_viewport_edges"] = sum(d["edge_distance"] >= 1 for d in dots)
            results["checks"].append({"name": "unordered_markers_have_multiple_small_components",
                                      "passed": len(dots) >= 3 and sum(d["edge_distance"] >= 1 for d in dots) >= 3})

            # The Vault list begins below tabs, title, address and filter. Its
            # first rows are directory-first in the model; adjust these two row
            # centers after reviewing 14-vault-list if local metrics differ.
            vault_top = int((menu_h + 8 + 27 + 6 + 26 + 6 + 24 + 6 + 28 + 6) * unit)
            vault_x = int(145 * unit)
            row_px = int(26 * unit)
            click(vault_x, vault_top + row_px // 2)
            vault_selected = capture("14-vault-directory-selected")
            # Expanding the first directory inserts its child row. The third row
            # is the next root-level file in the generated dir-first vault.
            click(vault_x, vault_top + row_px * 3 + row_px // 2, right=True)
            vault_menu = capture("15-vault-other-row-context")
            results["measurements"]["vault_selected_visual_change"] = crop_delta(initial, vault_selected[2], width, height,
                                                                                   (0, vault_top, int(panel * unit), vault_top + 3 * row_px))
            # Escape leaves the disposable vault untouched and closes the menu.
            send_key(hwnd, 0x1B)
            move(editor_left + 100 * unit, body_top + 10 * unit)
            dismissed = capture("16-vault-menu-dismiss")
            selection_pixel = (round((panel - 24) * unit), vault_top + row_px * 3 + row_px // 2)
            results["checks"].append({"name": "vault_context_selection_persists_after_dismiss", "passed":
                bgra_at(initial, width, *selection_pixel) != bgra_at(dismissed[2], width, *selection_pixel)})

        except Exception as exc:
            results["error"] = f"{type(exc).__name__}: {exc}"
            if hwnd and proc and proc.poll() is None:
                try:
                    if cad.ensure_foreground(hwnd):
                        w, h, px = cad.capture_client(hwnd)
                        cad.write_png(str(out / "failure-current-window.png"), w, h, px)
                        results["captures"].append({"state": "failure-current-window", "width": w, "height": h})
                except Exception as capture_error:
                    results["failure_capture_error"] = str(capture_error)
            raise
        finally:
            if proc and proc.poll() is None and hwnd:
                cad.user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    results["close_error"] = f"PID {proc.pid} did not exit after WM_CLOSE; process left running"
            results["exit_code"] = proc.returncode if proc else None
            results["sample_after_sha256"] = hashlib.sha256(doc.read_bytes()).hexdigest() if doc.exists() else None
            results["sample_preserved"] = results.get("sample_sha256") == results["sample_after_sha256"]
            (out / "conditions.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
            log.close()

    print(json.dumps({"captures": len(results["captures"]), "out": str(out),
                      "checks": results["checks"], "error": results.get("error")}, ensure_ascii=False, indent=2))
    if not results["sample_preserved"] or any(not c["passed"] for c in results["checks"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()

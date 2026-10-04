"""Real-window regression for fixed-pointer closes after horizontal tab scrolling.

This is intentionally not run by unit tests. It reuses the isolated-process,
manifest, capture, and ownership helpers from tab_close_interruptions.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

PROBE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(PROBE_DIR))

import win_capture as cad
import tab_close_interruptions as close_probe

WM_CLOSE = 0x0010
MOUSEEVENTF_WHEEL = 0x0800
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
TAB_PITCH_DIP = 180.0
TAB_WIDTH_DIP = 176.0
BAR_TO_CLOSE_CENTER_DIP = 161.0
FIXTURE_COUNT = 12


def ids(snapshot):
    return [record["id"] for record in close_probe.stable_records(snapshot)]


def exact_scroll_snapshot(run, label, expected_order, expected_active):
    snapshot = run.wait_records(
        lambda value: ids(value) == expected_order and
        int(value.get("active", 0)) == expected_active,
        label)
    run.require(label + " exact manifest order and active TabId",
                ids(snapshot) == expected_order and
                int(snapshot.get("active", 0)) == expected_active,
                {"order": ids(snapshot), "active": snapshot.get("active"),
                 "expected_order": expected_order, "expected_active": expected_active})
    return snapshot


def send_wheel(run, delta, label, x, y):
    run.move_to_client_px(x, y)
    pointer_before = run.cursor()
    run.owned()
    cad.user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, delta, 0)
    time.sleep(0.55)
    run.owned()
    pointer_after = run.cursor()
    run.require(label + " wheel kept pointer fixed", pointer_after == pointer_before,
                {"before": pointer_before, "after": pointer_after})
    return pointer_before


def click_without_moving(run, x, y):
    run.owned()
    expected = cad.wintypes.POINT(int(x), int(y))
    cad.user32.ClientToScreen(run.hwnd, cad.ctypes.byref(expected))
    actual = run.cursor()
    run.require("fixed close pointer remained at anchor", actual == (expected.x, expected.y),
                {"actual": actual, "expected": [expected.x, expected.y]})
    hit = cad.user32.WindowFromPoint(expected)
    run.require("fixed close anchor is covered by the owned window",
                cad.user32.GetAncestor(hit, 2) == run.hwnd,
                {"hit": int(hit or 0), "window": int(run.hwnd)})
    cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.45)
    run.owned()
    run.require("close did not move the physical pointer", run.cursor() == actual,
                {"before": actual, "after": run.cursor()})


def fit_candidate(bars, index, unit):
    """A card bracketed by both adjacent full-pitch bars is not edge-clipped."""
    if index <= 0 or index + 1 >= len(bars):
        return False
    left_pitch = (bars[index][0] - bars[index - 1][0]) / unit
    right_pitch = (bars[index + 1][0] - bars[index][0]) / unit
    return (abs(left_pitch - TAB_PITCH_DIP) <= 2.0 and
            abs(right_pitch - TAB_PITCH_DIP) <= 2.0 and
            bars[index][0] - bars[index - 1][0] >= round(TAB_WIDTH_DIP * unit))


def run_case(exe, expected_hash, out, theme):
    run = close_probe.ProbeRun(exe, expected_hash, out, "scroll_then_fixed_close", theme)
    passed = False
    try:
        files = close_probe.make_fixtures(out, FIXTURE_COUNT)
        run.start(files[0])
        for path in files[1:FIXTURE_COUNT]:
            run.open_forward(path)
        populated = run.wait_records(
            lambda snap: len(snap.get("records", [])) == FIXTURE_COUNT,
            "twelve_explicit_clean_tabs")
        records = close_probe.stable_records(populated)
        expected_order = [record["id"] for record in records]
        run.require("fixture has twelve unique explicit clean paths",
                    len(records) == FIXTURE_COUNT and len(set(expected_order)) == FIXTURE_COUNT and
                    all(record["path"] and not record["dirty"] for record in records), records)
        expected_paths = {str(path.resolve()).casefold() for path in files[:FIXTURE_COUNT]}
        actual_paths = {str(Path(record["path"]).resolve()).casefold() for record in records}
        run.require("manifest contains exactly the requested fixture paths",
                    actual_paths == expected_paths,
                    {"actual": sorted(actual_paths), "expected": sorted(expected_paths)})

        outer = cad.wintypes.RECT()
        run.owned()
        if not cad.user32.GetWindowRect(run.hwnd, cad.ctypes.byref(outer)):
            raise OSError("GetWindowRect failed before scroll-width resize")
        moved = cad.user32.SetWindowPos(run.hwnd, None, outer.left, outer.top, 1200, 900,
                                        SWP_NOZORDER | SWP_NOACTIVATE)
        run.require("requested normal scroll-test window resize succeeded", bool(moved),
                    {"target_outer_px": [1200, 900]})
        time.sleep(0.8)
        run.owned()
        resized = cad.wintypes.RECT()
        if not cad.user32.GetWindowRect(run.hwnd, cad.ctypes.byref(resized)):
            raise OSError("GetWindowRect failed after scroll-width resize")
        outer_px = [resized.right - resized.left, resized.bottom - resized.top]
        run.result["scroll_test_window_outer_px"] = outer_px
        run.require("window reached normal 1200x900 scroll-test size",
                    abs(outer_px[0] - 1200) <= 4 and abs(outer_px[1] - 900) <= 4,
                    outer_px)

        width, height, _, end_bars = run.client_rect()
        run.require("narrow normal window has horizontal overflow and visible bars",
                    len(end_bars) >= 3 and width < 2500, {"client": [width, height],
                                                         "visible_bars": len(end_bars)})
        pointer_x = end_bars[-2][0] + round(65.0 * run.unit)
        start_pointer = send_wheel(run, 120 * 100, "positive wheel to the first tabs", pointer_x,
                                   run.bar_y)
        _, _, _, first_bars = run.capture("after-wheel-to-first-tabs")
        after_up = run.snapshot("after_positive_wheel")
        run.require("scrolling left did not change tab order or active TabId",
                    ids(after_up) == expected_order and
                    int(after_up.get("active", 0)) == int(populated["active"]),
                    {"order": ids(after_up), "active": after_up.get("active")})
        run.require("positive wheel produced measurable horizontal movement",
                    first_bars and end_bars and
                    abs(first_bars[0][0] - end_bars[0][0]) >= round(24.0 * run.unit),
                    {"left_before_px": end_bars[0][0] if end_bars else None,
                     "left_after_px": first_bars[0][0] if first_bars else None})
        run.require("scroll-to-first capture was recorded", len(first_bars) >= 3,
                    {"visible_bars": len(first_bars), "capture": "after-wheel-to-first-tabs.png"})

        # At the minimum scroll position, the leftmost real card must select
        # the first manifest TabId; this avoids inferring global index from a
        # clipped or stale strip position.
        first_card_body_x = first_bars[0][0] + round(65.0 * run.unit)
        run.click_client_px(first_card_body_x, run.bar_y)
        selected_first = exact_scroll_snapshot(run, "select_first_tab_after_scroll",
                                               expected_order, expected_order[0])
        _, _, _, reset_bars = run.capture("first-tab-selected-at-left-edge")
        run.require("first tab selection kept the strip at its left edge",
                    reset_bars and first_bars and
                    abs(reset_bars[0][0] - first_bars[0][0]) <= 2,
                    {"before": first_bars[0][0] if first_bars else None,
                     "after": reset_bars[0][0] if reset_bars else None})

        # Win32 divides the raw delta by 120 before the tab strip applies its
        # 36 DIP step. Three full notches create a measurable 108 DIP offset.
        reverse_pointers = []
        for wheel_step in range(3):
            reverse_pointers.append(send_wheel(
                run, -120, f"reverse wheel step {wheel_step + 1}",
                first_card_body_x, run.bar_y))
        run.result["events"].append({"kind": "reverse_tab_strip_wheel",
                                     "deltas": [-120, -120, -120],
                                     "pointers": [list(point) for point in reverse_pointers]})
        _, _, _, offset_bars = run.capture("after-reverse-wheel-nonzero-offset")
        offset_snapshot = run.snapshot("after_reverse_wheel")
        run.require("reverse wheel preserved manifest and active TabId",
                    ids(offset_snapshot) == expected_order and
                    int(offset_snapshot.get("active", 0)) == expected_order[0],
                    {"order": ids(offset_snapshot), "active": offset_snapshot.get("active")})
        run.require("reverse wheel changed visible horizontal position",
                    offset_bars and reset_bars and
                    abs(offset_bars[0][0] - reset_bars[0][0]) >= round(24.0 * run.unit),
                    {"left_at_start_px": reset_bars[0][0] if reset_bars else None,
                     "left_after_reverse_px": offset_bars[0][0] if offset_bars else None})
        run.require("offset screenshot has visible real tab bars", len(offset_bars) >= 4,
                    {"visible_bars": len(offset_bars),
                     "capture": "after-reverse-wheel-nonzero-offset.png"})

        candidates = [index for index in range(1, len(offset_bars) - 1)
                      if fit_candidate(offset_bars, index, run.unit)]
        run.require("found a non-clipped card bracketed by measured pitch", bool(candidates),
                    {"bar_x": [bar[0] for bar in offset_bars], "unit": run.unit})
        candidate_index = candidates[0]
        target_bar_x = offset_bars[candidate_index][0]
        target_body_x = target_bar_x + round(65.0 * run.unit)
        run.click_client_px(target_body_x, run.bar_y)
        selected = run.snapshot("selected_visible_scrolled_card")
        target_id = int(selected.get("active", 0))
        target_index = expected_order.index(target_id) if target_id in expected_order else -1
        run.require("measured scrolled card selected a manifest TabId",
                    target_index >= 0 and target_index + 3 < len(expected_order),
                    {"target_id": target_id, "manifest_index": target_index,
                     "order": expected_order})
        selected_order = ids(selected)
        run.require("selection did not mutate fixture order",
                    selected_order == expected_order, selected_order)
        _, _, _, selected_bars = run.capture("measured-scrolled-target-selected")
        same_bar = [bar for bar in selected_bars if abs(bar[0] - target_bar_x) <= 2]
        run.require("selected target remained at its measured visible card position",
                    len(same_bar) == 1 and fit_candidate(
                        selected_bars, selected_bars.index(same_bar[0]), run.unit),
                    {"target_bar_x": target_bar_x,
                     "selected_bars": [bar[0] for bar in selected_bars]})

        close_x = target_bar_x + round(BAR_TO_CLOSE_CENTER_DIP * run.unit)
        run.move_to_client_px(close_x, run.bar_y)
        anchor = run.cursor()
        screen_center = cad.wintypes.POINT(int(close_x), int(run.bar_y))
        cad.user32.ClientToScreen(run.hwnd, cad.ctypes.byref(screen_center))
        run.require("initial target pointer is within one DIP of measured close center",
                    abs((close_x - (target_bar_x + BAR_TO_CLOSE_CENTER_DIP * run.unit)) /
                        run.unit) <= 1.0,
                    {"target_bar_x": target_bar_x, "click_x": close_x,
                     "center_from_bar_dip": BAR_TO_CLOSE_CENTER_DIP,
                     "error_dip": abs((close_x - (target_bar_x +
                                                    BAR_TO_CLOSE_CENTER_DIP * run.unit)) /
                                       run.unit)})
        run.result["scroll_close_chain"] = {
            "order_before": expected_order, "target_id": target_id,
            "target_manifest_index": target_index, "target_bar_x_px": target_bar_x,
            "fixed_pointer_screen_px": [anchor[0], anchor[1]],
            "measured_close_center_error_dip": abs(
                (close_x - (target_bar_x + BAR_TO_CLOSE_CENTER_DIP * run.unit)) / run.unit),
            "steps": []}
        run.require("close pointer corresponds to the owned window", anchor ==
                    (screen_center.x, screen_center.y),
                    {"actual": anchor, "expected": [screen_center.x, screen_center.y]})

        current_order = list(expected_order)
        for step in range(3):
            remove_id = expected_order[target_index + step]
            before = run.snapshot(f"before_scrolled_close_{step + 1}")
            before_order = ids(before)
            run.require(f"step {step + 1} starts from exact expected order",
                        before_order == current_order and int(before.get("active", 0)) == remove_id,
                        {"order": before_order, "active": before.get("active"),
                         "expected_active": remove_id})
            click_without_moving(run, close_x, run.bar_y)
            expected_after = [tab_id for tab_id in current_order if tab_id != remove_id]
            expected_active = (current_order[current_order.index(remove_id) + 1]
                               if current_order.index(remove_id) + 1 < len(current_order)
                               else current_order[current_order.index(remove_id) - 1])
            after = exact_scroll_snapshot(run, f"scrolled_close_{step + 1}",
                                          expected_after, expected_active)
            run.require(f"step {step + 1} removed exactly the clicked TabId",
                        len(before_order) - len(ids(after)) == 1 and
                        set(before_order) - set(ids(after)) == {remove_id},
                        {"removed": sorted(set(before_order) - set(ids(after))),
                         "target": remove_id})
            run.result["scroll_close_chain"]["steps"].append({
                "step": step + 1, "before_order": before_order,
                "removed_tab_id": remove_id, "after_order": ids(after),
                "active_next_tab_id": int(after["active"]),
                "pointer_screen_px": list(run.cursor()),
                "pointer_anchor_error_dip": 0.0})
            current_order = expected_after

        run.require("three fixed-pointer clicks removed three real consecutive TabIds",
                    len(current_order) == FIXTURE_COUNT - 3 and
                    current_order == expected_order[:target_index] +
                    expected_order[target_index + 3:], current_order)
        run.stop_normally()
        passed = all(check["passed"] for check in run.result["checks"])
        return passed
    except Exception as exc:
        run.result["exception"] = f"{type(exc).__name__}: {exc}"
        run.result["passed"] = False
        print("FAIL scroll_then_fixed_close: " + run.result["exception"], flush=True)
        return False
    finally:
        if run.proc is not None and run.proc.poll() is None and run.hwnd:
            # Cleanup never reclaims foreground. A lost foreground remains a
            # failed ownership check, even if WM_CLOSE can still be posted.
            try:
                run.owned()
                cad.user32.PostMessageW(run.hwnd, WM_CLOSE, 0, 0)
                run.proc.wait(timeout=8)
            except Exception as exc:
                run.result["cleanup_exception"] = f"{type(exc).__name__}: {exc}"
                try:
                    run.proc.terminate()
                    run.proc.wait(timeout=5)
                    run.result["abnormal_cleanup_terminate"] = True
                except subprocess.TimeoutExpired:
                    run.result["abnormal_cleanup_still_alive"] = True
        run.report(passed)


def main():
    parser = argparse.ArgumentParser(
        description="Real-window fixed-pointer close test after horizontal tab scrolling")
    parser.add_argument("--exe", required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--theme", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    if not exe.is_file():
        raise SystemExit(f"executable does not exist: {exe}")
    if out.exists() or not out.parent.exists():
        raise SystemExit(f"output must be a new directory with an existing parent: {out}")
    actual_hash = close_probe.digest(exe)
    if actual_hash.lower() != args.expected_sha256.lower():
        raise SystemExit(f"SHA256 mismatch: actual={actual_hash}, "
                         f"expected={args.expected_sha256.lower()}")
    out.mkdir(parents=False, exist_ok=False)
    cad.make_dpi_aware()
    cad.assert_unlocked("tab scroll close")
    passed = run_case(exe, actual_hash, out, args.theme)
    report = {"exe": str(exe), "sha256": actual_hash,
              "expected_sha256": args.expected_sha256.lower(), "theme": args.theme,
              "scenario": "scroll_then_fixed_close", "fixture_tab_count": FIXTURE_COUNT,
              "passed": bool(passed), "case_report": "scroll_then_fixed_close/case.json"}
    report_path = out / "report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"report: {report_path}", flush=True)
    print(f"SHA256: {actual_hash}", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

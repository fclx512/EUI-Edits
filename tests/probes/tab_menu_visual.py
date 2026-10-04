"""Own-process visual & interaction probe for the 2026-10-03 tab-strip / list / font-menu batch.

See docs/NeoEditor-标签页与菜单视觉改进执行计划-2026-10-03.md §11. Covered here:

  * 分组分色 / 父子合并（像素色条 + 会话清单 vaultRoot）
  * 统一面宽 176 DIP / 节距 180 DIP、dirty 前后关闭中心不动
  * 中间固定指针连续关闭 3 页、尾部固定指针连续关闭 3 页（临时左留白、无假 tab）
  * 关闭链解除（指针离开顶栏区域后绘制偏移归零）
  * 展开列表卡片密度（70 高 / 6 gap / 视口内 5 张完整卡）与三个 24×24 动作
  * 字号子级宽度 112 DIP，其他子级保持默认宽度

设计要点（与旧探针的差异）：
  * 弹层几何一律用「开/关两帧差分」测定，不再猜菜单行 y 或列表按钮 x；
    菜单行高由 hover 高亮带反推，列表卡高/节距同理。
  * 断言收集式：单项失败不中断，一次跑完拿到最大证据；末尾按失败数返回非零。
  * 一切点击前用 WindowFromPoint+GetAncestor 确认被测窗口没被遮挡。

Never touches a foreign instance: private APPDATA/TEMP + NEO_SINGLE_INSTANCE=0, and every
click goes through an owned/foreground check.
"""
import argparse
import ctypes
import hashlib
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

import win_capture as cad
from capture_markdown import window_for_pid

TAB_WIDTH_DIP = 176.0
TAB_PITCH_DIP = 180.0
BAR_INSET_DIP = 1.0
CLOSE_CENTER_DIP = 162.0
CLOSE_ICON_HALF_DIP = 7.0
# 色条（卡左缘）到关闭按钮中心的实测距离：6 张卡在 100% 下一致给出 161.5 DIP。
CLOSE_CENTER_FROM_BAR = 161.5
MENU_SEPARATOR_DIP = 8.0          # metrics_.spacing.tiny * 2
MENU_INSET_DIP = 6.0              # 菜单每级内边距（与计划中"inset=6×2"一致）
LIST_INSET_DIP = 8.0
CARD_HEIGHT_DIP = 70.0
CARD_GAP_DIP = 6.0
CARD_CONTENT_RIGHT_DIP = 10.0     # tab_bar.h: contentRight
CARD_ACTIONS_WIDTH_DIP = 80.0     # tab_bar.h: actionsWidth = 3*24 + 2*4
CARD_ACTION_SIZE_DIP = 24.0
CARD_ACTION_PITCH_DIP = 28.0
CARD_ACTION_TOP_DIP = 8.0         # tab_bar.h: actionsY = cardTop + 8
FONT_SUBMENU_DIP = 112.0
ROOT_MENU_ZH_DIP = 212.0
ROOT_MENU_EN_DIP = 300.0


class _NarrowDone(Exception):
    """窄窗分支自带退出流程，用它跳过后面的宽窗套件。"""


# ── 像素工具 ────────────────────────────────────────────────────────────────────
def px(raw, width, x, y):
    index = (int(y) * width + int(x)) * 4
    return raw[index + 2], raw[index + 1], raw[index]


def color_distance(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])


def row_background(raw, width, y, x0, x1):
    counts = {}
    for x in range(int(x0), int(x1)):
        c = px(raw, width, x, y)
        counts[c] = counts.get(c, 0) + 1
    return max(counts.items(), key=lambda kv: kv[1])[0]


def row_runs(raw, width, y, x0, x1, background, tolerance=40):
    runs = []
    start = None
    for x in range(int(x0), int(x1)):
        different = color_distance(px(raw, width, x, y), background) > tolerance
        if different and start is None:
            start = x
        elif not different and start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, int(x1) - 1))
    return runs


def diff_runs(a, b, width, y, x0, x1, tolerance=14):
    """两帧差分：返回同一行里发生变化的连续区间。"""
    runs = []
    start = None
    base = int(y) * width * 4
    for x in range(int(x0), int(x1)):
        i = base + int(x) * 4
        changed = (abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])) > tolerance
        if changed and start is None:
            start = x
        elif not changed and start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, int(x1) - 1))
    return runs


def diff_bands_column(a, b, width, x, y0, y1, tolerance=14, min_px=2):
    """两帧差分：返回某一列里发生变化、且长度 ≥ min_px 的纵向条带。"""
    bands = []
    start = None
    for y in range(int(y0), int(y1)):
        i = (int(y) * width + int(x)) * 4
        changed = (abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])) > tolerance
        if changed and start is None:
            start = y
        elif not changed and start is not None:
            if y - start >= min_px:
                bands.append((start, y - 1))
            start = None
    if start is not None and int(y1) - start >= min_px:
        bands.append((start, int(y1) - 1))
    return bands


def diff_bands_rows(a, b, width, x0, x1, y0, y1, tolerance=10, min_px=2):
    """两帧差分（行投影）：给定 x 区间内只要该行存在任一像素变化，即视为变化行，
    返回纵向连续变化带 [(y_start, y_end)]。

    比 diff_bands_column 稳健：单列取样一旦落在标题/路径文字上，文字像素在 hover
    前后几乎不变，会把整条连续带切碎而测不到（本探针曾因此在列表首卡误报）。
    """
    bands = []
    start = None
    for y in range(int(y0), int(y1)):
        row = int(y) * width
        changed = False
        for x in range(int(x0), int(x1)):
            i = (row + x) * 4
            if (abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])) > tolerance:
                changed = True
                break
        if changed and start is None:
            start = y
        elif not changed and start is not None:
            if y - start >= min_px:
                bands.append((start, y - 1))
            start = None
    if start is not None and int(y1) - start >= min_px:
        bands.append((start, int(y1) - 1))
    return bands


def saturation(color):
    return max(color) - min(color)


def detect_bars(raw, width, y, x0, x1, unit):
    """顶栏左侧 3 DIP 色条：返回 [(x_start, x_end, rgb)]，按 x 排序。

    脏页标记是一个 5 DIP 的实心圆点（彩色、也够饱和），必须先靠长度排除；
    再按 180 DIP 节距成链，把落在节距格点之外的点状物滤掉。
    """
    background = row_background(raw, width, y, x0, x1)
    candidates = []
    for start, end in row_runs(raw, width, y, x0, x1, background):
        # A custom card face can differ from the strip background across the
        # entire card. Split its difference run into saturated cores rather
        # than retaining unrelated coloured glyphs at the opposite end.
        core = None
        for x in range(start, end + 2):
            saturated = x <= end and saturation(px(raw, width, x, y)) >= 40
            if saturated and core is None:
                core = x
            elif not saturated and core is not None:
                length = x - core
                if 1.2 * unit <= length <= 4.5 * unit:
                    candidates.append((core, x - 1, px(raw, width, (core + x - 1) // 2, y)))
                core = None
    if not candidates:
        return candidates
    pitch = TAB_PITCH_DIP * unit
    origin = candidates[0][0]
    return [bar for bar in candidates
            if abs(bar[0] - origin - round((bar[0] - origin) / pitch) * pitch) <= 2.0 * unit]


def rightmost_content(raw, width, y0, y1, x0, x1, background, tolerance=60):
    best = None
    for y in range(int(y0), int(y1)):
        for x in range(int(x0), int(x1)):
            if color_distance(px(raw, width, x, y), background) > tolerance:
                best = x if best is None or x > best else best
    return best


def content_clusters(raw, width, y, x0, x1, background, tolerance=28, join_px=10):
    """把一行里的内容段按「间隔 ≤ join_px 视作同一簇」合并，便于定位右侧按钮组。"""
    merged = []
    for start, end in row_runs(raw, width, y, x0, x1, background, tolerance):
        if merged and start - merged[-1][1] <= join_px:
            merged[-1][1] = end
        else:
            merged.append([start, end])
    return merged


def panel_bounds(closed, opened, width, height, unit, y_start):
    """用开/关两帧差分定位弹层：返回 (x0, x1, y0, y1) 像素。

    只跟踪与首个宽变化行**连通**的那一段，遇到断开即结束。早期版本对全部行取并集，
    屏幕上任何无关瞬态（自动保存提示条、光标闪烁）都会把边界撑大——曾把 440×390 的
    列表面板测成 520×1263。
    """
    min_run = int(80 * unit)
    x0 = x1 = None
    y0 = y1 = None
    for y in range(int(y_start), height - 1):
        wide = [r for r in diff_runs(closed, opened, width, y, 0, width)
                if r[1] - r[0] + 1 >= min_run]
        if x0 is None:
            if not wide:
                continue
            x0, x1 = wide[0][0], wide[0][1]
            y0 = y1 = y
            continue
        follow = [r for r in wide if r[0] <= x1 + 2 * unit and r[1] >= x0 - 2 * unit]
        if not follow:
            break
        y1 = y
        for r in follow:
            x0 = min(x0, r[0])
            x1 = max(x1, r[1])
    return x0, x1, y0, y1


def luminance(color):
    return (color[0] + color[1] + color[2]) / 3.0


def _edge_at(frame, width, x, y, delta=10.0):
    """该像素是否为「竖直边缘」——与左右邻像素亮度差足够大；用于找竖边框列。"""
    c = luminance(px(frame, width, x, y))
    return (abs(c - luminance(px(frame, width, x - 1, y))) > delta or
            abs(c - luminance(px(frame, width, x + 1, y))) > delta)


def _edge_at_v(frame, width, x, y, delta=10.0):
    """该像素是否为「水平边缘」——与上下邻像素亮度差足够大；用于找横边框行。"""
    c = luminance(px(frame, width, x, y))
    return (abs(c - luminance(px(frame, width, x, y - 1))) > delta or
            abs(c - luminance(px(frame, width, x, y + 1))) > delta)


def _edge_run_row(frame, width, y, x0, x1, delta=10.0):
    """一行里连续为水平边缘（比上下邻像素明显亮/暗）的段，用于找弹层的横边框。"""
    runs = []
    start = None
    for x in range(int(x0) + 1, int(x1) - 1):
        if _edge_at_v(frame, width, x, y, delta):
            if start is None:
                start = x
        elif start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, int(x1) - 2))
    return runs


def panel_body_columns(frame, width, unit, x0, x1, y0, y1, min_straight=40.0):
    """在给定窗口里定位一个弹层**主体**，返回 (left, right, rows)。

    先用「最长横边框」拿到主体直边段 [a, b]：弹层顶/底那条横线明显长于任何文字行，
    窗口内所有长度 ≥ 一半的横线行合起来就是主体的上下边界。再在 a、b 两端就近各挑一列
    作竖边框——以「该列上有多少行是水平边缘」为主序（边框贯穿整个主体高度，文字列远
    达不到），以「与面板底色的偏离度」为次序（避开紧贴边框的阴影像素，否则宽度会各多
    1 DIP）。

    这样量到的是主体，不含阴影。直接对帧差分取外接框会把阴影算进去（根菜单 212 DIP 会
    测成 222），而且屏幕左侧有与面板同色的侧栏时，差分根本看不到面板的左半边。
    """
    height = len(frame) // (width * 4)
    best = (0, None, None, None)
    rows = []
    for y in range(int(y0), min(int(y1), height - 1)):
        for s, e in _edge_run_row(frame, width, y, x0, x1):
            if e - s + 1 > best[0]:
                best = (e - s + 1, y, s, e)
    length, _, a, b = best
    if length < min_straight * unit:
        return None, None, None
    for y in range(int(y0), min(int(y1), height - 1)):
        if any(e - s + 1 >= 0.5 * length for s, e in _edge_run_row(frame, width, y, x0, x1)):
            rows.append(y)
    if not rows:
        return None, None, None
    row_top, row_bottom = rows[0], rows[-1]
    # Exclude rounded corners in DIP, including their fractional-scale fringe.
    # A six-physical-pixel inset incorrectly favors the inner edge at 150%.
    corner_inset = max(6, round(16 * unit))
    y_lo, y_hi = row_top + corner_inset, min(row_bottom - corner_inset, height - 1)
    if y_hi - y_lo < 20:
        return None, None, None
    face_x = (a + b) // 2
    face_samples = sorted(luminance(px(frame, width, face_x, y)) for y in range(y_lo, y_hi))
    # The centre column crosses number glyphs. Their ink must not bias the panel
    # background toward a shadow when choosing the actual border column.
    face = face_samples[len(face_samples) // 2]

    def count(x):
        return sum(1 for y in range(y_lo, y_hi) if _edge_at(frame, width, x, y, delta=4.0))

    def mean_lum(x):
        return sum(luminance(px(frame, width, x, y)) for y in range(y_lo, y_hi)) / (y_hi - y_lo)

    def pick(lo, hi):
        # Do not borrow the parent's vertical border outside the requested child region.
        cand = list(range(max(1, int(x0), int(lo)), min(int(x1), int(hi), width - 1)))
        if not cand:
            return None
        counts = {x: count(x) for x in cand}
        top = max(counts.values())
        if top <= 0:
            return None
        best_cols = [x for x in cand if counts[x] >= 0.98 * top]
        return max(best_cols, key=lambda x: abs(mean_lum(x) - face))

    left = pick(a - 40 * unit, a + 4 * unit)
    right = pick(b - 4 * unit, b + 40 * unit)
    return left, right, (row_top, row_bottom)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--expected-sha256', default='',
                        help='expected executable SHA256; pass for final-candidate runs')
    parser.add_argument('--scale', type=float, default=1.0)
    parser.add_argument('--target-effective-scale', type=float, default=None,
                        help='assert GetDpiForWindow/96 * --scale matches this value')
    parser.add_argument('--theme', type=int, default=1)
    parser.add_argument('--theme-file', default='', help='Load a private CSS theme fixture at startup')
    parser.add_argument('--font', type=int, default=14)
    parser.add_argument('--lang', default='zh-CN')
    parser.add_argument('--window', default='2200x1300')
    parser.add_argument('--narrow', action='store_true',
                        help='窄窗 activeOnly 模式：只验证活动卡占满区域与列表可用')
    args = parser.parse_args()

    exe = Path(args.exe).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    cad.make_dpi_aware()
    cad.assert_unlocked('tab menu visual')
    sys.path.insert(0, str(Path(__file__).resolve().parent))

    config = out / 'appdata' / 'EUI-Edits'
    config.mkdir(parents=True)
    temp = out / 'temp'
    temp.mkdir()
    win_w, win_h = (int(v) for v in args.window.lower().split('x'))
    if args.narrow:
        win_w, win_h = 440, 640

    roots = {}
    for name in ['A', 'B'] + [f'lib{i}' for i in range(8)]:
        roots[name] = out / name
        roots[name].mkdir(parents=True)
    (roots['A'] / 'docs').mkdir(parents=True)
    files = {
        'child': roots['A'] / 'docs' / 'note.md',
        'A': roots['A'] / 'top.md',
        'B': roots['B'] / 'README.md',
        'lib0': roots['lib0'] / 'README.md',
        'longzh': roots['lib1'] / ('很长的中文文件名用于验证省略与等宽处理' * 2 + '.md'),
        'longen': roots['lib2'] / ('a-very-long-english-file-name-' * 3 + '.md'),
        'emoji': roots['lib3'] / '表情🀄与组合字é.md',
    }
    for index, key in enumerate(['child', 'A', 'B']):
        files[key].write_text(f'# {key}\n\nfixture body {index}\n', encoding='utf-8')
    files['lib0'].write_text('# B/README\n\nunrelated root, same basename\n', encoding='utf-8')
    for key in ['longzh', 'longen', 'emoji']:
        files[key].write_text(f'# {key}\n', encoding='utf-8')
    for i in range(4, 8):
        (roots[f'lib{i}'] / 'x.md').write_text(f'# lib{i}\n', encoding='utf-8')

    (config / 'settings.ini').write_text(
        f'mode=1\nui_scale={args.scale}\nui_font_size={args.font}\ntheme={args.theme}\n'
        f'animations=0\nui_language={args.lang}\nshow_status_bar=1\n'
        + (f'last_theme_file={Path(args.theme_file).resolve()}\n' if args.theme_file else ''), encoding='utf-8')

    env = dict(os.environ, APPDATA=str(config.parent), TEMP=str(temp), TMP=str(temp),
               NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')

    exe_sha256 = hashlib.sha256(exe.read_bytes()).hexdigest()
    result = dict(exe=str(exe), sha256=exe_sha256,
                  expected_sha256=args.expected_sha256.lower() or None,
                  scale=args.scale, font=args.font, theme=args.theme, lang=args.lang,
                  theme_file=str(Path(args.theme_file).resolve()) if args.theme_file else None,
                  narrow=bool(args.narrow), checks=[], captures=[], exits=[], measurements={},
                  normal_exit_verified=False)
    failures = []
    proc = None
    hwnd = None
    unit = 1.0
    card_height_dip = max(
        CARD_HEIGHT_DIP,
        16.0 + max(24.0, float(math.ceil(args.font * 1.25))) + 2.0 +
        2.0 * math.ceil(max(10.0, args.font - 3.0) * 1.25),
    )
    menu_item_height_dip = max(28.0, float(args.font + 18))
    card_pitch_dip = card_height_dip + CARD_GAP_DIP
    menu_bar_height_dip = max(30.0, args.font + 20.0)

    def estimated_menu_title_width(label, font):
        return sum(font if ord(ch) >= 0x2E80 else font * 0.55 for ch in label) + 20.0

    if args.lang == 'en':
        labels = ('File', 'Edit', 'View')
    else:
        labels = ('文件', '编辑', '视图')
    view_title_x = (10.0 + estimated_menu_title_width(labels[0], args.font) + 2.0 +
                    estimated_menu_title_width(labels[1], args.font) + 2.0 +
                    estimated_menu_title_width(labels[2], args.font) * 0.5)

    def check(name, value, detail=''):
        ok = bool(value)
        result['checks'].append(dict(name=name, passed=ok, detail=str(detail)))
        print(('PASS ' if ok else 'FAIL ') + name, detail, flush=True)
        if not ok:
            failures.append(name)
        return ok

    def measure(name, value):
        result['measurements'][name] = value
        print('MEAS ' + name, value, flush=True)
        return value

    def require(name, value, detail=''):
        if not check(name, value, detail):
            raise RuntimeError(name)

    def owned():
        cad.assert_unlocked('tab menu visual')
        pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        assert proc.poll() is None and pid.value == proc.pid and \
            cad.user32.GetForegroundWindow() == hwnd, \
            'owned foreground lost'

    def client_rect():
        rect = cad.wintypes.RECT()
        cad.user32.GetClientRect(hwnd, ctypes.byref(rect))
        outer = cad.wintypes.RECT()
        cad.user32.GetWindowRect(hwnd, ctypes.byref(outer))
        return (rect.right, rect.bottom,
                (outer.right - outer.left) - rect.right,
                (outer.bottom - outer.top) - rect.bottom)

    def ensure_client_size(target_w, target_h):
        """把客户端尺寸钉死：启动后 App 会套用它自己保存的窗口尺寸，覆盖我们的 SetWindowPos。"""
        for _ in range(12):
            cw, ch, dw, dh = client_rect()
            if abs(cw - target_w) <= 2 and abs(ch - target_h) <= 2:
                time.sleep(0.5)
                cw2, ch2, _, _ = client_rect()
                if abs(cw2 - target_w) <= 2 and abs(ch2 - target_h) <= 2:
                    return True
            # Stress the real layout below its normal tracking minimum, on this
            # probe's own window only. WM_SIZE/rendering remain real.
            flags = 0x0004 | (0x0400 if args.narrow else 0)
            cad.user32.SetWindowPos(hwnd, None, 20, 20, target_w + dw, target_h + dh, flags)
            time.sleep(0.4)
        return False

    def start(first_path=None):
        nonlocal proc, hwnd, unit
        proc = subprocess.Popen([str(exe)] + ([str(first_path)] if first_path else []),
                                cwd=exe.parent, env=env)
        hwnd = None
        for _ in range(150):
            hwnd = window_for_pid(proc.pid)
            if hwnd or proc.poll() is not None:
                break
            time.sleep(0.1)
        require('main window appeared', hwnd is not None)
        require('owned process window foreground',
                cad.ensure_foreground(hwnd) and cad.user32.GetForegroundWindow() == hwnd,
                f'pid={proc.pid}')
        pid = ctypes.c_ulong()
        cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        require('window belongs to launched process', pid.value == proc.pid,
                f'window pid={pid.value}, process pid={proc.pid}')
        cad.user32.SetWindowPos(hwnd, None, 0, 0, win_w, win_h, 0x0004)
        time.sleep(0.5)
        cw, ch, _, _ = client_rect()
        require('requested client size honoured', ensure_client_size(win_w, win_h),
                f'asked {win_w}x{win_h} px, client now {cw}x{ch} px')
        system_scale = cad.user32.GetDpiForWindow(hwnd) / 96
        unit = system_scale * args.scale
        result['effective_scale'] = unit
        result.setdefault('scale_samples', []).append(dict(
            dpi=cad.user32.GetDpiForWindow(hwnd), system_scale=system_scale,
            configured_ui_scale=args.scale, effective_scale=unit))
        result['client_px'] = [client_rect()[0], client_rect()[1]]
        print('effective scale', unit, 'client px', result['client_px'], flush=True)
        if args.target_effective_scale is not None:
            check('effective scale matches requested target',
                  abs(unit - args.target_effective_scale) <= 0.01,
                  f'{unit} vs {args.target_effective_scale}')

    def client_size():
        w, h, raw = cad.capture_client(hwnd)
        return w, h, raw

    def capture(name):
        owned()
        w, h, raw = cad.capture_client(hwnd)
        cad.write_png(str(out / (name + '.png')), w, h, raw)
        result['captures'].append(name)
        return w, h, raw

    def click_dip(x_dip, y_dip):
        owned()
        point = cad.wintypes.POINT(round(x_dip * unit), round(y_dip * unit))
        cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
        cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
        cad.user32.WindowFromPoint.restype = ctypes.c_void_p
        cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        cad.user32.GetAncestor.restype = ctypes.c_void_p
        assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(point), 2) == hwnd, 'client obscured'
        cad.click(hwnd, round(x_dip * unit), round(y_dip * unit))
        time.sleep(0.3)

    def move_dip(x_dip, y_dip):
        owned()
        point = cad.wintypes.POINT(round(x_dip * unit), round(y_dip * unit))
        cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
        cad.user32.SetCursorPos(point.x, point.y)
        time.sleep(0.35)

    def press_here():
        owned()
        cad.user32.mouse_event(0x0002, 0, 0, 0, 0)
        time.sleep(0.06)
        cad.user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(0.55)

    def cursor_position():
        point = cad.wintypes.POINT()
        if not cad.user32.GetCursorPos(ctypes.byref(point)):
            raise RuntimeError('GetCursorPos failed')
        return point.x, point.y

    def ids_in_order(snapshot=None):
        snap = manifest() if snapshot is None else snapshot
        return [int(record['id']) for record in snap.get('records', [])]

    def close_at_fixed_pointer(label, anchor_dip, bar_y, step):
        """Perform one non-repositioning click and prove exactly the hit TabId closed."""
        w_before, _, raw_before = capture(label + '-before')
        bars_before, _ = strip_scan(raw_before, w_before)
        order_before = ids_in_order()
        require(label + ' bars match manifest order', len(bars_before) == len(order_before),
                f'{len(bars_before)} bars vs {order_before}')
        centers_px = [bar[0] + (CLOSE_CENTER_FROM_BAR - BAR_INSET_DIP) * unit
                      for bar in bars_before]
        target_index = min(range(len(centers_px)),
                           key=lambda i: abs(centers_px[i] / unit - anchor_dip))
        target_id = order_before[target_index]
        require(label + ' fixed pointer is on a close center',
                abs(centers_px[target_index] / unit - anchor_dip) <= 1.0,
                f'center={centers_px[target_index]/unit:.2f}, pointer={anchor_dip:.2f}')
        before_cursor = cursor_position()
        expected_x = None
        point = cad.wintypes.POINT(round(anchor_dip * unit), int(bar_y))
        cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
        expected_x = (point.x, point.y)
        require(label + ' cursor starts at fixed anchor', before_cursor == expected_x,
                f'{before_cursor} vs {expected_x}')
        press_here()
        after_cursor = cursor_position()
        require(label + ' cursor remained fixed', after_cursor == before_cursor,
                f'{before_cursor} -> {after_cursor}')
        deadline = time.time() + 8.0
        order_after = ids_in_order()
        while time.time() < deadline and order_after == order_before:
            time.sleep(0.05)
            order_after = ids_in_order()
        expected_order = [tab_id for tab_id in order_before if tab_id != target_id]
        check(label + ' closed the targeted TabId',
              len(order_before) - len(order_after) == 1 and
              target_id not in order_after and order_after == expected_order,
              f'target={target_id}, before={order_before}, after={order_after}')
        return target_id, order_after, before_cursor, after_cursor

    def border_span(frame, width, y0, y1, x0, x1):
        left, right, _ = panel_body_columns(frame, width, unit, x0, x1, y0, y1)
        return left, right

    def border_rows(frame, width, x0, x1, y0, y1):
        _, _, rows = panel_body_columns(frame, width, unit, x0, x1, y0, y1)
        return rows if rows is not None else (None, None)

    def list_accent_runs(frame, width, x, y0, y1):
        runs = []
        start = None
        for y in range(max(0, int(y0)), min(int(y1), len(frame) // (width * 4))):
            colored = saturation(px(frame, width, int(x), y)) >= 40
            if colored and start is None:
                start = y
            elif not colored and start is not None:
                runs.append((start, y - 1))
                start = None
        if start is not None:
            runs.append((start, min(int(y1), len(frame) // (width * 4)) - 1))
        min_full = max(1.0, card_height_dip - 16.0 - 3.0) * unit
        return [run for run in runs if run[1] - run[0] + 1 >= min_full]

    def scroll_list_to_top(lx0, ly0, list_center):
        move_dip((lx0 + 120.0 * unit) / unit, (ly0 + 180.0 * unit) / unit)
        owned()
        cad.user32.mouse_event(0x0800, 0, 0, 120 * 20, 0)
        time.sleep(0.5)
        move_dip(list_center, menu_bar_height_dip * 0.5)

    def keys(*codes):
        owned()
        for code in codes:
            cad.user32.keybd_event(code, cad.user32.MapVirtualKeyW(code, 0), 0, 0)
            time.sleep(0.03)
        for code in reversed(codes):
            cad.user32.keybd_event(code, cad.user32.MapVirtualKeyW(code, 0), 2, 0)
            time.sleep(0.03)
        time.sleep(0.35)

    def open_forward(path):
        owned()
        request = temp / 'EUI-Edits.next-open'
        stage = temp / 'owned-open.tmp'
        stage.write_text(str(path), encoding='utf-8')
        stage.replace(request)
        keys(0x10)
        owned()
        check('deferred open consumed: ' + Path(path).name, not request.exists())

    def manifest():
        return json.loads((config / 'session' / 'manifest.json').read_text(encoding='utf-8'))

    def record_count():
        try:
            return len(manifest()['records'])
        except Exception:
            return -1

    def wait_count(target, label, timeout=8.0):
        deadline = time.time() + timeout
        last = -1
        while time.time() < deadline:
            last = record_count()
            if last == target:
                return True
            time.sleep(0.05)
        return check(label, False, f'count={last} expected={target}')

    def strip_scan(raw, w):
        y = int(round((menu_bar_height_dip * 0.5) * unit))
        return detect_bars(raw, w, y, 0, int(w * 0.95), unit), y

    def close_center_px(raw, w, bar_x, y):
        # 只扫关闭图标所在的那段（145..172 DIP），别把卡右缘边框当内容。
        card_left = bar_x - BAR_INSET_DIP * unit
        region_x0 = card_left + 145 * unit
        region_x1 = card_left + 172 * unit
        background = row_background(raw, w, y, int(card_left + 6 * unit), int(card_left + 20 * unit))
        right = rightmost_content(raw, w, y - 6 * unit, y + 6 * unit, region_x0, region_x1, background)
        if right is None:
            return None
        return right - CLOSE_ICON_HALF_DIP * unit

    def bar_at(raw, w, y, x_px, tolerance=4.0):
        """色条在卡左缘；给定关闭中心就换算回卡左缘再找。"""
        target = x_px - CLOSE_CENTER_DIP * unit
        for start, end, color in detect_bars(raw, w, y, 0, int(w * 0.95), unit):
            if abs(start - target) <= tolerance * unit:
                return (start, end, color)
        return None

    def nearest_close_center(bar_dips, target_dip):
        """离目标最近的那张卡的关闭中心（由色条位置推算，不依赖 × 字形的像素细节）。"""
        if not bar_dips:
            return None
        return round(min((b + CLOSE_CENTER_FROM_BAR - BAR_INSET_DIP for b in bar_dips),
                         key=lambda c: abs(c - target_dip)), 2)

    try:
        if args.expected_sha256:
            require('executable matches expected final SHA256',
                    exe_sha256.lower() == args.expected_sha256.lower(),
                    f'{exe_sha256} vs {args.expected_sha256.lower()}')
        if args.target_effective_scale is not None:
            require('requested effective scale is positive', args.target_effective_scale > 0,
                    args.target_effective_scale)
        # ══ 0. 窄窗 activeOnly（--narrow 走单独一套，自带退出）═══════════════════
        if args.narrow:
            start(files['child'])
            for key in ['A', 'B', 'lib0']:
                open_forward(files[key])
            require('narrow fixture has four tabs', wait_count(4, 'four fixture tabs'), record_count())
            w, h, raw = capture('narrow-active-only')
            bars, bar_y = strip_scan(raw, w)
            bg_bar = row_background(raw, w, bar_y, 0, w)
            check('narrow window collapses the strip to the active tab', len(bars) == 1, f'bars={len(bars)}')
            if bars:
                card_left = bars[0][0] - BAR_INSET_DIP * unit
                wide = [r for r in row_runs(raw, w, bar_y, int(card_left + 10 * unit), w, bg_bar,
                                            tolerance=26) if r[1] - r[0] + 1 >= 40 * unit]
                card_right = round(wide[0][1] / unit, 2) if wide else -1
                clusters = content_clusters(raw, w, bar_y, 0, w, bg_bar)
                settings_left = round(clusters[-1][0] / unit, 2) if clusters else -1
                measure('narrow_card_right_dip', card_right)
                measure('narrow_settings_content_left_dip', settings_left)
                # settingsX = 内容左缘 - 8；卡右缘 = newX = settingsX - 64。
                check('activeOnly card spans the whole region',
                      settings_left > 0 and card_right > 0 and
                      abs(card_right - (settings_left - 72.0)) <= 4.0,
                      f'{card_right} vs {settings_left - 72.0}')
                wb, hb, raw_closed = capture('narrow-list-closed')
                click_dip(settings_left - 30.0, menu_bar_height_dip * 0.5)
                wo, ho, raw_open = capture('narrow-list-open')
                lb = panel_bounds(raw_closed, raw_open, wb, hb, unit, (menu_bar_height_dip + 2) * unit)
                check('tab list still opens in a narrow window', lb[0] is not None, lb)
            owned()
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
            proc.wait(timeout=20)
            result['exits'].append(proc.returncode)
            result['normal_exit_verified'] = proc.returncode == 0
            check('probe exited normally', result['normal_exit_verified'], proc.returncode)
            raise _NarrowDone()

        # ══ 1. 分组分色 / 父子合并 / 等宽 / dirty ═══════════════════════════════
        start(files['child'])
        for key in ['A', 'B', 'lib0', 'longzh', 'longen']:
            open_forward(files[key])
        require('six fixture tabs present', wait_count(6, 'six fixture tabs'), record_count())
        w, h, raw = capture('strip-colored-active')
        bars, bar_y = strip_scan(raw, w)
        measure('bars_dip', [(round(b[0] / unit, 2), b[2]) for b in bars])
        require('six tab colour bars visible', len(bars) == 6, f'bars={len(bars)}')

        pitches = [(bars[i + 1][0] - bars[i][0]) / unit for i in range(len(bars) - 1)]
        measure('pitches_dip', [round(p, 2) for p in pitches])
        check('fixed 180 DIP pitch', all(abs(p - TAB_PITCH_DIP) <= 1.0 for p in pitches), pitches)
        check('A and A/docs share one colour',
              color_distance(bars[0][2], bars[1][2]) <= 12, f'{bars[0][2]} vs {bars[1][2]}')
        distinct = []
        for bar in bars[1:]:
            if not any(color_distance(bar[2], seen) <= 12 for seen in distinct):
                distinct.append(bar[2])
        measure('distinct_colours_besides_root_child', len(distinct))
        check('unrelated open roots get distinct colours', len(distinct) == 5, len(distinct))

        active_left = bars[-1][0] - BAR_INSET_DIP * unit
        background = row_background(raw, w, bar_y, int(active_left + 6 * unit), int(active_left + 20 * unit))
        ends = row_runs(raw, w, bar_y, int(active_left + 120 * unit),
                        int(active_left + (TAB_WIDTH_DIP + 1.0) * unit), background, tolerance=30)
        measured_width = ((ends[-1][1] - active_left) / unit) if ends else -1
        if not (TAB_WIDTH_DIP - 3.0 <= measured_width <= TAB_WIDTH_DIP + 1.0) and len(bars) >= 2:
            # 边框没检出时退化为「相邻色条间距 − gap」。
            measured_width = (bars[-1][0] - bars[-2][0]) / unit - (TAB_PITCH_DIP - TAB_WIDTH_DIP)
            result['measurements']['active_card_width_source'] = 'pitch-gap fallback'
        measure('active_card_width_dip', round(measured_width, 2))
        check('regular card face is 176 DIP wide', abs(measured_width - TAB_WIDTH_DIP) <= 1.5, measured_width)

        before_close = close_center_px(raw, w, bars[-1][0], bar_y)
        if before_close is not None:
            # 绝对位置只做记录：× 字形的像素外沿会带 ~2.5 DIP 系统偏差，
            # 真正的「close 位置固定」由 dirty 前后差分断言（系统偏差相消）。
            measure('active_close_centre_offset_dip', round((before_close - active_left) / unit, 2))
        # ══ 2. 中间固定指针连续关闭 ×3 ══════════════════════════════════════════
        region_left = bars[0][0] - BAR_INSET_DIP * unit
        measure('region_left_dip', round(region_left / unit, 2))
        anchor_middle = region_left + 2 * TAB_PITCH_DIP * unit + CLOSE_CENTER_FROM_BAR * unit
        move_dip(anchor_middle / unit, bar_y / unit)
        count_before = record_count()
        centres = []
        middle_closed_ids = []
        middle_orders = []
        for step in range(3):
            closed_id, order_after, cursor_before, cursor_after = close_at_fixed_pointer(
                f'middle-close-{step}', anchor_middle / unit, bar_y, step)
            middle_closed_ids.append(closed_id)
            middle_orders.append(order_after)
            w3, h3, raw3 = capture(f'middle-close-{step}')
            bars3, bar_y3 = strip_scan(raw3, w3)
            centres.append(nearest_close_center([b[0] / unit for b in bars3], anchor_middle / unit))
        measure('middle_close_centres_dip', centres)
        measure('middle_closed_tab_ids', middle_closed_ids)
        measure('middle_remaining_orders', middle_orders)
        check('middle chain closed 3 tabs', record_count() == count_before - 3,
              f'{count_before} -> {record_count()}')
        check('middle chain keeps the pointer anchored',
              all(c is not None and abs(c - anchor_middle / unit) <= 1.0 for c in centres),
              f'{centres} vs {anchor_middle / unit}')
        check('middle chain leaves no phantom tabs', len(bars3) == record_count(),
              f'bars={len(bars3)} records={record_count()}')

        w4, h4, raw4 = capture('middle-chain-offset')
        bars4, _ = strip_scan(raw4, h4 and w4)
        leftmost_with_offset = round(bars4[0][0] / unit, 2) if bars4 else None
        move_dip(w / unit * 0.5, 300.0)
        w5, h5, raw5 = capture('chain-released')
        bars5, _ = strip_scan(raw5, h5 and w5)
        leftmost_released = round(bars5[0][0] / unit, 2) if bars5 else None
        base_left = round((region_left + BAR_INSET_DIP * unit) / unit, 2)
        measure('chain_offset_leftmost_dip', leftmost_with_offset)
        measure('chain_released_leftmost_dip', leftmost_released)
        check('leaving the tab strip releases the draw offset',
              leftmost_released is not None and abs(leftmost_released - base_left) <= 1.5,
              f'{leftmost_with_offset} -> {leftmost_released} (base {base_left})')

        # ══ 3. 尾部固定指针连续关闭 ×3（先备两页干净新稿）══════════════════════
        keys(0x11, 0x4E)
        keys(0x11, 0x4E)
        require('tail fixture has five clean tabs', wait_count(5, 'five tabs'), record_count())
        move_dip(w / unit * 0.5, 300.0)
        w6, h6, raw6 = capture('tail-before')
        bars6, bar_y6 = strip_scan(raw6, w6)
        require('five bars before tail chain', len(bars6) == 5, len(bars6))
        anchor_tail = bars6[-1][0] - BAR_INSET_DIP * unit + CLOSE_CENTER_FROM_BAR * unit
        measure('tail_anchor_dip', round(anchor_tail / unit, 2))
        move_dip(anchor_tail / unit, bar_y6 / unit)
        tail_before = record_count()
        tail_centres = []
        tail_closed_ids = []
        tail_orders = []
        for step in range(3):
            closed_id, order_after, cursor_before, cursor_after = close_at_fixed_pointer(
                f'tail-close-{step}', anchor_tail / unit, bar_y6, step)
            tail_closed_ids.append(closed_id)
            tail_orders.append(order_after)
            w7, h7, raw7 = capture(f'tail-close-{step}')
            bars7, bar_y7 = strip_scan(raw7, h7 and w7)
            tail_centres.append(nearest_close_center([b[0] / unit for b in bars7], anchor_tail / unit))
        measure('tail_close_centres_dip', tail_centres)
        measure('tail_closed_tab_ids', tail_closed_ids)
        measure('tail_remaining_orders', tail_orders)
        check('tail chain closed 3 tabs', record_count() == tail_before - 3,
              f'{tail_before} -> {record_count()}')
        check('tail chain keeps the pointer anchored',
              all(c is not None and abs(c - anchor_tail / unit) <= 1.0 for c in tail_centres),
              f'{tail_centres} vs {anchor_tail / unit}')
        check('tail chain leaves no phantom tabs', len(bars7) == record_count(),
              f'bars={len(bars7)} records={record_count()}')

        w8, h8, raw8 = capture('tail-blank-left')
        bars8, _ = strip_scan(raw8, h8 and w8)
        leftmost_tail = round(bars8[0][0] / unit, 2) if bars8 else None
        expected_offset = 3 * TAB_PITCH_DIP
        measure('tail_leftmost_dip', leftmost_tail)
        check('tail close leaves exactly three pitches of deliberate blank',
              leftmost_tail is not None and
              abs(leftmost_tail - (base_left + expected_offset)) <= 1.0,
              f'{leftmost_tail} (base {base_left}, expected offset {expected_offset})')
        move_dip(w / unit * 0.5, 320.0)
        w9, h9, raw9 = capture('tail-released')
        bars9, _ = strip_scan(raw9, h9 and w9)
        leftmost_after = round(bars9[0][0] / unit, 2) if bars9 else None
        measure('tail_released_leftmost_dip', leftmost_after)
        check('tail chain releases and reflows', leftmost_after is not None and
              abs(leftmost_after - base_left) <= 1.5, f'{leftmost_tail} -> {leftmost_after}')

        # ══ 3b. dirty 不挪关闭中心（新建一页并写入内容后复测）════════════════════
        keys(0x11, 0x4E)
        time.sleep(0.7)
        wd0, hd0, raw_clean_last = capture('dirty-before')
        bars_c, bar_y_c = strip_scan(raw_clean_last, wd0)
        require('dirty fixture has a visible last card', len(bars_c) >= 2, len(bars_c))
        clean_close = close_center_px(raw_clean_last, wd0, bars_c[-1][0], bar_y_c)
        for ch in 'dirty':
            cad.user32.PostMessageW(hwnd, 0x0102, ord(ch), 1)
        time.sleep(0.6)
        wd, hd, raw_dirty_last = capture('strip-dirty')
        bars_d, bar_y_d = strip_scan(raw_dirty_last, wd)
        require('dirty capture still shows the same card count', len(bars_d) == len(bars_c),
                f'{len(bars_c)} -> {len(bars_d)}')
        dirty_close = close_center_px(raw_dirty_last, wd, bars_d[-1][0], bar_y_d)
        drift = abs(dirty_close - clean_close) / unit
        measure('dirty_close_drift_dip', round(drift, 2))
        check('dirty does not move the close centre', drift <= 1.0, drift)

        # ══ 4. 展开列表：面板几何 / 卡片密度 / 三动作 ════════════════════════════
        for _ in range(6):
            keys(0x11, 0x4E)
        time.sleep(0.6)
        require('overflow fixture has nine tabs', wait_count(9, 'nine tabs'), record_count())
        wb, hb, raw_closed = capture('list-closed')

        y_bar = int(round(menu_bar_height_dip * 0.5 * unit))
        bg_bar = row_background(raw_closed, wb, y_bar, 0, wb)
        clusters = content_clusters(raw_closed, wb, y_bar, 0, wb, bg_bar)
        measure('bar_clusters_px', clusters)
        require('settings button cluster located', len(clusters) >= 1, clusters)
        # English gear and label are separate pixel clusters. The last text cluster
        # cannot locate the list action; use the menu's documented label geometry.
        settings_label = 'Settings' if args.lang == 'en' else '设置'
        settings_text_width = sum(args.font if ord(ch) >= 0x2E80 else args.font * 0.55
                                  for ch in settings_label)
        settings_width = 8.0 + min(16.0, menu_bar_height_dip - 14.0) + 6.0 + settings_text_width + 12.0
        list_center = wb / unit - settings_width - 10.0 - 22.0
        measure('list_button_center_dip', round(list_center, 2))
        click_dip(list_center, menu_bar_height_dip * 0.5)
        wo, ho, raw_open = capture('list-open-cards')
        expected_width = min(440.0, max(300.0, (wb / unit) * 0.42), (wb / unit) - 16.0)
        expected_left = wb / unit - settings_width - 10.0 - expected_width
        lx0, lx1, list_rows = panel_body_columns(raw_open, wb, unit,
            int((expected_left - 24) * unit), int((expected_left + expected_width + 24) * unit),
            int((menu_bar_height_dip + 2) * unit),
            min(hb - 1, int((menu_bar_height_dip + 490) * unit)))
        ly0, ly1 = list_rows if list_rows else (None, None)
        require('list panel detected', lx0 is not None and ly1 is not None, (lx0, lx1, ly0, ly1))
        scroll_list_to_top(lx0, ly0, list_center)
        wo, ho, raw_open = capture('list-open-at-top')
        list_width = (lx1 - lx0 + 1) / unit
        list_height = (ly1 - ly0 + 1) / unit
        measure('list_panel_dip', [round(list_width, 2), round(list_height, 2)])
        check('list panel width follows the 440/42% rule', abs(list_width - expected_width) <= 2.0,
              f'{list_width} vs {expected_width}')
        check('list panel caps at 390 DIP high', abs(list_height - 390.0) <= 2.0, list_height)

        # 卡片 hover 只改底色/边框，整卡宽度上都会变；差分取「卡内 x 区间做行投影」，
        # 不取单列——单列会压在标题文字上而测不到变化（见 diff_bands_rows 注释）。
        # x 区间取卡内 20..140 DIP：既避开左侧 3 DIP 色条与圆角，也远离卡外
        # 信息浮层（悬停行会在面板左侧弹出，最右缘距卡左仍有 8 DIP 间隙）。
        probe_x0 = int(lx0 + (LIST_INSET_DIP + 20.0) * unit)
        probe_x1 = int(lx0 + (LIST_INSET_DIP + 140.0) * unit)

        def card_hover_bands(reference, frame):
            return diff_bands_rows(reference, frame, wb, probe_x0, probe_x1, ly0, ly1,
                                   tolerance=10, min_px=int(20 * unit))

        hover_x_dip = (lx0 + (LIST_INSET_DIP + 60.0) * unit) / unit
        move_dip(hover_x_dip, (ly0 + (LIST_INSET_DIP + 12.0) * unit) / unit)
        _, _, raw_row0 = capture('list-row0-hover')
        row0_bands = card_hover_bands(raw_open, raw_row0)
        measure('list_row0_hover_bands_px', row0_bands)
        require('first card hover band detected', len(row0_bands) >= 1, row0_bands)
        check('list scroll is at first card',
              abs(row0_bands[0][0] - (ly0 + LIST_INSET_DIP * unit)) <= 2.0 * unit,
              f'band top={row0_bands[0][0]}, expected={ly0 + LIST_INSET_DIP * unit}')
        card_h = (row0_bands[0][1] - row0_bands[0][0] + 1) / unit
        measure('list_card_height_dip', round(card_h, 2))
        measure('expected_list_card_height_dip', card_height_dip)
        check('list card height follows UI font formula',
              abs(card_h - card_height_dip) <= 2.0,
              f'{card_h} vs {card_height_dip} at UI font {args.font}')

        pitch = card_pitch_dip
        move_dip(hover_x_dip, (row0_bands[0][0] + (pitch + 12.0) * unit) / unit)
        _, _, raw_row1 = capture('list-row1-hover')
        row1_bands = card_hover_bands(raw_open, raw_row1)
        measure('list_row1_hover_bands_px', row1_bands)
        require('second card hover band detected', len(row1_bands) >= 1, row1_bands)
        measured_pitch = (row1_bands[0][0] - row0_bands[0][0]) / unit
        measure('list_card_pitch_dip', round(measured_pitch, 2))
        check('list card pitch follows card height plus 6 DIP gap',
              abs(measured_pitch - pitch) <= 2.0, f'{measured_pitch} vs {pitch}')
        accent_x = lx0 + (LIST_INSET_DIP + 1.5) * unit
        accent_runs = list_accent_runs(raw_open, wb, accent_x,
                                       ly0 + LIST_INSET_DIP * unit,
                                       ly1 - LIST_INSET_DIP * unit)
        visible = len(accent_runs)
        measure('list_full_cards_visible', visible)
        # One raster pixel of rounded panel edge must not turn exactly five
        # complete cards into floor(4.99). Card count itself comes from real accents.
        expected_visible = int(((list_height - 2 * LIST_INSET_DIP) + CARD_GAP_DIP + 1.0 / unit) // pitch)
        check('complete card count matches measured viewport and UI font',
              visible == expected_visible and (args.font != 14 or visible == 5),
              f'{visible} expected {expected_visible}, UI font {args.font}')

        # 三动作：按钮位于卡内 cardTop+8，各 24×24、间隔 4（中心相距 28）。
        # 取按钮纵向中心那一行，按卡底色找图标墨迹；三个期望中心都要有墨迹。
        #
        # 用 session manifest 的稳定 TabId/order 验证动作，不假设顶栏所有标签都可见，
        # 也不依赖不同标签恰好具有不同颜色。
        list_card_width = list_width - 2 * LIST_INSET_DIP
        actions_left_card = (list_card_width - CARD_CONTENT_RIGHT_DIP - CARD_ACTIONS_WIDTH_DIP)
        centers_card = [actions_left_card + CARD_ACTION_PITCH_DIP * i + CARD_ACTION_SIZE_DIP * 0.5
                        for i in range(3)]
        action_card_top = row1_bands[0][0]
        action_y = int(action_card_top + (CARD_ACTION_TOP_DIP + CARD_ACTION_SIZE_DIP * 0.5) * unit)
        card_bg = px(raw_row1, wb, int(lx0 + (LIST_INSET_DIP + 22.0) * unit),
                     int(action_card_top + 4 * unit))
        hits = []
        for center in centers_card:
            rx0 = int(lx0 + (LIST_INSET_DIP + center - 9.0) * unit)
            rx1 = int(lx0 + (LIST_INSET_DIP + center + 9.0) * unit)
            hits.append(bool(row_runs(raw_row1, wb, action_y, rx0, rx1, card_bg, tolerance=45)))
        measure('card_action_icon_hits', hits)
        check('up / down / close icons all present on the card', all(hits), hits)

        clusters = content_clusters(
            raw_row1, wb, action_y,
            int(lx0 + (LIST_INSET_DIP + actions_left_card - 6.0) * unit),
            int(lx0 + (LIST_INSET_DIP + actions_left_card + CARD_ACTIONS_WIDTH_DIP + 6.0) * unit),
            card_bg, tolerance=45, join_px=10)
        measure('card_action_clusters_dip', [[round((s - lx0) / unit - LIST_INSET_DIP, 1),
                                              round((e - lx0) / unit - LIST_INSET_DIP, 1)] for s, e in clusters])
        check('exactly three action icon clusters', len(clusters) == 3, clusters)
        if len(clusters) == 3:
            gaps = [round((clusters[i + 1][0] - clusters[i][0]) / unit, 2) for i in range(2)]
            widths = [round((e - s + 1) / unit, 2) for s, e in clusters]
            measure('card_action_gap_dip', gaps)
            measure('card_action_glyph_widths_dip', widths)
            check('action clusters sit on the 28 DIP pitch', all(abs(g - CARD_ACTION_PITCH_DIP) <= 4.0
                                                                 for g in gaps), gaps)
            check('action glyphs fit inside the 24 DIP buttons',
                  all(w <= CARD_ACTION_SIZE_DIP for w in widths), widths)

            order_before_action = ids_in_order()
            require('list action fixture has at least three records',
                    len(order_before_action) >= 3, order_before_action)
            expected_after_down = (order_before_action[:1] + [order_before_action[2],
                                    order_before_action[1]] + order_before_action[3:])
            click_dip((lx0 + (LIST_INSET_DIP + centers_card[1]) * unit) / unit, action_y / unit)
            _, _, raw_after_down = capture('list-after-down')
            order_after_down = ids_in_order()
            check('down action moves the selected TabId one slot right',
                  order_after_down == expected_after_down,
                  f'before={order_before_action}, after={order_after_down}')
            # 面板仍在（动作点击不应关闭列表）。
            lx0b, _, _, _ = panel_bounds(raw_closed, raw_after_down, wb, hb, unit,
                                         (menu_bar_height_dip + 2) * unit)
            check('list stays open after an action click', lx0b is not None, lx0b)
            # 收尾：被下移的标签现在在第 3 张卡，点它的「上移」还原顺序，后续步骤沿用。
            card2_top = action_card_top + pitch * unit
            button_dy = (CARD_ACTION_TOP_DIP + CARD_ACTION_SIZE_DIP * 0.5) * unit
            move_dip(hover_x_dip, (card2_top + 12.0 * unit) / unit)
            time.sleep(0.2)
            click_dip((lx0 + (LIST_INSET_DIP + centers_card[0]) * unit) / unit,
                      (card2_top + button_dy) / unit)
            _, _, raw_restored = capture('list-after-up')
            check('up action restores the original order',
                  ids_in_order() == order_before_action,
                  f'expected={order_before_action}, actual={ids_in_order()}')

        keys(0x1B)
        time.sleep(0.3)

        # ══ 5. 字号子级宽度 112 DIP；其他子级保持默认 ══════════════════════════
        _, _, raw_menus_closed = capture('menu-closed')
        click_dip(view_title_x, menu_bar_height_dip * 0.5)
        wm, hm, raw_view_open = capture('menu-view-open')
        # 主体宽度只从帧本身量：差分外接框含阴影（212 会测成 222），而且屏幕左侧有与
        # 面板同色的侧栏，差分根本看不到面板左半边。菜单面板顶端固定为菜单栏高度。
        menu_y0 = int(menu_bar_height_dip * unit)
        menu_y1 = min(hm - 1, int((menu_bar_height_dip + 12 * menu_item_height_dip + 64) * unit))
        root_left, root_right = border_span(raw_view_open, wm, menu_y0 + max(2, round(4 * unit)), menu_y1,
                                            int(20 * unit), int(500 * unit))
        require('view menu body edges found', root_left is not None, (root_left, root_right))
        root_width = (root_right - root_left + 1) / unit
        expected_root = ROOT_MENU_EN_DIP if args.lang == 'en' else ROOT_MENU_ZH_DIP
        measure('view_menu_root_body_px', [root_left, root_right])
        measure('view_menu_root_width_dip', round(root_width, 2))
        check('view menu root width unchanged', abs(root_width - expected_root) <= 2.5,
              f'{root_width} vs {expected_root}')

        # 用 hover 高亮带反推行高，顺带拿到首项顶部（含 inset）。行投影取面板左侧
        # 无文字的一段，避免文字像素把 32 DIP 的高亮带切碎。
        hover_x_dip = (root_left + (MENU_INSET_DIP + 30.0) * unit) / unit
        move_dip(hover_x_dip, menu_bar_height_dip + MENU_INSET_DIP + 16.0)
        _, _, raw_row_hover = capture('menu-row0-hover')
        row_bands = diff_bands_rows(raw_view_open, raw_row_hover, wm,
                                    int(root_left + 5 * unit), int(root_left + 25 * unit),
                                    menu_y0, menu_y1, tolerance=10, min_px=int(16 * unit))
        measure('menu_row0_bands_px', row_bands)
        require('view menu row band detected', len(row_bands) >= 1, row_bands)
        item_h = (row_bands[0][1] - row_bands[0][0] + 1) / unit
        measure('menu_item_height_dip', round(item_h, 2))
        check('menu item height follows UI font metrics',
              abs(item_h - menu_item_height_dip) <= 2.0,
              f'{item_h} vs {menu_item_height_dip} at UI font {args.font}')

        child_x0 = int(root_right + 3)
        child_x1 = int(root_right + 400 * unit)

        def child_body(frame, width):
            """子级面板主体：只在父级右边界之外取样，避开父级自己的竖边框。"""
            left, right, _ = panel_body_columns(frame, width, unit, child_x0, child_x1,
                                                menu_y0 + max(2, round(4 * unit)), menu_y1)
            return left, right

        font_top = row_bands[0][0] / unit + 6 * item_h + 2 * MENU_SEPARATOR_DIP
        measure('font_size_row_top_dip', round(font_top, 2))
        move_dip(hover_x_dip, font_top + item_h * 0.5)
        wf, hf, raw_font = capture('menu-font-submenu')
        child_left, child_right = child_body(raw_font, wf)
        require('font submenu body edges found', child_left is not None, (child_left, child_right))
        child_width = (child_right - child_left + 1) / unit
        measure('font_submenu_body_px', [child_left, child_right])
        measure('font_submenu_width_dip', round(child_width, 2))
        check('font-size submenu is 112 DIP wide', abs(child_width - FONT_SUBMENU_DIP) <= 1.1, child_width)
        gap_dip = (child_left - root_right - 1) / unit
        measure('font_submenu_gap_dip', round(gap_dip, 2))
        check('font submenu sits immediately right of the parent', 0.0 <= gap_dip <= 12.0, gap_dip)

        # 布局子级（index 0）沿用默认宽度，不被一并收窄。
        move_dip(hover_x_dip, menu_bar_height_dip + MENU_INSET_DIP + item_h * 0.5)
        wl, hl, raw_layout = capture('menu-layout-submenu')
        layout_left, layout_right = child_body(raw_layout, wl)
        measure('layout_submenu_body_px', [layout_left, layout_right])
        require('layout submenu body edges found', layout_left is not None, (layout_left, layout_right))
        layout_child = (layout_right - layout_left + 1) / unit
        measure('layout_submenu_width_dip', round(layout_child, 2))
        check('other submenus keep the default width', abs(layout_child - root_width) <= 2.5,
              f'{layout_child} vs {root_width}')
        keys(0x1B)
        time.sleep(0.3)

        # ══ 6. 正常退出 ════════════════════════════════════════════════════════
        # The geometry fixture made one known untitled draft dirty. Discard it by
        # selecting that exact TabId and using the dialog's explicit Discard choice.
        current_manifest = manifest()
        dirty_ids = [int(record['id']) for record in current_manifest['records']
                     if record.get('dirty')]
        for dirty_id in dirty_ids:
            current_manifest = manifest()
            order = ids_in_order(current_manifest)
            active_index = order.index(int(current_manifest['active']))
            target_index = order.index(dirty_id)
            for _ in range((target_index - active_index) % len(order)):
                keys(0x11, 0x09)
            deadline = time.time() + 8.0
            while time.time() < deadline and int(manifest()['active']) != dirty_id:
                time.sleep(0.05)
            require('dirty draft selected before explicit discard',
                    int(manifest()['active']) == dirty_id,
                    f'target={dirty_id}, active={manifest().get("active")}')
            before_order = ids_in_order()
            keys(0x11, 0x57)
            capture('final-dirty-discard-dialog')
            keys(0x25)  # Cancel is initially selected; Left selects Discard.
            keys(0x0D)
            deadline = time.time() + 8.0
            after_order = ids_in_order()
            while time.time() < deadline and after_order == before_order:
                time.sleep(0.05)
                after_order = ids_in_order()
            check('explicit discard removes only the known dirty TabId',
                  dirty_id not in after_order and
                  after_order == [tab_id for tab_id in before_order if tab_id != dirty_id],
                  f'target={dirty_id}, before={before_order}, after={after_order}')
        check('all remaining tabs are clean before process close',
              not any(record.get('dirty') for record in manifest()['records']))
        owned()
        cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
        proc.wait(timeout=20)
        result['exits'].append(proc.returncode)
        result['normal_exit_verified'] = proc.returncode == 0
        check('probe exited normally', result['normal_exit_verified'], proc.returncode)
        check('normal close clears the owned session manifest',
              not (config / 'session' / 'manifest.json').exists())
    except _NarrowDone:
        pass
    except Exception as exc:
        detail = f'{type(exc).__name__}: {exc}'
        result['exception'] = detail
        if not any(name == 'unhandled probe exception' for name in failures):
            failures.append('unhandled probe exception')
        print('FAIL unhandled probe exception', detail, flush=True)
    finally:
        if proc is not None and proc.poll() is None and hwnd:
            # Cleanup is deliberately separate from the pass path. Any exit here is
            # abnormal evidence and cannot be reported as a successful normal close.
            if cad.user32.GetForegroundWindow() != hwnd:
                cad.ensure_foreground(hwnd)
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                failures.append('process remained alive after cleanup WM_CLOSE')
                result['cleanup_terminated'] = True
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    result['cleanup_terminated'] = False
                result.setdefault('exits', []).append(proc.poll())
                print('FAIL process remained alive after cleanup WM_CLOSE', flush=True)
            if not result.get('normal_exit_verified'):
                failures.append('process exited only during abnormal cleanup')
        if proc is not None and proc.poll() is None:
            failures.append('probe process still alive at report time')
            result.setdefault('exits', []).append(None)
        result['failures'] = failures
        (out / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print('failures:', len(failures), failures, flush=True)

    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

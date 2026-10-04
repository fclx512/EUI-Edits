"""独立分析 press_flicker_probe 的时间线：按压/松手过渡是否越出两端。

press_flicker_probe 自带的 excursions 用的是固定毫秒窗口取"按住端点"，
窗口落在按压之前，端点会退化成静止色，指标不可用。这里改用 MARK 时序：

  rest = 按压前 120ms~20ms（悬停稳态）
  held = 松开前 120ms~20ms（按住稳态）
  越界 = 按压到松手后 400ms 内任何采样点超出 [min(rest,held), max(rest,held)] ± tol

单通道越界即为闪动（此前直插 alpha 的中间点会比两端都暗）。
"""
import argparse
import json
from pathlib import Path


def rgb_of(sample):
    """采样色：JSON 往返后元组会变成数组，这里统一成 3 元组，MARK 行返回 None。"""
    color = sample['color']
    if isinstance(color, list) and len(color) == 3:
        return tuple(color)
    if isinstance(color, tuple):
        return color
    return None


def median_window(samples, lo, hi):
    colors = [rgb_of(s) for s in samples if lo <= s['ms'] <= hi and rgb_of(s) is not None]
    if not colors:
        return None
    pick = sorted(colors)[len(colors) // 2]
    return pick


def steady(samples, mark_ms, before=True, lead=120.0, gap=20.0):
    lo, hi = (mark_ms - lead, mark_ms - gap) if before else (mark_ms + gap, mark_ms + lead)
    return median_window(samples, lo, hi)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--conditions', required=True)
    ap.add_argument('--tol', type=int, default=2)
    ap.add_argument('--tail', type=float, default=400.0)
    args = ap.parse_args()

    data = json.loads(Path(args.conditions).read_text(encoding='utf-8'))
    print('exe_sha256', data.get('exe_sha256', '')[:16], '| animations', data.get('animations'),
          '| theme', data.get('theme'))
    worst = 0
    for target in data['targets']:
        timeline = target['timeline']
        marks = [(s['ms'], s['text']) for s in timeline if s['color'] == 'MARK']
        presses = [ms for ms, text in marks if text == 'press']
        releases = [ms for ms, text in marks if text == 'release']
        cycles = 0
        bad_press = []
        bad_release = []
        ranges = []
        for index in range(min(len(presses), len(releases))):
            p, r = presses[index], releases[index]
            rest = steady(timeline, p, before=True)
            held = steady(timeline, r, before=True)
            if rest is None or held is None:
                continue
            cycles += 1
            low = [min(rest[i], held[i]) - args.tol for i in range(3)]
            high = [max(rest[i], held[i]) + args.tol for i in range(3)]
            ranges.append((rest, held))
            for s in timeline:
                color = rgb_of(s)
                if color is None or not (p <= s['ms'] <= r + args.tail):
                    continue
                if any(not (low[i] <= color[i] <= high[i]) for i in range(3)):
                    # 按压窗口内的偏离 = 过渡中途闪动；松开之后才开始的偏离多半是
                    # 这个目标按下去会打开菜单/面板（导航结果），分开统计。
                    if s['ms'] <= r:
                        bad_press.append(s)
                    else:
                        bad_release.append(s)
        worst = max(worst, len(bad_press))
        label = ','.join('%s→%s' % (tuple(a), tuple(b)) for a, b in ranges[:1])
        print('%-12s cycles=%d rest→held %s | press-window=%d post-release=%d'
              % (target['target'], cycles, label, len(bad_press), len(bad_release)))
        for s in bad_press[:5]:
            print('     [press]', s)
        for s in bad_release[:3]:
            print('     [post ]', s)
    print('WORST press-window excursions', worst)


if __name__ == '__main__':
    main()

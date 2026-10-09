"""Compare independent process medians of warm CPU pipeline stages."""
import argparse
import json
from pathlib import Path
from statistics import median


def summarize(directory, expected_scenarios=8):
    groups = {}
    stages = ("total_us", "build_us", "plan_us", "decoration_us")
    for phase in ("before", "after"):
        for path in sorted(directory.glob(f"{phase}-*.jsonl")):
            rows = [json.loads(s) for s in path.read_text(encoding="utf-8-sig").splitlines()]
            if len(rows) != 3 or {r["round"] for r in rows} != {0, 1, 2}:
                raise ValueError(f"incomplete rounds: {path}")
            keys = {(r["bytes"], r["markdown"], r["numbers"]) for r in rows}
            if len(keys) != 1 or any(r["iterations"] != 80 for r in rows):
                raise ValueError(f"inconsistent scenario: {path}")
            key = keys.pop()
            values = {s: median(r[s] for r in rows) for s in stages}
            groups.setdefault(key, {}).setdefault(phase, []).append(values)
    if len(groups) != expected_scenarios:
        raise ValueError(f"expected {expected_scenarios} size/content/gutter scenarios")
    result = []
    for (size, markdown, numbers), phases in sorted(groups.items()):
        if any(len(phases.get(p, [])) != 3 for p in ("before", "after")):
            raise ValueError("expected three independent processes for each phase/scenario")
        item = {"bytes": size, "markdown": markdown, "numbers": numbers}
        for phase, processes in phases.items():
            totals = [p["total_us"] for p in processes]
            item[phase] = {"median_us": median(totals), "min_us": min(totals),
                           "max_us": max(totals), "process_medians_us": totals,
                           "stages_us": {s: median(p[s] for p in processes) for s in stages}}
        item["reduction_percent"] = 100 * (1 - item["after"]["median_us"] /
                                           item["before"]["median_us"])
        result.append(item)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--expected-scenarios", type=int, default=8)
    args = parser.parse_args()
    result = summarize(args.directory, args.expected_scenarios)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for item in result:
        print(f'{item["bytes"]} bytes, markdown={item["markdown"]}, numbers={item["numbers"]}: '
              f'{item["before"]["median_us"]:.3f} -> {item["after"]["median_us"]:.3f} us, '
              f'{item["reduction_percent"]:.2f}%')

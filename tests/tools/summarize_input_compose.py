"""Summarize independent process medians; round averages are not frame p95."""
import argparse
import json
from pathlib import Path
from statistics import median


def summarize(directory):
    grouped = {}
    for mode in ("owned", "borrowed"):
        for path in sorted(directory.glob(f"{mode}-[0-9]*.jsonl")):
            process = {}
            for line in path.read_text(encoding="utf-8-sig").splitlines():
                row = json.loads(line)
                if row["mode"] != mode:
                    raise ValueError(f"mode mismatch: {path}")
                key = (row["bytes"], row["long_line"])
                process.setdefault(key, []).append(row)
            for key, rows in process.items():
                if len(rows) != 3 or {r["round"] for r in rows} != {0, 1, 2}:
                    raise ValueError(f"incomplete rounds: {path}, {key}")
                values = {s: median(r[s] for r in rows)
                          for s in ("transfer_us", "build_us", "tail_us")}
                values["total_us"] = median(
                    r["transfer_us"] + r["build_us"] + r["tail_us"] for r in rows)
                grouped.setdefault(key, {}).setdefault(mode, []).append(values)
    result = []
    for (size, long_line), modes in sorted(grouped.items()):
        if any(len(modes.get(m, [])) != 3 for m in ("owned", "borrowed")):
            raise ValueError("exactly three independent processes per mode required")
        item = {"bytes": size, "long_line": long_line, "processes_per_mode": 3,
                "rounds_per_process": 3, "iterations_per_round": 80}
        for mode, processes in modes.items():
            totals = [p["total_us"] for p in processes]
            item[mode] = {"median_us": median(totals), "min_us": min(totals),
                          "max_us": max(totals), "process_medians_us": totals,
                          "stages_us": {s: median(p[s] for p in processes)
                                        for s in ("transfer_us", "build_us", "tail_us")}}
        item["reduction_percent"] = 100 * (1 - item["borrowed"]["median_us"] /
                                           item["owned"]["median_us"])
        result.append(item)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = summarize(args.directory)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for item in result:
        print(f'{item["bytes"]} bytes, long_line={item["long_line"]}: '
              f'{item["owned"]["median_us"]:.3f} -> '
              f'{item["borrowed"]["median_us"]:.3f} us, '
              f'{item["reduction_percent"]:.2f}%')

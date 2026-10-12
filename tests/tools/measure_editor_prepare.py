"""Interleaved CPU-only editor preparation A/B; no render or GUI claims."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("fixtures", type=Path,
                        help="Use an ASCII relative path for the Windows narrow-argv probe")
    parser.add_argument("output", type=Path, help="Must not exist")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    executables = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    identity = {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                for name, path in executables.items()}
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("NEO_")}
    scenes = [(count, width, shape, animations)
              for width, shape, counts in [(1120, "duplicate", (1, 2, 4, 32, 128, 256)),
                                           (500, "duplicate", (1, 32, 256)),
                                           (1120, "unique", (32, 256))]
              for count in counts for animations in ("off", "on")]
    processes = []
    for count, width, shape, animations in scenes:
        scene = f"{count}-{width}-{shape}-{animations}"
        for pair in range(3):
            order = ("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")
            for build in order:
                label = f"{scene}-p{pair + 1}-{build}"
                command = [str(executables[build]), str(args.fixtures), str(count), animations, str(width), shape]
                start = time.time()
                result = subprocess.run(command, env=environment, capture_output=True, text=True)
                (args.output / f"{label}.jsonl").write_text(result.stdout, encoding="utf-8")
                (args.output / f"{label}.stderr").write_text(result.stderr, encoding="utf-8")
                receipt = {"scene": scene, "build": build, "pair": pair, "command": command,
                           "started_unix": start, "exit_code": result.returncode}
                (args.output / f"{label}.receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
                if result.returncode:
                    raise RuntimeError(f"{label} failed with {result.returncode}; raw output retained")
                rows = [json.loads(line) for line in result.stdout.splitlines()]
                if len(rows) != 3 or [row["round"] for row in rows] != [0, 1, 2] or any(
                                        row["iterations"] != 40 or
                                        row["tabs"] != count or row["width"] != width or
                                        row["animations"] != (animations == "on") or
                                        row["unique"] != (shape == "unique") or
                                        any(not math.isfinite(row[key]) or row[key] < 0
                                            for key in ("language_us", "vault_us", "tabs_us", "total_us"))
                                        for row in rows):
                    raise RuntimeError(f"Invalid scenario output: {label}")
                processes.append({**receipt, "medians": {key: statistics.median(row[key] for row in rows)
                                                         for key in ("language_us", "vault_us", "tabs_us", "total_us")}})
        print(f"Completed {scene}", flush=True)
    summary = []
    for count, width, shape, animations in scenes:
        scene = f"{count}-{width}-{shape}-{animations}"
        row = {"scene": scene, "tabs": count, "width": width, "shape": shape, "animations": animations}
        for key in ("language_us", "vault_us", "tabs_us", "total_us"):
            values = {build: [p["medians"][key] for p in processes if p["scene"] == scene and p["build"] == build]
                      for build in executables}
            medians = {build: statistics.median(samples) for build, samples in values.items()}
            row[key] = {build: {"median": medians[build], "range": [min(samples), max(samples)],
                               "process_medians": samples} for build, samples in values.items()}
            row[key]["reduction_percent"] = 100 * (1 - medians["candidate"] / medians["baseline"])
        summary.append(row)
    fixtures = [{"path": str(path.relative_to(args.fixtures)),
                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                for path in sorted(args.fixtures.rglob("*")) if path.is_file()]
    output = {"identity": identity, "cwd": str(Path.cwd()),
              "fixture_root": str(args.fixtures.resolve()), "fixture_files": fixtures,
              "measurement_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "machine": {"platform": platform.platform(),
              "processor": os.environ.get("PROCESSOR_IDENTIFIER"), "python": platform.python_version()},
              "method": "3 interleaved process pairs per scene (BC/CB/BC); 5 warm iterations then 3 rounds of 40. "
                        "Median per process then median of 3 processes; ranges are process medians. "
                        "Actual language/vault/tab builders and interaction defaults plus UI layout. "
                        "Total contains stage times, do not add them to total. No renderer, real input, "
                        "switching/parser/index validation or physical display measurement. No trace. "
                        "Default scheduler/affinity; all rounds retained.",
              "processes": processes, "scenes": summary}
    (args.output / "summary.json").write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"All {len(processes)} processes exited 0; summary saved", flush=True)


if __name__ == "__main__":
    main()

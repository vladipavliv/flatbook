#!/usr/bin/env python3
"""Regenerate the machine-specific benchmark numbers in docs/BENCHMARKS.md.

Reads a Google Benchmark JSON export and replaces the block between the
`<!-- BENCHMARKS:REPORT -->` markers with the current machine's CPU, OS,
compiler, and per-benchmark median +/- stddev.

Usage: make_report.py <bench.json> <docs/BENCHMARKS.md>
"""

import json
import platform
import subprocess
import sys


def cpu_model() -> str:
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def compiler() -> str:
    try:
        return subprocess.check_output(["c++", "--version"], text=True).splitlines()[0].strip()
    except Exception:
        return "unknown"


def main() -> None:
    json_path, md_path = sys.argv[1], sys.argv[2]
    with open(json_path, encoding="utf-8") as f:
        data = json.load(f)

    medians: dict[str, float] = {}
    stddevs: dict[str, float] = {}
    for b in data["benchmarks"]:
        agg = b.get("aggregate_name")
        name = b["name"]
        base = name[: -len(agg) - 1] if (agg and name.endswith("_" + agg)) else name
        if agg == "median":
            medians[base] = b["real_time"]
        elif agg == "stddev":
            stddevs[base] = b["real_time"]

    lines = [
        "<!-- BENCHMARKS:REPORT -->",
        f"- CPU: {cpu_model()}",
        f"- OS: {platform.platform()}",
        f"- Compiler: {compiler()}",
        "- Method: Google Benchmark, `-O3` (`-DFLATBOOK_NATIVE=ON` for `-march=native`), "
        "10 repetitions, median ± stddev (ns).",
        "",
        "| benchmark | median ± stddev (ns) |",
        "|---|---|",
    ]
    for name in sorted(medians):
        sd = stddevs.get(name, 0.0)
        lines.append(f"| `{name}` | {medians[name]:.1f} ± {sd:.1f} |")
    lines.append("<!-- BENCHMARKS:REPORT -->")

    with open(md_path, encoding="utf-8") as f:
        text = f.read()

    start = text.find("<!-- BENCHMARKS:REPORT -->")
    end = text.find("<!-- BENCHMARKS:REPORT -->", start + 1)
    if start == -1 or end == -1:
        sys.exit("markers not found in " + md_path)

    new_text = text[:start] + "\n".join(lines) + text[end + len("<!-- BENCHMARKS:REPORT -->"):]
    with open(md_path, "w", encoding="utf-8") as f:
        f.write(new_text)
    print(f"wrote {md_path} ({len(medians)} benchmarks)")


if __name__ == "__main__":
    main()

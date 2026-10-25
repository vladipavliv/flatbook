#!/usr/bin/env python3
"""Plot FlatBook scaling results from a Google Benchmark JSON report.

Usage:
    1. Run the benchmarks and dump JSON:
       ./build/benchmarks/flatbook_bench \
           --benchmark_filter="BM_Latency|BM_MapLatency|BM_RestLatency|BM_MapRestLatency|BM_CppTraderRestLatency|BM_Cancel|BM_MapCancel|BM_OrderType" \
           --benchmark_min_time=0.2s --benchmark_repetitions=10 --benchmark_report_aggregates_only=true \
           --benchmark_format=json --benchmark_out=bench.json

    2. Plot:
       python3 benchmarks/plot/plot_scaling.py bench.json [out_dir]

Produces:
    add_vs_price_levels.png        - addOrder latency vs price range (FlatBook vs std::map)
    rest_vs_price_levels.png       - resting-only add vs price range (FlatBook vs std::map)
    cpptrader_vs_price_levels.png  - resting-only add vs price range (CppTrader baseline, log scale)
    cancel_vs_price_levels.png     - add/cancel mix (25% cancel) vs price range
    order_type.png                 - addOrder latency per order type
"""

import json
import re
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is required: pip install matplotlib")

LATENCY_RE = re.compile(r"BM_Latency<(\d+), (\d+), PriceDistribution::(\w+)>")
MAP_RE = re.compile(r"BM_MapLatency<(\d+), PriceDistribution::(\w+)>")
REST_RE = re.compile(r"BM_RestLatency<(\d+), (\d+), PriceDistribution::(\w+)>")
MAP_REST_RE = re.compile(r"BM_MapRestLatency<(\d+), PriceDistribution::(\w+)>")
CPPTRADER_REST_RE = re.compile(r"BM_CppTraderRestLatency<(\d+), PriceDistribution::(\w+)>")
CANCEL_RE = re.compile(r"BM_Cancel<(\d+), (\d+), PriceDistribution::(\w+)>/(\d+)")
MAP_CANCEL_RE = re.compile(r"BM_MapCancel<(\d+), PriceDistribution::(\w+)>/(\d+)")
ORDER_TYPE_RE = re.compile(
    r"BM_OrderType<(\d+), (\d+), PriceDistribution::(\w+)>/orderType:(\d+)"
)

CANCEL_RATE = 25  # use the /25 cancel-rate variant for the cancel chart

STYLE = {
    ("FlatBook", "Uniform"): ("#e74c3c", "o", "-"),
    ("FlatBook", "Clustered"): ("#e67e22", "s", "-"),
    ("std::map", "Uniform"): ("#2980b9", "o", "--"),
    ("std::map", "Clustered"): ("#16a085", "s", "--"),
    ("CppTrader", "Uniform"): ("#8e44ad", "^", "-"),
    ("CppTrader", "Clustered"): ("#2c3e50", "v", "-"),
}


def load(path):
    with open(path) as f:
        data = json.load(f)
    # With --benchmark_repetitions + --benchmark_report_aggregates_only, each
    # benchmark appears multiple times as _mean/_median/_stddev/_cv. Keep only
    # the median (and plain runs without an aggregate_name), never stddev/cv.
    return [b for b in data["benchmarks"] if b.get("aggregate_name", "median") == "median"]


def cpu_ns(b):
    return b["cpu_time"]


def collect(benchmarks):
    """Return rows of (workload, kind, levels, dist, ns)."""
    rows = []
    for b in benchmarks:
        name, ns = b["name"], cpu_ns(b)
        m = LATENCY_RE.match(name)
        if m:
            rows.append(("add", "FlatBook", int(m.group(2)), m.group(3), ns)); continue
        m = MAP_RE.match(name)
        if m:
            rows.append(("add", "std::map", int(m.group(1)), m.group(2), ns)); continue
        m = REST_RE.match(name)
        if m:
            rows.append(("rest", "FlatBook", int(m.group(2)), m.group(3), ns)); continue
        m = MAP_REST_RE.match(name)
        if m:
            rows.append(("rest", "std::map", int(m.group(1)), m.group(2), ns)); continue
        m = CPPTRADER_REST_RE.match(name)
        if m:
            rows.append(("rest", "CppTrader", int(m.group(1)), m.group(2), ns)); continue
        m = CANCEL_RE.match(name)
        if m and int(m.group(4)) == CANCEL_RATE:
            rows.append(("cancel", "FlatBook", int(m.group(2)), m.group(3), ns)); continue
        m = MAP_CANCEL_RE.match(name)
        if m and int(m.group(3)) == CANCEL_RATE:
            rows.append(("cancel", "std::map", int(m.group(1)), m.group(2), ns)); continue
    return rows


def plot_workload(rows, workload, title, out_path, log_y=False, kinds=None):
    fig, ax = plt.subplots(figsize=(8, 5))

    for (kind, dist), (color, marker, linestyle) in STYLE.items():
        if kinds is not None and kind not in kinds:
            continue
        pts = sorted((r[2], r[4]) for r in rows if r[0] == workload and r[1] == kind and r[3] == dist)
        if not pts:
            continue
        ax.plot([p[0] for p in pts], [p[1] for p in pts],
                color=color, marker=marker, linestyle=linestyle, label=f"{kind} {dist}")

    ax.set_xscale("log", base=2)
    if log_y:
        ax.set_yscale("log")
    ax.set_xlabel("Price levels (log2)")
    ax.set_ylabel("Latency (ns)")
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()

    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def plot_order_types(benchmarks, out_dir):
    labels = ["LimitGTC", "Market", "LimitIOC", "LimitFOK", "PostOnly"]
    rows = []
    for b in benchmarks:
        m = ORDER_TYPE_RE.match(b["name"])
        if m:
            rows.append({"dist": m.group(3), "type": int(m.group(4)), "ns": cpu_ns(b)})

    dists = sorted({r["dist"] for r in rows})
    fig, axes = plt.subplots(1, len(dists), figsize=(6 * len(dists), 4.5), sharey=True)
    if len(dists) == 1:
        axes = [axes]

    for ax, dist in zip(axes, dists):
        values = {r["type"]: r["ns"] for r in rows if r["dist"] == dist}
        ys = [values.get(i, 0.0) for i in range(len(labels))]
        ax.bar(labels, ys)
        ax.set_title(f"Order types - {dist}")
        ax.grid(True, axis="y", alpha=0.3)
        for i, v in enumerate(ys):
            ax.text(i, v, f"{v:.1f}", ha="center", va="bottom", fontsize=8)

    axes[0].set_ylabel("addOrder latency (ns)")
    axes[0].set_ylim(bottom=0)
    fig.tight_layout()
    fig.savefig(out_dir / "order_type.png", dpi=150)
    plt.close(fig)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    path = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else path.parent

    benchmarks = load(path)
    rows = collect(benchmarks)

    has_map = any(r[1] == "std::map" for r in rows)
    suffix = " vs std::map" if has_map else ""

    plot_workload(rows, "add", f"addOrder latency vs price range (FlatBook{suffix})",
                  out_dir / "add_vs_price_levels.png")
    plot_workload(rows, "rest", f"Resting-only add vs price range (FlatBook{suffix})",
                  out_dir / "rest_vs_price_levels.png", kinds={"FlatBook", "std::map"})
    plot_workload(rows, "cancel", f"add/cancel mix ({CANCEL_RATE}% cancel) vs price range",
                  out_dir / "cancel_vs_price_levels.png")

    has_cpptrader = any(r[0] == "rest" and r[1] == "CppTrader" for r in rows)
    if has_cpptrader:
        plot_workload(rows, "rest", "Resting-only add vs price range (CppTrader optimized baseline)",
                      out_dir / "cpptrader_vs_price_levels.png", log_y=True)

    names = ["add_vs_price_levels.png", "rest_vs_price_levels.png", "cancel_vs_price_levels.png"]
    if has_cpptrader:
        names.append("cpptrader_vs_price_levels.png")
    for name in names:
        print(f"wrote {out_dir / name}")

    if any(ORDER_TYPE_RE.match(b["name"]) for b in benchmarks):
        plot_order_types(benchmarks, out_dir)
        print(f"wrote {out_dir / 'order_type.png'}")


if __name__ == "__main__":
    main()

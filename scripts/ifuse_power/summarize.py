#!/usr/bin/env python3
"""Summarize per_simpoint.csv into per-app and average power/energy tables.

Energy of a run = (runtime dynamic + subthreshold + gate leakage) x time,
with time = cycles / 3.2 GHz. Every run executes the same instructions
(20M warmup + 10M measured), so energy ratios are energy-per-instruction
ratios. Per-app values are SimPoint-weighted; averages are geometric means
of per-app ratios.
"""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path

FREQ = 3.2e9
APPS = ["bfs", "dfs", "pagerank", "corebench", "appworld", "terminal_bench",
        "cachebench", "clickhouse", "duckdb", "leveldb", "memcached"]
RUNS = [("baseline", "nominal"), ("ifuse", "nominal"),
        ("ifuse", "conservative"), ("ifuse-1c", "nominal"),
        ("ifuse-1c", "conservative")]


def geomean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.out / "per_simpoint.csv")))
    # app -> run -> weighted sums
    agg = defaultdict(lambda: defaultdict(lambda: defaultdict(float)))
    for r in rows:
        key = (r["config"], r["variant"])
        w = float(r["weight"])
        t = float(r["cycles"]) / FREQ
        f = lambda k: float(r[k]) if r.get(k) else 0.0
        core_e = (f("core_runtime_dynamic") + f("core_subthreshold_leakage") +
                  f("core_gate_leakage")) * t
        ifuse_e = (f("ifuse_runtime_dynamic") + f("ifuse_subthreshold_leakage") +
                   f("ifuse_gate_leakage")) * t
        a = agg[r["app"]][key]
        a["w"] += w
        a["time"] += w * t
        a["core_energy"] += w * core_e
        a["ifuse_energy"] += w * ifuse_e
        a["core_dyn_energy"] += w * f("core_runtime_dynamic") * t
        a["core_area"] = f("core_area")
        a["ifuse_area"] = f("ifuse_area")
        a["core_peak"] = (f("core_peak_dynamic") if r.get("core_peak_dynamic")
                          else 0.0)

    # Normalize weights (some apps have simpoints without results).
    for app in agg:
        for key, a in agg[app].items():
            for k in ["time", "core_energy", "ifuse_energy", "core_dyn_energy"]:
                a[k] /= a["w"]

    out_rows = []
    for app in APPS:
        base = agg[app][("baseline", "nominal")]
        for key in RUNS:
            a = agg[app].get(key)
            if not a or not base:
                continue
            out_rows.append({
                "app": app, "config": key[0], "l1d_bound": key[1],
                "speedup": base["time"] / a["time"],
                "core_energy_rel": a["core_energy"] / base["core_energy"],
                "core_avg_power_W": a["core_energy"] / a["time"],
                "core_avg_power_rel": (a["core_energy"] / a["time"]) /
                                      (base["core_energy"] / base["time"]),
                "ifuse_share_of_core_energy": a["ifuse_energy"] /
                                              a["core_energy"],
                "ifuse_avg_power_W": a["ifuse_energy"] / a["time"],
            })
    with open(args.out / "per_app.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(out_rows[0]))
        w.writeheader()
        w.writerows(out_rows)

    # Averages.
    print(f"{'config':10s} {'L1D bound':12s} {'speedup':>8s} "
          f"{'core E':>8s} {'core P':>8s} {'I-Fuse P (W)':>13s} "
          f"{'I-Fuse % of core E':>19s}")
    summary = []
    for key in RUNS:
        sel = [r for r in out_rows if (r["config"], r["l1d_bound"]) == key]
        s = {"config": key[0], "l1d_bound": key[1],
             "speedup_gmean": geomean([r["speedup"] for r in sel]),
             "core_energy_rel_gmean": geomean([r["core_energy_rel"] for r in sel]),
             "core_avg_power_rel_gmean": geomean([r["core_avg_power_rel"]
                                                  for r in sel]),
             "ifuse_avg_power_W_mean": sum(r["ifuse_avg_power_W"]
                                           for r in sel) / len(sel),
             "ifuse_share_of_core_energy_mean": sum(
                 r["ifuse_share_of_core_energy"] for r in sel) / len(sel)}
        summary.append(s)
        print(f"{key[0]:10s} {key[1]:12s} {s['speedup_gmean']:8.3f} "
              f"{s['core_energy_rel_gmean']:8.3f} "
              f"{s['core_avg_power_rel_gmean']:8.3f} "
              f"{s['ifuse_avg_power_W_mean']:13.4f} "
              f"{100 * s['ifuse_share_of_core_energy_mean']:18.2f}%")
    with open(args.out / "summary.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(summary[0]))
        w.writeheader()
        w.writerows(summary)

    # Area.
    any_if = next(a for app in agg for k, a in agg[app].items()
                  if k[0] != "baseline")
    base_area = agg[APPS[0]][("baseline", "nominal")]["core_area"]
    print(f"\nCore area: baseline {base_area:.3f} mm^2, with I-Fuse "
          f"{any_if['core_area']:.3f} mm^2; I-Fuse tables "
          f"{any_if['ifuse_area']:.4f} mm^2 "
          f"({100 * any_if['ifuse_area'] / base_area:.2f}% of baseline core)")


if __name__ == "__main__":
    main()

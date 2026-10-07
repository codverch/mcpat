#!/usr/bin/env python3
"""McPAT power, area, and timing for baseline, I-Fuse, and 1-cycle-delayed I-Fuse.

Reads each Scarab run's mcpat_infile.xml and ifuse.stat.0.out from git, adds
an IFuse component with the run's table activity, charges each fused LD2 for
the work Scarab's power interface drops, runs McPAT, and aggregates the
results by SimPoint weight.

Fused LD2 add-backs (Scarab removes fused LD2s from every core counter):
  decode         total_instructions, int_instructions        +F
  rename         rename_reads +2F, rename_writes +F
  address check  int_regfile_reads +F, ialu_accesses +F (LD2 address for ACI)
  LD2 address    ialu_accesses +P (LD1 adds the delta, one per prediction)
  writeback      int_regfile_writes +F, cdb_alu_accesses +F
  wakeup         inst_window_wakeup_accesses +F
  L1D            dcache.read_accesses +F (conservative bound only)
where F is IFUSE_FUSED_LOADS and P is IFUSE_LOAD1_PREDICTIONS.
"""

import argparse
import csv
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SCARAB = Path("/users/deepmish/scarab")
GRAPHS = Path("/users/deepmish/scarab-infra/hpca2027-main-graphs")
TRACE_ROOT = Path("/dev/shm/ifuse")

MAIN = "hpca2027-revision-main"
REV_IFUSE = "hpca2027-revision-ifuse"
MAIN_DIR = "src/hpca2027-revision-main-results/datacenter"

# Config name -> (branch, run directory, has iFUSE hardware).
CONFIGS = {
    "baseline": (MAIN, f"{MAIN_DIR}/baseline", False),
    "ifuse": (MAIN, f"{MAIN_DIR}/ifuse", True),
    "ifuse-1c": (REV_IFUSE, "src/simulations/1-cycle-delayed-ifuse", True),
}
# Table activity comes from the delayed run for both I-Fuse configs; the
# no-delay run did not save ifuse.stat. Counts differ by <0.1% between delays.
IFUSE_STAT_SRC = (REV_IFUSE, "src/simulations/1-cycle-delayed-ifuse")

APPS = ["bfs", "dfs", "pagerank", "corebench", "appworld", "terminal_bench",
        "cachebench", "clickhouse", "duckdb", "leveldb", "memcached"]


def git_show(branch, path):
    r = subprocess.run(["git", "-C", str(SCARAB), "show", f"{branch}:{path}"],
                       capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def git_ls(branch, path):
    r = subprocess.run(["git", "-C", str(SCARAB), "ls-tree", "--name-only",
                        f"{branch}:{path}"], capture_output=True, text=True)
    return r.stdout.split() if r.returncode == 0 else []


def read_ifuse_stat(text):
    """Cumulative column (warmup + measured), matching the XML's window."""
    stats = {}
    for line in text.splitlines():
        t = line.split()
        if len(t) >= 3 and re.match(r"^[A-Z][A-Z0-9_]+$", t[0]):
            try:
                stats[t[0]] = float(t[2].replace(",", ""))
            except ValueError:
                pass
    return stats


def table_activity(s):
    g = lambda k: s.get(k, 0.0)
    fused = g("IFUSE_FUSED_LOADS")
    return {
        "fct": (g("FCT_LOOKUP_HITS") + g("FCT_LOOKUP_MISSES"),
                g("FCT_RUNTIME_INSERTS") + g("FCT_RUNTIME_REPLACEMENTS") +
                g("FCT_RUNTIME_REINFORCEMENTS") +
                g("IFUSE_CORRECT_PREDICTIONS") +
                g("IFUSE_INCORRECT_PREDICTIONS")),
        # Lookups at decode, plus LD2 reading its register at rename. Writes:
        # insert, LD1 writing the register at rename, LD2 clearing the entry.
        "apt": (g("APT_LOOKUP_HITS") + g("APT_LOOKUP_MISSES") + fused,
                2 * g("APT_INSERTS") + g("APT_LOOKUP_HITS")),
        "aci": (g("ACI_LOOKUP_HITS") + g("ACI_LOOKUP_MISSES"),
                g("ACI_PREDICTION_INSERTS") + g("ACI_REPLAYED_INSERTS")),
        "tt": (g("TRAINING_TABLE_LOOKUPS"),
               g("TRAINING_TABLE_OBSERVATIONS") + g("TRAINING_TABLE_INSERTS")),
        # Every retired load looks up and inserts; every retired store probes.
        "rlb": (g("IFUSE_ALL_LOADS") + g("IFUSE_TRAINING_STORE_INVALIDATIONS"),
                g("IFUSE_ALL_LOADS")),
    }


def set_stat(xml, comp, name, fn):
    """Apply fn to <stat name=...> inside component id=comp (first match)."""
    start = xml.index(f'<component id="{comp}"')
    pat = re.compile(r'(<stat name="%s"\s+value=")([^"]*)(")' % re.escape(name))
    m = pat.search(xml, start)
    new = fn(float(m.group(2)))
    return xml[:m.start(2)] + f"{new:.0f}" + xml[m.end(2):]


def ifuse_block(act):
    lines = ['\t\t<param name="number_of_ifuse" value="1"/>',
             '\t\t<component id="system.core0.IFuse" name="IFuse">',
             '\t\t\t<param name="enabled" value="1"/>']
    for t, (r, w) in act.items():
        lines.append(f'\t\t\t<stat name="{t}_read_accesses" value="{r:.0f}"/>')
        lines.append(f'\t\t\t<stat name="{t}_write_accesses" value="{w:.0f}"/>')
    lines.append('\t\t</component>')
    return "\n".join(lines) + "\n"


def make_xml(base_xml, stats, l1d_extra):
    f = stats.get("IFUSE_FUSED_LOADS", 0.0)
    p = stats.get("IFUSE_LOAD1_PREDICTIONS", 0.0)
    c = "system.core0"
    x = base_xml
    for name, add in [("total_instructions", f), ("int_instructions", f),
                      ("rename_reads", 2 * f), ("rename_writes", f),
                      ("int_regfile_reads", f), ("int_regfile_writes", f),
                      ("ialu_accesses", f + p), ("cdb_alu_accesses", f),
                      ("inst_window_wakeup_accesses", f)]:
        x = set_stat(x, c, name, lambda v, a=add: v + a)
    if l1d_extra:
        x = set_stat(x, "system.core0.dcache", "read_accesses",
                     lambda v: v + f)
    # Insert the IFuse component right before the BTB component.
    i = x.index('<component id="system.core0.BTB"')
    i = x.rindex("\n", 0, i) + 1
    return x[:i] + ifuse_block(table_activity(stats)) + x[i:]


def parse_mcpat(text):
    """Core and processor totals plus per-table I-Fuse rows."""
    out = {}

    def block(title):
        i = text.find(title)
        return text[i:] if i >= 0 else ""

    num = r"([-+0-9.eE]+)"
    for key, title in [("proc", "Processor:"), ("core", "Core:")]:
        b = block(title)
        for field in ["Area", "Peak Power", "Peak Dynamic",
                      "Subthreshold Leakage", "Gate Leakage",
                      "Runtime Dynamic"]:
            m = re.search(r"\n\s*%s = %s" % (re.escape(field), num), b)
            if m:
                out[f"{key}_{field.lower().replace(' ', '_')}"] = float(m.group(1))
    b = block("I-Fuse:")
    if b:
        m = re.search(r"Area = %s" % num, b)
        out["ifuse_area"] = float(m.group(1))
        for field in ["Peak Dynamic", "Subthreshold Leakage", "Gate Leakage",
                      "Runtime Dynamic"]:
            m = re.search(r"\n\s*%s = %s" % (re.escape(field), num), b)
            out[f"ifuse_{field.lower().replace(' ', '_')}"] = float(m.group(1))
        tables = re.findall(
            r"  (\S[^\n]*\(([A-Z]+)\)):\n\s*Organization = ([^\n]*)\n"
            r"\s*Storage = (\d+) bits\n\s*Area = %s mm\^2\n"
            r"\s*Access time = %s ps \(([0-9.]+) cycles\)\n"
            r"\s*Cycle time = %s ps\n\s*Read energy = %s pJ\n"
            r"\s*Write energy = %s pJ\n\s*Leakage \(sub\+gate\) = %s mW"
            % (num, num, num, num, num, num), b)
        out["tables"] = [
            {"table": t[1], "organization": t[2], "storage_bits": int(t[3]),
             "area_mm2": float(t[4]), "access_ps": float(t[5]),
             "access_cycles": float(t[6]), "cycle_ps": float(t[7]),
             "read_pj": float(t[8]), "write_pj": float(t[9]),
             "leak_mw": float(t[10])} for t in tables]
    return out


def total_cycles(xml):
    return float(re.search(r'<stat name="total_cycles"\s+value="([0-9.]+)"',
                           xml).group(1))


def run_one(job, mcpat, workdir):
    cfg, variant, app, sp = job
    branch, rdir, has_ifuse = CONFIGS[cfg]
    xml = git_show(branch, f"{rdir}/{app}/{sp}/mcpat_infile.xml")
    if xml is None:
        return None
    if has_ifuse:
        st = git_show(IFUSE_STAT_SRC[0],
                      f"{IFUSE_STAT_SRC[1]}/{app}/{sp}/ifuse.stat.0.out")
        if st is None:
            return None
        xml = make_xml(xml, read_ifuse_stat(st), variant == "conservative")
    d = workdir / cfg / variant / app / sp
    d.mkdir(parents=True, exist_ok=True)
    (d / "mcpat_infile.xml").write_text(xml)
    r = subprocess.run([str(mcpat), "-infile", str(d / "mcpat_infile.xml"),
                        "-print_level", "5"], capture_output=True, text=True)
    (d / "mcpat.out").write_text(r.stdout)
    res = parse_mcpat(r.stdout)
    res["cycles"] = total_cycles(xml)
    return job, res


def load_weights():
    sys.path.insert(0, str(GRAPHS))
    import plot_ipc
    return plot_ipc.load_simpoint_trace_weights(TRACE_ROOT, APPS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcpat", type=Path,
                    default=Path(__file__).resolve().parents[2] / "mcpat")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("-j", type=int, default=16)
    args = ap.parse_args()

    weights = load_weights()
    jobs = []
    for cfg, (branch, rdir, has_ifuse) in CONFIGS.items():
        variants = ["conservative", "nominal"] if has_ifuse else ["nominal"]
        for app in APPS:
            for sp in git_ls(branch, f"{rdir}/{app}"):
                if weights.get((app, sp), 0) > 0:
                    for v in variants:
                        jobs.append((cfg, v, app, sp))

    with ThreadPoolExecutor(args.j) as ex:
        results = [r for r in ex.map(
            lambda j: run_one(j, args.mcpat, args.out / "runs"), jobs) if r]

    # Per-simpoint CSV.
    fields = ["config", "variant", "app", "simpoint", "weight", "cycles",
              "core_area", "core_runtime_dynamic", "core_subthreshold_leakage",
              "core_gate_leakage", "core_peak_power", "ifuse_area",
              "ifuse_runtime_dynamic", "ifuse_subthreshold_leakage",
              "ifuse_gate_leakage", "ifuse_peak_dynamic"]
    with open(args.out / "per_simpoint.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        for (cfg, v, app, sp), res in results:
            w.writerow({"config": cfg, "variant": v, "app": app,
                        "simpoint": sp, "weight": weights[(app, sp)], **res})

    # Per-table rows (identical across runs; take the first I-Fuse run).
    for (cfg, v, app, sp), res in results:
        if res.get("tables"):
            with open(args.out / "ifuse_tables.csv", "w", newline="") as fh:
                w = csv.DictWriter(fh, fieldnames=list(res["tables"][0]))
                w.writeheader()
                w.writerows(res["tables"])
            break
    print(f"{len(results)} McPAT runs written to {args.out}")


if __name__ == "__main__":
    main()

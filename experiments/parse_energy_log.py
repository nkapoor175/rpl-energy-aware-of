#!/usr/bin/env python3
"""Extract the energy-OF debug records from a Cooja log into CSV files.

Usage: python3 parse_energy_log.py LOG_FILE

Reads BOTH log formats:
  * headless run logs (COOJA.testlog / results/.../cooja.log):  "<microseconds> node-<id> <message>"
  * the Cooja GUI "Mote output" copy:                          "mm:ss.fff  ID:<id>  <message>"

Writes next to the log: <log>_depletion.csv, _adaptive.csv, _energy.csv, _parent_switches.csv
and _summary.csv. The depletion/adaptive records need RPL logging at DBG level
(#define LOG_CONF_LEVEL_RPL LOG_LEVEL_DBG); parent switches need at least INFO.
The ENERGY lines are always present. For the per-run metrics (lifetimes, delivery,
latency, energy balance) use parse_run.py instead.
"""
import csv
import re
import sys
from pathlib import Path

# "<us> node-<id> ...."  or  "mm:ss.fff ... ID:<id> ...."
PREFIX = (r"(?:(?P<us>\d+)\s+node-(?P<node_h>\d+)\s+.*?"
          r"|(?P<ts>\d+:\d{2}(?::\d{2})?\.\d+)\s.*?ID:\s*(?P<node_g>\d+)\s.*?)")

BODIES = {
    # full-window estimate (valid=1) and the "not enough history yet" form (valid=0)
    "depletion": (r"\[DEPLETION\]\s+(?:old=(?P<old>\d+)\s+new=(?P<new>\d+)\s+"
                  r"drop=(?P<drop>-?\d+)\s+dt_s=(?P<dt_s>\d+)\s+rate_milli=(?P<rate_milli>-?\d+)\s+"
                  r"samples=(?P<samples>\d+)\s+valid=(?P<valid>\d+)"
                  r"|samples=(?P<samples0>\d+)\s+valid=(?P<valid0>\d+))"),
    "adaptive": (r"\[ADAPTIVE\]\s+spread=(?P<spread>-?\d+)\s+ETX=(?P<etx>\d+)\s+"
                 r"ENERGY=(?P<energy>\d+)\s+HOP=(?P<hop>\d+)"),
    "energy": (r"ENERGY\s+id=(?P<id>\d+)\s+time=(?P<time>\d+)\s+"
               r"consumed_uj=(?P<consumed_uj>\d+)\s+capacity_uj=(?P<capacity_uj>\d+)\s+"
               r"remaining=(?P<remaining>\d+)"),
    "parent_switches": (r"parent switch:\s*(?P<old_parent>.+?)\s*->\s*(?P<new_parent>.+?)\s*$"),
}
PATTERNS = {name: re.compile(PREFIX + body) for name, body in BODIES.items()}
INT_FIELDS = {"node", "old", "new", "drop", "dt_s", "rate_milli", "samples", "valid",
              "spread", "etx", "energy", "hop", "id", "time", "consumed_uj",
              "capacity_uj", "remaining"}


def normalise(d):
    """Unify the two log formats into node + log_time_s columns."""
    row = {}
    row["node"] = d.pop("node_h") or d.pop("node_g")
    d.pop("node_g", None)
    d.pop("node_h", None)
    us, ts = d.pop("us"), d.pop("ts")
    if us is not None:
        row["log_time_s"] = round(int(us) / 1e6, 3)
    else:
        parts = [float(x) for x in ts.split(":")]
        row["log_time_s"] = round(sum(p * 60 ** (len(parts) - 1 - i) for i, p in enumerate(parts)), 3)
    # the depletion "not enough history" alternative uses suffixed group names
    if d.get("samples") is None and d.get("samples0") is not None:
        d["samples"], d["valid"] = d["samples0"], d["valid0"]
    d.pop("samples0", None)
    d.pop("valid0", None)
    row.update({k: v for k, v in d.items() if v is not None})
    return row


def main():
    if len(sys.argv) != 2:
        sys.exit("Usage: python3 parse_energy_log.py LOG_FILE")
    log_path = Path(sys.argv[1])
    if not log_path.is_file():
        sys.exit("Log file not found: %s" % log_path)

    records = {name: [] for name in PATTERNS}
    deaths = 0
    with log_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if "NODE_DEAD" in line:
                deaths += 1
            for name, pat in PATTERNS.items():
                m = pat.search(line)
                if not m:
                    continue
                row = normalise(m.groupdict())
                if name == "energy":      # the message carries the node id (id=) and its own time (s)
                    row["node"] = row.pop("id", row["node"])
                for k in INT_FIELDS & row.keys():
                    row[k] = int(row[k])
                records[name].append(row)
                break

    base = log_path.with_suffix("")
    for name, rows in records.items():
        if not rows:
            print("%s: no matching records" % name)
            continue
        columns = []
        for r in rows:
            for k in r:
                if k not in columns:
                    columns.append(k)
        out = Path("%s_%s.csv" % (base, name))
        with out.open("w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=columns, restval="")
            w.writeheader()
            w.writerows(rows)
        print("%s: %d records -> %s" % (name, len(rows), out.name))

    summary = Path("%s_summary.csv" % base)
    with summary.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["metric", "count"])
        for name, rows in records.items():
            w.writerow([name, len(rows)])
        w.writerow(["NODE_DEAD", deaths])
    print("Summary -> %s" % summary.name)
    print("Node-death events: %d" % deaths)


if __name__ == "__main__":
    main()

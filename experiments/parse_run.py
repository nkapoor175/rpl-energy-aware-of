#!/usr/bin/env python3
"""Per-run metrics from a headless Cooja log of a lifetime experiment.

Usage:
    parse_run.py RUN_DIR [RUN_DIR ...]      one or more results/<name>/<date>/N<n>_s<seed>_<of>/
    parse_run.py --all RESULTS_ROOT         every run folder below RESULTS_ROOT, plus a combined all_runs.csv

Each run folder must contain cooja.log (written by run_lifetime.sh); summary.txt is
used for metadata when present. Written into the run folder:

    run_metrics.csv   one row per run (see COLUMNS)
    node_metrics.csv  one row per client: death time, requests, responses, latency, last energy report
    energy_series.csv every ENERGY report (node, time_s, consumed_uj, capacity_uj, remaining)

Log lines understood (all start with "<simulated microseconds> node-<id> "):
    [INFO: App ] Sending request K to ...            client sends request K
    [INFO: App ] Received request 'hello K' from fd00::20X:X:X:X   the root received it (2XX = client id as two hex digits)
    [INFO: App ] Received response 'hello K' from ...  the client got the answer
    ENERGY id= time= consumed_uj= capacity_uj= remaining=   every 60 s per client
    NODE_DEAD id= time=
    [INFO: RPL ] parent switch: A -> B               only when RPL logging is at INFO or DBG
    LIFETIME nodes= clients= dead= cutoff= FND_s= HND_s= LND_s=   (no prefix)
    EXPERIMENT_OF=n

Definitions (match CLAUDE.md):
    pdr_request    = requests the root received / requests the clients sent
    pdr_roundtrip  = responses the clients received / requests the clients sent
    latency        = one-way time from the client's "Sending request K" to the root's
                     "Received request K" (end-to-end, sensor send -> root receive)
    rtt            = client send -> client receives the response
    energy columns = consumed energy of every client at the reference time (the first death,
                     FND, or the last report if nobody died): mean / max / min / std. The max is
                     the worst-case node; the std shows how uneven the drain is. After a node
                     dies its consumption equals its capacity, so compare at FND, not at the end.
"""
import csv
import math
import re
import statistics
import sys
from pathlib import Path

LINE = re.compile(r"^(\d+)\s+node-(\d+)\s+(.*)$")
SEND = re.compile(r"Sending request (\d+) to")
RX_REQ = re.compile(r"Received request 'hello (\d+)' from fd00::2([0-9a-fA-F]{2}):")
RX_RESP = re.compile(r"Received response 'hello (\d+)' from")
ENERGY = re.compile(r"ENERGY id=(\d+) time=(\d+) consumed_uj=(\d+) capacity_uj=(\d+) remaining=(\d+)")
DEAD = re.compile(r"NODE_DEAD id=(\d+) time=(\d+)")
SWITCH = re.compile(r"parent switch:")
LIFETIME = re.compile(
    r"LIFETIME nodes=(\d+) clients=(\d+) dead=(\d+) cutoff=(\d) "
    r"FND_s=(\S+) HND_s=(\S+) LND_s=(\S+)")
OFMARK = re.compile(r"EXPERIMENT_OF=(\d)")
OF_LABEL = {"0": "energy", "1": "mrhof", "2": "of0"}

COLUMNS = [
    "run", "nodes", "seed", "of_code", "of_label", "clients", "dead", "cutoff",
    "FND_s", "HND_s", "LND_s",
    "requests_sent", "requests_at_root", "responses_at_clients",
    "pdr_request", "pdr_roundtrip",
    "latency_mean_ms", "latency_p95_ms", "rtt_mean_ms",
    "energy_ref_s", "consumed_mean_uj", "consumed_max_uj", "consumed_min_uj", "consumed_std_uj",
    "capacity_uj", "parent_switches", "parent_switch_logging",
]


def pct(sorted_vals, p):
    if not sorted_vals:
        return None
    k = max(0, min(len(sorted_vals) - 1, math.ceil(p / 100.0 * len(sorted_vals)) - 1))
    return sorted_vals[k]


def na(v):
    return "NA" if v is None else v


def parse_summary(run_dir):
    """nodes / seed / OF code / variant label, from summary.txt or the folder name."""
    meta = {}
    s = run_dir / "summary.txt"
    if s.is_file():
        m = re.search(r"nodes=(\d+) seed=(\d+) of=(\d)(?: \((\w+)\))?", s.read_text(errors="replace"))
        if m:
            meta = {"nodes": m.group(1), "seed": m.group(2), "of_code": m.group(3),
                    "label": m.group(4) or ""}
    if not meta:
        m = re.match(r"N(\d+)_s(\d+)_(\w+)", run_dir.name)
        if m:
            label = m.group(3).split("_")[0]
            code = {"mrhof": "1", "of0": "2"}.get(label, "0")
            meta = {"nodes": m.group(1), "seed": m.group(2), "of_code": code, "label": label}
    return meta


def parse_run(run_dir):
    run_dir = Path(run_dir)
    log = run_dir / "cooja.log"
    if not log.is_file():
        raise SystemExit("no cooja.log in %s" % run_dir)

    sent = {}            # (client, k) -> us
    got_root = {}        # (client, k) -> us
    rtt = {}             # (client, k) -> us
    deaths = {}          # client -> seconds
    energy = {}          # client -> [(time_s, consumed_uj, capacity_uj, remaining)]
    switches = 0
    rpl_info_seen = False
    lifetime = None
    ofmark = None

    with log.open("r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            if raw.startswith("LIFETIME"):
                m = LIFETIME.search(raw)
                if m:
                    lifetime = m.groups()
                continue
            m = LINE.match(raw)
            if not m:
                continue
            us, node, msg = int(m.group(1)), int(m.group(2)), m.group(3)
            if "[INFO: App" in msg:
                if "Sending request" in msg:
                    mm = SEND.search(msg)
                    if mm:
                        sent[(node, int(mm.group(1)))] = us
                elif "Received request" in msg:
                    mm = RX_REQ.search(msg)
                    if mm:
                        got_root[(int(mm.group(2), 16), int(mm.group(1)))] = us
                elif "Received response" in msg:
                    mm = RX_RESP.search(msg)
                    if mm and (node, int(mm.group(1))) in sent:
                        rtt[(node, int(mm.group(1)))] = us - sent[(node, int(mm.group(1)))]
            elif msg.startswith("ENERGY id="):
                mm = ENERGY.search(msg)
                if mm:
                    i, t, c, cap, rem = map(int, mm.groups())
                    energy.setdefault(i, []).append((t, c, cap, rem))
            elif msg.startswith("NODE_DEAD"):
                mm = DEAD.search(msg)
                if mm:
                    deaths[int(mm.group(1))] = int(mm.group(2))
            elif "[INFO: RPL" in msg or "[DBG : RPL" in msg:
                rpl_info_seen = True
                if SWITCH.search(msg):
                    switches += 1
            elif "EXPERIMENT_OF=" in msg and ofmark is None:
                mm = OFMARK.search(msg)
                if mm:
                    ofmark = mm.group(1)

    meta = parse_summary(run_dir)
    of_code = meta.get("of_code") or ofmark or ""
    nodes = meta.get("nodes") or (lifetime[0] if lifetime else "")
    clients = int(lifetime[1]) if lifetime else (int(nodes) - 1 if nodes else len(energy))

    # delivery and latency
    n_sent = len(sent)
    at_root = [k for k in sent if k in got_root]
    lat_ms = sorted((got_root[k] - sent[k]) / 1000.0 for k in at_root)
    rtt_ms = [v / 1000.0 for v in rtt.values()]

    # energy at the reference time
    if lifetime and lifetime[4] != "NA":
        ref = int(lifetime[4])
    elif deaths:
        ref = min(deaths.values())
    else:
        ref = max((s[-1][0] for s in energy.values()), default=0)
    consumed = []
    capacity = None
    for i, series in energy.items():
        upto = [s for s in series if s[0] <= ref] or series[:1]
        consumed.append(upto[-1][1])
        capacity = upto[-1][2]

    row = {
        "run": run_dir.name, "nodes": nodes, "seed": meta.get("seed", ""),
        "of_code": of_code, "of_label": meta.get("label") or OF_LABEL.get(of_code, ""),
        "clients": clients,
        "dead": lifetime[2] if lifetime else len(deaths),
        "cutoff": lifetime[3] if lifetime else "NA",
        "FND_s": lifetime[4] if lifetime else "NA",
        "HND_s": lifetime[5] if lifetime else "NA",
        "LND_s": lifetime[6] if lifetime else "NA",
        "requests_sent": n_sent,
        "requests_at_root": len(at_root),
        "responses_at_clients": len(rtt),
        "pdr_request": na(round(len(at_root) / n_sent, 5) if n_sent else None),
        "pdr_roundtrip": na(round(len(rtt) / n_sent, 5) if n_sent else None),
        "latency_mean_ms": na(round(statistics.mean(lat_ms), 3) if lat_ms else None),
        "latency_p95_ms": na(round(pct(lat_ms, 95), 3) if lat_ms else None),
        "rtt_mean_ms": na(round(statistics.mean(rtt_ms), 3) if rtt_ms else None),
        "energy_ref_s": ref,
        "consumed_mean_uj": na(round(statistics.mean(consumed)) if consumed else None),
        "consumed_max_uj": na(max(consumed) if consumed else None),
        "consumed_min_uj": na(min(consumed) if consumed else None),
        "consumed_std_uj": na(round(statistics.pstdev(consumed)) if len(consumed) > 1 else None),
        "capacity_uj": na(capacity),
        "parent_switches": switches if rpl_info_seen else "NA",
        "parent_switch_logging": 1 if rpl_info_seen else 0,
    }

    with (run_dir / "run_metrics.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        w.writeheader()
        w.writerow(row)

    with (run_dir / "node_metrics.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["node", "died_s", "requests_sent", "requests_at_root", "responses",
                    "latency_mean_ms", "last_report_s", "last_consumed_uj", "last_remaining"])
        ids = sorted(set(i for i, _ in sent) | set(energy) | set(deaths))
        for i in ids:
            ks = [k for k in sent if k[0] == i]
            ar = [k for k in ks if k in got_root]
            lm = [(got_root[k] - sent[k]) / 1000.0 for k in ar]
            last = energy.get(i, [(None, None, None, None)])[-1]
            w.writerow([i, deaths.get(i, "NA"), len(ks), len(ar),
                        sum(1 for k in ks if k in rtt),
                        round(statistics.mean(lm), 3) if lm else "NA",
                        na(last[0]), na(last[1]), na(last[3])])

    with (run_dir / "energy_series.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["node", "time_s", "consumed_uj", "capacity_uj", "remaining"])
        for i in sorted(energy):
            for t, c, cap, rem in energy[i]:
                w.writerow([i, t, c, cap, rem])

    return row


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == "--all":
        root = Path(argv[2])
        runs = sorted(p.parent for p in root.rglob("cooja.log"))
        rows = []
        for r in runs:
            rows.append(parse_run(r))
            print("parsed", r)
        out = root / "all_runs.csv"
        with out.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=COLUMNS)
            w.writeheader()
            w.writerows(rows)
        print("combined ->", out, "(%d runs)" % len(rows))
    else:
        for r in argv[1:]:
            row = parse_run(r)
            print(r)
            for c in COLUMNS:
                print("  %-22s %s" % (c, row[c]))


if __name__ == "__main__":
    main(sys.argv)


import csv
import re
import sys
from pathlib import Path


PATTERNS = {
    "depletion": re.compile(
        r"(?P<timestamp>\d+:\d{2}(?::\d{2})?\.\d+).*?"
        r"ID:\s*(?P<node>\d+).*?\[DEPLETION\]\s+"
        r"old=(?P<old>\d+)\s+new=(?P<new>\d+)\s+"
        r"drop=(?P<drop>-?\d+)\s+dt_s=(?P<dt>\d+)\s+"
        r"rate_milli=(?P<rate>-?\d+)\s+"
        r"samples=(?P<samples>\d+)\s+valid=(?P<valid>\d+)"
    ),
    "adaptive": re.compile(
        r"(?P<timestamp>\d+:\d{2}(?::\d{2})?\.\d+).*?"
        r"ID:\s*(?P<node>\d+).*?\[ADAPTIVE\]\s+"
        r"spread=(?P<spread>-?\d+)\s+"
        r"ETX=(?P<etx>\d+)\s+ENERGY=(?P<energy>\d+)\s+"
        r"HOP=(?P<hop>\d+)"
    ),
    "energy": re.compile(
        r"(?P<timestamp>\d+:\d{2}(?::\d{2})?\.\d+).*?"
        r"ENERGY\s+id=(?P<node>\d+)\s+"
        r"time=(?P<time>\d+)\s+"
        r"consumed_uj=(?P<consumed_uj>\d+)\s+"
        r"capacity_uj=(?P<capacity_uj>\d+)\s+"
        r"remaining=(?P<remaining>\d+(?:\.\d+)?)"
    ),
    "parent_switches": re.compile(
        r"(?P<timestamp>\d+:\d{2}(?::\d{2})?\.\d+).*?"
        r"ID:\s*(?P<node>\d+).*?parent switch:\s*"
        r"\((?P<old_parent>.*?)\)\s*->\s*(?P<new_parent>\S+)"
    ),
}


NUMERIC_FIELDS = {
    "depletion": {
        "node", "old", "new", "drop", "dt", "rate", "samples", "valid"
    },
    "adaptive": {"node", "spread", "etx", "energy", "hop"},
    "energy": {"node", "time", "consumed_uj", "capacity_uj", "remaining"},
    "parent_switches": {"node"},
}

def main():
    if len(sys.argv) != 2:
        sys.exit("Usage: python3 parse_energy_log.py LOG_FILE")

    log_path = Path(sys.argv[1])
    if not log_path.is_file():
        sys.exit(f"Log file not found: {log_path}")

    records = {name: [] for name in PATTERNS}
    node_death_lines = []

    with log_path.open("r", encoding="utf-8", errors="replace") as file:
        for line in file:
            if "NODE_DEAD" in line:
                node_death_lines.append(line.strip())

            for name, pattern in PATTERNS.items():
                match = pattern.search(line)
                if not match:
                    continue

                row = match.groupdict()
                for field in NUMERIC_FIELDS[name]:
                    row[field] = float(row[field]) if field == "remaining" else int(row[field])
                records[name].append(row)
                break

    base = log_path.with_suffix("")
    for name, rows in records.items():
        if not rows:
            print(f"{name}: no matching records")
            continue

        columns = list(rows[0].keys())
        output = Path(f"{base}_{name}.csv")
        with output.open("w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)
        print(f"{name}: {len(rows)} records -> {output.name}")

    summary_path = Path(f"{base}_summary.csv")
    with summary_path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["metric", "count"])
        for name, rows in records.items():
            writer.writerow([name, len(rows)])
        writer.writerow(["NODE_DEAD", len(node_death_lines)])

    print(f"Summary -> {summary_path.name}")
    print(f"Node-death events: {len(node_death_lines)}")

if __name__ == "__main__":
    main()


import sys
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

if len(sys.argv) != 2:
    sys.exit("Usage: python3 analyze_energy.py ENERGY_CSV")

input_path = Path(sys.argv[1])
df = pd.read_csv(input_path)

required = {"node", "time", "consumed_uj", "capacity_uj", "remaining"}
missing = required - set(df.columns)
if missing:
    sys.exit(f"Missing CSV columns: {sorted(missing)}")

df = df.sort_values(["node", "time"]).copy()

# Interval energy consumption between consecutive reports.
df["interval_consumed_uj"] = (
    df.groupby("node")["consumed_uj"].diff()
)
df["interval_seconds"] = df.groupby("node")["time"].diff()
df["interval_drain_uj_per_s"] = (
    df["interval_consumed_uj"] / df["interval_seconds"]
)

# Per-node summary using the first and last available reports.
summary = (
    df.groupby("node")
    .agg(
        first_time_s=("time", "first"),
        last_time_s=("time", "last"),
        first_consumed_uj=("consumed_uj", "first"),
        last_consumed_uj=("consumed_uj", "last"),
        first_remaining=("remaining", "first"),
        last_remaining=("remaining", "last"),
    )
    .reset_index()
)

summary["duration_s"] = summary["last_time_s"] - summary["first_time_s"]
summary["energy_consumed_uj"] = (
    summary["last_consumed_uj"] - summary["first_consumed_uj"]
)
summary["average_drain_uj_per_s"] = (
    summary["energy_consumed_uj"] / summary["duration_s"]
)

base = input_path.with_suffix("")
df.to_csv(f"{base}_intervals.csv", index=False)
summary.to_csv(f"{base}_node_summary.csv", index=False)

# Plot average energy drain per node.
plot_data = summary.dropna(subset=["average_drain_uj_per_s"])
plt.figure()
plt.bar(
    plot_data["node"].astype(str),
    plot_data["average_drain_uj_per_s"]
)
plt.xlabel("Node ID")
plt.ylabel("Average energy consumption (µJ/s)")
plt.title("Average Energy Drain per Node")
plt.tight_layout()
plt.savefig(f"{base}_drain_by_node.png", dpi=200)
plt.close()

print(summary.to_string(index=False))
print(f"\nSaved: {base}_intervals.csv")
print(f"Saved: {base}_node_summary.csv")
print(f"Saved: {base}_drain_by_node.png")

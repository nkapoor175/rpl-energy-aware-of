#!/bin/bash
# Install this repo's Contiki-NG additions into a Contiki-NG checkout.
# Idempotent: safe to run again after pulling changes.
#
#   experiments/install.sh [CONTIKI_DIR]        (default: ~/contiki-ng)
#
# Copies battery-sim into os/sys/, the energy OF into the rpl-lite module,
# registers the OF in rpl-dag.c (the one-line extern patch), and copies the
# rpl-udp example files (Makefile, project-conf.h, apps) into examples/rpl-udp/.
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CONTIKI="${1:-$HOME/contiki-ng}"

[ -d "$CONTIKI/os/net/routing/rpl-lite" ] || { echo "not a Contiki-NG tree: $CONTIKI"; exit 1; }

cp "$REPO/battery-sim/battery-sim.c" "$REPO/battery-sim/battery-sim.h" "$CONTIKI/os/sys/"
cp "$REPO/of-energy/rpl-energy-of.c" "$REPO/of-energy/rpl-energy-of.h" "$CONTIKI/os/net/routing/rpl-lite/"

DAG="$CONTIKI/os/net/routing/rpl-lite/rpl-dag.c"
if ! grep -q "rpl_energy_of" "$DAG"; then
  sed -i 's/extern rpl_of_t rpl_of0, rpl_mrhof;/extern rpl_of_t rpl_of0, rpl_mrhof, rpl_energy_of;/' "$DAG"
fi
grep -q "rpl_energy_of;" "$DAG" || { echo "rpl-dag.c patch failed"; exit 1; }

for f in Makefile project-conf.h udp-client.c udp-server.c; do
  cp "$REPO/rpl-udp/$f" "$CONTIKI/examples/rpl-udp/$f"
done

echo "installed into $CONTIKI"

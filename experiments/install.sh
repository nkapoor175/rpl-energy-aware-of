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

# Apply the tracked core patch.
# If it is already applied, leave the existing changes untouched.
if git -C "$CONTIKI" apply --check "$REPO/patches/energy-of-core.patch" 2>/dev/null; then
  git -C "$CONTIKI" apply "$REPO/patches/energy-of-core.patch"
  echo "applied energy-of-core.patch"
elif git -C "$CONTIKI" apply --reverse --check "$REPO/patches/energy-of-core.patch" 2>/dev/null; then
  echo "energy-of-core.patch already applied; skipping"
else
  echo "cannot apply energy-of-core.patch cleanly"
  echo "the core files may contain conflicting changes"
  exit 1
fi
for f in Makefile project-conf.h udp-client.c udp-server.c; do
  cp "$REPO/rpl-udp/$f" "$CONTIKI/examples/rpl-udp/$f"
done

echo "installed into $CONTIKI"

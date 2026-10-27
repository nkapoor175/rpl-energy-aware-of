#!/usr/bin/env python3
"""Generate a Cooja scenario (.csc) for the lifetime experiments.

One scenario = (nodes, seed). The topology and the Cooja random seed depend ONLY
on those two numbers, so OF0, MRHOF and our OF are all run on the identical
scenario file (same positions, same traffic, same seed); only EXPERIMENT_OF in
the firmware build differs.

Topology: node 1 is the RPL root (udp-server) at the centre of a square; nodes
2..N are clients (udp-client) placed uniformly at random. The square is sized so
the expected number of neighbours within the radio range is about --avg-degree
(boundary effects make the real average a little lower). Positions are
re-drawn (deterministically from the seed) until the unit-disk graph is
connected, so every node can reach the root.

The embedded Cooja script runs the simulation until the LAST CLIENT dies or
--max-seconds of simulated time pass, whichever comes first, and prints

  LIFETIME nodes=N clients=C dead=K cutoff=0|1 FND_s=.. HND_s=.. LND_s=..

FND = first client death. HND = the ceil(C/2)-th client death ("half dead").
LND = last client death. A metric not reached before the cutoff prints NA.
The root is mains-powered and never dies, so it is not counted.
"""
import argparse
import math
import random
import sys

MOTE_INTERFACES = """      <moteinterface>org.contikios.cooja.interfaces.Position</moteinterface>
      <moteinterface>org.contikios.cooja.interfaces.Battery</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiVib</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiMoteID</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiRS232</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiBeeper</moteinterface>
      <moteinterface>org.contikios.cooja.interfaces.RimeAddress</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiIPAddress</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiRadio</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiButton</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiPIR</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiClock</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiLED</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiCFS</moteinterface>
      <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiEEPROM</moteinterface>
      <moteinterface>org.contikios.cooja.interfaces.Mote2MoteRelations</moteinterface>
      <moteinterface>org.contikios.cooja.interfaces.MoteAttributes</moteinterface>"""

SCRIPT = """/* Run until the last client dies or the cutoff, then report lifetimes.
   Deliberately implicit globals (no "var"/"function" declarations): Cooja
   evaluates the TIMEOUT code outside this script's scope, where local
   declarations are invisible ("finish is not defined"). */
CLIENTS = %(clients)d;
HALF = Math.ceil(CLIENTS / 2);
deaths = [];

pick = function(k) { return deaths.length >= k ? deaths[k - 1] : "NA"; };

finish = function(cutoff) {
  log.log("LIFETIME nodes=" + (CLIENTS + 1) + " clients=" + CLIENTS +
          " dead=" + deaths.length + " cutoff=" + (cutoff ? 1 : 0) +
          " FND_s=" + pick(1) + " HND_s=" + pick(HALF) +
          " LND_s=" + pick(CLIENTS) + "\\n");
  log.testOK();
};

TIMEOUT(%(max_ms)d, finish(true));

while(true) {
  YIELD();
  log.log(time + " node-" + id + " " + msg + "\\n");
  if(msg.indexOf("NODE_DEAD") == 0) {
    deaths.push(Math.round(time / 1000000));
    if(deaths.length >= CLIENTS) {
      finish(false);
    }
  }
}"""


def connected(points, rng_range):
    n = len(points)
    seen = {0}
    stack = [0]
    while stack:
        i = stack.pop()
        for j in range(n):
            if j not in seen and math.dist(points[i], points[j]) <= rng_range:
                seen.add(j)
                stack.append(j)
    return len(seen) == n


def make_topology(nodes, seed, tx_range, avg_degree):
    area_per_node = math.pi * tx_range ** 2 / avg_degree
    side = math.sqrt(nodes * area_per_node)
    rng = random.Random(seed * 100003 + nodes)
    for _ in range(100000):
        pts = [(side / 2.0, side / 2.0)]
        pts += [(rng.uniform(0, side), rng.uniform(0, side)) for _ in range(nodes - 1)]
        if connected(pts, tx_range):
            return pts, side
    sys.exit("could not draw a connected topology for nodes=%d seed=%d" % (nodes, seed))


def motetype(ident, description, source, target):
    return """    <motetype>
      org.contikios.cooja.contikimote.ContikiMoteType
      <identifier>%s</identifier>
      <description>%s</description>
      <source>[CONTIKI_DIR]/%s/%s.c</source>
      <commands>$(MAKE) -j$(CPUS) %s.cooja TARGET=cooja</commands>
%s
    </motetype>
""" % (ident, description, source[0], target, target, MOTE_INTERFACES)


def mote(mote_id, x, y, ident):
    return """    <mote>
      <interface_config>
        org.contikios.cooja.interfaces.Position
        <x>%.2f</x>
        <y>%.2f</y>
        <z>0.0</z>
      </interface_config>
      <interface_config>
        org.contikios.cooja.contikimote.interfaces.ContikiMoteID
        <id>%d</id>
      </interface_config>
      <interface_config>
        org.contikios.cooja.contikimote.interfaces.ContikiRadio
        <bitrate>250.0</bitrate>
      </interface_config>
      <motetype_identifier>%s</motetype_identifier>
    </mote>
""" % (x, y, mote_id, ident)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nodes", type=int, required=True, help="total nodes incl. the root")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--max-seconds", type=int, required=True,
                    help="cutoff in simulated seconds (safety limit, not a design parameter)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--example-dir", default="examples/rpl-udp",
                    help="firmware directory relative to the Contiki-NG root")
    ap.add_argument("--tx-range", type=float, default=50.0)
    ap.add_argument("--interference-range", type=float, default=100.0)
    ap.add_argument("--avg-degree", type=float, default=10.0,
                    help="nominal neighbour density; 10 is the frozen project value (CLAUDE.md)")
    a = ap.parse_args()
    if a.nodes < 3:
        sys.exit("need at least 3 nodes")

    pts, side = make_topology(a.nodes, a.seed, a.tx_range, a.avg_degree)
    clients = a.nodes - 1
    title = "of-energy lifetime: %d nodes, seed %d (square %.0f m)" % (a.nodes, a.seed, side)

    xml = """<?xml version="1.0" encoding="UTF-8"?>
<simconf>
  <simulation>
    <title>%s</title>
    <randomseed>%d</randomseed>
    <motedelay_us>1000000</motedelay_us>
    <radiomedium>
      org.contikios.cooja.radiomediums.UDGM
      <transmitting_range>%.1f</transmitting_range>
      <interference_range>%.1f</interference_range>
      <success_ratio_tx>1.0</success_ratio_tx>
      <success_ratio_rx>1.0</success_ratio_rx>
    </radiomedium>
    <events>
      <logoutput>40000</logoutput>
    </events>
""" % (title, a.seed, a.tx_range, a.interference_range)
    xml += motetype("mtype_server", "RPL root (udp-server)", (a.example_dir,), "udp-server")
    xml += motetype("mtype_client", "RPL node (udp-client)", (a.example_dir,), "udp-client")
    for i, (x, y) in enumerate(pts):
        xml += mote(i + 1, x, y, "mtype_server" if i == 0 else "mtype_client")
    xml += """  </simulation>
  <plugin>
    org.contikios.cooja.plugins.SimControl
    <width>229</width>
    <z>3</z>
    <height>157</height>
    <location_x>11</location_x>
    <location_y>6</location_y>
  </plugin>
  <plugin>
    org.contikios.cooja.plugins.Visualizer
    <plugin_config>
      <moterelations>true</moterelations>
      <skin>org.contikios.cooja.plugins.skins.IDVisualizerSkin</skin>
      <skin>org.contikios.cooja.plugins.skins.UDGMVisualizerSkin</skin>
    </plugin_config>
    <width>400</width>
    <z>1</z>
    <height>400</height>
    <location_x>10</location_x>
    <location_y>170</location_y>
  </plugin>
  <plugin>
    org.contikios.cooja.plugins.ScriptRunner
    <plugin_config>
      <script>%s</script>
      <active>true</active>
    </plugin_config>
    <width>600</width>
    <z>2</z>
    <height>700</height>
    <location_x>710</location_x>
    <location_y>30</location_y>
  </plugin>
</simconf>
""" % (SCRIPT % {"clients": clients, "max_ms": a.max_seconds * 1000})

    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(xml)

    degs = [sum(1 for j, q in enumerate(pts) if j != i and math.dist(p, q) <= a.tx_range)
            for i, p in enumerate(pts)]
    print("wrote %s: %d nodes, seed %d, square %.0f m, neighbours min/avg/max = %d/%.1f/%d"
          % (a.out, a.nodes, a.seed, side, min(degs), sum(degs) / len(degs), max(degs)))


if __name__ == "__main__":
    main()

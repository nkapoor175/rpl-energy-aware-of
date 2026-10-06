# of-energy: integration steps

`rpl-energy-of.c` / `rpl-energy-of.h` implement a custom RPL Lite Objective
Function. They don't live inside `~/contiki-ng` in this repo (that's a
separate clone, not tracked here), so wiring them up is a manual copy +
one-line patch:

1. Copy both files into Contiki-NG:
   ```
   cp of-energy/rpl-energy-of.{c,h} ~/contiki-ng/os/net/routing/rpl-lite/
   ```
   RPL Lite auto-globs every `.c` file in that directory, so no Makefile
   changes are needed for the `.c` file itself.

2. Patch `~/contiki-ng/os/net/routing/rpl-lite/rpl-dag.c`: find the line
   ```c
   extern rpl_of_t rpl_of0, rpl_mrhof;
   ```
   and add our OF to it:
   ```c
   extern rpl_of_t rpl_of0, rpl_mrhof, rpl_energy_of;
   ```
   This is the same pattern Contiki-NG already uses for `of0`/`mrhof` — the
   symbol needs to be visible wherever `RPL_SUPPORTED_OFS` is textually
   expanded (`rpl-dag.c`), and nowhere else.

3. Install the OF-independent battery model (needed by the OF and by the
   apps; kills nodes whose battery is empty, identically for every OF):
   ```
   cp battery-sim/battery-sim.{c,h} ~/contiki-ng/os/sys/
   ```
   `os/sys/` is auto-globbed too. The apps (`udp-client.c`, `udp-server.c`)
   call `battery_sim_start()`; the server passes `true` (mains-powered sink).

4. Copy `rpl-udp/{Makefile,project-conf.h,udp-client.c,udp-server.c}` into
   `~/contiki-ng/examples/rpl-udp/`.

   The energy model (duty cycle D, capacity) is set in `project-conf.h`
   (`BATTERY_SIM_CONF_DUTY_CYCLE`, `BATTERY_SIM_CONF_CAPACITY_MJ`) and is
   identical for every OF; see CLAUDE.md for the frozen values.

5. Pick the OF per build (same topology, traffic and battery for all):
   ```
   rm -rf build && make TARGET=cooja EXPERIMENT_OF=0   # our energy OF (default)
   rm -rf build && make TARGET=cooja EXPERIMENT_OF=1   # MRHOF
   rm -rf build && make TARGET=cooja EXPERIMENT_OF=2   # OF0
   ```
   For Cooja runs `export EXPERIMENT_OF=<n>` before `./gradlew run`; the
   `rm -rf build` is required because objects are not rebuilt when it
   changes. Confirm which OF ran with `#define LOG_CONF_LEVEL_RPL
   LOG_LEVEL_INFO` and grep the log for `OCP` (100 energy, 1 MRHOF, 0 OF0).

**Why `project-conf.h` doesn't just `#include "rpl-energy-of.h"`:**
`project-conf.h` is pulled in very early in Contiki-NG's build (before
`stdint.h`/`process.h`/etc. are set up). Including `rpl.h`'s transitive
headers that early breaks the build in confusing ways (missing `uint8_t`,
undeclared `PROCESS_NONE`, macro redefinitions). Keep `project-conf.h` to
plain `#define`s only — this cost real debugging time once already, see
step 2 above for how the symbol gets declared instead.

## Verified working

`make TARGET=native` builds clean (`udp-client.native` + `udp-server.native`
link successfully) with this OF selected. Not yet tested in Cooja — that's
the next checklist item ("Get baseline weighted-sum OF running end-to-end
in a small Cooja test").

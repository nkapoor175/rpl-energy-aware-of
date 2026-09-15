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

3. Copy `rpl-udp/project-conf.h` from this repo into
   `~/contiki-ng/examples/rpl-udp/` (or wherever the example under test
   lives) to select the energy-aware OF and enable the energy metric
   container.

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

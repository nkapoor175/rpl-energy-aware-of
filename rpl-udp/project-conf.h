#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* Which objective function runs. Chosen per build, so OF0, MRHOF and our OF
 * are compared on identical topology, traffic and battery:
 *   make TARGET=cooja EXPERIMENT_OF=0   our energy-aware OF (default)
 *   make TARGET=cooja EXPERIMENT_OF=1   MRHOF
 *   make TARGET=cooja EXPERIMENT_OF=2   OF0
 * (rm -rf build when switching.) Our OF needs of-energy/rpl-energy-of.{c,h}
 * in $(CONTIKI)/os/net/routing/rpl-lite/ and ", rpl_energy_of" added to the
 * "extern rpl_of_t rpl_of0, rpl_mrhof;" line of rpl-dag.c.
 *
 * NOTE: plain #defines only. project-conf.h is pulled in very early (before
 * stdint.h/process.h are set up); including rpl.h here breaks the build.
 * 100 is RPL_OCP_ENERGY, see rpl-energy-of.h. */
#ifndef EXPERIMENT_OF
#define EXPERIMENT_OF 0
#endif

#if EXPERIMENT_OF == 0
#define RPL_CONF_OF_OCP 100
#define RPL_CONF_SUPPORTED_OFS {&rpl_energy_of}
/* Needed so nodes advertise/receive the RPL_DAG_MC_ENERGY metric container
 * in their DIOs, which is how neighbors learn each other's residual energy
 * (see nbr_energy_percent() in rpl-energy-of.c). The baselines need no MC. */
#define RPL_CONF_WITH_MC 1
#elif EXPERIMENT_OF == 1
#define RPL_CONF_OF_OCP RPL_OCP_MRHOF
#define RPL_CONF_SUPPORTED_OFS {&rpl_mrhof}
#elif EXPERIMENT_OF == 2
#define RPL_CONF_OF_OCP RPL_OCP_OF0
#define RPL_CONF_SUPPORTED_OFS {&rpl_of0}
#else
#error "EXPERIMENT_OF must be 0 (energy OF), 1 (MRHOF) or 2 (OF0)"
#endif

/* Off by default on cooja/native; needed for
 * battery-sim to read real CPU/radio on-time instead of always 0. */
#define ENERGEST_CONF_ON 1

/* PER-EXPERIMENT KNOB: modeled battery capacity in millijoules. A node dies
 * (prints "NODE_DEAD id=<id> time=<s>", radio + all processes stopped) when
 * it is used up. With the radio always on a node burns ~65.4 mW
 * (3 V * (20 mA listen + 1.8 mA CPU)), so lifetime_s ~= capacity_mJ / 65.4:
 *   39000 -> ~10 min   60000 -> ~15 min   78000 -> ~20 min
 * See battery-sim/battery-sim.c (the battery is OF-independent: it is
 * identical for OF0, MRHOF and our OF). Built-in default is 500000 (~2 h). */
#define BATTERY_SIM_CONF_CAPACITY_MJ 60000.0

#endif /* PROJECT_CONF_H_ */

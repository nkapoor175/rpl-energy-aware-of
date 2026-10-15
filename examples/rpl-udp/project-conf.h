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

/* Energy-aware OF mechanism toggles. */
#define RPL_ENERGY_OF_CONF_ENABLE_DEPLETION 0
#define RPL_ENERGY_OF_CONF_ENABLE_ADAPTIVE_WEIGHTING 0
#define LOG_CONF_LEVEL_RPL LOG_LEVEL_DBG

/* 30-minute depletion sensitivity experiment */
#define RPL_ENERGY_OF_CONF_DEPLETION_WINDOW_MINUTES 60
#define RPL_ENERGY_OF_CONF_DEPLETION_SAMPLE_SPACING_MINUTES 10

/* ENERGY MODEL (frozen, identical for OF0, MRHOF and our OF; see
 * battery-sim/battery-sim.c for the formula and CLAUDE.md for the rationale).
 * Radio and CPU follow one duty-cycle ASSUMPTION D; transmit is measured and
 * not scaled. Primary experiments D = 0.01; sensitivity runs 0.005 and 0.02
 * (change only this line, rm -rf build, rerun all three OFs). */
#define BATTERY_SIM_CONF_DUTY_CYCLE 0.01

/* Modeled battery capacity per node, in millijoules: a simulation parameter,
 * NOT a real battery, and NOT to be tuned to get a convenient death time.
 * PLACEHOLDER: the final common value is still to be agreed with Ananya.
 * Under D = 0.01 a node idles at ~0.82 mW, so 60000 mJ is ~20 h of simulated
 * time (nothing dies in a short run). */
#define BATTERY_SIM_CONF_CAPACITY_MJ 60000.0

/* Each client prints "ENERGY id=.. time=.. consumed_uj=.. capacity_uj=..
 * remaining=.." every 60 s of simulated time, for the log -> CSV parser. */
#define BATTERY_SIM_CONF_REPORT_INTERVAL_S 60

#endif /* PROJECT_CONF_H_ */

#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* Select our energy-aware OF as the RPL Lite objective function.
 * See of-energy/rpl-energy-of.c for the implementation; copy
 * of-energy/rpl-energy-of.{c,h} into
 * $(CONTIKI)/os/net/routing/rpl-lite/ before building, and add
 * ", rpl_energy_of" to the "extern rpl_of_t rpl_of0, rpl_mrhof;" line
 * near the top of rpl-dag.c (same pattern Contiki-NG already uses for
 * of0/mrhof there).
 *
 * NOTE: deliberately not #include-ing rpl-energy-of.h/rpl.h here.
 * project-conf.h is pulled in very early in Contiki-NG's header order
 * (before stdint.h/process.h/etc. are set up), and pulling in rpl.h's
 * transitive includes at that point breaks the build in confusing ways.
 * Keep this file to plain #defines only, matching how RPL_SUPPORTED_OFS
 * is expanded textually inside rpl-dag.c where the real declaration
 * already exists. RPL_OCP_ENERGY == 100, see rpl-energy-of.h. */
#define RPL_CONF_OF_OCP 100
#define RPL_CONF_SUPPORTED_OFS {&rpl_energy_of}

/* Needed so nodes advertise/receive the RPL_DAG_MC_ENERGY metric container
 * in their DIOs, which is how neighbors learn each other's residual
 * energy (see nbr_energy_percent() in rpl-energy-of.c). */
#define RPL_CONF_WITH_MC 1

/* Off by default on cooja/native; needed for
 * rpl_energy_of_get_own_energy_percent() to read real CPU/radio on-time
 * instead of always returning 0. */
#define ENERGEST_CONF_ON 1

#endif /* PROJECT_CONF_H_ */

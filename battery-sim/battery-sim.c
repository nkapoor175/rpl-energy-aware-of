/*
 * battery-sim: an OF-independent simulated battery for Contiki-NG nodes.
 * See battery-sim.h for why this is separate from the objective function.
 *
 * Model: consumed energy = supply voltage * sum over Energest states of
 * (assumed current draw * time spent in that state). The node dies when the
 * modeled battery is empty: "NODE_DEAD id=<id> time=<seconds>" is printed
 * exactly once, the radio is switched off and every process is exited.
 *
 * Killing the processes matters as much as the radio: Cooja's radio_send()
 * temporarily powers the radio back on to transmit, so a node with only its
 * radio off would still send from timers. With the timers and processes gone
 * nothing can trigger a send, and with the radio off nothing is received
 * (so nothing is forwarded). Neighbors notice through missed DIOs / failing
 * link statistics and re-select parents.
 *
 * All knobs are BATTERY_SIM_CONF_* macros, set per experiment in
 * project-conf.h.
 */

#include "contiki.h"
#include "net/netstack.h"
#include "sys/energest.h"
#include "sys/node-id.h"
#include "sys/battery-sim.h"
#include <stdio.h>

/*---------------------------------------------------------------------------*/
/* Assumed current draw per Energest state, in mA (Tmote Sky-class figures,
 * commonly used in WSN/RPL energy literature). */
#ifndef BATTERY_SIM_CONF_CPU_MA
#define BATTERY_SIM_CPU_MA 1.8
#else
#define BATTERY_SIM_CPU_MA BATTERY_SIM_CONF_CPU_MA
#endif

#ifndef BATTERY_SIM_CONF_LPM_MA
#define BATTERY_SIM_LPM_MA 0.0545
#else
#define BATTERY_SIM_LPM_MA BATTERY_SIM_CONF_LPM_MA
#endif

#ifndef BATTERY_SIM_CONF_TRANSMIT_MA
#define BATTERY_SIM_TRANSMIT_MA 17.7
#else
#define BATTERY_SIM_TRANSMIT_MA BATTERY_SIM_CONF_TRANSMIT_MA
#endif

/* Effective current while the radio listens. 20.0 mA models an always-on
 * radio (Contiki-NG's csma never duty-cycles). To model a duty-cycled radio,
 * set this to 20.0 * duty_cycle, e.g. 0.5 for ~2.5%: idle listening then no
 * longer dominates and transmitting (i.e. forwarding) becomes what drains a
 * node -- see CLAUDE.md for why this matters for the research question. */
#ifndef BATTERY_SIM_CONF_LISTEN_MA
#define BATTERY_SIM_LISTEN_MA 20.0
#else
#define BATTERY_SIM_LISTEN_MA BATTERY_SIM_CONF_LISTEN_MA
#endif

#define BATTERY_SIM_SUPPLY_VOLTAGE 3.0

/* Modeled starting battery capacity, in millijoules. Set it per experiment:
 *   #define BATTERY_SIM_CONF_CAPACITY_MJ 60000.0
 * A simulation knob, NOT a real battery (two AA cells are ~20,000,000 mJ,
 * which would never drain inside a Cooja run).
 *
 * Choosing a value for a target node lifetime T (seconds) with the radio
 * always on:
 *   power = V * (I_listen + I_cpu) = 3.0 V * (20.0 + 1.8) mA = 65.4 mW
 *   capacity_mJ = T * 65.4          (mW * s = mJ)
 * so 10 min -> ~39,000   15 min -> ~59,000   20 min -> ~78,000.
 * Verify against the NODE_DEAD lines of a first run and rescale.
 * The default (500,000 mJ, ~2 h) deliberately never triggers death in the
 * short smoke tests. */
#ifndef BATTERY_SIM_CONF_CAPACITY_MJ
#define BATTERY_SIM_CAPACITY_MJ 500000.0
#else
#define BATTERY_SIM_CAPACITY_MJ BATTERY_SIM_CONF_CAPACITY_MJ
#endif

/* Optional per-node capacity spread (percent, default 0 = identical
 * batteries). With identical batteries and an always-on radio every node
 * drains at nearly the same rate and all die within about a second of each
 * other, so there is no surviving network to re-form around the dead ones.
 *   #define BATTERY_SIM_CONF_CAPACITY_SPREAD_PCT 30
 * gives node N a deterministic factor in [0.70, 1.30] from its node id, so
 * deaths are staggered and reproducible. It is an EXPERIMENT CONFIGURATION
 * (heterogeneous initial energy), not part of the protocol. Every run that
 * is compared (OF0, MRHOF, ablations, full OF) must use the same value. */
#ifndef BATTERY_SIM_CONF_CAPACITY_SPREAD_PCT
#define BATTERY_SIM_CAPACITY_SPREAD_PCT 0
#else
#define BATTERY_SIM_CAPACITY_SPREAD_PCT BATTERY_SIM_CONF_CAPACITY_SPREAD_PCT
#endif

/* The sink models a mains-powered gateway: it never dies. Set to 1 to let
 * the root die too (ends the experiment when it does). */
#ifndef BATTERY_SIM_CONF_ROOT_CAN_DIE
#define BATTERY_SIM_ROOT_CAN_DIE 0
#else
#define BATTERY_SIM_ROOT_CAN_DIE BATTERY_SIM_CONF_ROOT_CAN_DIE
#endif

#define BATTERY_CHECK_INTERVAL CLOCK_SECOND

static bool started = false;
static bool immortal = false;

PROCESS(battery_sim_process, "battery sim");
/*---------------------------------------------------------------------------*/
static double
node_capacity_mj(void)
{
  /* node_id * 7 mod 11 maps consecutive ids to well-separated values 0..10 */
  double position = ((double)((node_id * 7) % 11) - 5.0) / 5.0; /* -1 .. +1 */
  return BATTERY_SIM_CAPACITY_MJ *
    (1.0 + (BATTERY_SIM_CAPACITY_SPREAD_PCT / 100.0) * position);
}
/*---------------------------------------------------------------------------*/
uint8_t
battery_sim_energy_percent(void)
{
  double consumed_mj;
  double remaining_fraction;

  if(immortal) {
    return 255;
  }

  energest_flush();

  consumed_mj = BATTERY_SIM_SUPPLY_VOLTAGE * (
    BATTERY_SIM_CPU_MA * ((double)energest_type_time(ENERGEST_TYPE_CPU) / ENERGEST_SECOND) +
    BATTERY_SIM_LPM_MA * ((double)energest_type_time(ENERGEST_TYPE_LPM) / ENERGEST_SECOND) +
    BATTERY_SIM_TRANSMIT_MA * ((double)energest_type_time(ENERGEST_TYPE_TRANSMIT) / ENERGEST_SECOND) +
    BATTERY_SIM_LISTEN_MA * ((double)energest_type_time(ENERGEST_TYPE_LISTEN) / ENERGEST_SECOND)
  );

  remaining_fraction = 1.0 - (consumed_mj / node_capacity_mj());
  if(remaining_fraction < 0.0) {
    remaining_fraction = 0.0;
  } else if(remaining_fraction > 1.0) {
    remaining_fraction = 1.0;
  }

  return (uint8_t)(remaining_fraction * 255);
}
/*---------------------------------------------------------------------------*/
bool
battery_sim_is_immortal(void)
{
  return immortal;
}
/*---------------------------------------------------------------------------*/
static void
kill_node(void)
{
  struct process *p;
  struct process *next;

  printf("NODE_DEAD id=%u time=%lu\n", (unsigned)node_id,
         (unsigned long)clock_seconds());

  NETSTACK_RADIO.off();

  for(p = PROCESS_LIST(); p != NULL; p = next) {
    next = p->next; /* process_exit() unlinks p */
    if(p != &battery_sim_process) {
      process_exit(p);
    }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(battery_sim_process, ev, data)
{
  static struct etimer timer;

  PROCESS_BEGIN();

  etimer_set(&timer, BATTERY_CHECK_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
    if(!immortal && battery_sim_energy_percent() == 0) {
      kill_node();
      PROCESS_EXIT(); /* dead nodes stay dead; the thread never runs again */
    }
    etimer_reset(&timer);
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
void
battery_sim_start(bool is_sink)
{
  if(started) {
    return;
  }
  started = true;
  immortal = is_sink && !BATTERY_SIM_ROOT_CAN_DIE;
  process_start(&battery_sim_process, NULL);
}
/*---------------------------------------------------------------------------*/

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

/* Radio listening current with the radio fully on (Tmote Sky class). */
#ifndef BATTERY_SIM_CONF_LISTEN_MA
#define BATTERY_SIM_LISTEN_MA 20.0
#else
#define BATTERY_SIM_LISTEN_MA BATTERY_SIM_CONF_LISTEN_MA
#endif

/* Duty-cycle ASSUMPTION D (0 < D <= 1), a simulation parameter that stands in
 * for a low-power MAC; nothing here enforces it on the real radio or CPU.
 * 1.0 = always on (the default, which reproduces the plain Energest model).
 * Frozen experiment values (agreed with Ananya): primary 0.01, sensitivity
 * 0.005 and 0.02. The SAME D, currents, voltage and capacity must be used
 * for OF0, MRHOF and our OF.
 *
 * With Contiki-NG's csma the radio is always on and Cooja counts the CPU as
 * on 100% of the time, so plain Energest energy is a function of elapsed
 * time only and cannot tell routing protocols apart. The model therefore is
 *
 *   I_cpu_eff    = I_cpu * D + I_lpm * (1 - D)         applied to cpu+lpm time
 *   I_listen_eff = I_listen * D                        applied to LISTEN time
 *   I_tx         = I_tx, NOT scaled                    applied to TRANSMIT time
 *
 * Energest LISTEN and TRANSMIT are separate states, so scaling LISTEN never
 * touches TX (no double counting). Receiving a packet is accounted inside
 * LISTEN (Cooja/Energest accounting; there is no separate RX state), so no
 * per-packet receive cost is added. */
#ifndef BATTERY_SIM_CONF_DUTY_CYCLE
#define BATTERY_SIM_DUTY_CYCLE 1.0
#else
#define BATTERY_SIM_DUTY_CYCLE BATTERY_SIM_CONF_DUTY_CYCLE
#endif

#define BATTERY_SIM_CPU_EFF_MA \
  (BATTERY_SIM_CPU_MA * BATTERY_SIM_DUTY_CYCLE + \
   BATTERY_SIM_LPM_MA * (1.0 - BATTERY_SIM_DUTY_CYCLE))
#define BATTERY_SIM_LISTEN_EFF_MA (BATTERY_SIM_LISTEN_MA * BATTERY_SIM_DUTY_CYCLE)

#define BATTERY_SIM_SUPPLY_VOLTAGE 3.0

/* Modeled starting battery capacity, in millijoules, per node. A simulation
 * parameter, NOT a real battery (two AA cells are ~20,000,000 mJ). Set it
 * from project-conf.h:  #define BATTERY_SIM_CONF_CAPACITY_MJ <value>
 *
 * Do NOT choose it to get a convenient death time. Freeze one common value
 * for OF0, MRHOF and our OF, justify it as a simulation parameter, and check
 * sensitivity to it if lifetime is a headline result. To read off what a
 * value means under the frozen model:
 *   power_mW = V * (I_cpu_eff + I_listen_eff) + transmit share
 *   idle lifetime_s ~= capacity_mJ / power_mW
 * (always on: 65.4 mW, so 60000 mJ ~ 15 min; D = 0.01: ~0.82 mW, so
 * 60000 mJ ~ 20 h; transmitting shortens it). The built-in default
 * (500,000 mJ) deliberately never triggers death in short smoke tests. */
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

/* Every BATTERY_SIM_CONF_REPORT_INTERVAL_S seconds each non-sink node prints
 *   ENERGY id=<id> time=<s> consumed_uj=<microjoules> capacity_uj=<microjoules> remaining=<0..255>
 * for the log -> CSV parser (energy consumed / remaining per node over time).
 * 0 (default) = no periodic report. */
#ifndef BATTERY_SIM_CONF_REPORT_INTERVAL_S
#define BATTERY_SIM_REPORT_INTERVAL_S 0
#else
#define BATTERY_SIM_REPORT_INTERVAL_S BATTERY_SIM_CONF_REPORT_INTERVAL_S
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
static double
consumed_mj_now(void)
{
  energest_flush();

  /* mA * s * V = mJ. CPU current is applied to the whole cpu+lpm time (the
   * duty-cycle assumption replaces the measured CPU/LPM split). */
  return BATTERY_SIM_SUPPLY_VOLTAGE * (
    BATTERY_SIM_CPU_EFF_MA *
      ((double)(energest_type_time(ENERGEST_TYPE_CPU) +
                energest_type_time(ENERGEST_TYPE_LPM)) / ENERGEST_SECOND) +
    BATTERY_SIM_LISTEN_EFF_MA *
      ((double)energest_type_time(ENERGEST_TYPE_LISTEN) / ENERGEST_SECOND) +
    BATTERY_SIM_TRANSMIT_MA *
      ((double)energest_type_time(ENERGEST_TYPE_TRANSMIT) / ENERGEST_SECOND)
  );
}
/*---------------------------------------------------------------------------*/
uint8_t
battery_sim_energy_percent(void)
{
  double remaining_fraction;

  if(immortal) {
    return 255;
  }

  remaining_fraction = 1.0 - (consumed_mj_now() / node_capacity_mj());
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
  static unsigned long ticks = 0;

  PROCESS_BEGIN();

  etimer_set(&timer, BATTERY_CHECK_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
    ticks++;
    if(!immortal && battery_sim_energy_percent() == 0) {
      kill_node();
      PROCESS_EXIT(); /* dead nodes stay dead; the thread never runs again */
    }
    if(BATTERY_SIM_REPORT_INTERVAL_S > 0 &&
       ticks % BATTERY_SIM_REPORT_INTERVAL_S == 0) {
      printf("ENERGY id=%u time=%lu consumed_uj=%lu capacity_uj=%lu remaining=%u\n",
             (unsigned)node_id, (unsigned long)clock_seconds(),
             (unsigned long)(consumed_mj_now() * 1000.0),
             (unsigned long)(node_capacity_mj() * 1000.0),
             (unsigned)battery_sim_energy_percent());
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

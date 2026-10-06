/*
 * Energy-aware RPL Objective Function (OF) for Contiki-NG RPL Lite.
 *
 * BACSE203 Computer Networks course project.
 * Team: Navika Kapoor (25BCE0835), Ananya Thapliyal (25BCE0695)
 * Guide: Prof. Sarwesh P.
 *
 * Composite score (lower is better, matches RPL's "lower rank = better
 * parent" convention):
 *
 *   score = w1 * ETX_norm + w2 * (1 - Energy_norm) + w3 * HopCount_norm
 *
 * where all three terms are min-max normalized to [0, 1] and w1+w2+w3 = 1.
 *
 * STATUS: baseline + mechanism 1 of 3 (see CLAUDE.md checklist).
 *   - energy-gated filtering: IMPLEMENTED (see nbr_is_acceptable_parent /
 *     compute_energy_gate_threshold below)
 *   - depletion-rate tracking (rolling history instead of a snapshot): TODO,
 *     depends on Ananya's DIO extension work
 *   - local adaptive weighting (w2 adjusted from observed neighbor variance):
 *     TODO, depends on Ananya's DIO extension work
 * Each remaining TODO is left at the point it will hook in, so the file's
 * structure won't need to change when they're added.
 *
 * Residual energy comes from Contiki-NG's Energest module (see
 * rpl_energy_of_get_own_energy_percent): cumulative CPU/LPM/radio on-time
 * is converted to consumed energy using an assumed current draw per state
 * (RPL_ENERGY_OF_*_MA below, Tmote Sky-class figures) and an assumed
 * starting battery capacity (RPL_ENERGY_OF_CONF_BATTERY_CAPACITY_MJ). Both
 * are simulation-design choices, not code correctness ones -- the default
 * capacity is picked to visibly deplete over a Cooja-scale run (minutes),
 * not to model a real battery's lifetime. Override per-experiment via
 * project-conf.h once the team agrees on evaluation timescales.
 * Requires ENERGEST_CONF_ON 1 (off by default on cooja/native), set in
 * project-conf.h.
 *
 * Normalization bounds below are still fixed constants rather than
 * dynamically computed from the current neighbor set. Energy-gated
 * filtering does scan the neighbor set (see compute_energy_gate_threshold),
 * but that's a separate pass for the percentile threshold, not a rework of
 * the min-max normalization used in scoring.
 *
 * ABLATION CONFIGS: each mechanism can be toggled independently via
 * project-conf.h, so the ablation study (OF with only gating / only
 * depletion / only adaptive weighting / all three) doesn't need separate
 * source files:
 *   RPL_ENERGY_OF_CONF_ENABLE_GATING              (default 1, implemented)
 *   RPL_ENERGY_OF_CONF_ENABLE_DEPLETION            (default 0, reserved --
 *     no-op until the depletion-rate mechanism itself is implemented)
 *   RPL_ENERGY_OF_CONF_ENABLE_ADAPTIVE_WEIGHTING   (default 0, reserved --
 *     no-op until the adaptive-weighting mechanism itself is implemented)
 * The baseline weighted-sum formula itself is not gated by a toggle -- it's
 * always active, since every ablation config and the full combined OF are
 * all "baseline + a subset of mechanisms," never baseline-less.
 */

#include "net/routing/rpl-lite/rpl.h"
#include "net/routing/rpl-lite/rpl-energy-of.h"
#include "net/nbr-table.h"
#include "net/link-stats.h"
#include "net/netstack.h"
#include "sys/energest.h"
#include "sys/node-id.h"
#include <stdbool.h>
#include <stdio.h>

/* The RPL Lite neighbor table, declared in rpl-neighbor.c. Needed to
 * iterate all currently visible neighbors for the energy-gated filtering
 * percentile threshold below. */
NBR_TABLE_DECLARE(rpl_neighbors);

#include "sys/log.h"
#define LOG_MODULE "RPL"
#define LOG_LEVEL LOG_LEVEL_RPL

/*---------------------------------------------------------------------------*/
/* Ablation toggles -- see the ABLATION CONFIGS note in the file header. */
#ifndef RPL_ENERGY_OF_CONF_ENABLE_GATING
#define RPL_ENERGY_OF_ENABLE_GATING 1
#else
#define RPL_ENERGY_OF_ENABLE_GATING RPL_ENERGY_OF_CONF_ENABLE_GATING
#endif

#ifndef RPL_ENERGY_OF_CONF_ENABLE_DEPLETION
#define RPL_ENERGY_OF_ENABLE_DEPLETION 0
#else
#define RPL_ENERGY_OF_ENABLE_DEPLETION RPL_ENERGY_OF_CONF_ENABLE_DEPLETION
#endif

#ifndef RPL_ENERGY_OF_CONF_ENABLE_ADAPTIVE_WEIGHTING
#define RPL_ENERGY_OF_ENABLE_ADAPTIVE_WEIGHTING 0
#else
#define RPL_ENERGY_OF_ENABLE_ADAPTIVE_WEIGHTING RPL_ENERGY_OF_CONF_ENABLE_ADAPTIVE_WEIGHTING
#endif

/*---------------------------------------------------------------------------*/
/* Weights (milli-units, i.e. parts per 1000, so they sum to 1000). Override
 * via project-conf.h for the weight-sensitivity sweep, e.g.:
 *   #define RPL_ENERGY_OF_CONF_W1 500
 *   #define RPL_ENERGY_OF_CONF_W2 300
 *   #define RPL_ENERGY_OF_CONF_W3 200
 */
#ifdef RPL_ENERGY_OF_CONF_W1
#define W1_ETX_MILLI RPL_ENERGY_OF_CONF_W1
#else
#define W1_ETX_MILLI 400
#endif

#ifdef RPL_ENERGY_OF_CONF_W2
#define W2_ENERGY_MILLI RPL_ENERGY_OF_CONF_W2
#else
#define W2_ENERGY_MILLI 400
#endif

#ifdef RPL_ENERGY_OF_CONF_W3
#define W3_HOP_MILLI RPL_ENERGY_OF_CONF_W3
#else
#define W3_HOP_MILLI 200
#endif

/* Fixed min-max normalization bounds. */
#define ETX_MIN (1 * LINK_STATS_ETX_DIVISOR)   /* ETX 1.0: perfect link */
#define ETX_MAX (8 * LINK_STATS_ETX_DIVISOR)   /* ETX 8.0: worst usable link */

#define ENERGY_MIN 0    /* energy_est byte: 0 = battery empty */
#define ENERGY_MAX 255  /* energy_est byte: 255 = battery full */

#define HOPCOUNT_MIN 0
#define HOPCOUNT_MAX 16 /* generous upper bound for course-scale topologies (10-50 nodes) */

/* Reject parents whose link/path metric is worse than these ceilings,
 * mirroring MRHOF's MAX_LINK_METRIC / MAX_PATH_COST gates. */
#define MAX_LINK_METRIC 512   /* ETX of 4 */
#define MAX_PATH_COST   32768

/* Scale applied to the normalized [0,1000] composite score before adding it
 * to the accumulated rank, so it lands in a range comparable to MRHOF's ETX
 * based rank increments (roughly 128-1024 per hop). */
#define SCORE_TO_RANK_SCALE 1

/* Hysteresis: only switch preferred parent if the gain exceeds this, or the
 * candidate has been consistently better for TIME_THRESHOLD. Same pattern
 * and defaults as rpl-mrhof.c. */
#define RANK_THRESHOLD 192
#define TIME_THRESHOLD (10 * 60 * CLOCK_SECOND)

/* Energy-gated filtering: a neighbor is hard-excluded from scoring if its
 * residual energy falls in the bottom ENERGY_GATE_PERCENTILE% of currently
 * visible neighbors. Below ENERGY_GATE_MIN_NEIGHBORS neighbors, "bottom
 * 20%" isn't a meaningful signal (e.g. with 2 neighbors it would just
 * exclude whichever one has less energy, always), so the gate is disabled. */
#define ENERGY_GATE_PERCENTILE 20
#define ENERGY_GATE_MIN_NEIGHBORS 5

/*---------------------------------------------------------------------------*/
/* Not under "#if RPL_WITH_MC": the battery model also drives node death
 * (see the battery monitor below), which must work with MC disabled too. */
/* Assumed current draw per Energest state, in mA (Tmote Sky-class figures,
 * commonly used in RPL energy literature). Override via project-conf.h if
 * modeling a different platform. */
#ifndef RPL_ENERGY_OF_CONF_CPU_MA
#define RPL_ENERGY_OF_CPU_MA 1.8
#else
#define RPL_ENERGY_OF_CPU_MA RPL_ENERGY_OF_CONF_CPU_MA
#endif

#ifndef RPL_ENERGY_OF_CONF_LPM_MA
#define RPL_ENERGY_OF_LPM_MA 0.0545
#else
#define RPL_ENERGY_OF_LPM_MA RPL_ENERGY_OF_CONF_LPM_MA
#endif

#ifndef RPL_ENERGY_OF_CONF_TRANSMIT_MA
#define RPL_ENERGY_OF_TRANSMIT_MA 17.7
#else
#define RPL_ENERGY_OF_TRANSMIT_MA RPL_ENERGY_OF_CONF_TRANSMIT_MA
#endif

#ifndef RPL_ENERGY_OF_CONF_LISTEN_MA
#define RPL_ENERGY_OF_LISTEN_MA 20.0
#else
#define RPL_ENERGY_OF_LISTEN_MA RPL_ENERGY_OF_CONF_LISTEN_MA
#endif

#define RPL_ENERGY_OF_SUPPLY_VOLTAGE 3.0

/* Assumed starting battery capacity, in millijoules. Set it per experiment
 * from project-conf.h:
 *   #define RPL_ENERGY_OF_CONF_BATTERY_CAPACITY_MJ 60000.0
 * It is a simulation knob, NOT a real battery's capacity (two AA cells are
 * ~20,000,000 mJ, which would never drain inside a Cooja run).
 *
 * Choosing a value for a target node lifetime T (seconds) with the radio
 * always on (Contiki-NG's default csma MAC never duty-cycles the radio):
 *   power  = V * (I_listen + I_cpu) = 3.0 V * (20.0 + 1.8) mA = 65.4 mW
 *   capacity_mJ = T * 65.4        (mW * s = mJ)
 * so 10 min -> ~39,000   15 min -> ~59,000   20 min -> ~78,000.
 * Transmitting draws slightly LESS than listening in this model (17.7 vs
 * 20.0 mA), so traffic barely moves the lifetime; verify the real number
 * for your platform from the NODE_DEAD lines of a first run and rescale.
 * Default below (500,000 mJ, ~2 h) is deliberately too large to trigger
 * death in short smoke tests. */
#ifndef RPL_ENERGY_OF_CONF_BATTERY_CAPACITY_MJ
#define RPL_ENERGY_OF_BATTERY_CAPACITY_MJ 500000.0
#else
#define RPL_ENERGY_OF_BATTERY_CAPACITY_MJ RPL_ENERGY_OF_CONF_BATTERY_CAPACITY_MJ
#endif

/* Optional per-node capacity spread (percent, default 0 = every node gets
 * exactly the capacity above). With the radio always on, every node drains
 * at nearly the same rate, so identical batteries make all nodes die within
 * about a second of each other and there is no surviving network to re-form
 * around the dead ones. Setting e.g.
 *   #define RPL_ENERGY_OF_CONF_CAPACITY_SPREAD_PCT 30
 * gives node N a deterministic capacity factor in [0.70, 1.30] derived from
 * its node id, so nodes die in a staggered, reproducible order. Compare
 * runs only with the same spread value. */
#ifndef RPL_ENERGY_OF_CONF_CAPACITY_SPREAD_PCT
#define RPL_ENERGY_OF_CAPACITY_SPREAD_PCT 0
#else
#define RPL_ENERGY_OF_CAPACITY_SPREAD_PCT RPL_ENERGY_OF_CONF_CAPACITY_SPREAD_PCT
#endif

static double
node_battery_capacity_mj(void)
{
  /* node_id * 7 mod 11 maps consecutive ids to well-separated values 0..10 */
  double position = ((double)((node_id * 7) % 11) - 5.0) / 5.0; /* -1 .. +1 */
  return RPL_ENERGY_OF_BATTERY_CAPACITY_MJ *
    (1.0 + (RPL_ENERGY_OF_CAPACITY_SPREAD_PCT / 100.0) * position);
}

static uint8_t
rpl_energy_of_get_own_energy_percent(void)
{
  double consumed_mj;
  double remaining_fraction;

  energest_flush();

  consumed_mj = RPL_ENERGY_OF_SUPPLY_VOLTAGE * (
    RPL_ENERGY_OF_CPU_MA * ((double)energest_type_time(ENERGEST_TYPE_CPU) / ENERGEST_SECOND) +
    RPL_ENERGY_OF_LPM_MA * ((double)energest_type_time(ENERGEST_TYPE_LPM) / ENERGEST_SECOND) +
    RPL_ENERGY_OF_TRANSMIT_MA * ((double)energest_type_time(ENERGEST_TYPE_TRANSMIT) / ENERGEST_SECOND) +
    RPL_ENERGY_OF_LISTEN_MA * ((double)energest_type_time(ENERGEST_TYPE_LISTEN) / ENERGEST_SECOND)
  );

  remaining_fraction = 1.0 - (consumed_mj / node_battery_capacity_mj());
  if(remaining_fraction < 0.0) {
    remaining_fraction = 0.0;
  } else if(remaining_fraction > 1.0) {
    remaining_fraction = 1.0;
  }

  return (uint8_t)(remaining_fraction * ENERGY_MAX);
}
/*---------------------------------------------------------------------------*/
/* Node death. A once-per-second monitor process checks the battery; when
 * rpl_energy_of_get_own_energy_percent() reaches 0 the node is killed:
 * "NODE_DEAD id=<id> time=<seconds>" is printed exactly once, the radio is
 * switched off, and every process (application, tcpip, etimer/ctimer, ...)
 * is exited. Killing the processes matters as much as the radio: Cooja's
 * radio_send() temporarily powers the radio back on to transmit, so a node
 * with only its radio off would still send DIOs and application packets
 * from timers. With the timers and processes gone nothing can trigger a
 * send, and with the radio off nothing is received (so nothing is
 * forwarded). Neighbors notice through missed DIOs / failing link
 * statistics and re-select parents.
 *
 * The root is the mains-powered sink: it never dies (and advertises a full
 * battery, see update_metric_container) unless
 * RPL_ENERGY_OF_CONF_ROOT_CAN_DIE is set to 1. */
#ifdef RPL_ENERGY_OF_CONF_ROOT_CAN_DIE
#define RPL_ENERGY_OF_ROOT_CAN_DIE RPL_ENERGY_OF_CONF_ROOT_CAN_DIE
#else
#define RPL_ENERGY_OF_ROOT_CAN_DIE 0
#endif

#define BATTERY_CHECK_INTERVAL CLOCK_SECOND

static bool battery_monitor_started = false;

PROCESS(rpl_energy_of_battery_process, "energy OF battery monitor");

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
    if(p != &rpl_energy_of_battery_process) {
      process_exit(p);
    }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(rpl_energy_of_battery_process, ev, data)
{
  static struct etimer timer;

  PROCESS_BEGIN();

  etimer_set(&timer, BATTERY_CHECK_INTERVAL);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
    if(rpl_energy_of_get_own_energy_percent() == ENERGY_MIN
       && (RPL_ENERGY_OF_ROOT_CAN_DIE || !rpl_dag_root_is_root())) {
      kill_node();
      PROCESS_EXIT(); /* dead nodes stay dead; the thread never runs again */
    }
    etimer_reset(&timer);
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/* Called from reset() and update_metric_container(), i.e. as soon as the OF
 * is in use on this node (root and non-root); idempotent. */
static void
ensure_battery_monitor_started(void)
{
  if(!battery_monitor_started) {
    battery_monitor_started = true;
    process_start(&rpl_energy_of_battery_process, NULL);
  }
}
/*---------------------------------------------------------------------------*/
static uint8_t
nbr_energy_percent(rpl_nbr_t *nbr)
{
#if RPL_WITH_MC
  if(nbr->mc.type == RPL_DAG_MC_ENERGY) {
    return nbr->mc.obj.energy.energy_est;
  }
#endif /* RPL_WITH_MC */
  /* No energy data available for this neighbor yet (MC disabled, or no DIO
   * received with the energy MC so far). Treat as "unknown -> full", so the
   * energy term contributes no penalty rather than an artificial one. */
  return ENERGY_MAX;
}
/*---------------------------------------------------------------------------*/
static uint16_t
nbr_hopcount(rpl_nbr_t *nbr)
{
  uint16_t min_hoprankinc = curr_instance.min_hoprankinc;
  uint16_t hops;

  if(min_hoprankinc == 0) {
    return 0;
  }
  hops = nbr->rank / min_hoprankinc;
  return MIN(hops, HOPCOUNT_MAX);
}
/*---------------------------------------------------------------------------*/
/* Min-max normalize `value` against [min, max] into milli-units [0, 1000],
 * clamped at both ends. */
static uint16_t
normalize_milli(uint32_t value, uint32_t min, uint32_t max)
{
  if(max <= min) {
    return 0;
  }
  if(value <= min) {
    return 0;
  }
  if(value >= max) {
    return 1000;
  }
  return (uint16_t)(((value - min) * 1000) / (max - min));
}
/*---------------------------------------------------------------------------*/
static void
reset(void)
{
  LOG_INFO("reset energy-aware OF\n");
  ensure_battery_monitor_started();
}
/*---------------------------------------------------------------------------*/
static uint16_t
nbr_link_metric(rpl_nbr_t *nbr)
{
  const struct link_stats *stats = rpl_neighbor_get_link_stats(nbr);
  return stats != NULL ? stats->etx : 0xffff;
}
/*---------------------------------------------------------------------------*/
/* Composite weighted-sum score for this single hop (not path-accumulated),
 * scaled into rank-comparable units. Lower is better. */
static uint16_t
weighted_link_cost(rpl_nbr_t *nbr)
{
  uint16_t etx = nbr_link_metric(nbr);
  uint8_t energy_pct = nbr_energy_percent(nbr);
  uint16_t hopcount = nbr_hopcount(nbr);

  uint16_t etx_norm = normalize_milli(etx, ETX_MIN, ETX_MAX);
  uint16_t energy_norm = normalize_milli(energy_pct, ENERGY_MIN, ENERGY_MAX);
  uint16_t hop_norm = normalize_milli(hopcount, HOPCOUNT_MIN, HOPCOUNT_MAX);

  /* Energy-gated filtering already excluded low-energy candidates before
   * this function is reached (see nbr_is_acceptable_parent /
   * compute_energy_gate_threshold) -- this is scoring among the survivors,
   * not where the exclusion happens. */

  /* TODO (depletion-rate tracking, checklist item, Ananya's DIO work):
   * fold in a penalty based on the neighbor's rate of energy decline
   * (rolling DIO history), not just this energy_norm snapshot. Guard the
   * addition with "#if RPL_ENERGY_OF_ENABLE_DEPLETION" so the ablation
   * toggle in the file header actually does something once this lands. */

  uint32_t score_milli =
    (uint32_t)W1_ETX_MILLI * etx_norm +
    (uint32_t)W2_ENERGY_MILLI * (1000 - energy_norm) +
    (uint32_t)W3_HOP_MILLI * hop_norm;
  score_milli /= 1000; /* weights were in milli-units too */

  return (uint16_t)MIN(score_milli * SCORE_TO_RANK_SCALE, 0xffff);
}
/*---------------------------------------------------------------------------*/
static uint16_t
nbr_path_cost(rpl_nbr_t *nbr)
{
  uint16_t base;

  if(nbr == NULL) {
    return 0xffff;
  }

  /* Path cost accumulated so far, same pattern as OF0/MRHOF. */
  base = nbr->rank;

  return MIN((uint32_t)base + weighted_link_cost(nbr), 0xffff);
}
/*---------------------------------------------------------------------------*/
static rpl_rank_t
rank_via_nbr(rpl_nbr_t *nbr)
{
  uint16_t min_hoprankinc;
  uint16_t path_cost;

  if(nbr == NULL) {
    return RPL_INFINITE_RANK;
  }

  min_hoprankinc = curr_instance.min_hoprankinc;
  path_cost = nbr_path_cost(nbr);

  return MAX(MIN((uint32_t)nbr->rank + min_hoprankinc, RPL_INFINITE_RANK), path_cost);
}
/*---------------------------------------------------------------------------*/
static int
nbr_has_usable_link(rpl_nbr_t *nbr)
{
  uint16_t link_metric = nbr_link_metric(nbr);
  return link_metric <= MAX_LINK_METRIC;
}
/*---------------------------------------------------------------------------*/
#if RPL_ENERGY_OF_ENABLE_GATING
/* The residual-energy value at the ENERGY_GATE_PERCENTILE-th percentile of
 * currently visible neighbors, i.e. the cutoff below which a neighbor is
 * considered "bottom 20%" and hard-excluded. Returns ENERGY_MIN (0, i.e.
 * gate disabled / nobody excluded) when there aren't enough neighbors for
 * the percentile to be meaningful.
 *
 * This is a separate pass over the neighbor table from the scoring in
 * weighted_link_cost() -- deliberately so: filtering must see the whole
 * candidate set to define "bottom 20%", while scoring only ever looks at
 * one candidate at a time. Compiled out entirely when
 * RPL_ENERGY_OF_ENABLE_GATING is 0 (ablation config without gating). */
static uint8_t
compute_energy_gate_threshold(void)
{
  uint8_t energies[NBR_TABLE_MAX_NEIGHBORS];
  int n = 0;
  int i, j, idx;
  uint8_t tmp;
  rpl_nbr_t *nbr;

  for(nbr = nbr_table_head(rpl_neighbors); nbr != NULL && n < NBR_TABLE_MAX_NEIGHBORS;
      nbr = nbr_table_next(rpl_neighbors, nbr)) {
    energies[n++] = nbr_energy_percent(nbr);
  }

  if(n < ENERGY_GATE_MIN_NEIGHBORS) {
    return ENERGY_MIN;
  }

  /* Insertion sort ascending; n is small (<= NBR_TABLE_MAX_NEIGHBORS). */
  for(i = 1; i < n; i++) {
    tmp = energies[i];
    j = i - 1;
    while(j >= 0 && energies[j] > tmp) {
      energies[j + 1] = energies[j];
      j--;
    }
    energies[j + 1] = tmp;
  }

  idx = (n * ENERGY_GATE_PERCENTILE) / 100;
  return energies[idx];
}
#endif /* RPL_ENERGY_OF_ENABLE_GATING */
/*---------------------------------------------------------------------------*/
static int
nbr_is_acceptable_parent(rpl_nbr_t *nbr)
{
  uint16_t path_cost;

#if RPL_ENERGY_OF_ENABLE_GATING
  /* Energy-gated filtering: hard-exclude candidates in the bottom
   * ENERGY_GATE_PERCENTILE% of currently visible neighbors' residual
   * energy, before any weighted scoring happens. This is a two-stage
   * design (filter, then score), not a soft weight adjustment -- a
   * candidate excluded here never reaches weighted_link_cost(). */
  if(nbr_energy_percent(nbr) < compute_energy_gate_threshold()) {
    return 0;
  }
#endif /* RPL_ENERGY_OF_ENABLE_GATING */

  path_cost = nbr_path_cost(nbr);
  return nbr_has_usable_link(nbr) && path_cost <= MAX_PATH_COST;
}
/*---------------------------------------------------------------------------*/
static int
within_hysteresis(rpl_nbr_t *nbr)
{
  uint16_t path_cost = nbr_path_cost(nbr);
  uint16_t parent_path_cost = nbr_path_cost(curr_instance.dag.preferred_parent);

  int within_rank_hysteresis = path_cost + RANK_THRESHOLD > parent_path_cost;
  int within_time_hysteresis = nbr->better_parent_since == 0
    || (clock_time() - nbr->better_parent_since) <= TIME_THRESHOLD;

  return within_rank_hysteresis && within_time_hysteresis;
}
/*---------------------------------------------------------------------------*/
static rpl_nbr_t *
best_parent(rpl_nbr_t *nbr1, rpl_nbr_t *nbr2)
{
  int nbr1_is_acceptable;
  int nbr2_is_acceptable;

  nbr1_is_acceptable = nbr1 != NULL && nbr_is_acceptable_parent(nbr1);
  nbr2_is_acceptable = nbr2 != NULL && nbr_is_acceptable_parent(nbr2);

  if(!nbr1_is_acceptable) {
    return nbr2_is_acceptable ? nbr2 : NULL;
  }
  if(!nbr2_is_acceptable) {
    return nbr1_is_acceptable ? nbr1 : NULL;
  }

  if(nbr1 == curr_instance.dag.preferred_parent && within_hysteresis(nbr2)) {
    return nbr1;
  }
  if(nbr2 == curr_instance.dag.preferred_parent && within_hysteresis(nbr1)) {
    return nbr2;
  }

  return nbr_path_cost(nbr1) < nbr_path_cost(nbr2) ? nbr1 : nbr2;
}
/*---------------------------------------------------------------------------*/
#if !RPL_WITH_MC
static void
update_metric_container(void)
{
  ensure_battery_monitor_started();
  curr_instance.mc.type = RPL_DAG_MC_NONE;
}
#else /* RPL_WITH_MC */
static void
update_metric_container(void)
{
  ensure_battery_monitor_started();

  if(!curr_instance.used) {
    LOG_WARN("cannot update the metric container when not joined\n");
    return;
  }

  /* Unlike MRHOF's ETX (which is path-accumulated), residual energy is a
   * per-node snapshot: every node advertises its OWN current battery level,
   * not a value derived from its parent's rank. Neighbors read this back
   * via nbr->mc.obj.energy.energy_est (see nbr_energy_percent above). */
  curr_instance.mc.type = RPL_DAG_MC_ENERGY;
  curr_instance.mc.flags = 0;
  curr_instance.mc.aggr = RPL_DAG_MC_AGGR_ADDITIVE;
  curr_instance.mc.prec = 0;
  curr_instance.mc.length = sizeof(curr_instance.mc.obj.energy);
  if(!RPL_ENERGY_OF_ROOT_CAN_DIE && rpl_dag_root_is_root()) {
    /* Mains-powered sink: always advertise a full battery, so neighbors
     * neither score it down nor gate it out as its modeled battery drains. */
    curr_instance.mc.obj.energy.flags = RPL_DAG_MC_ENERGY_TYPE_MAINS << RPL_DAG_MC_ENERGY_TYPE;
    curr_instance.mc.obj.energy.energy_est = ENERGY_MAX;
  } else {
    curr_instance.mc.obj.energy.flags = RPL_DAG_MC_ENERGY_TYPE_BATTERY << RPL_DAG_MC_ENERGY_TYPE;
    curr_instance.mc.obj.energy.energy_est = rpl_energy_of_get_own_energy_percent();
  }

  /* TODO (local adaptive weighting, checklist item, Ananya's DIO work):
   * every K DIO intervals, inspect the spread of nbr_energy_percent()
   * across current neighbors and nudge W2_ENERGY_MILLI (and W1/W3 to
   * compensate, keeping the sum at 1000) up or down. Needs the DIO
   * extension to carry depletion-rate data too, not just the snapshot
   * already carried by RPL_DAG_MC_ENERGY. Guard with
   * "#if RPL_ENERGY_OF_ENABLE_ADAPTIVE_WEIGHTING" so the ablation toggle in
   * the file header actually does something once this lands. Weights
   * (W1_ETX_MILLI etc.) are currently #define constants set at compile
   * time -- adaptive weighting will need to turn them into mutable state
   * (e.g. static variables initialized from the same defaults) since they
   * change at runtime. */
}
#endif /* RPL_WITH_MC */
/*---------------------------------------------------------------------------*/
rpl_of_t rpl_energy_of = {
  reset,
  nbr_link_metric,
  nbr_has_usable_link,
  nbr_is_acceptable_parent,
  nbr_path_cost,
  rank_via_nbr,
  best_parent,
  update_metric_container,
  RPL_OCP_ENERGY
};

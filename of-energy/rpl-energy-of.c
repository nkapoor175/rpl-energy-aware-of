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
 * STATUS: this is the BASELINE skeleton only (see CLAUDE.md checklist).
 * The three novel mechanisms are intentionally NOT implemented yet:
 *   - energy-gated filtering (hard exclusion of low-battery neighbors)
 *   - depletion-rate tracking (rolling history instead of a snapshot)
 *   - local adaptive weighting (w2 adjusted from observed neighbor variance)
 * Each is left as a TODO at the point it will hook in, so the file's
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
 * Normalization bounds below are fixed constants rather than dynamically
 * computed from the current neighbor set. This keeps the baseline simple;
 * energy-gated filtering will need to scan the neighbor set anyway, so
 * dynamic (min-max over currently visible neighbors) normalization can be
 * added at that point if fixed bounds prove too coarse.
 */

#include "net/routing/rpl-lite/rpl.h"
#include "net/routing/rpl-lite/rpl-energy-of.h"
#include "net/nbr-table.h"
#include "net/link-stats.h"
#include "sys/energest.h"

#include "sys/log.h"
#define LOG_MODULE "RPL"
#define LOG_LEVEL LOG_LEVEL_RPL

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

/*---------------------------------------------------------------------------*/
#if RPL_WITH_MC
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

/* Assumed starting battery capacity, in millijoules. Deliberately small so
 * depletion is visible within a Cooja-scale run; NOT a real battery's
 * capacity. Revisit once the team settles on simulated run durations for
 * the actual evaluation (10/30/50-node, FND/HND/LND runs). */
#ifndef RPL_ENERGY_OF_CONF_BATTERY_CAPACITY_MJ
#define RPL_ENERGY_OF_BATTERY_CAPACITY_MJ 500000.0
#else
#define RPL_ENERGY_OF_BATTERY_CAPACITY_MJ RPL_ENERGY_OF_CONF_BATTERY_CAPACITY_MJ
#endif

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

  remaining_fraction = 1.0 - (consumed_mj / RPL_ENERGY_OF_BATTERY_CAPACITY_MJ);
  if(remaining_fraction < 0.0) {
    remaining_fraction = 0.0;
  } else if(remaining_fraction > 1.0) {
    remaining_fraction = 1.0;
  }

  return (uint8_t)(remaining_fraction * ENERGY_MAX);
}
#endif /* RPL_WITH_MC */
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

  /* TODO (energy-gated filtering, checklist item): before scoring, exclude
   * candidates whose residual energy falls in the bottom 20% of currently
   * visible neighbors. That is a hard-exclusion stage in
   * nbr_is_acceptable_parent(), not a change to this scoring formula. */

  /* TODO (depletion-rate tracking, checklist item): fold in a penalty based
   * on the neighbor's rate of energy decline (rolling DIO history), not
   * just this energy_norm snapshot. */

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
static int
nbr_is_acceptable_parent(rpl_nbr_t *nbr)
{
  uint16_t path_cost = nbr_path_cost(nbr);

  /* TODO (energy-gated filtering, checklist item): this is where the hard
   * exclusion stage belongs, e.g.:
   *   if(nbr_energy_percent(nbr) < dynamic_bottom_20pct_threshold()) {
   *     return 0;
   *   }
   * before falling through to the scoring-based checks below. */

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
  curr_instance.mc.type = RPL_DAG_MC_NONE;
}
#else /* RPL_WITH_MC */
static void
update_metric_container(void)
{
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
  curr_instance.mc.obj.energy.flags = RPL_DAG_MC_ENERGY_TYPE_BATTERY << RPL_DAG_MC_ENERGY_TYPE;
  curr_instance.mc.obj.energy.energy_est = rpl_energy_of_get_own_energy_percent();

  /* TODO (local adaptive weighting, checklist item): every K DIO intervals,
   * inspect the spread of nbr_energy_percent() across current neighbors and
   * nudge W2_ENERGY_MILLI (and W1/W3 to compensate, keeping the sum at
   * 1000) up or down. Needs the DIO extension to carry depletion-rate data
   * too, not just the snapshot already carried by RPL_DAG_MC_ENERGY. */
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

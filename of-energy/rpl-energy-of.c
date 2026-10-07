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
 * Residual energy comes from the OF-independent battery-sim module (see
 * battery-sim/battery-sim.h): Energest on-time -> modeled consumed energy ->
 * 0..255 remaining. The same module also kills nodes whose battery is empty,
 * identically for OF0, MRHOF and this OF, so lifetime comparisons are fair.
 * The application must call battery_sim_start() (udp-client / udp-server do).
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
#include "sys/energest.h"
#include "sys/battery-sim.h"

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
/* The battery model (current draw, capacity, node death) is NOT part of this
 * OF: it lives in the OF-independent battery-sim module so that OF0, MRHOF
 * and this OF all run under the identical battery. This OF only READS the
 * value via battery_sim_energy_percent(). */
/*---------------------------------------------------------------------------*/
static uint8_t
nbr_energy_percent(rpl_nbr_t *nbr)
{
#if RPL_WITH_MC
  if(nbr->mc.type == RPL_DAG_MC_ENERGY) {
    return nbr->mc.obj.energy.energy_est;
  }

/*---------------------------------------------------------------------------*/
/*
 * Update the rolling depletion history for one neighbor.
 *
 * Neighbor energy is received through the standard RPL DAG Metric
 * Container energy_est field. Four observations are retained in a
 * circular buffer and the depletion rate is calculated between the
 * oldest and newest observations.
 */
/*---------------------------------------------------------------------------*/
void
rpl_energy_of_update_neighbor_energy(rpl_nbr_t *nbr)
{
  uint8_t current_energy;
  clock_time_t now;
  uint8_t index;
  uint8_t oldest_index;
  uint8_t newest_index;
  clock_time_t dt;
  int32_t energy_drop;
  int32_t rate_milli;

  if(nbr == NULL) {
    return;
  }

#if RPL_WITH_MC
  if(nbr->mc.type != RPL_DAG_MC_ENERGY) {
    return;
  }

  current_energy = nbr->mc.obj.energy.energy_est;
#else
  return;
#endif

  now = clock_time();

  /*
   * Store the new energy observation in the circular history buffer.
   */
  index = nbr->energy_history_index;

  nbr->energy_history[index] = current_energy;
  nbr->energy_time_history[index] = now;

  /*
   * Advance the circular-buffer index.
   */
  nbr->energy_history_index =
    (index + 1) % RPL_ENERGY_DEPLETION_HISTORY_LEN;

  /*
   * Increase the number of valid observations until the
   * history buffer becomes full.
   */
  if(nbr->energy_history_count < RPL_ENERGY_DEPLETION_HISTORY_LEN) {
    nbr->energy_history_count++;
  }

  /*
   * Keep a total observation count for the depletion scoring logic.
   */
  if(nbr->energy_samples < 255) {
    nbr->energy_samples++;
  }

  /*
   * A depletion rate cannot be calculated until the complete
   * rolling history is available.
   */
  if(nbr->energy_history_count < RPL_ENERGY_DEPLETION_HISTORY_LEN) {
    printf("[DEPLETION] history=%u/%u energy=%u rate=0\\\\n",
           nbr->energy_history_count,
           RPL_ENERGY_DEPLETION_HISTORY_LEN,
           current_energy);
    return;
  }

  /*
   * The next position to be written is the oldest observation.
   * The previous position is the newest observation.
   */
  oldest_index = nbr->energy_history_index;

  if(nbr->energy_history_index == 0) {
    newest_index = RPL_ENERGY_DEPLETION_HISTORY_LEN - 1;
  } else {
    newest_index = nbr->energy_history_index - 1;
  }

  dt = nbr->energy_time_history[newest_index] -
       nbr->energy_time_history[oldest_index];

  energy_drop =
    (int32_t)nbr->energy_history[oldest_index] -
    (int32_t)nbr->energy_history[newest_index];

  if(dt == 0) {
    rate_milli = 0;
  } else {
    rate_milli =
      (energy_drop * 1000 * CLOCK_SECOND) / dt;
  }

  /*
   * Energy increasing or remaining constant means no depletion.
   */
  if(rate_milli < 0) {
    rate_milli = 0;
  }

  nbr->depletion_rate_milli = rate_milli;

  printf("[DEPLETION] oldest=%u newest=%u drop=%ld rate=%ld/s history=%u\\\\n",
         nbr->energy_history[oldest_index],
         nbr->energy_history[newest_index],
         (long)energy_drop,
         (long)rate_milli,
         nbr->energy_history_count);
}

/*---------------------------------------------------------------------------*/

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

/*---------------------------------------------------------------------------*/
/* Local adaptive weighting.                                                 */
/*---------------------------------------------------------------------------*/

#ifndef ADAPTIVE_WEIGHT_INTERVAL
#define ADAPTIVE_WEIGHT_INTERVAL 4
#endif

#ifndef ADAPTIVE_WEIGHT_STEP
#define ADAPTIVE_WEIGHT_STEP 50
#endif

#ifndef ADAPTIVE_W2_MIN
#define ADAPTIVE_W2_MIN 300
#endif

#ifndef ADAPTIVE_W2_MAX
#define ADAPTIVE_W2_MAX 600
#endif

#ifndef ADAPTIVE_SPREAD_HIGH
#define ADAPTIVE_SPREAD_HIGH 250
#endif

#ifndef ADAPTIVE_SPREAD_LOW
#define ADAPTIVE_SPREAD_LOW 100
#endif

static uint16_t adaptive_w1 = W1_ETX_MILLI;
static uint16_t adaptive_w2 = W2_ENERGY_MILLI;
static uint16_t adaptive_w3 = W3_HOP_MILLI;
static uint8_t adaptive_update_count = 0;

/*---------------------------------------------------------------------------*/
/* Calculate the energy spread among the node's current RPL neighbors.       */
/*---------------------------------------------------------------------------*/
static uint16_t
compute_neighbor_energy_spread(void)
{
  uint16_t min_energy = ENERGY_MAX;
  uint16_t max_energy = ENERGY_MIN;
  uint8_t found = 0;
  rpl_nbr_t *nbr;

  for(nbr = nbr_table_head(rpl_neighbors);
      nbr != NULL;
      nbr = nbr_table_next(rpl_neighbors, nbr)) {

    uint8_t energy = nbr_energy_percent(nbr);

    if(energy < min_energy) {
      min_energy = energy;
    }

    if(energy > max_energy) {
      max_energy = energy;
    }

    found = 1;
  }

  if(!found) {
    return 0;
  }

  return normalize_milli(max_energy - min_energy,
                         ENERGY_MIN,
                         ENERGY_MAX);
}

/*---------------------------------------------------------------------------*/
/* Adjust local weights according to neighbor energy variation.              */
/*---------------------------------------------------------------------------*/
static void
update_adaptive_weights(void)
{
  uint16_t spread;

  if(!RPL_ENERGY_OF_ENABLE_ADAPTIVE_WEIGHTING) {
    return;
  }

  adaptive_update_count++;

  if(adaptive_update_count < ADAPTIVE_WEIGHT_INTERVAL) {
    return;
  }

  adaptive_update_count = 0;

  spread = compute_neighbor_energy_spread();

  /*
   * Large energy variation:
   * give more importance to residual energy.
   */
  if(spread > ADAPTIVE_SPREAD_HIGH) {

    if(adaptive_w2 + ADAPTIVE_WEIGHT_STEP <= ADAPTIVE_W2_MAX &&
       adaptive_w1 >= 40 &&
       adaptive_w3 >= 10) {

      adaptive_w2 += ADAPTIVE_WEIGHT_STEP;
      adaptive_w1 -= 40;
      adaptive_w3 -= 10;
    }
  }

  /*
   * Small energy variation:
   * energy is less useful for distinguishing parents,
   * so give some weight back to ETX.
   */
  else if(spread < ADAPTIVE_SPREAD_LOW) {

    if(adaptive_w2 >= ADAPTIVE_W2_MIN &&
       adaptive_w1 + 40 <= 1000 &&
       adaptive_w3 + 10 <= 1000) {

      adaptive_w2 -= ADAPTIVE_WEIGHT_STEP;
      adaptive_w1 += 40;
      adaptive_w3 += 10;
    }
  }

  printf("[ADAPTIVE] spread=%u ETX=%u ENERGY=%u HOP=%u\n",
         spread,
         adaptive_w1,
         adaptive_w2,
         adaptive_w3);
}

/*---------------------------------------------------------------------------*/

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

#if RPL_ENERGY_OF_ENABLE_DEPLETION
  /*
   * Penalize neighbors whose advertised energy is declining rapidly.
   *
   * depletion_rate_milli is expressed in energy-estimate units per
   * second, scaled by 1000.
   */
  uint32_t depletion_penalty = 0;

  if(nbr->energy_samples >= RPL_ENERGY_DEPLETION_HISTORY_LEN &&
     nbr->depletion_rate_milli > 0) {

    uint32_t rate = (uint32_t)nbr->depletion_rate_milli;

    if(rate > DEPLETION_RATE_REF_MILLI) {
      rate = DEPLETION_RATE_REF_MILLI;
    }

    depletion_penalty =
      (rate * DEPLETION_PENALTY_MAX_MILLI) /
      DEPLETION_RATE_REF_MILLI;

    printf("[DEPLETION SCORE] rate=%lu penalty=%lu\\n",
           (unsigned long)nbr->depletion_rate_milli,
           (unsigned long)depletion_penalty);
  }
#endif

  /*
   * Use adaptive weights when adaptive weighting is enabled.
   * Otherwise use the fixed baseline weights.
   */
#if RPL_ENERGY_OF_ENABLE_ADAPTIVE_WEIGHTING
  uint16_t score_w1 = adaptive_w1;
  uint16_t score_w2 = adaptive_w2;
  uint16_t score_w3 = adaptive_w3;
#else
  uint16_t score_w1 = W1_ETX_MILLI;
  uint16_t score_w2 = W2_ENERGY_MILLI;
  uint16_t score_w3 = W3_HOP_MILLI;
#endif

  uint32_t score_milli =
    (uint32_t)score_w1 * etx_norm +
    (uint32_t)score_w2 * (1000 - energy_norm) +
    (uint32_t)score_w3 * hop_norm;

#if RPL_ENERGY_OF_ENABLE_DEPLETION
  score_milli += depletion_penalty;
#endif

  score_milli /= 1000; /* weights are in milli-units */

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
  uint8_t gate_threshold = compute_energy_gate_threshold();
  if(nbr_energy_percent(nbr) < gate_threshold) {
    LOG_DBG("energy gate: excluded neighbor, energy %u < threshold %u\n",
            (unsigned)nbr_energy_percent(nbr), (unsigned)gate_threshold);
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
  if(battery_sim_is_immortal()) {
    /* Mains-powered sink: always advertise a full battery, so neighbors
     * neither score it down nor gate it out. */
    curr_instance.mc.obj.energy.flags = RPL_DAG_MC_ENERGY_TYPE_MAINS << RPL_DAG_MC_ENERGY_TYPE;
    curr_instance.mc.obj.energy.energy_est = ENERGY_MAX;
  } else {
    curr_instance.mc.obj.energy.flags = RPL_DAG_MC_ENERGY_TYPE_BATTERY << RPL_DAG_MC_ENERGY_TYPE;
    curr_instance.mc.obj.energy.energy_est = battery_sim_energy_percent();
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

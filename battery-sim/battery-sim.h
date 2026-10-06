/*
 * battery-sim: an OF-independent simulated battery for Contiki-NG nodes.
 *
 * BACSE203 Computer Networks course project (Navika Kapoor, Ananya
 * Thapliyal; guide Prof. Sarwesh P.).
 *
 * WHY THIS IS NOT PART OF THE OBJECTIVE FUNCTION: the evaluation compares
 * OF0, MRHOF and our energy-aware OF on network lifetime (first / half /
 * last node death). That comparison is only fair if every OF runs under the
 * IDENTICAL battery model and the same death rule. If the battery lived
 * inside our OF, the baselines would never run out of energy. So the battery
 * is a separate module, started by the application, and the OF merely READS
 * the value (battery_sim_energy_percent()).
 *
 * Install: copy battery-sim.{c,h} into $(CONTIKI)/os/sys/ (every .c file in
 * that directory is compiled automatically). Include as "sys/battery-sim.h".
 *
 * Needs ENERGEST_CONF_ON 1 (off by default on cooja/native); without it the
 * battery never drains.
 */

#ifndef BATTERY_SIM_H_
#define BATTERY_SIM_H_

#include "contiki.h"
#include <stdbool.h>
#include <stdint.h>

/* Start the battery model and the once-per-second death monitor on this node.
 * Call once from the application's main process. is_sink = true for the RPL
 * root: it models a mains-powered sink, never drains and never dies (unless
 * BATTERY_SIM_CONF_ROOT_CAN_DIE is 1). Idempotent. */
void battery_sim_start(bool is_sink);

/* Remaining battery on a 0..255 scale (255 = full, 0 = empty). Immortal
 * nodes always report 255. Can be called at any time, from any OF. */
uint8_t battery_sim_energy_percent(void);

/* True for a node that never dies (the sink), so an OF can advertise it as
 * mains-powered. */
bool battery_sim_is_immortal(void);

#endif /* BATTERY_SIM_H_ */

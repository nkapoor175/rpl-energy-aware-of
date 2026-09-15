/*
 * Energy-aware RPL Objective Function (OF) for Contiki-NG RPL Lite.
 *
 * BACSE203 Computer Networks course project.
 * Team: Navika Kapoor (25BCE0835), Ananya Thapliyal (25BCE0695)
 * Guide: Prof. Sarwesh P.
 *
 * This file declares the OF descriptor so it can be listed in
 * RPL_CONF_SUPPORTED_OFS and selected via RPL_CONF_OF_OCP.
 */

#ifndef RPL_ENERGY_OF_H_
#define RPL_ENERGY_OF_H_

#include "net/routing/rpl-lite/rpl.h"

/* Objective Code Point for our OF. RFC6550/6719 reserve 0 (OF0) and 1
 * (MRHOF); any value not already in use by the deployment is fine for a
 * research/course OF. */
#define RPL_OCP_ENERGY 100

/* The OF descriptor, referenced from project-conf.h via
 * RPL_CONF_SUPPORTED_OFS = {&rpl_energy_of} and
 * RPL_CONF_OF_OCP = RPL_OCP_ENERGY. */
extern rpl_of_t rpl_energy_of;

#endif /* RPL_ENERGY_OF_H_ */

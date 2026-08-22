/*
 * Candis-S31 BSP - private TG28 reference fuel-gauge model declaration.
 *
 * SPDX-License-Identifier: Apache-2.0      (this declaration only)
 *
 * The DATA declared here lives in bsp_pmic_reference_model.c and is
 * verbatim GPL-origin vendor content: it is not Apache-2.0, carries an
 * upstream/external-release blocker, and must keep its provenance
 * header there. See that file before redistributing.
 */

#pragma once


#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Byte count of the vendor reference model (see the .c provenance). */
#define BSP_PMIC_REFERENCE_BATTERY_MODEL_SIZE 128

/** Vendor generic 4.2 V-class reference model; GPL-origin data, see the
 *  source-file header before redistributing. */
extern const uint8_t bsp_pmic_reference_battery_model[
     BSP_PMIC_REFERENCE_BATTERY_MODEL_SIZE];

#ifdef __cplusplus
}
#endif

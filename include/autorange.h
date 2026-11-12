/**
 * @file  autorange.h
 * @brief PROBE -> SWEEP -> FUSION pipeline.
 */

#ifndef ESWCAP_AUTORANGE_H
#define ESWCAP_AUTORANGE_H

#include "eswcap.h"

/** Full autoranging cycle. Fills g_sweep with the decision trace and returns
 *  the fused result. */
fusion_t autorange_measure(void);

/** Measure one range only (locked-range mode). */
fusion_t autorange_measure_range(uint8_t range_idx);

/** Single-method measurements on one range. Used by the calibration commands
 *  (`zero`, `cal*`, `adccal*`, `probe`) so they capture through exactly the
 *  same path the runtime uses — a mismatch here silently biases every stored
 *  constant. */
sample_t autorange_measure_osc(uint8_t range_idx);
sample_t autorange_measure_adc(uint8_t range_idx);

/** Remembered preferred range, used to seed the probe. */
uint8_t autorange_preferred_range(void);
void    autorange_set_preferred(uint8_t range_idx);

/** Abort the cycle at the next safe point. Used by exclusive console
 *  commands. */
void autorange_abort(void);
bool autorange_aborting(void);
void autorange_clear_abort(void);

#endif /* ESWCAP_AUTORANGE_H */

/**
 * @file  rmode_math.c
 * @brief Pure Kelvin/TIA resistance arithmetic.
 *
 * Kept separate from rmode.c so the host test suite can compile the exact
 * equations the firmware uses without pulling in the HAL. The BSP-facing
 * measurement code lives in rmode.c.
 */

#include "rmode.h"

#include "config.h"


double rmode_resistance(double rref_ohm, double vn_mv, double vb_mv)
{
    if (rref_ohm <= 0.0) {
        return -1.0;
    }
    if (vb_mv < RMODE_VB_MIN_MV || vn_mv <= vb_mv) {
        return -1.0;
    }
    return rref_ohm * (vn_mv - vb_mv) / vb_mv;
}

double rmode_tia_resistance(double rf_ohm, double vn_mv, double vbias_mv,
                            double vtia_mv)
{
    if (rf_ohm <= 0.0 || vn_mv <= vbias_mv || vtia_mv >= vbias_mv) {
        return -1.0;
    }
    if (vtia_mv < RMODE_TIA_VOUT_MIN_MV) {
        return -1.0;   /* output against the rail: no usable signal */
    }
    return rf_ohm * (vn_mv - vbias_mv) / (vbias_mv - vtia_mv);
}

double rmode_current_ua(double rref_ohm, double vb_mv)
{
    if (rref_ohm <= 0.0) {
        return 0.0;
    }
    return (vb_mv * 1e-3) / rref_ohm * 1e6;
}


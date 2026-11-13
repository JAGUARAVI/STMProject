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

/**
 * @brief Kelvin current with the permanently-connected SH divider removed.
 *
 * The C-mode 10M/1M divider loads SH in every mode.  In R-mode its current
 * does not flow through the DUT, so the force current inferred from V_B is
 * larger than the DUT current.  Subtract I_div = V_SH / R_div before dividing
 * the measured DUT voltage by it.
 *
 * Returns -1 when the corrected current is not positive; that means the
 * divider current alone exceeds the force current and the Kelvin result is
 * not physical (the autorange should move to a larger element or the TIA).
 */
double rmode_current_ua_div(double rref_ohm, double vn_mv, double vb_mv,
                            double rdiv_ohm)
{
    if (rref_ohm <= 0.0 || rdiv_ohm <= 0.0 ||
        vb_mv < RMODE_VB_MIN_MV || vn_mv <= vb_mv) {
        return -1.0;
    }

    double i_force = (vb_mv * 1e-3) / rref_ohm;
    double i_div   = (vn_mv * 1e-3) / rdiv_ohm;
    double i_dut   = i_force - i_div;

    if (!(i_dut > 0.0)) {
        return -1.0;
    }
    return i_dut * 1e6;
}

double rmode_resistance_div(double rref_ohm, double vn_mv, double vb_mv,
                            double rdiv_ohm)
{
    double i_dut_ua = rmode_current_ua_div(rref_ohm, vn_mv, vb_mv, rdiv_ohm);
    if (i_dut_ua <= 0.0) {
        return -1.0;
    }
    /* mV / uA = kohm; scale to ohms. */
    return (vn_mv - vb_mv) / i_dut_ua * 1e3;
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


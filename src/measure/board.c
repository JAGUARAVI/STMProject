/**
 * @file  board.c
 * @brief Board constants and the C_block series transformation.
 *
 * The range resistors and the DC-blocking capacitor are *physical* properties
 * of the front end, not of the MCU, so the measured values carried over from
 * the ESP32 build remain valid. They are mutable at runtime because a per-unit
 * calibration record in flash may override them without a reflash.
 *
 * The DC-blocking capacitor matters to the maths: the DUT is measured *through*
 * C_block, so the circuit sees the series combination
 *
 *     C_eq = (C_block * C_dut) / (C_block + C_dut)
 *
 * and every measurement must invert that to recover the DUT. The inversion has
 * a singularity at C_dut -> C_block, which is why both directions return a
 * negative sentinel instead of inf/nan.
 *
 * Pure arithmetic: no HAL, host-testable.
 */

#include "eswcap.h"

/* ------------------------------------------------------------------------- */
/* Range table                                                               */
/* ------------------------------------------------------------------------- */

range_config_t g_ranges[RANGE_COUNT] = {
    { 2, RES_100_OHM,   "100 R"  },
    { 1, RES_1K_OHM,    "1 kR"   },
    { 0, RES_100K_OHM,  "100 kR" },
    { 3, RES_1M_OHM,    "1 MR"   },
};

/* Effective board values: compiled defaults, optionally replaced by a stored
 * per-unit override. */
static double s_c_block_f  = C_BLOCK_ELEC_F;
static double s_r_bias_ohm = R_BIAS_OHM;
static bool   s_overridden = false;

double board_c_block_f(void)
{
    return s_c_block_f;
}

double board_r_bias_ohm(void)
{
    return s_r_bias_ohm;
}

bool board_cal_overridden(void)
{
    return s_overridden;
}

void board_set_c_block(double f)
{
    if (f > 0.0) {
        s_c_block_f = f;
        s_overridden = true;
    }
}

void board_set_r_bias(double ohm)
{
    if (ohm > 0.0) {
        s_r_bias_ohm = ohm;
        s_overridden = true;
    }
}

void board_set_range_r(uint8_t idx, double ohm)
{
    if (idx < RANGE_COUNT && ohm > 0.0) {
        g_ranges[idx].resistance_ohms = ohm;
        s_overridden = true;
    }
}

/**
 * @brief Restore every board constant to its compiled default.
 *
 * Note that the range labels and mux codes are not affected: only the measured
 * resistance values are restored.
 */
void board_cal_reset(void)
{
    s_c_block_f  = C_BLOCK_ELEC_F;
    s_r_bias_ohm = R_BIAS_OHM;
    s_overridden = false;

    g_ranges[0].resistance_ohms = RES_100_OHM;
    g_ranges[1].resistance_ohms = RES_1K_OHM;
    g_ranges[2].resistance_ohms = RES_100K_OHM;
    g_ranges[3].resistance_ohms = RES_1M_OHM;
}

/* ------------------------------------------------------------------------- */
/* C_block series transform                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Series-equivalent capacitance seen by the circuit, for a given DUT.
 *
 * Returns -1 when the DUT is at or beyond the singularity (C_dut >= C_block),
 * which is physically impossible for a series combination and indicates a bad
 * calibration constant rather than a real measurement.
 */
double cblock_forward(double c_dut)
{
    double cb = s_c_block_f;

    if (cb <= 0.0 || c_dut < 0.0) {
        return -1.0;
    }
    if (c_dut >= cb) {
        return -1.0;
    }
    return (cb * c_dut) / (cb + c_dut);
}

/**
 * @brief Recover the DUT capacitance from the measured series equivalent.
 *
 * Inverts  C_eq = Cb*Cd/(Cb+Cd)  as  Cd = Cb*C_eq/(Cb-C_eq).
 *
 * Returns -1 when C_eq >= C_block, i.e. at or past the singularity. A
 * measurement there is non-physical: it means the fit produced a tau larger
 * than the range could ever produce, usually a failed capture or a shorted DUT.
 */
double cblock_invert(double c_eq)
{
    double cb = s_c_block_f;

    if (cb <= 0.0 || c_eq < 0.0 || c_eq >= cb) {
        return -1.0;
    }
    return (cb * c_eq) / (cb - c_eq);
}

/**
 * @file  calib.c
 * @brief Calibration state and solvers, persisted as one flash record.
 *
 * Three independent calibration sets are kept, all carried over from the ESP32
 * firmware because they describe the *analog front end*, not the MCU:
 *
 *  - oscillator:  T = K*R*C_eq + T0  (per range)
 *  - ADC RC-step: tau = R_eff*(C_dut + C0) through the C_block series
 *                 combination (per range)
 *  - board:       C_block, R_bias and the four measured range resistors
 *
 * On the G474 the oscillator's T0 is expected to come out smaller and more
 * repeatable than on the ESP32, because the period is captured in hardware and
 * no software-loop latency remains to absorb. The *model* is unchanged, so the
 * old values are still meaningful as a starting point.
 */

#include "calib.h"

#include <stddef.h>
#include <string.h>

#include "bsp_store.h"
#include "config.h"

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static osc_cal_t s_osc[RANGE_COUNT];
static adc_cal_t s_adc[RANGE_COUNT];
static cal_record_t s_rec;
static bool s_loaded;

/* R-mode sense elements. Defaults are the measured C-mode range resistors plus
 * the 4.7 Mohm top element called for by the resistance plan; an element the
 * board does not actually fit simply never becomes valid. */
static const double s_rm_default[4] = {
    RSENSE_100_OHM, RSENSE_1K_OHM, RSENSE_100K_OHM, RSENSE_4M7_OHM
};

/**
 * @brief Restore compiled defaults in RAM.
 *
 * Defaults are deliberately conservative: the oscillator starts with an
 * un-tared nominal-K model (OSC_K_NOMINAL, from the COMP1 hysteresis) and
 * no ADC calibration, which makes the firmware usable but not accurate until
 * the operator runs the calibration commands.
 */
void cal_defaults(void)
{
    memset(&s_rec, 0, sizeof(s_rec));

    for (int i = 0; i < RANGE_COUNT; ++i) {
        s_osc[i].k = OSC_K_NOMINAL;
        s_osc[i].delay_us = 0.0;
        s_osc[i].t0_us = 0.0;
        s_osc[i].valid = false;
        s_osc[i].has_t0 = false;

        s_adc[i].r_eff_ohm = g_ranges[i].resistance_ohms;
        s_adc[i].c0_f = STRAY_CAPACITANCE_F;
        s_adc[i].valid = false;
    }

    for (int i = 0; i < 4; ++i) {
        s_rec.rm_rref[i] = s_rm_default[i];
        s_rec.rm_gain[i] = 1.0;
        s_rec.rm_valid[i] = 0U;
    }

    s_rec.tia_rf = RMODE_RF_OHM;
    s_rec.tia_vbias_mv = RMODE_VBIAS_MV;
    s_rec.tia_gain = 1.0;
    s_rec.tia_valid = 0U;

    s_rec.magic = STORE_RECORD_MAGIC;
    s_rec.version = STORE_RECORD_VERSION;

    board_cal_reset();
    s_loaded = true;
}

/* ------------------------------------------------------------------------- */
/* Record packing                                                            */
/* ------------------------------------------------------------------------- */

cal_record_t *cal_record(void)
{
    /* Snapshot the live state into the record before returning it, so callers
     * always serialise what is actually in use. */
    memcpy(s_rec.osc, s_osc, sizeof(s_osc));
    memcpy(s_rec.adc, s_adc, sizeof(s_adc));
    s_rec.c_block_f = board_c_block_f();
    s_rec.r_bias_ohm = board_r_bias_ohm();
    for (int i = 0; i < RANGE_COUNT; ++i) {
        s_rec.range_r[i] = g_ranges[i].resistance_ohms;
    }
    s_rec.magic = STORE_RECORD_MAGIC;
    s_rec.version = STORE_RECORD_VERSION;
    return &s_rec;
}

/**
 * @brief Apply a record to the live state.
 *
 * Validates the magic and version, then sanity-checks every value: a corrupt
 * record must never install a zero or negative resistance, because the
 * measurement maths divides by it.
 */
bool cal_apply_record(const cal_record_t *rec)
{
    if (rec == NULL || rec->magic != STORE_RECORD_MAGIC) {
        return false;
    }
    if (rec->version != STORE_RECORD_VERSION) {
        return false;
    }

    for (int i = 0; i < RANGE_COUNT; ++i) {
        if (rec->osc[i].k > 0.0 && isfinite(rec->osc[i].k)) {
            s_osc[i] = rec->osc[i];
        }
        if (rec->adc[i].r_eff_ohm > 0.0 && isfinite(rec->adc[i].r_eff_ohm)) {
            s_adc[i] = rec->adc[i];
        } else {
            s_adc[i].r_eff_ohm = g_ranges[i].resistance_ohms;
        }
    }

    if (rec->c_block_f > 0.0 && isfinite(rec->c_block_f)) {
        board_set_c_block(rec->c_block_f);
    }
    if (rec->r_bias_ohm > 0.0 && isfinite(rec->r_bias_ohm)) {
        board_set_r_bias(rec->r_bias_ohm);
    }
    for (int i = 0; i < RANGE_COUNT; ++i) {
        if (rec->range_r[i] > 0.0 && isfinite(rec->range_r[i])) {
            board_set_range_r((uint8_t)i, rec->range_r[i]);
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (rec->rm_rref[i] > 0.0 && isfinite(rec->rm_rref[i])) {
            s_rec.rm_rref[i] = rec->rm_rref[i];
        }
        if (rec->rm_gain[i] > 0.0 && isfinite(rec->rm_gain[i])) {
            s_rec.rm_gain[i] = rec->rm_gain[i];
        }
        s_rec.rm_valid[i] = rec->rm_valid[i];
    }

    if (rec->tia_rf > 0.0 && isfinite(rec->tia_rf)) {
        s_rec.tia_rf = rec->tia_rf;
    }
    if (rec->tia_vbias_mv > 0.0 && rec->tia_vbias_mv <= 3300.0 &&
        isfinite(rec->tia_vbias_mv)) {
        s_rec.tia_vbias_mv = rec->tia_vbias_mv;
    }
    if (rec->tia_gain > 0.0 && isfinite(rec->tia_gain)) {
        s_rec.tia_gain = rec->tia_gain;
    }
    s_rec.tia_valid = rec->tia_valid;

    s_loaded = true;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Load / save                                                               */
/* ------------------------------------------------------------------------- */

bool cal_load(void)
{
    /* Start from known-good defaults so a missing or corrupt record leaves the
     * instrument usable rather than uninitialised. */
    cal_defaults();

    cal_record_t rec;
    uint16_t len = sizeof(rec);
    uint16_t version = 0;

    if (!store_read(&rec, &len, &version)) {
        return false;
    }
    if (len != sizeof(rec)) {
        return false;
    }
    if (version != STORE_RECORD_VERSION) {
        return false;
    }

    /* The store already verified its own CRC over the payload; the record also
     * carries a CRC of its own so that a record copied by other means can be
     * checked. */
    uint32_t crc = store_crc32(&rec, (uint32_t)offsetof(cal_record_t, crc));
    if (crc != rec.crc) {
        return false;
    }

    return cal_apply_record(&rec);
}

bool cal_save(void)
{
    cal_record_t *rec = cal_record();
    rec->crc = store_crc32(rec, (uint32_t)offsetof(cal_record_t, crc));
    return store_write(rec, (uint16_t)sizeof(*rec), STORE_RECORD_VERSION);
}

/* ------------------------------------------------------------------------- */
/* Accessors                                                                 */
/* ------------------------------------------------------------------------- */

double cal_osc_k(uint8_t r)
{
    return (r < RANGE_COUNT) ? s_osc[r].k : 1.0;
}

double cal_osc_t0(uint8_t r)
{
    return (r < RANGE_COUNT) ? s_osc[r].t0_us : 0.0;
}

double cal_osc_delay(uint8_t r)
{
    return (r < RANGE_COUNT) ? s_osc[r].delay_us : 0.0;
}

bool cal_osc_has_t0(uint8_t r)
{
    return (r < RANGE_COUNT) && s_osc[r].has_t0;
}

bool cal_osc_valid(uint8_t r)
{
    return (r < RANGE_COUNT) && s_osc[r].valid;
}

void cal_osc_set(uint8_t r, double k, double t0, bool has_t0, bool valid)
{
    if (r >= RANGE_COUNT) {
        return;
    }
    s_osc[r].k = k;
    s_osc[r].t0_us = t0;
    s_osc[r].has_t0 = has_t0;
    s_osc[r].valid = valid;
}

void cal_osc_clear(uint8_t r)
{
    if (r >= RANGE_COUNT) {
        return;
    }
    s_osc[r].k = OSC_K_NOMINAL;
    s_osc[r].delay_us = 0.0;
    s_osc[r].t0_us = 0.0;
    s_osc[r].valid = false;
    s_osc[r].has_t0 = false;
}

double cal_adc_r_eff(uint8_t r)
{
    return (r < RANGE_COUNT) ? s_adc[r].r_eff_ohm : 0.0;
}

double cal_adc_c0(uint8_t r)
{
    return (r < RANGE_COUNT) ? s_adc[r].c0_f : 0.0;
}

bool cal_adc_valid(uint8_t r)
{
    return (r < RANGE_COUNT) && s_adc[r].valid;
}

void cal_adc_set(uint8_t r, double r_eff, double c0, bool valid)
{
    if (r >= RANGE_COUNT) {
        return;
    }
    s_adc[r].r_eff_ohm = r_eff;
    s_adc[r].c0_f = c0;
    s_adc[r].valid = valid;
}

void cal_adc_clear(uint8_t r)
{
    if (r >= RANGE_COUNT) {
        return;
    }
    s_adc[r].r_eff_ohm = g_ranges[r].resistance_ohms;
    s_adc[r].c0_f = STRAY_CAPACITANCE_F;
    s_adc[r].valid = false;
}

bool cal_board_overridden(void)
{
    return board_cal_overridden();
}

void cal_board_set_c_block(double f)
{
    board_set_c_block(f);
}

void cal_board_set_r_bias(double ohm)
{
    board_set_r_bias(ohm);
}

void cal_board_set_range_r(uint8_t r, double ohm)
{
    board_set_range_r(r, ohm);
}

void cal_board_clear(void)
{
    board_cal_reset();
}

double cal_c_block(void)
{
    return board_c_block_f();
}

double cal_r_bias(void)
{
    return board_r_bias_ohm();
}

double cal_range_r(uint8_t r)
{
    return (r < RANGE_COUNT) ? g_ranges[r].resistance_ohms : 0.0;
}

/* ------------------------------------------------------------------------- */
/* Solvers                                                                   */
/* ------------------------------------------------------------------------- */

/** Series combination through C_block, matching cblock_forward() but taking
 *  the capacitor explicitly so the solvers are independent of live state. */
static double series_eq(double c_dut, double c_block)
{
    if (c_block <= 0.0 || c_dut < 0.0 || c_dut >= c_block) {
        return -1.0;
    }
    return (c_block * c_dut) / (c_block + c_dut);
}

/**
 * @brief One-point ADC calibration.
 *
 *     tau = R_eff * C_eq(C_ref + C0)
 *  => R_eff = tau / C_eq(C_ref + C0)
 */
bool cal_solve_adc_one_point(double tau_us, double c_ref_f,
                             double c_block_f, double c0_f,
                             double *r_eff_out)
{
    if (r_eff_out == NULL || tau_us <= 0.0 || c_ref_f <= 0.0 || c_block_f <= 0.0) {
        return false;
    }

    double ceq = series_eq(c_ref_f + c0_f, c_block_f);
    if (ceq <= 0.0) {
        return false;
    }

    double r = (tau_us * 1e-6) / ceq;
    if (!(r > 0.0) || !isfinite(r) || r > 1e9) {
        return false;
    }

    *r_eff_out = r;
    return true;
}

/**
 * @brief Two-point ADC calibration: solve R_eff and C0 jointly.
 *
 * Both equations share the same R_eff:
 *
 *     tau1 = R * C_eq(C1 + C0)
 *     tau2 = R * C_eq(C2 + C0)
 *
 * Dividing eliminates R and leaves a scalar equation in C0. Because
 * C_eq(x) = Cb*x/(Cb+x) is increasing and concave, the ratio
 * C_eq(C1+x)/C_eq(C2+x) is monotonic in x for C1 != C2, so a bisection is both
 * sufficient and robust — no derivative bookkeeping and no chance of
 * diverging, which matters on an embedded target where a bad calibration must
 * simply fail rather than loop.
 */
bool cal_solve_adc_two_point(double tau1_us, double c1_f,
                             double tau2_us, double c2_f,
                             double c_block_f,
                             double *r_eff_out, double *c0_out)
{
    if (r_eff_out == NULL || c0_out == NULL) {
        return false;
    }
    if (tau1_us <= 0.0 || tau2_us <= 0.0) {
        return false;
    }
    if (c1_f <= 0.0 || c2_f <= 0.0 || c_block_f <= 0.0) {
        return false;
    }

    /* Order so that C1 > C2; the ratio test below assumes it. */
    double ta = tau1_us, tb = tau2_us;
    double ca = c1_f, cb2 = c2_f;
    if (ca < cb2) {
        double t;
        t = ca; ca = cb2; cb2 = t;
        t = ta; ta = tb; tb = t;
    }

    /* The references must differ, and both must sit below C_block. */
    double dc = ca - cb2;
    if (dc < 0.05 * ca) {
        return false;
    }
    if (ca >= c_block_f) {
        return false;
    }

    double ratio_target = ta / tb;
    if (!(ratio_target > 1.0)) {
        /* A larger reference must produce a proportionally larger tau. */
        return false;
    }

    /* g(x) = C_eq(ca+x)/C_eq(cb+x) - ratio_target, monotonically decreasing. */
    #define RATIO_AT(x)                                                       \
        (((c_block_f * ((ca) + (x))) / (c_block_f + (ca) + (x))) /            \
         ((c_block_f * ((cb2) + (x))) / (c_block_f + (cb2) + (x))))

    double g0 = RATIO_AT(0.0) - ratio_target;
    if (g0 < 0.0) {
        /* Even with zero offset the ratio is already below the measurement,
         * so no non-negative C0 can explain the data. */
        return false;
    }

    double hi = c_block_f - ca;
    if (hi <= 0.0) {
        return false;
    }
    hi *= 0.99;
    /* A node offset above 1 mF is not a parasitic, it is a bad measurement. */
    if (hi > 1e-3) {
        hi = 1e-3;
    }

    double ghi = RATIO_AT(hi) - ratio_target;
    if (ghi > 0.0) {
        return false;   /* ratio_target too small: inconsistent references */
    }

    double lo = 0.0;
    for (int it = 0; it < 100; ++it) {
        double mid = 0.5 * (lo + hi);
        double g = RATIO_AT(mid) - ratio_target;
        if (g > 0.0) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    double c0 = 0.5 * (lo + hi);

    double ceq = series_eq(ca + c0, c_block_f);
    if (ceq <= 0.0) {
        return false;
    }
    double r = (ta * 1e-6) / ceq;
    if (!(r > 0.0) || !isfinite(r) || r > 1e9) {
        return false;
    }

    #undef RATIO_AT

    *r_eff_out = r;
    *c0_out = c0;
    return true;
}

/* ------------------------------------------------------------------------- */
/* R-mode calibration accessors                                              */
/* ------------------------------------------------------------------------- */

double cal_rm_rref(uint8_t element)
{
    return (element < 4) ? s_rec.rm_rref[element] : 0.0;
}

double cal_rm_gain(uint8_t element)
{
    return (element < 4) ? s_rec.rm_gain[element] : 1.0;
}

bool cal_rm_valid(uint8_t element)
{
    return (element < 4) && (s_rec.rm_valid[element] != 0U);
}

void cal_rm_set(uint8_t element, double rref, double gain, bool valid)
{
    if (element >= 4) {
        return;
    }
    if (rref > 0.0 && isfinite(rref)) {
        s_rec.rm_rref[element] = rref;
    }
    if (gain > 0.0 && isfinite(gain)) {
        s_rec.rm_gain[element] = gain;
    }
    s_rec.rm_valid[element] = valid ? 1U : 0U;
}

void cal_rm_clear(uint8_t element)
{
    if (element >= 4) {
        return;
    }
    s_rec.rm_rref[element] = s_rm_default[element];
    s_rec.rm_gain[element] = 1.0;
    s_rec.rm_valid[element] = 0U;
}

/* ------------------------------------------------------------------------- */
/* TIA calibration accessors                                                 */
/* ------------------------------------------------------------------------- */

double cal_rm_tia_rf(void)
{
    return s_rec.tia_rf;
}

double cal_rm_tia_vbias_mv(void)
{
    return s_rec.tia_vbias_mv;
}

double cal_rm_tia_gain(void)
{
    return s_rec.tia_gain;
}

bool cal_rm_tia_valid(void)
{
    return s_rec.tia_valid != 0U;
}

void cal_rm_tia_set(double rf, double vbias_mv, double gain, bool valid)
{
    if (rf > 0.0 && isfinite(rf)) {
        s_rec.tia_rf = rf;
    }
    if (vbias_mv > 0.0 && vbias_mv <= 3300.0 && isfinite(vbias_mv)) {
        s_rec.tia_vbias_mv = vbias_mv;
    }
    if (gain > 0.0 && isfinite(gain)) {
        s_rec.tia_gain = gain;
    }
    s_rec.tia_valid = valid ? 1U : 0U;
}

void cal_rm_tia_clear(void)
{
    s_rec.tia_rf = RMODE_RF_OHM;
    s_rec.tia_vbias_mv = RMODE_VBIAS_MV;
    s_rec.tia_gain = 1.0;
    s_rec.tia_valid = 0U;
}

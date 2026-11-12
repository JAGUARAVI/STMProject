/**
 * @file  test_main.c
 * @brief Host unit tests for the ESWCap measurement maths.
 *
 * These run on the build machine against the exact source files the firmware
 * uses. The point is not coverage for its own sake: every test here encodes a
 * physical fact the instrument depends on, and a failure means a reading would
 * be wrong on the bench.
 *
 * Build and run: make -C test/host
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "eswcap.h"
#include "rc_fit.h"
#include "osc_calc.h"
#include "fusion.h"
#include "adc_measure.h"
#include "osc_measure.h"
#include "calib.h"
#include "rmode.h"

static int g_fail;
static int g_pass;
static const char *g_test;

static void begin(const char *name)
{
    g_test = name;
    printf("  %-52s ", name);
    fflush(stdout);
}

static void ok(void)
{
    ++g_pass;
    printf("ok\n");
}

static void fail(const char *fmt, ...)
{
    ++g_fail;
    printf("FAIL\n      %s: ", g_test);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fail("check failed: %s", #cond);                                   \
            return;                                                            \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b), _t = (tol);                                  \
        if (!(fabs(_a - _b) <= _t)) {                                           \
            fail("%s=%.9g vs %s=%.9g (tol %.3g)", #a, _a, #b, _b, _t);          \
            return;                                                            \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------------- */
/* RC fit                                                                    */
/* ------------------------------------------------------------------------- */

/** Generate a clean exponential curve. */
static int synth(double *t_us, double *v_mv, int n, double dt,
                 double v_inf, double tau, double t0, double v0)
{
    for (int i = 0; i < n; ++i) {
        t_us[i] = (double)i * dt;
        v_mv[i] = v_inf - (v_inf - v0) * exp(-(t_us[i] - t0) / tau);
    }
    return n;
}

static void test_rc_fit_exact(void)
{
    begin("rc_fit recovers tau from a clean curve");

    static double t[200], v[200];
    const double tau = 1200.0, v_inf = 3300.0, v0 = 20.0, t0 = -5.0;
    int n = synth(t, v, 120, 40.0, v_inf, tau, t0, v0);

    rc_fit_t f = rc_fit_samples(t, v, n, v0, 0.0);
    CHECK(f.ok);
    CHECK_NEAR(f.tau_us, tau, tau * 0.005);
    CHECK_NEAR(f.v_inf_mv, v_inf, 15.0);
    CHECK(f.r2 > 0.999);
    ok();
}

static void test_rc_fit_biased_hint(void)
{
    begin("rc_fit is insensitive to a biased asymptote hint");

    /* This is the regression the voltage-domain fit was introduced for: the
     * old log-linearisation turned a 60 mV asymptote error into ~7 % tau. */
    static double t[200], v[200];
    const double tau = 800.0;
    int n = synth(t, v, 100, 30.0, 3300.0, tau, 0.0, 0.0);

    rc_fit_t f = rc_fit_samples(t, v, n, 0.0, 3240.0);   /* 60 mV low */
    CHECK(f.ok);
    CHECK_NEAR(f.tau_us, tau, tau * 0.01);
    ok();
}

static void test_rc_fit_rejects_flat(void)
{
    begin("rc_fit rejects a flat capture");

    static double t[50], v[50];
    for (int i = 0; i < 50; ++i) {
        t[i] = i * 10.0;
        v[i] = 100.0;
    }
    rc_fit_t f = rc_fit_samples(t, v, 50, 0.0, 0.0);
    CHECK(!f.ok);
    ok();
}

/* ------------------------------------------------------------------------- */
/* C_block transform                                                         */
/* ------------------------------------------------------------------------- */

static void test_cblock_roundtrip(void)
{
    begin("cblock forward/invert round-trips");

    const double dut = 1e-6;
    double eq = cblock_forward(dut);
    CHECK(eq > 0.0);
    CHECK_NEAR(cblock_invert(eq), dut, dut * 1e-9);

    /* At the singularity the transform must refuse rather than divide by ~0. */
    CHECK(cblock_forward(board_c_block_f()) < 0.0);
    CHECK(cblock_invert(board_c_block_f()) < 0.0);
    ok();
}

/* ------------------------------------------------------------------------- */
/* Oscillator model                                                          */
/* ------------------------------------------------------------------------- */

static void test_osc_two_point(void)
{
    begin("osc two-point solve recovers K and T0");

    /* Build synthetic data from a known model, then solve it back. */
    const double r = RES_100K_OHM;
    const double k_true = 2.2;
    const double t0_true = 136.0;
    const double c_block = board_c_block_f();

    const double c1 = 100e-12, c2 = 1000e-12;
    double ceq1 = cblock_forward(c1);
    double ceq2 = cblock_forward(c2);
    double p1 = (k_true * r * ceq1 * 1e6) + t0_true;
    double p2 = (k_true * r * ceq2 * 1e6) + t0_true;

    double k = 0, t0 = 0;
    CHECK(osc_solve_two_point(p1, c1, p2, c2, r, c_block, &k, &t0));
    CHECK_NEAR(k, k_true, 0.01);
    CHECK_NEAR(t0, t0_true, 0.5);
    ok();
}

static void test_osc_two_point_rejects_close_refs(void)
{
    begin("osc two-point rejects references that are too close");
    double k = 0, t0 = 0;
    CHECK(!osc_solve_two_point(100.0, 100e-12, 101.0, 102e-12,
                               RES_100K_OHM, board_c_block_f(), &k, &t0));
    ok();
}

/* The G474 oscillator's K is set by the COMP1 hysteresis (70 mV around a
 * 1.65 V threshold, 0 .. 3.3 V drive). Check the nominal constant against the
 * relaxation formula, and that the solvers accept K across the hysteresis
 * tolerance -- the old 0.05 gate refused a real G474 calibration. */
static void test_osc_accepts_g474_k(void)
{
    begin("osc solvers accept the G474 COMP1 K (70 mV hysteresis)");

    const double vdd = 3300.0, vh = 1685.0, vl = 1615.0;
    double k_formula = log((vdd - vl) / (vdd - vh)) + log(vh / vl);
    CHECK_NEAR(k_formula, OSC_K_NOMINAL, OSC_K_NOMINAL * 0.02);

    const double r = RES_1M_OHM;
    const double t0_true = 0.4;
    const double c_block = board_c_block_f();
    const double ks[] = { OSC_K_NOMINAL, 0.5 * OSC_K_NOMINAL };

    for (size_t i = 0; i < sizeof ks / sizeof ks[0]; ++i) {
        const double c1 = 100e-12, c2 = 1000e-12;
        double p1 = (ks[i] * r * cblock_forward(c1) * 1e6) + t0_true;
        double p2 = (ks[i] * r * cblock_forward(c2) * 1e6) + t0_true;

        double k = 0, t0 = 0;
        CHECK(osc_solve_two_point(p1, c1, p2, c2, r, c_block, &k, &t0));
        CHECK_NEAR(k, ks[i], ks[i] * 0.01);
        CHECK_NEAR(osc_k_from_ref(p2, t0_true, r, c2, c_block), ks[i],
                   ks[i] * 0.01);
    }
    ok();
}

static void test_osc_measure_roundtrip(void)
{
    begin("osc measure inverts the model to the DUT value");

    const uint8_t r = 2;
    const double k = 2.2, t0 = 136.0;
    const double dut = 470e-12;
    double ceq = cblock_forward(dut);
    double period_us = (k * g_ranges[r].resistance_ohms * ceq * 1e6) + t0;

    sample_t s = osc_measure_sample(r, period_us, 60, k, t0, true, 0.0,
                                    g_ranges[r].resistance_ohms,
                                    board_c_block_f());
    CHECK(s.valid);
    CHECK(s.plausible);
    CHECK_NEAR(s.capacitance_f, dut, dut * 0.01);
    CHECK(s.quality > 0.5);
    ok();
}

static void test_osc_open_socket(void)
{
    begin("osc reports ~0 pF for an open socket when tared");

    const uint8_t r = 3;
    const double k = 2.2, t0 = 300.0;
    /* An empty socket produces exactly T0. */
    sample_t s = osc_measure_sample(r, t0, 60, k, t0, true, 0.0,
                                    g_ranges[r].resistance_ohms,
                                    board_c_block_f());
    CHECK(s.valid);
    CHECK(s.plausible);
    CHECK(s.capacitance_f < 1e-13);
    ok();
}

/* ------------------------------------------------------------------------- */
/* ADC measurement                                                           */
/* ------------------------------------------------------------------------- */

static void test_adc_measure_roundtrip(void)
{
    begin("adc measure recovers the DUT capacitance");

    const uint8_t r = 0;                 /* 100 ohm */
    const double dut = 100e-6;
    const double r_chain = g_ranges[r].resistance_ohms;
    const double v_inf = 3300.0;

    /* tau = R * C_eq, with C_eq being the series combination through C_block. */
    double ceq = cblock_forward(dut);
    double tau_us = r_chain * ceq * 1e6;

    static double v[300];
    const double dt = tau_us / 20.0;     /* 20 points per tau */
    for (int i = 0; i < 300; ++i) {
        v[i] = v_inf * (1.0 - exp(-(i * dt) / tau_us));
    }

    adc_capture_t cap;
    cap.v_mv = v;
    cap.n = 300;
    cap.dt_us = dt;
    cap.v0_mv = 0.0;
    cap.v_inf_hint_mv = v_inf;
    cap.range_idx = r;
    cap.fit_ok = false;

    double vinf_out = 0.0;
    sample_t s = adc_measure_sample(&cap, &vinf_out);

    CHECK(s.valid);
    CHECK(s.plausible);
    CHECK_NEAR(s.capacitance_f, dut, dut * 0.02);
    CHECK(s.fit_r2 > 0.99);
    ok();
}

static void test_adc_measure_fallback(void)
{
    begin("adc measure falls back when the fit is poor");

    /* A deliberately coarse, low-point capture: the fit may still succeed, but
     * either way the sample must remain usable and physical. */
    const uint8_t r = 2;
    static double v[12];
    const double v_inf = 3300.0;
    for (int i = 0; i < 12; ++i) {
        v[i] = v_inf * (1.0 - exp(-(double)i / 4.0));
    }

    adc_capture_t cap;
    cap.v_mv = v;
    cap.n = 12;
    cap.dt_us = 500.0;
    cap.v0_mv = 0.0;
    cap.v_inf_hint_mv = v_inf;
    cap.range_idx = r;
    cap.fit_ok = false;

    sample_t s = adc_measure_sample(&cap, NULL);
    CHECK(s.valid);
    CHECK(isfinite(s.capacitance_f));
    CHECK(s.capacitance_f >= 0.0);
    ok();
}

/* ------------------------------------------------------------------------- */
/* Fusion                                                                    */
/* ------------------------------------------------------------------------- */

static void test_fusion_gates_outlier(void)
{
    begin("fusion median-gates a single outlier");

    sample_t s[3];
    memset(s, 0, sizeof(s));
    for (int i = 0; i < 3; ++i) {
        s[i].valid = true;
        s[i].plausible = true;
        s[i].method = METHOD_OSC;
        s[i].range_idx = 2;
        s[i].quality = 0.9;
        s[i].c_eq_f = 1e-9;
    }
    s[0].capacitance_f = 1.00e-9;
    s[1].capacitance_f = 1.01e-9;
    s[2].capacitance_f = 10.0e-9;   /* absurd */

    fusion_t f = fuse_samples(s, 3);
    CHECK(f.valid);
    CHECK(f.n_gated == 1);
    CHECK(f.n_kept == 3);
    CHECK_NEAR(f.capacitance_f, 1.005e-9, 0.02e-9);
    ok();
}

static void test_fusion_method_mismatch(void)
{
    begin("fusion flags an ADC/oscillator disagreement");

    sample_t s[2];
    memset(s, 0, sizeof(s));
    s[0].valid = s[0].plausible = true;
    s[0].method = METHOD_ADC_STEP;
    s[0].range_idx = 0;
    s[0].quality = 1.0;
    s[0].capacitance_f = 1.0e-6;
    s[0].c_eq_f = 1.0e-6;

    s[1].valid = s[1].plausible = true;
    s[1].method = METHOD_OSC;
    s[1].range_idx = 2;
    s[1].quality = 1.0;
    s[1].capacitance_f = 3.0e-6;
    s[1].c_eq_f = 3.0e-6;

    fusion_t f = fuse_samples(s, 2);
    CHECK(f.valid);
    CHECK(f.method_mismatch);
    CHECK_NEAR(f.method_mismatch_ratio, 3.0, 0.01);
    ok();
}

static void test_fusion_log_mean(void)
{
    begin("fusion averages in the log domain");

    /* Two equal-quality samples a decade apart must fuse to their geometric
     * mean, not their arithmetic one. Capacitance spans six decades, so the
     * arithmetic mean would be dominated by the larger value. */
    sample_t s[2];
    memset(s, 0, sizeof(s));
    for (int i = 0; i < 2; ++i) {
        s[i].valid = s[i].plausible = true;
        s[i].method = METHOD_OSC;
        s[i].range_idx = 2;
        s[i].quality = 1.0;
    }
    s[0].capacitance_f = 1e-9;
    s[0].c_eq_f = 1e-9;
    s[1].capacitance_f = 1e-8;
    s[1].c_eq_f = 1e-8;

    fusion_t f = fuse_samples(s, 2);
    CHECK(f.valid);
    CHECK_NEAR(f.capacitance_f, 3.16227766e-9, 1e-12);
    ok();
}

/* ------------------------------------------------------------------------- */
/* Calibration solvers                                                       */
/* ------------------------------------------------------------------------- */

static void test_cal_adc_two_point(void)
{
    begin("adc two-point calibration recovers R_eff and C0");

    const double r_true = 120.5;      /* nominal 100 + series */
    const double c0_true = 30e-12;
    const double c_block = board_c_block_f();

    const double c1 = 100e-9, c2 = 470e-9;
    double tau1 = r_true * cblock_forward(c1 + c0_true) * 1e6;
    double tau2 = r_true * cblock_forward(c2 + c0_true) * 1e6;

    double r = 0, c0 = 0;
    CHECK(cal_solve_adc_two_point(tau1, c1, tau2, c2, c_block, &r, &c0));
    CHECK_NEAR(r, r_true, r_true * 0.01);
    CHECK_NEAR(c0, c0_true, 1e-12);
    ok();
}

static void test_cal_adc_one_point(void)
{
    begin("adc one-point calibration recovers R_eff");

    const double r_true = 1010.0;
    const double c0 = 12e-12;
    const double c_ref = 2.2e-6;

    double tau = r_true * cblock_forward(c_ref + c0) * 1e6;
    double r = 0;
    CHECK(cal_solve_adc_one_point(tau, c_ref, board_c_block_f(), c0, &r));
    CHECK_NEAR(r, r_true, r_true * 0.01);
    ok();
}

static void test_cal_rejects_bad_input(void)
{
    begin("calibration solvers reject degenerate input");
    double r = 0, c0 = 0;
    /* Identical references cannot separate R from C0. */
    CHECK(!cal_solve_adc_two_point(100.0, 1e-6, 100.0, 1e-6,
                                   board_c_block_f(), &r, &c0));
    CHECK(!cal_solve_adc_one_point(-1.0, 1e-6, board_c_block_f(), 0.0, &r));
    ok();
}


/* ------------------------------------------------------------------------- */
/* R-mode equations                                                          */
/* ------------------------------------------------------------------------- */

static void test_rmode_kelvin(void)
{
    begin("Kelvin equation recovers R from V_N and V_B");

    /* V_B is servoed to VREF, so V_N = VREF * (R/RSENSE + 1). */
    const double rref = 993.6;
    const double r_dut = 1000.0;
    const double vb = 405.0;
    const double vn = vb * (r_dut / rref + 1.0);

    CHECK_NEAR(rmode_resistance(rref, vn, vb), r_dut, r_dut * 0.001);

    /* The current is VREF/RSENSE. */
    CHECK_NEAR(rmode_current_ua(rref, vb), 407.6, 0.5);
    ok();
}

static void test_rmode_kelvin_rejects(void)
{
    begin("Kelvin equation rejects out-of-compliance inputs");

    CHECK(rmode_resistance(993.6, 500.0, 10.0) < 0.0);   /* V_B too small */
    CHECK(rmode_resistance(993.6, 400.0, 405.0) < 0.0);  /* V_N <= V_B  */
    CHECK(rmode_resistance(0.0, 800.0, 405.0) < 0.0);    /* bad RSENSE  */
    ok();
}

static void test_rmode_tia(void)
{
    begin("TIA equation recovers R from V_N, VBIAS and V_TIA");

    /* R = Rf * (V_N - VBIAS) / (VBIAS - V_TIA) */
    const double rf = 10.0e6;
    const double vbias = 1650.0;
    const double vn = 3300.0;      /* Q1 fully on, minus a small drop */
    const double vtia = 1100.0;    /* 1.65 - (1.65 * 10M / 30M) */

    CHECK_NEAR(rmode_tia_resistance(rf, vn, vbias, vtia), 30.0e6, 1.0);

    /* The 1 Gohm end: V_TIA is only 16.5 mV below VBIAS. */
    const double vtia_1g = vbias - (vn - vbias) * rf / 1.0e9;
    CHECK_NEAR(rmode_tia_resistance(rf, vn, vbias, vtia_1g), 1.0e9, 1.0e4);
    ok();
}

static void test_rmode_tia_rejects(void)
{
    begin("TIA equation rejects out-of-range inputs");

    CHECK(rmode_tia_resistance(10e6, 1600.0, 1650.0, 1000.0) < 0.0); /* V_N < VBIAS */
    CHECK(rmode_tia_resistance(10e6, 3300.0, 1650.0, 1700.0) < 0.0); /* V_TIA > VBIAS */
    CHECK(rmode_tia_resistance(10e6, 3300.0, 1650.0, 20.0) < 0.0);   /* rail */
    ok();
}


static void test_cal_rmode_setters(void)
{
    begin("R-mode calibration setters accept real values and reject typos");

    cal_defaults();

    /* Nominal state: compiled defaults, nothing marked calibrated. */
    CHECK(cal_rm_tia_valid() == false);
    CHECK_NEAR(cal_rm_tia_rf(), RMODE_RF_OHM, 1.0);
    CHECK_NEAR(cal_rm_tia_vbias_mv(), (double)RMODE_VBIAS_MV, 0.5);

    /* A plausible bench measurement is stored verbatim. */
    cal_rm_tia_set(10.02e6, 1648.0, 1.013, true);
    CHECK(cal_rm_tia_valid() == true);
    CHECK_NEAR(cal_rm_tia_rf(), 10.02e6, 1.0);
    CHECK_NEAR(cal_rm_tia_vbias_mv(), 1648.0, 0.01);
    CHECK_NEAR(cal_rm_tia_gain(), 1.013, 1e-9);

    /* Out-of-range values are ignored, not clamped into the record: a typo
     * must not silently become the calibration. */
    cal_rm_tia_set(-1.0, 1648.0, 1.013, true);
    CHECK_NEAR(cal_rm_tia_rf(), 10.02e6, 1.0);

    cal_rm_tia_set(10.02e6, 5000.0, 1.013, true);
    CHECK_NEAR(cal_rm_tia_vbias_mv(), 1648.0, 0.01);

    cal_rm_tia_set(10.02e6, 1648.0, 0.0, true);
    CHECK_NEAR(cal_rm_tia_gain(), 1.013, 1e-9);

    cal_rm_tia_clear();
    CHECK(cal_rm_tia_valid() == false);
    CHECK_NEAR(cal_rm_tia_rf(), RMODE_RF_OHM, 1.0);

    /* Sense elements: same contract, plus the validity flag that gates the
     * "calibrated" wording in `rcal?`. */
    cal_rm_set(0U, 99.42, 1.006, false);
    CHECK(cal_rm_valid(0U) == false);
    CHECK_NEAR(cal_rm_rref(0U), 99.42, 1e-9);

    cal_rm_set(0U, 99.42, 1.006, true);
    CHECK(cal_rm_valid(0U) == true);

    cal_rm_clear(0U);
    CHECK(cal_rm_valid(0U) == false);
    CHECK(cal_rm_rref(0U) > 0.0);

    /* An out-of-range element index must be refused rather than corrupt the
     * record or read past the array. */
    CHECK(cal_rm_rref(RMODE_ELEMENTS) == 0.0);
    CHECK(cal_rm_valid(RMODE_ELEMENTS) == false);

    ok();
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    printf("ESWCap host tests\n\n");

    test_rc_fit_exact();
    test_rc_fit_biased_hint();
    test_rc_fit_rejects_flat();

    test_cblock_roundtrip();

    test_osc_two_point();
    test_osc_two_point_rejects_close_refs();
    test_osc_accepts_g474_k();
    test_osc_measure_roundtrip();
    test_osc_open_socket();

    test_adc_measure_roundtrip();
    test_adc_measure_fallback();

    test_fusion_gates_outlier();
    test_fusion_method_mismatch();
    test_fusion_log_mean();

    test_cal_adc_two_point();
    test_cal_adc_one_point();
    test_cal_rejects_bad_input();
    test_cal_rmode_setters();

    test_rmode_kelvin();
    test_rmode_kelvin_rejects();
    test_rmode_tia();
    test_rmode_tia_rejects();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}

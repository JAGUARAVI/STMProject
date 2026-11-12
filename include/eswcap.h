/**
 * @file    eswcap.h
 * @brief   Shared types and constants for the ESWCap instrument.
 *
 * These types are the contract between the hardware layer (src/bsp), the
 * measurement maths (src/measure, src/calib) and the protocol layer
 * (src/proto). The maths modules must stay free of HAL calls so they can be
 * compiled and unit-tested on the host.
 */

#ifndef ESWCAP_ESWCAP_H
#define ESWCAP_ESWCAP_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "config.h"

/* ------------------------------------------------------------------------- */
/* Measurement methods                                                       */
/* ------------------------------------------------------------------------- */

typedef enum {
    METHOD_ADC_STEP = 0,   /**< RC charge curve, tau fit */
    METHOD_OSC      = 1,   /**< comparator relaxation oscillator */
} method_t;

/* ------------------------------------------------------------------------- */
/* Range configuration                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t     mux_channel;      /**< MAX4619 select code */
    double      resistance_ohms;  /**< measured range resistor */
    const char *label;            /**< "100 R", "1 kR", ... */
} range_config_t;

/* Mutable at runtime: a per-unit flash override may replace the measured
 * values without a reflash. */
extern range_config_t g_ranges[RANGE_COUNT];

/* Effective board constants (compiled defaults, overridable from flash). */
double board_c_block_f(void);
double board_r_bias_ohm(void);
bool   board_cal_overridden(void);
void   board_set_c_block(double f);
void   board_set_r_bias(double ohm);
void   board_set_range_r(uint8_t idx, double ohm);
void   board_cal_reset(void);

/* ------------------------------------------------------------------------- */
/* A single measurement sample                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool     valid;
    bool     plausible;      /**< passed hard physics sanity checks */
    method_t method;
    uint8_t  range_idx;
    double   capacitance_f;  /**< reconstructed DUT capacitance */
    double   c_eq_f;         /**< raw series-equivalent seen by the circuit */
    double   quality;        /**< 0..1 physics-based confidence */
    double   tau_us;         /**< ADC: fitted tau. OSC: measured full period */
    double   fit_r2;         /**< ADC: R^2 (1 = perfect, <0 = fallback) */
    double   fit_rmse_mv;    /**< ADC: voltage-domain RMS residual */
    double   tau_unc_us;     /**< ADC: 1-sigma standard error of tau (<0 none) */
    double   v_inf_mv;       /**< ADC: asymptote used/fitted */
    double   freq_hz;        /**< OSC only */
    bool     suspicious;     /**< timing/health looks wrong */
} sample_t;

/* ------------------------------------------------------------------------- */
/* Fusion                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t range_idx;
    uint8_t method;          /**< method_t */
    double  c_f;
    double  c_eq_f;
    double  q;
    double  w;
    double  r2;
    bool    kept;
} fusion_contrib_t;

#define FUSION_MAX_CONTRIB MAX_SAMPLES
extern fusion_contrib_t g_fusion_contrib[FUSION_MAX_CONTRIB];
extern int              g_fusion_n_contrib;

typedef struct {
    bool   valid;
    double capacitance_f;
    double rel_spread;
    double total_weight;
    int    n_adc;
    int    n_osc;
    double adc_estimate;          /**< NAN if none */
    double osc_estimate;          /**< NAN if none */
    bool   method_mismatch;
    double method_mismatch_ratio; /**< NAN if only one method */
    double quality;
    int    raw_samples;
    /* breakdown telemetry */
    double median_f;
    int    n_kept;
    int    n_gated;
    double c_min_f;
    double c_max_f;
} fusion_t;

/* ------------------------------------------------------------------------- */
/* Autoranging decision trace                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool   valid;
    bool   locked;
    int    lock_range;
    bool   have_rough;
    double rough_f;
    bool   sub_nf;
    bool   saturated;
    bool   adc_sub;
    int    osc_best_range;
    bool   adc_tried[RANGE_COUNT];
} sweep_info_t;

extern sweep_info_t g_sweep;

/* ------------------------------------------------------------------------- */
/* Calibration records                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    double k;        /**< dimensionless geometry constant */
    double delay_us; /**< legacy per-cycle offset (kept for compatibility) */
    double t0_us;    /**< measured open-node period offset */
    bool   valid;
    bool   has_t0;
} osc_cal_t;

typedef struct {
    double r_eff_ohm; /**< measured R_nom + R_series */
    double c0_f;      /**< node offset capacitance */
    bool   valid;
} adc_cal_t;

/* ------------------------------------------------------------------------- */
/* Front-end state machine                                                   */
/* ------------------------------------------------------------------------- */

typedef enum {
    FE_IDLE = 0,    /**< all SSRs/relays off, drive low: safe to touch */
    FE_PRECHARGE,   /**< charging C_block/N_DUT at bias */
    FE_PRECHARGED,  /**< isolated, N_DUT still biased: do NOT remove DUT */
    FE_MEASURING,
    FE_DISCHARGE,   /**< bleeding DUT/C_block to GND */
    FE_R_ISOLATE,   /**< R mode: B lifted, C_block isolated, V_N buffer on */
    FE_R_KELVIN,    /**< R mode: K2 on, R mux on, OPAMP1 regulating */
    FE_R_TIA,       /**< R mode: TIA feedback closed, Q1 fully on */
    FE_FAULT,       /**< overvoltage or interlock fault; force path removed */
} fe_state_t;

/* ------------------------------------------------------------------------- */
/* RC fit result                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool   ok;
    double tau_us;
    double v_inf_mv;
    double t0_us;
    double r2;
    double rmse_mv;
    double tau_se_us;
    int    n;
} rc_fit_t;

/* ------------------------------------------------------------------------- */
/* R-mode (4-wire resistance)                                                */
/* ------------------------------------------------------------------------- */

typedef enum {
    RMODE_KELVIN = 0,
    RMODE_TIA    = 1,
} rmode_method_t;

typedef struct {
    bool           valid;
    rmode_method_t method;
    int            element;      /**< sense-resistor index, -1 for the TIA */
    double         r_ohm;        /**< measured resistance */

    /* Node voltages in the vocabulary of the actual circuit:
     *   v_n_mv   = N_DUT   (force node, sensed through OPAMP3)
     *   v_b_mv   = B       (Kelvin sense) or VBIAS in TIA mode
     *   v_tia_mv = TIA_OUT (TIA mode only; -1 in Kelvin)
     * The earlier v_top/v_bot names described the wrong topology and were
     * replaced by docs/handoff-verdict.md. */
    double         v_n_mv;
    double         v_b_mv;
    double         v_tia_mv;

    double         current_ua;
    double         quality;
    bool           saturated;
} res_sample_t;

/* ------------------------------------------------------------------------- */
/* Small helpers shared across modules                                       */
/* ------------------------------------------------------------------------- */

static inline double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/** Series combination of the DUT with the DC-blocking capacitor.
 *  Returns -1 when the DUT is at or beyond the singularity. */
double cblock_forward(double c_dut);
/** Inverse of cblock_forward: recovers the total from the series equivalent.
 *  Returns -1 when non-physical. */
double cblock_invert(double c_eq);

#endif /* ESWCAP_ESWCAP_H */

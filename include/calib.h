/**
 * @file  calib.h
 * @brief Calibration state: oscillator K/T0, ADC R_eff/C0, board constants.
 *
 * Persisted through bsp_store as a single versioned record. Accessors are
 * cheap and safe to call from the measurement path.
 */

#ifndef ESWCAP_CALIB_H
#define ESWCAP_CALIB_H

#include "eswcap.h"

/** Load from flash; falls back to compiled defaults when absent/corrupt. */
bool cal_load(void);

/** Persist the current state. */
bool cal_save(void);

/** Restore compiled defaults in RAM (does not save). */
void cal_defaults(void);

/* Oscillator calibration, per range. */
double cal_osc_k(uint8_t r);
double cal_osc_t0(uint8_t r);
double cal_osc_delay(uint8_t r);
bool   cal_osc_has_t0(uint8_t r);
bool   cal_osc_valid(uint8_t r);
void   cal_osc_set(uint8_t r, double k, double t0, bool has_t0, bool valid);
void   cal_osc_clear(uint8_t r);

/* ADC calibration, per range. */
double cal_adc_r_eff(uint8_t r);
double cal_adc_c0(uint8_t r);
bool   cal_adc_valid(uint8_t r);
void   cal_adc_set(uint8_t r, double r_eff, double c0, bool valid);
void   cal_adc_clear(uint8_t r);

/* Board constants (override the compiled defaults). */
bool   cal_board_overridden(void);
void   cal_board_set_c_block(double f);
void   cal_board_set_r_bias(double ohm);
void   cal_board_set_range_r(uint8_t r, double ohm);
void   cal_board_clear(void);
/* Convenience mirrors used by the maths modules. */
double cal_c_block(void);
double cal_r_bias(void);
double cal_range_r(uint8_t r);

/* ------------------------------------------------------------------------- */
/* Solver helpers (pure)                                                     */
/* ------------------------------------------------------------------------- */

/** Two-point ADC calibration: solve R_eff and C0 from two known references.
 *  tau_i = R_eff * (C_ref_i + C0)   [after removing the C_block combination]
 *  @return false on degenerate input. */
bool cal_solve_adc_two_point(double tau1_us, double c1_f,
                             double tau2_us, double c2_f,
                             double c_block_f,
                             double *r_eff_out, double *c0_out);

/** One-point ADC calibration: R_eff from tau and a known reference, C0 given. */
bool cal_solve_adc_one_point(double tau_us, double c_ref_f,
                             double c_block_f, double c0_f,
                             double *r_eff_out);

/** Serialise/restore the whole calibration record (used by the store). */
typedef struct {
    uint32_t  magic;
    uint16_t  version;
    uint16_t  reserved;
    osc_cal_t osc[RANGE_COUNT];
    adc_cal_t adc[RANGE_COUNT];
    double    c_block_f;
    double    r_bias_ohm;
    double    range_r[RANGE_COUNT];
    /* R-mode: per sense element. Stored as plain arrays so the record layout
     * stays obvious when inspected in a hex dump. */
    double    rm_rref[4];
    double    rm_gain[4];
    uint32_t  rm_valid[4];
    /* TIA (LMC6482 A) calibration. */
    double    tia_rf;
    double    tia_vbias_mv;
    double    tia_gain;
    uint32_t  tia_valid;
    uint32_t  crc;
} cal_record_t;

cal_record_t *cal_record(void);
bool          cal_apply_record(const cal_record_t *rec);

/* R-mode calibration accessors. */
double cal_rm_rref(uint8_t element);
double cal_rm_gain(uint8_t element);
bool   cal_rm_valid(uint8_t element);
void   cal_rm_set(uint8_t element, double rref, double gain, bool valid);
void   cal_rm_clear(uint8_t element);

/* TIA calibration accessors. */
double cal_rm_tia_rf(void);
double cal_rm_tia_vbias_mv(void);
double cal_rm_tia_gain(void);
bool   cal_rm_tia_valid(void);
void   cal_rm_tia_set(double rf, double vbias_mv, double gain, bool valid);
void   cal_rm_tia_clear(void);

#endif /* ESWCAP_CALIB_H */

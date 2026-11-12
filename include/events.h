/**
 * @file  events.h
 * @brief @@EVT single-line JSON telemetry.
 *
 * The host dashboard parses these byte-for-byte; the contract must not drift.
 * Event kinds and field names are preserved from the ESP32 firmware. The new
 * `res` event is additive.
 */

#ifndef ESWCAP_EVENTS_H
#define ESWCAP_EVENTS_H

#include "eswcap.h"
#include "bsp_frontend.h"

void evt_boot(void);
void evt_cycle(const fusion_t *f, uint32_t cycle_ms);
void evt_sample(const char *phase, const sample_t *s);
void evt_fuse(const fusion_t *f);
void evt_sweep(void);
void evt_stat(void);
void evt_fe(void);
void evt_ack(const char *cmd, int ok, const char *msg);

void evt_calpt(int range_idx, const char *op, double ref_pf, double freq);
void evt_calres(int range_idx, const char *op, double k, double t0, int ok);
void evt_adccalpt(int range_idx, const char *op, double ref_pf, double tau_us);
void evt_adccalres(int range_idx, const char *op, double r_eff, double c0_f, int ok);
void evt_tare(int range_idx, int idx, int total, double freq, double t0_us,
              double stray_pf, int done);

/** Full ADC charge curve plus the fitted model. Point i is emitted at
 *  i * dt_us, matching the hardware-timed uniform capture. */
void evt_curve(int range_idx, int v_inf, int v0, double t0_us, double tau_us,
               double r2, const double *v_mv, int n, double dt_us);

/** R-mode result. Additive to the original protocol. */
void evt_res(const res_sample_t *r);

/** Called by the console dispatcher. */
void evt_set_stream(bool on);
bool evt_stream_enabled(void);

/** Emit a raw preformatted JSON body after the sentinel (internal helper). */
void evt_emit_json(const char *json);

#endif /* ESWCAP_EVENTS_H */

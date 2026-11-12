/**
 * @file  instrument.h
 * @brief Global instrument state shared by the scheduler and the console.
 */

#ifndef ESWCAP_INSTRUMENT_H
#define ESWCAP_INSTRUMENT_H

#include "eswcap.h"
#include "bsp_frontend.h"

typedef struct {
    bool       run;            /**< autoranging enabled */
    bool       single;         /**< one cycle then stop */
    bool       locked;         /**< locked-range mode */
    int        lock_range;     /**< -1 when autoranging */
    bool       auto_precharge;
    bool       auto_discharge;
    bool       stream;         /**< emit @@EVT telemetry */
    bool       curve;          /**< include charge curves */
    uint32_t   cycles;         /**< completed cycles */
    uint32_t   last_cycle_ms;
    op_mode_t  mode;
} instrument_t;

extern instrument_t g_inst;

/** Boot-time defaults: stopped, both automation flags off (matches the ESP32
 *  safe-boot behaviour). */
void instrument_init(void);

#endif /* ESWCAP_INSTRUMENT_H */

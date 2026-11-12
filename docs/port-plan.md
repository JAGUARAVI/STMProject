# ESWCap on STM32G474 — Port & Rework Plan

**Status:** implemented. The firmware, the host test suite and this document set
are complete; hardware bring-up and calibration remain (see §6, Phase 6).
**Supersedes:** the ESP32/ESP-IDF implementation and the two carried-over
resistance-mode notes in `docs/`.

This document records what the ESP32 firmware did, what the G474 offers instead,
every document that had to change, and the build sequence that was followed.

---

## 0. Executive summary

The ESP32 firmware is ~4 000 lines in one `main.c`, built on three ideas:
a **software-polled ADC** capturing an RC charge curve, a **software ISR +
PCNT** oscillator loop around an external LM393, and **NVS** for calibration.
It works, but four of its hardest-won mitigations exist only because the ESP32
hardware is weak:

| ESP32 pain, visible in the code/comments | Root cause | G474 fix |
| --- | --- | --- |
| Adaptive decimation, ~100 µs poll jitter, `ADC_POINT_AVG` fudges | Software ADC polling | **Timer-triggered ADC + circular DMA** — jitter-free, fixed sample period |
| `OSC_MIN_EDGE_INTERVAL_US = 3`, ISR self-limiting, "too fast = device freeze" | ISR mirrors comparator to drive pin | **COMP1 → TIM2 input capture** — no CPU in the loop |
| `T0` tare absorbs ISR latency; latency jitter caps sub-100 pF | Software loop latency | **Hardware capture**: 5.88 ns resolution, latency removed |
| "ADC nonlinear above ~2.45 V", eFuse curve fitting, per-board gain patch | ESP32 SAR ADC | **12-bit G474 ADC + 64–256× HW oversampling** → 14–16 effective bits |
| 136 pF node offset, sub-nF gate | Unbuffered high-Z node | G474 ADC is far better *and* an **OPAMP follower** is available |
| NVS blob store | ESP32 flash abstraction | **Reserved flash page + CRC record store** (or TAMP backup registers) |

**The port is therefore not a translation — it is an upgrade.** The physics
(`T = K·R·C + T0`, `R = V_bot/I`) and the host protocol are preserved exactly;
the acquisition layer is replaced with silicon.

### The single most important consequence

Because hardware now timestamps and converts, most of the **empirical
fudge factors disappear**, and with them most of the calibration ritual:

* `OSC_DELAY_IDEAL_US`, `osc_delay()`, the ISR rate limiter — **deleted**, no
  software delay exists any more.
* `ADC_POINT_AVG`, adaptive decimation — **deleted**, replaced by oversampling.
* `STRAY_CAPACITANCE_F` fallback — retained but only as a cold-start guess;
  `T0` remains the real mechanism.
* `K` and `T0` calibration **stay** (they model real analog geometry, not CPU
  latency) and become *more* stable, so the two-point `cal1`/`cal2` procedure
  gets better, not obsolete.

---

## 1. What the ESP32 firmware does today (audit)

Command of record: `~/Documents/PlatformIO/Projects/ESWCap/src/main.c` — 4 021
lines, ESP-IDF, single file.

### 1.1 Hardware interface used

| Function | ESP32 | Notes |
| --- | --- | --- |
| `V_cap` sense | `GPIO10` = ADC1_CH9, `adc_oneshot` + `adc_cali` | software polls |
| LM393 output | `GPIO14`, `gpio_isr_handler_add` ANYEDGE | software oscillator loop |
| Excitation drive | `GPIO16` (SN74LVC1G34) | toggled by ISR |
| Range mux A0/A1 | `GPIO4`, `GPIO5` | MAX4619 |
| Precharge SSR1+SSR3 | `GPIO18` | shared pin |
| Discharge SSR2 | `GPIO12` | |
| V_BIAS return | `GPIO3` | held low |
| Status LED | `GPIO48` WS2812 via RMT | |
| Oscillator counting | PCNT, 20 ms quick gate, 500 ms sub-gated | |

### 1.2 Measurement methods (unchanged physics)

* **ADC RC-step** — drive the node high, capture `V(t)`, fit
  `V(t) = V_inf − (V_inf − V0)·e^(−(t−t0)/τ)` by damped Gauss–Newton, solving
  `V_inf`, `τ` and `t0` jointly; `C = τ / R_eff`. Includes a voltage-domain RMSE
  quality metric and a τ standard error from the fit covariance.
* **LM393 relaxation oscillator** — the ISR mirrors the comparator back to the
  drive pin, closing a hardware-in-the-loop loop. `T = K·R·C_dut + T0`, with
  per-range `K` and a **measured** open-node `T0` tare.
* **Fusion** — score every (range, method) sample, median-gate at ±87.5 %, then
  a **log-domain weighted average**; report an ADC-vs-OSC method-mismatch alarm
  above 1.5×.

### 1.3 Persistent state (the NVS surface to replace)

| Namespace | Contents |
| --- | --- |
| `osc_cal` | per range: `K`, `delay_us`, `T0`, `valid`, `has_t0` |
| `adc_cal` | per range: `R_eff_ohm`, `c0_f`, `valid` |
| `board_cal` | `C_block`, `R_bias`, and the four `RES_*` bank values |

### 1.4 Host-facing contract (must not break)

`@@EVT {json}` single-line events over UART at 115 200, consumed by
`host/server.py` (FastAPI + WebSocket) and rendered by `host/static/app.js`
and `precision.js`:

`boot`, `cycle`, `sample`, `curve`, `fuse`, `sweep`, `stat`, `fe`, `tare`,
`calpt`, `calres`, `adccalpt`, `adccalres`, `ack`, and the additive `res`.

**This contract is preserved byte-for-byte.** The only addition is `res`, the
R-mode result, which no existing consumer reads:

```
@@EVT {"t":"res","valid":true,"rx":999.9,"method":"kelvin","element":1,
       "vn":405.1,"vb":405.0,"vtia":null,"i":407.6,"q":0.93,
       "flags":["uncal"]}
```

* `rx` — measured resistance, ohms.
* `vn` — `N_DUT` (force node), mV.
* `vb` — `B` (Kelvin sense) or the VBIAS reference in TIA mode, mV.
* `vtia` — TIA output in TIA mode, `null` in Kelvin.
* `i` — forced current, µA.
* `q` — 0..1 quality, the same number the status LED uses.
* `flags` — subset of `invalid`, `saturated`, `uncal`, `tia_uncal`.

The plan sketch called the envelope key `ev`; the firmware uses `t` like every
other event so one parser handles the whole stream.

### 1.5 Command surface to preserve

`help, status|p, 0-3, start, stop, single, precharge, discharge, idle,
autoprecharge on|off, autodischarge on|off, auto?, auto, range <0-3>, zero,
zeroall, cal <pF>, cal1/cal2 <pF>, cal?, calclear, adccal <pF>,
adccal1/adccal2 <pF>, adccal?, adccalclear, board?, boardset, boardclear,
probe <0-3>, stream on|off, curve on|off, led auto|off|<r> <g> <b>`

New commands this plan adds: `mode c|r`, `rcal`, `rcal?`, `rcalclear`,
`rset`, plus R-mode telemetry.

### 1.6 Front-end state machine (preserve exactly)

```
FE_IDLE → FE_PRECHARGE → FE_PRECHARGED → FE_MEASURING → FE_DISCHARGE → FE_IDLE
```

The **bias-preserving rule** is safety-critical and must survive the port:
a measurement **never closes SSR2**; the DUT stays at `V_BIAS` through
`R_bias` while only the MCU-side node is reset. `g_fe_charged` /
`g_fe_discharged` track the persistent charge state independently of the
momentary phase, and `FE_PRECHARGED` is reported after a stop.

**Mode interlock is new** (from the R-mode plans): C-mode and R-mode must be
mutually exclusive. Rule 2 in `docs/kelvin-ohmmeter-g474.md` §8 matters —
"never close SSR2 during a measurement" is a **C-mode-only** rule, so the
reviewer must not read R-mode holding SSR2 on as a regression.

---

## 2. Target hardware capability (verified)

All figures verified against the local CubeG4 package, RM0440 and the
STM32G474CE pin database. Full tables are in `README.md`.

### 2.1 What is available

| Block | Available on G474CEU6 | Use in this design |
| --- | --- | --- |
| ADC | 5 × 12-bit, 4 Msps, 60 MHz max ADC clock, **HW oversampling 2–256×** | RC-step capture, ratiometric V_top/V_bot |
| COMP | **COMP1–7 all bonded** | Replaces the LM393 |
| DAC | DAC1 (PA4/PA5), DAC2 (PA6), **DAC3/DAC4 internal-only** | DAC3_CH1 = COMP1 threshold; DAC1_CH1 (internal) = COMP4 overvoltage |
| OPAMP | **OPAMP1/2/3**, 13 MHz GBW, PGA ×2…×64, `InternalOutput`→ADC | Node buffer, TIA |
| TIM | TIM2/TIM5 32-bit; TIM6/7 basic; HRTIM1 | Period capture, ADC trigger, precise step |
| VREF+ | **Dedicated pin 20** | Clean external reference |
| Storage | 512 KB flash, **2 KB pages, dual-bank**, 32 × 32-bit TAMP backup regs | Calibration store |
| Watchdog | IWDG (independent) + WWDG | Safety net |

### 2.2 The important silicon facts

1. **Timer-triggered ADC.** `TIM6 TRGO_UPDATE → ADC external trigger`, with
   `DNAContinuousRequests = ENABLE` into a circular `uint16_t` buffer, gives a
   **crystal-locked, CPU-free sample clock**. `ARR = 170/f − 1` (100 kHz → 1699).
   This alone deletes the entire adaptive-decimation apparatus.
2. **Comparator input capture.** `COMP1_OUT → TIM2_CH1` internally, no pin.
   TIM2 is 32-bit: at 170 MHz that is 5.88 ns resolution and a 25.3 s wrap,
   covering 10 Hz–1 MHz with no overflow handling. `CCR1` is latched by
   hardware; a DMA stream of captures averaged over N periods gives
   ~0.37 ns standard error at N=256.
3. **Oversampling semantics.** Ratio 2–256×, shift right 0–8, output
   **truncated at 16 bits**. To keep a mean-preserving scale (best for
   ratiometric work) set `shift = log2(ratio)`; to maximise resolution set
   `shift = log2(ratio) − 4` (e.g. 256× → shift 4 → 16 bits).
4. **DAC3/DAC4 are internal-only and free.** Driving COMP1's `INM` from
   `DAC3_CH1` costs **no pin**, leaving PA4/PA5 for other use. This is a
   strictly better option than the `DAC1_CH1` route in the old notes.
5. **OPAMP1 `VINP` can come from `DAC3_CH1`, and `VOUT` can go to
   `ADC1_IN13` with no pin** — a fully internal `DAC→OPAMP→ADC` chain.

### 2.3 Errata that shape the code (from ES0430) — read before writing ADC code

These are the highest-risk items and they have **direct code consequences**:

| Erratum | Effect | Required mitigation |
| --- | --- | --- |
| **§2.7.2** channel switching disturbs conversions | Noise/DNL when the input mux switches | **Single channel, continuous/bulb mode**; never interleave channels in one sequence |
| **§2.7.3** conversion >1 ms after the previous one is wrong | Slow-rate readings are silently corrupted | **Keep a continuous conversion stream** (circular DMA) so no gap exceeds 1 ms, or issue + discard a dummy |
| **§2.7.4** spurious conversion returns zero | Zero samples after stop / injected-regular collision | Never mix injected with regular; use circular (not one-shot) DMA; discard the first sample after any stop |
| Offset registers ignored under oversampling | `ADC_OFRy` silently does nothing | Do offset correction in **software math**, never rely on `OFRy` with oversampling on |
| 10/8/6-bit EOC disturbs other ADCs | Cross-ADC corruption | **Stay 12-bit everywhere** |

Items 2 and 3 are the ones that will bite first. The circular-DMA pattern
satisfies both simultaneously, which is another reason it is the right design.

---

## 3. Proposed architecture

### 3.1 Module decomposition

`main.c` is split, because 4 000 lines in one file with hardware access
interleaved with maths is not maintainable — and the maths must be
**host-testable** (it currently is not).

```
src/
  main.c                  entry point; hands control to app_init/app_run
  app.c/.h                cooperative scheduler; the only place that measures
  bsp/                    hardware layer (only place that touches registers)
    bsp.c/.h              clocks, tick, DWT, IWDG, USB clock
    bsp_adc.c/.h          ADC1 stream + ADC2/3/4 single reads + VREFINT
    bsp_comp.c/.h         COMP1 oscillator + TIM2 TISEL capture; COMP4 guard
    bsp_frontend.c/.h     SSR/relay/mux GPIO, state machine, interlocks
    bsp_dac.c/.h          DAC3_CH1 (COMP1) and DAC1_CH1 (COMP4), internal
    bsp_opamp.c/.h        OPAMP1 Kelvin error amplifier
    bsp_opamp3.c/.h       OPAMP3 V_N unity buffer
    bsp_led.c/.h          PC6 software-PWM status LED
    bsp_console.c/.h      USART1, line pump
    bsp_store.c/.h        flash record store (NVS replacement)
  measure/                pure arithmetic where possible
    rc_fit.c/.h           Gauss-Newton exponential fit   [PORT: near-verbatim]
    osc_calc.c/.h         K/T0 model, frequency -> C     [PORT: near-verbatim]
    fusion.c/.h           quality, median gate, weighted average [PORT]
    autorange.c/.h        PROBE -> SWEEP -> FUSION pipeline [PORT + rework]
    rmode.c/.h            R-mode: Kelvin, TIA, autoranging [NEW]
    rmode_math.c          pure Kelvin/TIA arithmetic (host-tested)
  calib/calib.c/.h        osc + ADC + R-mode calibration, flash record
  proto/
    events.c/.h           @@EVT emitters                 [PORT: keep byte-exact]
    console_cmds.c/.h     command table                  [PORT]
```

**Rule:** `measure/`, `calib/` and the maths in `proto/` contain **no HAL
calls**. They take plain numbers and return plain numbers. This makes the
fusion/fit/solver code compilable on the host for unit tests — a real gain the
ESP32 code never had.

### 3.2 C-mode acquisition — the rework

**ADC RC-step (replaces the software poll loop):**

1. `TIM6` at a fixed period triggers `ADC1` (single channel, 12-bit,
   oversampling 64×, mean-preserving shift).
2. ADC → circular DMA into a `uint16_t` ring; `V(t)` is assembled from the
   ring with **exact** sample instants `t = n · T_tim6`.
3. The fit consumes the ring directly — no timestamps need to be recorded,
   because the period is exact. The old code had to *measure* each point's
   time; now it is known analytically.

This removes `ts[]`, the adaptive decimation, `RC_FIT_GLITCH_MV` filtering
(jitter is gone), and the `esp_timer_get_time()` calls.

**Caveat to design for — this is the most important numeric constraint.**
Oversampling decimates the output rate by the ratio. At a fixed 64× with a
47.5-cycle sample time (1.41 µs per conversion), the output is **11.07 kHz**;
at 256× it is **2.77 kHz**. That is fine for slow ranges and **catastrophic for
fast ones**:

| Case | τ | Points available within 3τ @64× | @256× |
| --- | --- | --- | --- |
| 100 Ω + 1 nF | 0.10 µs | **0.0** | **0.0** |
| 100 Ω + 100 nF | 9.9 µs | **0.3** | **0.08** |
| 1 MΩ + 1 nF | 1.01 ms | 33.5 | 8.4 |
| 100 Ω + 100 µF | 9.9 ms | 328 | 82 |

A 100 Ω range with a 1 nF DUT has **no usable sample points at all** under
fixed 256× oversampling, and only a third of a point at 64×. The fit needs at
least ~8 points spanning the curve.

**Therefore the oversampling ratio must be per-range**, chosen so that
`N_oversample · t_conv ≪ τ` for that range's useful DUT band. Small ratios (or
none) where τ is short; large ratios where τ is long. Add
`adc_os_ratio[range]` to the calibration set, and sanity-check the choice with
`n_points = 3τ / (ratio · t_conv) ≥ 8`.

**Oscillator mode (replaces the ISR + PCNT):**

1. `DAC3_CH1` → COMP1 `INM` (threshold; no external divider).
2. `V_cap` → COMP1 `INP` (keep the BAT54S clamp).
3. `COMP1_OUT` → `TIM2_CH1` input capture, internal, no pin.
4. Average N captured periods; `f = 170 MHz / mean_period_ticks`.

Then `C = (T − T0)/(K·R)` exactly as today. **`K` and `T0` remain and their
meaning is unchanged** — they no longer absorb CPU latency, so `T0` should come
out smaller and more repeatable, and the existing calibration commands keep
working.

**Drive pin:** on the ESP32 the ISR toggled it. Now the comparator output can
drive the timing directly; the drive pin becomes a simple GPIO square-wave
source the timer/HRTIM can generate, so the loop is closed in hardware.

### 3.3 R-mode (new capability, reworked from `docs/`)

The old notes assumed a 100-pin part. On the 48-pin part the plan is **more**
viable than I first thought, because OPAMP1/2/3 and DAC3 are available:

* Force into **`N_DUT`**, not `V_cap`: `C_block` sits between them and is an
  open circuit at DC. The force element is Q1 (AO3401 P-FET), driven by
  `OPAMP1` with **`VINP = B` (PA7)** and **`VINM = VREF` (PA3)**. The old
  `VINP = DAC` arrangement was positive feedback.
* Sense `V_N` at `N_DUT` (1k/1M -> OPAMP3 -> ADC3_IN1) and `V_B` at `B`
  (LMC6482 B -> ADC4_IN3). The Kelvin ratio is
  `R = RSENSE_eff * (V_N - V_B) / V_B`.
* `B` is grounded in C mode by `K1A`; `K1B` isolates `C_block` in R mode. The
  K1/K2 G6S-2 relays implement the three B-node states.
* Above ~22 Mohm the external **LMC6482 A** TIA takes over:
  `R = Rf * (V_N - VBIAS) / (VBIAS - V_TIA)`.
* `DAC3_CH1` belongs to COMP1 only; `DAC1_CH1` (internal) is the COMP4
  overvoltage threshold. Neither costs a pin.

**Verify on the bench:** the LMC6482 input bias current at the 1 Gohm end, and
the OPAMP1 loop stability with the Q1 gate network (R13/R14/C3).

### 3.4 Calibration storage

Replace NVS with a **reserved 2 KB page in flash bank 2**, append-only records
of `{magic, version, crc32, payload}`, newest valid record wins at boot. Roughly
256 writes per erase cycle, atomicity from the CRC. Use TAMP backup registers
for a "calibration valid" flag.

**Migration:** the ESP32 `osc_cal`/`adc_cal`/`board_cal` *values* describe the
board, not the MCU, so they remain meaningful — but the node parasitics and
latency components of `T0` will differ on the new hardware. Plan on
**re-running `zero`/`cal1`/`cal2`**, and expect a smaller `T0`.

### 3.5 Status LED

WS2812 moved from ESP32 RMT to a **software-PWM driver on the WeAct board's
user LED: PC6, active high** (PC13 is the user button, not an LED). The
animation state machine (`LED_MODE_*`) is hardware-independent and ports as-is.
The ESP32 RMT pitfalls in `HANDOVER.md` (ping-pong symbols, non-blocking frames,
static buffer, `wait_all_done`) disappear; the behaviour they produced (never
block the measurement path) is preserved by `led_tick()`.

---

## 4. Document rework list

Every document that assumes the ESP32 or the 100-pin part must change. **This is
part of the deliverable, not an afterthought.**

| Document | Action |
| --- | --- |
| `README.md` (this repo) | **Done** — peripheral tables, COMP replacement, caveats |
| `docs/kelvin-ohmmeter-g474.md` | **Rework §9 pin map** (48-pin reality: DAC3 for COMP refs, OPAMP1/2/3, no PD/PE). §2's "5 ADC, 6 OPAMP" table needs a 48-pin column. §10's "port from ESP-IDF" section is **superseded by this plan**. §12 bring-up checklist needs hardware-capture steps |
| `docs/resistance-autoranging-plan.md` | **Rework §1** (its confirmed-facts table cites a 100-pin pin map), **§2 D4/D5** (internal OPAMP is viable, MAX4619 spare is external and unaffected), **§8** (its `fe_state_t` extension and DMA/OPAMP HAL sketch now supersede to §3.2/§3.3 of this plan) |
| **New** `docs/port-plan.md` | **This file** |
| **New** `docs/hardware-map.md` | The definitive 48-pin pin allocation, produced after §5 decisions are settled |
| **New** `docs/peripherals-g474.md` | The peripheral research, condensed and corrected (ADC/COMP/DAC/OPAMP/TIM facts + errata), so the findings are not lost in chat |
| `HANDOVER.md` (ESP32 repo) | Leave as the ESP32 record, but add a header pointing here. Its §4 latency/latency-limiter/decimation material becomes historical — much of it describes problems the G474 does not have |
| `CODE_EXPLAINED.md` (ESP32 repo) | Same treatment: it documents ESP-IDF specifics that do not port |
| `ESWCap_*.md` (ESP32 repo) | Presentation material; update the hardware claims when the port lands |
| `host/README.md` | Add `res` to the event table when R-mode telemetry lands |

---

## 5. Decisions taken

These were open when the plan was written. The values below are what the
implementation assumes; each is isolated so it can be changed without a rewrite.

| # | Decision | Taken | Where to change it |
| --- | --- | --- | --- |
| 1 | Front-end PCB | Existing analog front end carried over, control lines remapped to the G474 | `include/config.h` pin map |
| 2 | LM393 | **Removed.** COMP1 + DAC3 replaces it | `src/bsp/bsp_comp.c` |
| 3 | VREF+ | **VDDA**, with the dedicated pin available for a precision reference later. The maths is ratiometric, so absolute accuracy is not critical; noise is, and the ADC/ DAC share the same reference so the ratio stays exact | `bsp_dac3_vref_mv()`, `adc_vdda_mv()` |
| 4 | R-mode scope | **Implemented** (Kelvin primary, TIA for >22 MΩ, capped at 0.5 quality until bench-verified) | `src/measure/rmode.c` |
| 5 | R-mode amplifier | **Internal OPAMP1**, `VINP = B` (PA7/IO2), `VINM = VREF` (PA3/IO0). Negative feedback with the high-side P-FET. OPAMP2 cannot be used (wrong polarity, no DAC path) | `src/bsp/bsp_opamp.c` |
| 6 | RTOS | **Bare-metal superloop** | `src/app.c` |
| 7 | Switch polarity | **SSR and relay drive active-high**, MAX4619 `EN` active-low. Each is isolated behind one helper | `FE_SSR_ACTIVE_HIGH`, `FE_RELAY_ACTIVE_HIGH`, `FE_MUX_EN_ACTIVE_LOW` in `bsp_frontend.c` |
| 8 | ADC oversampling | **Mean-preserving** (`shift = log2(ratio)`), so codes stay on the 0..4095 scale and ratios are exact | `bsp_adc.c` |
| 9 | Oscillator loop | **Fully hardware**: PA8 is COMP1_OUT (AF8) and the drive-buffer input, so the comparator closes the loop with no CPU. TIM2 captures the period via `TISEL.TI1SEL = 1`. PA8 is switched back to GPIO for the RC step | `bsp_comp.c` |

**Still requires hardware:** items 1, 2, 7 and 9 cannot be proven without the
board. They are written so a wrong assumption is a local edit, not a redesign.

---

## 6. Build sequence

All eight phases are implemented; only the hardware-dependent verification in
Phase 6 remains. The sequence is kept because it is the order the work was done
in and the order a re-port should follow.

Ordered so that each step is independently verifiable, and so the risky
hardware assumptions are tested before the maths is ported onto them.

**Phase 0 — skeleton (done).** Build, DFU upload, UART console, LED.

**Phase 1 — clock and platform.** Real 170 MHz PLL from HSE (PF0/PF1),
48 MHz USB if needed, IWDG, flash record store. *Verify:* the board boots and
the console reports the expected SYSCLK.

**Phase 2 — ADC path.** TIM6 trigger + ADC1 + circular DMA + oversampling.
*Verify:* a known DC voltage reads correctly; a scope/logic-analyser confirms
the sample period is exactly `T_tim6` and free of the errata §2.7.3 gap.

**Phase 3 — COMP path.** COMP1 + DAC3 threshold + TIM2 input capture. *Verify:*
inject a known square wave, confirm `CCR1` matches expectation to within a
timer tick, and that no ISR is needed.

**Phase 4 — port the maths.** `rc_fit`, `osc_calc`, `fusion`, `calib` moved
across **with host unit tests** against the existing recorded curves
(`host/fit_analysis.py` can replay them). *Verify:* identical results on
identical inputs.

**Phase 5 — front-end and protocol.** SSR/mux state machine, interlocks,
`@@EVT` emitters, console commands. *Verify:* the existing dashboard connects
and renders without host-side changes.

**Phase 6 — calibrate.** `zero`/`cal1`/`cal2`/`adccal*`, confirm the reduced
`T0`, update the docs with the new measured constants.

**Phase 7 — R-mode.** Only after 1–6 are solid.

**Phase 8 — remove the ESP32-specific apparatus.** Delete the decimation,
ISR-limiter, `osc_delay` and PCNT code paths, and update the docs that describe
them.

---

## 7. Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| **R-mode topology** | **Resolved in firmware; verify the built board** | The DUT is `N_DUT -> B`, with `C_block` between `V_cap` and `N_DUT`. This is confirmed by the ESP32 `docs/combined-circuit.md` netlist and the ESP32 front-end code. The firmware now forces `N_DUT` and senses `V_N`/`V_B`; see `docs/handoff-verdict.md`. The remaining risk is the as-built board matching the plan |
| Errata §2.7.3 silently corrupting slow readings | **High** | Circular DMA keeps a continuous stream; explicitly test the >1 ms gap case |
| Oversampling too slow for fast ranges | **High** | Per-range ratio; the 100 Ω + small-cap case has **zero** usable points at fixed 256× — see §3.2. Validate with `3τ/(ratio·t_conv) ≥ 8` |
| High-Z node + ADC sample-cap kickback | Medium | Long sample time and/or OPAMP1 follower buffer |
| TIA input bias current at 1 GΩ (R-mode) | Medium | The TIA is the external LMC6482 A (20 fA class), not the internal op-amp. Bench-verify the 1 GΩ end before trusting it |
| Switching thresholds lose the tared `T0` benefits | Medium | Re-run `zero`/`cal1`/`cal2`; expect smaller `T0`, treat as new baseline |
| Host protocol drift | Medium | Preserve `@@EVT` byte-for-byte; `res` is additive only |
| Behavioural regressions in the safety interlock | **High** | Port §1.6 verbatim, including the C-mode-only SSR2 rule |

---

## 8. Open questions

* Does the ADC's 4 Msps with 12-bit hold at 170 MHz SYSCLK, or does the ÷4
  async clock (42.5 MHz) cap it near 2.8 Msps? (Affects only the fastest
  ranges; the report says 42.5 MHz is the practical single-ADC rate.)
* How much of `T0` was genuinely latency versus real parasitic capacitance?
  Answering this quantifies how much the port improves the sub-100 pF end.
* Is the MAX4619 spare half actually available for the R-mode sense mux, or
  consumed elsewhere?
* Does the DUT bias path (`R_bias`, `C_block`) need re-rating for the new
  board, or is the analog front-end carried over unchanged?
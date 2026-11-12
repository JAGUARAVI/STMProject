# ESWCap — STM32G474

Port of the **ESWCap** autoranging capacitance meter (originally ESP32-S3 / ESP-IDF)
to an **STM32G474CEU6**, targeting the **WeAct Studio "Long" core board**.

> **Status: firmware implemented, not yet run on silicon.** C-mode (RC-step +
> comparator oscillator), autoranging, fusion, calibration, the console and the
> `@@EVT` protocol are complete. R-mode (Kelvin/TIA) is implemented for the
> combined board and is compile-time gated behind `ESWCAP_HAS_RMODE`; the
> default build refuses `mode r` until the Q1/LMC6482/K1/K2/OPAMP3 hardware is
> fitted. See `docs/handoff-verdict.md` for the topology review and
> `docs/port-plan.md` §6 for bring-up.

---

## Target hardware

| Property | Value |
| --- | --- |
| Board | WeAct Studio STM32G474CEU6 "Long" core board |
| MCU | STM32G474CEU6 — Cortex-M4F, 170 MHz |
| Package | UFQFPN48 (48 pins) |
| Flash | 512 KB |
| SRAM | 128 KB |
| HSE crystal | 8 MHz, on **PF0/PF1** (not PA) |
| Regulator | ME6231A33M3G (5 V pin accepts ~3.5–18 V) |
| User LED | **PC6**, active high (`PC6 -> 5.1k -> LED -> GND`) |
| User button | **PC13**, active low to GND |

Board repo (schematics, datasheets, examples):
<https://github.com/WeActStudio/WeActStudio.STM32G474CoreBoard>

### Carrier PCB (Rev B)

The combined C+R front end is built as a 111 × 85 mm, 2-layer carrier under the
WeAct Long module.  The module plugs into two rows of 2.54 mm sockets; the
analog front end, relays, SSRs, DUT/bias terminals and console header are on the
carrier.

| Item | Value |
| --- | --- |
| Schematic | `~/Documents/KiCad/Project/Project.kicad_sch` (KiCad 10) |
| PCB | `~/Documents/KiCad/Project/Project.kicad_pcb` |
| Fab outputs | `~/Documents/KiCad/Project/fab/` (Gerbers, drill, BOM, PnP) |
| Design record | [`docs/pcb-design.md`](docs/pcb-design.md) |
| DRC | 0 errors, 0 unconnected, 25 silkscreen-only warnings |
| Module pin order | P1/P2 pin 1 at the **right** end; P3/P4 four leftmost pins |
| Rev B fixes | D6 body-diode block, C1 35 V bipolar, R9 47 Ω, C3 to VREF, console J7, per-IC decoupling, fail-safe pulls |

The module's +5 V comes from USB VBUS (P3-4); the relay coils need USB power.

### Peripheral availability on the 48-pin package

Verified against the STM32G474CE pin database (UFQFPN48). The analog peripherals
are **better** here than the 48-pin pin count suggests — the earlier assumption
that the internal analog is unusable was wrong.

**Comparators — all 7 are bonded out.** Each `INM` can be driven internally by a
DAC, so the comparator reference needs no external divider.

| | `INP` | `INM` options | Internal DAC on `INM` | Output pins (AF8) |
| --- | --- | --- | --- | --- |
| COMP1 | PA1, PB1 | PA0, PA4 | **DAC1_CH1, DAC3_CH1** | PA0, PA6, PA8, PA11 |
| COMP2 | PA3, PA7 | PA2, PA5 | **DAC1_CH2, DAC3_CH2** | PA2, PA7, PA9, PA12 |
| COMP3 | PA0 | PF1 *(HSE!)* | DAC1_CH1, DAC3_CH1 | PB7, PB15 (AF3) |
| COMP4 | PB0 | PB2 | **DAC1_CH1, DAC3_CH2** | PB1, PB6, PB14 |
| COMP5 | PB13 | PB10 | DAC1_CH2, DAC4_CH1 | PA9 |
| COMP6 | PB11 | PB15 | DAC2_CH1, DAC4_CH2 | PA10, PC6 (AF7) |
| COMP7 | PB14 | PB12 | DAC2_CH1, DAC4_CH1 | PA8 |

**OPAMPs — 1, 2 and 3 are bonded out** (USB is on PA11/PA12, not PA2/PA3).
`InternalOutput` routes `VOUT` straight to an ADC channel with **no pin**:

| | `VINP` | Internal `VINP` | `VINM` | `VOUT` pin | `VOUT`→ADC (internal) |
| --- | --- | --- | --- | --- | --- |
| OPAMP1 | PA1, PA3, PA7 | **DAC3_CH1** | PA3 | PA2 | ADC1_IN13 |
| OPAMP2 | PA7, PB14, PB0 | — | PA5 | PA6 | ADC2_IN16 |
| OPAMP3 | PB0, PB13, PA1 | **DAC3_CH2** | PB2, PB10 | PB1 | ADC2_IN18, ADC3_IN13 |

OPAMP4/5/6 need PD/PE pins that do not exist on the 48-pin package.

**ADCs** — 5 instances, channels available:

* `ADC1`: PA0(IN1), PA1(IN2), PA2(IN3), PA3(IN4), PB0(IN15), PB1(IN12),
  PB11(IN14), PB12(IN11), PB14(IN5), PF0(IN10)
* `ADC2`: PA0(IN1), PA1(IN2), PA4(IN17), PA5(IN13), PA6(IN3), PA7(IN4),
  PB11(IN14), PB15(IN15), PB2(IN12), PC4(IN5), PF1(IN10)
* `ADC3`: PB0(IN12), PB1(IN1), PB13(IN5)
* `ADC4`: PB12(IN3), PB14(IN4), PB15(IN5)
* `ADC5`: PA8(IN1), PA9(IN2)

**DAC — 4 instances, but only DAC1/DAC2 have pins:**

| | Pin output | Internal-only use | Max rate |
| --- | --- | --- | --- |
| DAC1 | `OUT1 = PA4`, `OUT2 = PA5` | COMP1/3/4, COMP2/5 | 1 MSPS |
| DAC2 | `OUT1 = PA6` | COMP6/7 only | 1 MSPS |
| **DAC3** | **none** | **COMP1/3, COMP4, OPAMP1, OPAMP3, OPAMP6** | 15 MSPS |
| **DAC4** | **none** | COMP5/7, COMP6, OPAMP4, OPAMP5 | 15 MSPS |

**DAC3/DAC4 are internal-only by design** — they exist purely to feed comparators
and op-amps with no pin at all. That is a significant asset for this project:
**two extra 12-bit threshold sources at zero pin cost.** Prefer them for the
comparator reference so PA4/PA5 stay free.

**Reference:** pin 20 is a dedicated **`VREF+`** — a clean external reference can
be fitted instead of relying on VDDA. `VREFBUF` (2.048 / 2.5 / 2.9 V) is also
available but draw only ~6.5 mA and needs 1 µF + 100 nF at the pin.

#### Comparator → timer routing (the key capability)

A comparator output can reach a timer's input capture **internally, with no pin**.
This is what lets the ESP32's software ISR be replaced by hardware period
measurement. Routing flexibility differs sharply by timer:

| Timer | Width | COMPs accepted on `TI1` | Notes |
| --- | --- | --- | --- |
| **TIM2** | **32-bit** | **COMP1…COMP7** | Best choice: no overflow bookkeeping to 25 s |
| TIM3 | 16-bit | COMP1…COMP7 | Also very flexible |
| TIM4 | 16-bit | COMP1…COMP7 | |
| TIM1/TIM8/TIM20 | 16-bit | COMP1…COMP4 only | `TI2/3/4` accept no comparator |

**Recommendation: `COMP1_OUT` → `TIM2_CH1` input capture.** TIM2 is 32-bit, accepts
any comparator on `TI1`, and at 170 MHz gives 5.88 ns resolution with a 25.3 s
wrap — covering the whole 10 Hz…1 MHz oscillator band in one timer, unpowered by
CPU jitter.

#### Real constraints to plan around

* **`INM` and the DAC output share pins.** `PA4` is both `DAC1_OUT1` and
  COMP1's `INM`; `PA5` is both `DAC1_OUT2` and COMP2's `INM`. Using the internal
  DAC costs those pins. **Use DAC3 instead** to avoid the conflict entirely.
* **Comparator output must not land on its own input pin.** COMP1's output
  shares PA0 with its `INM`; COMP2's shares PA2 with its `INM`. Pick a different
  output (COMP1 → PA6/PA8, COMP2 → PA7/PA9) — or better, route it internally to
  the timer and use **no output pin at all**.
* **OPAMP1 `VOUT` (PA2) overlaps COMP2's `INM`**, and OPAMP2 `VOUT` (PA6)
  overlaps COMP1's output. Mode-dependent, but allocate deliberately.
* **`OPAMP_NONINVERTINGINPUT_DAC` and `..._IO3` are the same bit encoding**
  (`OPAMP_CSR_VPSEL`). Which one is active is per-instance. Set it deliberately.
* **No OPAMP→OPAMP and no OPAMP→COMP internal chaining** on G4. Internal chains
  are only `DAC→COMP`, `DAC→OPAMP`, `OPAMP→ADC`.
* **No `DAC→ADC` direct channel.** To read a DAC internally, route it through an
  op-amp: `DAC3_CHx → OPAMP(follower) → ADC` (all internal).
* **No comparator window mode on G4** (contrary to many blog posts) and **no
  clocked comparator output filter** — only programmable hysteresis and
  timer-based blanking.
* **COMP3's only `INM` is `PF1`, which is the HSE crystal pin.** COMP3 is
  effectively unusable while the external crystal is fitted.
* **Reserved:** PA13/PA14 (SWD), PC6 (user LED), PC13 (user button),
  PF0/PF1 (HSE crystal), PA11/PA12 (USB D−/D+), PB8 (BOOT0).

### Replacing the LM393 with COMP1 — strongly recommended

The external LM393 oscillator can be replaced by **COMP1** entirely. The
comparator's `INM` can be driven by `DAC1_CH1` internally, so the external
reference divider (10 kΩ/10 kΩ), the 100 kΩ hysteresis resistor and the 10 kΩ
pull-up all disappear with the chip.

Suggested mapping (avoiding the collisions above):

| Function | Pin | Notes |
| --- | --- | --- |
| COMP1 `INP` | **PB1** | `V_cap` sense node (BAT54S clamped) |
| COMP1 `INM` | internal | `DAC1_CH1` — replaces the 1.65 V divider |
| `DAC1_OUT1` | PA4 | leave unrouted, or reserve |
| COMP1 `OUT` | **PA6** or **PA8** | timer input-capture input |

Why this is an upgrade, not just a part-count reduction:

* **Programmable hysteresis.** The LM393's hysteresis is fixed by the 100 kΩ
  feedback resistor. COMP1 sets hysteresis on a 0–1023 scale in software, so it
  can be tuned **per range** instead of one compromise value.
* **Timer input-capture replaces the ISR.** Route `COMP1_OUT` to a timer's
  capture input and the period is measured in hardware. This directly addresses
  the *"ISR Latency Profiling"* pending item in the ESP32 `HANDOVER.md` — the
  ESP32's software oscillator loop put CPU jitter straight into the timing.
* **The rate limiter becomes unnecessary.** `OSC_MIN_EDGE_INTERVAL_US = 3`
  exists because the ESP32 ISR had to mirror the comparator output back to the
  drive pin, so a too-fast loop could freeze the device. With COMP1 driving the
  timing directly, a too-fast loop self-limits rather than hanging the CPU.
* **Far faster.** G4 comparator propagation is ns-class versus the LM393's
  ~1.3 µs, which is what actually limited the sub-100 pF ranges.

This does not change the *physics* of `T = K·R·C + T0`, so the existing `K`/`T0`
calibration model carries over unchanged.

---

## Toolchain

| Component | Version / location |
| --- | --- |
| PlatformIO Core | 6.2.0 — `~/.platformio/penv/bin/pio` |
| STM32 platform | `ststm32` 20.0.0 |
| Framework | **STM32Cube HAL/LL** (`framework = stm32cube`) |
| Toolchain | `arm-none-eabi-gcc` 7.2.1 (installed by PlatformIO) |
| Upload | **USB DFU** bootloader via `dfu-util` |
| Debug | Optional — ST-Link / SWD (see below) |

PlatformIO Core is installed under `~/.platformio/penv` and is **not on the
default PATH**. The project's `.vscode/settings.json` prepends it for integrated
terminals. For your own shell:

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
```

---

## Build, upload, monitor

```bash
pio run                      # build (default env: weact_g474ceu6)
pio run -t upload            # flash over USB DFU
pio device monitor -b 115200 # serial console, USART1 on PA9/PA10
```

> **Console wiring:** the WeAct Long board has **native USB only** (PA11/PA12);
> there is no on-board USB-UART bridge. The Type-C port is for DFU/programming.
> For the console, connect an external 3.3 V USB-UART adapter to the P1 header:
> adapter TX -> **PA10**, adapter RX -> **PA9**, GND -> GND. Do not connect the
> adapter's 3.3 V if the board is already powered.

### Flashing over USB DFU

The board ships with ST's ROM DFU bootloader. No probe is required.

1. Hold **BOOT0**, then tap **NRST**, then release **BOOT0**.
2. The board enumerates as ST's ROM bootloader — USB ID `0483:DF11`.
3. Run `pio run -t upload`. PlatformIO adds the DFU suffix to `firmware.bin`
   automatically and flashes it with `dfu-util`.
4. Reset (or power-cycle) to run the firmware.

Check the device is visible:

```bash
lsusb | grep -i 0483        # expect: 0483:df11 STMicroelectronics STM32 BOOTLOADER
dfu-util -l                 # lists DFU interfaces
```

If `dfu-util` reports no device, the board is not in DFU mode, or the udev rules
do not grant access (see **udev** below).

### Debugging over SWD (optional)

DFU cannot debug. To single-step, watch variables or use breakpoints, attach a
probe to the SWD header and use the debug environment:

```bash
pio run  -e weact_g474ceu6_debug -t upload   # upload over SWD
pio debug -e weact_g474ceu6_debug            # start a debug session
```

This env is built with `-DDEBUG -DUSE_FULL_ASSERT` and flashes via `stlink`.

---

## Environments

| Environment | Framework | Upload | Purpose |
| --- | --- | --- | --- |
| `weact_g474ceu6` | stm32cube | **dfu** | Default. C-mode-only build; flash over USB. |
| `weact_g474ceu6_debug` | stm32cube | stlink | `-DDEBUG -DUSE_FULL_ASSERT`, for GDB/Live Watch. |
| `weact_g474ceu6_stlink` | ststm32 | stlink | Release build, flashed via an SWD probe. |
| `weact_g474ceu6_combined` | stm32cube | dfu | `-DESWCAP_HAS_RMODE=1`; requires the Q1/LMC6482/K1/K2/OPAMP3 front end. |

The combined board is a **compile-time** choice on purpose: the default build
must not energise relay coils or drive a Q1 gate that is not fitted. `mode r`
refuses with a clear message until `ESWCAP_HAS_RMODE=1`.

---

## IDE setup

The project is configured for **VS Code** (and compatible forks).

1. Install the recommended extensions — VS Code prompts on open, or run
   *Extensions: Show Recommended Extensions*:
   * `platformio.platformio-ide` (required)
   * `ms-vscode.cpptools` (required by PlatformIO IDE for IntelliSense)
2. Open this folder. PlatformIO indexes the project and writes
   `.vscode/c_cpp_properties.json`, which supplies all Cube HAL include paths
   and the `STM32G474xx` define automatically.
3. Build once (`pio run`) to populate IntelliSense and the compile database.

`.vscode/settings.json` is committed and already handles:

* `pio` on the integrated-terminal PATH,
* C standard / IntelliSense mode (`gcc-arm`),
* serial monitor defaults (115200, DTR/RTS de-asserted),
* excluding `.pio/` from search and the explorer.

`.vscode/c_cpp_properties.json` is gitignored — it is regenerated by PlatformIO
after each build and contains absolute paths. `.vscode/launch.json` **is**
committed, because DFU cannot debug and so PlatformIO never generates it for
this setup.

### udev rules (Linux, for DFU and probes)

PlatformIO's own rules file is already installed at
`/etc/udev/rules.d/99-platformio-udev.rules`, which covers ST-Link and the STM32
DFU bootloader. If `dfu-util` cannot open the device, reinstall them:

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Then unplug/replug the board. Note this system has no `plugdev` group; if access
still fails, add a rule scoped to your user or group.

---

## Firmware architecture

The ESP32 build was 4 000 lines in one file. This port splits it so the maths is
testable on a host and the hardware access is confined to one layer.

```
src/
  main.c              entry point (nothing else)
  app.c               cooperative scheduler: the only place that decides when to measure
  bsp/                hardware layer — the only code that touches HAL/registers
    bsp.c             170 MHz PLL from HSE, SysTick, DWT, IWDG, USB clock
    bsp_console.c     USART1 interrupt-driven RX, line assembly, printf
    bsp_adc.c         ADC1 stream (TIM6+DMA+oversampling), ADC2/3/4 single reads, VREFINT
    bsp_comp.c        COMP1 + DAC3 + TIM2 TISEL capture; COMP4 overvoltage guard
    bsp_frontend.c    SSR/relay/mux control, safety state machine, mode interlock
    bsp_dac.c         DAC3_CH1 (COMP1) and DAC1_CH1 (COMP4), both internal-only
    bsp_opamp.c       OPAMP1: R-mode Kelvin error amplifier (VINP=B, VINM=VREF)
    bsp_opamp3.c      OPAMP3: V_N unity-gain buffer
    bsp_store.c       flash record store (NVS replacement)
    bsp_led.c         PC6 software-PWM status LED
  measure/            pure arithmetic — no HAL, host-testable
    rc_fit.c          voltage-domain Gauss-Newton exponential fit
    osc_calc.c        T = K*R*C + T0 model and its solvers
    board.c           board constants and the C_block series transform
    adc_measure.c     curve -> scored sample (fit, gates, quality)
    osc_measure.c     period -> scored sample
    fusion.c          median gate and log-domain weighted fusion
    autorange.c       PROBE -> SWEEP -> FUSION pipeline
    rmode.c           4-wire Kelvin / TIA resistance (BSP-facing)
    rmode_math.c      pure Kelvin/TIA arithmetic (host-tested)
  calib/calib.c       calibration state, solvers, flash record
  proto/
    events.c          @@EVT JSON telemetry (contract preserved)
    console_cmds.c    the command set
```

What the hardware changed, relative to the ESP32:

| ESP32 mechanism | G474 mechanism |
| --- | --- |
| Software ADC poll loop + adaptive decimation | TIM6-triggered ADC1 + circular DMA, hardware oversampling |
| LM393 + 10k/10k divider + 100k hysteresis | COMP1 + DAC3 internal threshold (20 mV hysteresis) |
| ISR mirrors comparator, PCNT measures it | **No CPU in the loop**: COMP1_OUT/PA8 drives the buffer; TIM2 captures via TISEL |
| NVS blob store | reserved 2 KB flash page, append-only CRC records |
| WS2812 over RMT | PC6 software PWM, same operational language |

## Testing

The measurement maths compiles and runs on the build machine:

```bash
make -C test/host        # builds and runs the unit tests
```

21 tests cover the exponential fit (including its insensitivity to a biased
asymptote), the C_block transform, the oscillator model and two-point solve, ADC
and oscillator sample scoring, fusion gating and log-domain averaging, both
calibration solvers, the Kelvin/TIA resistance equations, and the R-mode
calibration setters. They run against
the **same source files** the firmware uses; only the flash store is stubbed.

---

## Repository layout

```
platformio.ini          Build/upload/debug configuration (3 environments)
src/main.c              Entry point
src/app.c               Scheduler
src/bsp/                Hardware layer
src/measure/            Measurement maths (HAL-free)
src/calib/              Calibration state and solvers
src/proto/              Telemetry and console
include/                Public headers, pin map (config.h), stm32_assert.h
test/host/              Host unit tests (make -C test/host)
docs/                   Port plan, peripheral reference, design notes
.vscode/                Editor configuration
```

---

## Documentation

| Document | Purpose |
| --- | --- |
| **`docs/port-plan.md`** | **The port plan.** Audit of the ESP32 firmware, the G474 architecture it maps to, the document rework list, and the phased build sequence. Start here. |
| **`docs/peripherals-g474.md`** | Verified peripheral facts — ADC/COMP/DAC/OPAMP/timer/storage, plus the ES0430 errata that constrain the code. |
| **`docs/handoff-verdict.md`** | **Review of the other agent's handoff**: what it gets right, what is unsafe to apply literally, what the previous firmware got wrong, and the corrected pin map. Read with the R-mode work. |
| `docs/kelvin-ohmmeter-g474.md` | R-mode electrical design. Superseded banner + corrected topology, pin map and equations. |
| `docs/resistance-autoranging-plan.md` | R-mode autoranging + Kelvin/TIA topology. The topology is adopted; the pin table is corrected by the verdict. |

The two R-mode notes were written for a **100-pin G474VE**. They remain useful for
the *electrical* design (sensing topology, ratiometric maths, error budget,
interlock rules); their pin maps and internal-peripheral assumptions have been
corrected in place against the verified 48-pin data.

---

## Next steps

The firmware is complete; what remains needs the physical board.

1. **Verify the pin map against the actual front-end wiring.** `include/config.h`
   is the single source of truth. The switch polarities (SSR/relay active-high,
   MAX4619 EN active-low) are isolated behind three constants in
   `bsp_frontend.c`. The R-mode pins are only meaningful once the combined board
   is fitted.
2. **Flash and confirm the platform comes up.** Expect the banner, `SYSCLK
   170000000 Hz`, and a plausible `VDDA` reading. A blink-code loop at boot
   identifies which `require()` failed (1 = clocks … 8 = R-mode analog).
3. **Verify the ADC stream.** A known DC level on `V_cap` should read correctly,
   and the sample period should match the requested period (scope PA8 for the
   comparator, or trigger on a GPIO toggle).
4. **Verify the comparator.** Inject a square wave on the DUT node and confirm
   the console's `status`/`probe` see oscillation and that `zero` produces a
   stable `T0`. This is the step that proves `TISEL` routing works on silicon.
5. **Calibrate.** `mode c`, then `zero`, `cal1`/`cal2` with 1 % C0G/NP0
   references, `adccal1`/`adccal2` with film references. Record the new `T0`;
   it should be smaller than the ESP32 value because no ISR latency remains.
6. **R-mode bring-up.** Build `weact_g474ceu6_combined` (it sets
   `ESWCAP_HAS_RMODE=1`), flash, `discharge`, `mode r`, then `res`. Check
   V_B ~= 405 mV on a 1 kohm DUT before trusting the top element or the TIA.
   On a C-mode-only board the default build refuses `mode r` instead of
   energising relays that do not exist.

See `docs/port-plan.md` §6 for the phase-by-phase acceptance criteria and §7
for the risk list.
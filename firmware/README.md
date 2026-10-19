# BX Tacho firmware

ESP-IDF **v6.1** firmware for the BX Tacho v0.2 board (ESP32-C3-WROOM-02).
Spec: `HANDOVER.md` section 6.

## Build and flash

In VS Code with the ESP-IDF extension, open **this `firmware/` folder** (not the
repo root), select the serial port of the board's USB Serial/JTAG, then run
**ESP-IDF: Build, Flash and Monitor**.

From an ESP-IDF terminal:

```
idf.py build
idf.py -p COMx flash monitor
```

If automatic download mode fails, hold BOOT (SW3), tap RESET (SW2), release BOOT.

Tunables are under **BX Tacho** in `idf.py menuconfig` (`main/Kconfig.projbuild`).

## Layout

| File | Purpose |
|---|---|
| `main/board.h` | Pin map (HANDOVER.md 3.9) |
| `main/rpm.c` | RPM channel power sequencing, threshold PWM, edge-timestamp ISR |
| `main/load.c` | Load sensor: ambient-cancelled ADC reads, calibrated hysteresis classifier |
| `main/launch.c` | EMPTY → LOADED → PULLING → RESULT state machine, peak RPM |
| `main/battery.c` | VBAT, USB presence, charge state |
| `main/display.c` | SSD1315 OLED via `esp_lcd`, framebuffer and 5×7 font |
| `main/settings.c` | Calibration stored in NVS |
| `main/cli.c` | Bring-up and calibration console |
| `main/main.c` | Tasks, light sleep, display dimming, deep sleep |

## Console (USB Serial/JTAG)

| Command | What it does |
|---|---|
| `status` | Battery, sensors, calibration, last launch |
| `diag on` / `diag off` | Pause the launch state machine so you can drive the sensors by hand |
| `meter on` / `meter off` | Continuous RPM mode for bench tests: RPM channel always on, display shows live RPM (1 s average, min/max, rejected edges) |
| `meter` / `meter log [s] [ms]` | Print meter readings as CSV, once or every `ms` for `s` seconds |
| `rpm on` / `rpm off` | Power the RPM channel (diag mode) |
| `rpm sense [n]` | Print RPM_SENSE in mV (channel must be on) |
| `rpm scan [s] [apply]` | Record RPM_SENSE min/max while you turn the launcher; `apply` saves the midpoint as the threshold |
| `rpm thr [mV]` | Show or set the comparator threshold (capped at 2000 mV) |
| `load [n]` | Print ambient-cancelled LOAD_SENSE readings |
| `cal load empty` / `cal load loaded` / `cal load clear` | Calibrate the load sensor |
| `cfg edges <n>` | Rising edges per launcher revolution |
| `cfg lightsleep on\|off` | Automatic light sleep on battery (default on), for power debugging |
| `events` / `events clear` | Power/sleep event log: boots and reset reasons, load changes, state changes, USB, display on/off, deep sleep. Kept in RTC memory, so it survives deep sleep and resets (not power-on) |
| `sleep` | Deep sleep now |

The console is only reachable while USB is plugged in; light sleep is
disabled then so the USB link stays up.

## Bring-up order

1. `status`: check the battery reading and USB/charge state.
2. `diag on`, `rpm on`, `rpm sense`: confirm RPM_SENSE changes between black and white.
3. `rpm scan 5 apply` while turning the launcher slowly. Aim for ≥ 200 mV contrast.
4. `diag off`. Count the launcher's black/white segments and set `cfg edges <n>`.
5. `cal load empty` with no top attached, then `cal load loaded` with one attached.
6. Launch and compare the peak RPM with a reference.

Without a load calibration the RPM channel stays armed all the time, so
launches are still measured; the display shows **ARMED** instead of **READY**.

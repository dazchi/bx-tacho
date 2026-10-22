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
| `main/bbp.c` | Battle Pass (BX-09) emulation over BLE and the shot history, see below |
| `main/cli.c` | Bring-up and calibration console |
| `main/ble_console.c` | The console over BLE (unlocked with a code on the display), see below |
| `main/main.c` | Tasks, light sleep, display dimming, deep sleep |

## Console (USB Serial/JTAG, or Bluetooth)

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
| `load led on\|off` | Hold the load LED on (diag or meter mode), to test its crosstalk into the RPM sensor. In normal use it is only flashed while the RPM channel is armed: its light reflected off a loaded top hides RPM edges |
| `cal load empty` / `cal load loaded` / `cal load clear` | Calibrate the load sensor |
| `cal vbat start` … `stop` | Production VBAT calibration, see below |
| `cfg edges <n>` | Rising edges per launcher revolution |
| `cfg lightsleep on\|off` | Automatic light sleep on battery (default on), for power debugging |
| `events` / `events clear` | Power/sleep event log: boots and reset reasons, load changes, state changes, USB, display on/off, deep sleep. Kept in RTC memory, so it survives deep sleep and resets (not power-on) |
| `launch [n]` | Last launch edge by edge (first `n` edges, default 60): time, interval, RPM, with the release seen by the load sensor and any edges the chatter filter dropped |
| `ble` | Battle Pass link state |
| `ble on\|off` | Battle Pass emulation and the BLE console (default on); saves and restarts |
| `history` / `history clear` | Shot history sent to Battle Pass apps; `clear` wipes the last 50 shots, the total count, the max and the latest curve |
| `sleep` | Deep sleep now |

Over USB, light sleep is disabled while a host is connected so the link
stays up. Ctrl-C stops `meter log`, `rpm scan`, `rpm sense` and `load`
(on USB, other keys typed while a command runs are dropped).

### Over Bluetooth

The same commands also run over BLE, from the web app's Bluetooth tab
(`web/`), without a cable: this is how the VBAT calibration runs with no USB
power. It is a GATT service of its own (`main/ble_console.h` has the UUIDs and
the protocol), next to the Battle Pass one.

- **Code:** on every connection, opening the console shows
  `BLUETOOTH CONSOLE` and a 6-digit code on the display, to type into the web
  app. Until then the console runs nothing, so only someone who can see the
  display can use it. A wrong code gets a new one after a second's delay, and a
  code expires after 2 minutes. There is no Bluetooth pairing: nothing is
  stored on either side, and the link isn't encrypted (someone nearby with a
  Bluetooth sniffer could read the console traffic, but can't run commands
  without the code). The Battle Pass service is unaffected.
- One command runs at a time: while USB runs one, Bluetooth gets a "busy" note,
  and the other way round.

## Bring-up order

1. `status`: check the battery reading and USB/charge state.
2. `diag on`, `rpm on`, `rpm sense`: confirm RPM_SENSE changes between black and white.
3. `rpm scan 5 apply` while turning the launcher slowly. Aim for ≥ 200 mV contrast.
4. `diag off`. Count the launcher's black/white segments and set `cfg edges <n>`.
5. `cal load empty` with no top attached, then `cal load loaded` with one attached.
6. Launch and compare the peak RPM with a reference.

Without a load calibration the RPM channel stays armed all the time, so
launches are still measured; the display shows **ARMED** instead of **READY**.

## Battle Pass emulation (BLE)

BX Tacho advertises as a Takara Tomy BEYBATTLE PASS (BX-09), `BEYBLADE_TOOL01`,
so third-party Battle Pass apps can connect and read launches. Protocol,
sources and open questions: [docs/bbp-protocol.md](docs/bbp-protocol.md).

- Loading a top and taking it off send the attach/detach status.
- Each launch adds its peak RPM to the history (last 50, plus lifetime count
  and max) and sends the history with the launch curve: the first 32
  revolutions from the start of the pull, including the spin-down.
- The history is kept in NVS (namespace `bbp`) and survives power cycles,
  deep sleep and `cfg reset`; `history clear` wipes it. It is recorded even with
  BLE off. Flashing the merged binary at 0x0 erases it, like the other NVS
  settings.
- Commands from the app: `0x51` status, `0x74` history and curve; anything
  else (including `0x75`, meaning unconfirmed) is logged and ignored.
- BLE runs alongside automatic light sleep (modem sleep, with the main crystal
  kept on), which raises the sleep current. `ble off` if you don't use an app.
  Deep sleep ends the connection; the app reconnects after a wake-up.
- **Switching BLE without a console:** put a top on and take it off 5 times in
  a row, each load within 1.5 s of the previous one. The display shows
  `BLE ON` or `BLE OFF`, then BX Tacho restarts with the new setting (same as
  `ble on|off`; logged as `BLE GESTURE` in `events`). Flashing the merged
  binary resets it to on.

## VBAT calibration (production)

Corrects the R3/R9 divider and ADC error (typically 10-40 mV) with a
straight-line fit. Run once per board.

**Setup**
- Power the board **only** from a bench supply on **J1** (pad 1 VBAT+,
  pad 2 GND), power switch ON, current limit about 300 mA, no battery.
- Run the console **over Bluetooth** (web app, Bluetooth tab) with USB
  unplugged, or connect USB **data-only** (VBUS cut, e.g. a USB power
  blocker). With VBUS present the TP4054 charger pushes current into J1 and
  spoils the reading; the firmware refuses to calibrate then.
- Read the voltage **at the J1 pads** with a multimeter, not on the supply.
- Stay within 3.2-4.4 V (the board runs from the supply; the ADC sees VBAT/2).

**Steps**
```
cal vbat start
cal vbat point 3300      # set supply, enter the multimeter reading (mV)
cal vbat point 3700
cal vbat point 4000
cal vbat point 4300
cal vbat save            # fit, check, store
cal vbat verify 3600     # different voltage; PASS within +-10 mV
cal vbat stop
```
`cal vbat show` (and `status`) print the stored calibration.

The calibration lives in its own NVS partition (`fctry`, see
`partitions.csv`). `cfg reset` and NVS recovery never touch it; only a new
calibration or a full flash erase replaces it.

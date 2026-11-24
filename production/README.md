# BX Tacho production flasher

Flashes boards one after another and records their IDs. For each ESP32-C3
plugged in over USB it:

1. **erases the whole chip** and writes the merged firmware image at 0x0
   (bootloader, partition table, `otadata` and app), with esptool;
2. lets the new firmware start and asks its console for `id`: the factory MAC
   (the serial number), the Bluetooth MAC and the eFuse unique ID (on chips
   that have one), checking the MAC against what esptool read;
3. records them in the production database `production.db` (in the current
   folder) and asks for the board to be removed. The next
   board is only taken once this one is unplugged, so none is flashed twice by
   accident.

A local web page shows each step in large type, with a tone when a board is
done or has failed, the count and the latest records.

## Use

From an ESP-IDF terminal (it has esptool and pyserial), or after
`pip install -r requirements.txt`:

```
python bx_production.py
```

**On macOS** (no ESP-IDF needed), in this folder:

```
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python bx_production.py
```

No drivers are needed: the ESP32-C3's USB serial and the PPK2 are standard
USB CDC devices (`/dev/cu.usbmodem…`). `certifi` in `requirements.txt` gives
Python from python.org the certificates to reach GitHub. The PPK2 shows up as
two ports there; the tool finds its command port by asking each for its
metadata (on Windows it goes by the port's USB location).

The page opens at http://127.0.0.1:8765/. **Automatic flashing is off** at
start and every time the Flash tab is opened (from the VBAT calibration or
Database tab), so a board plugged in
is left alone: press **Turn automatic flashing on** with no board on USB (it
refuses otherwise), then plug in a board; when it says **Done: remove the
board**, unplug it and plug in the next. Turn it off before plugging in a
board you want to keep as it is.

- **Firmware:** by default the merged image of the latest GitHub release
  (asked from GitHub at each start; without internet, the newest release
  downloaded before), downloaded once into `firmware/<tag>/` and checked
  against the release's
  `SHA256SUMS.txt`. `--release v2.5.0` starts with another release;
  `--firmware file.bin` with a local merged image (no checksum to check).
  On the Flash and Update tabs, the **Firmware** dropdown lists the GitHub
  releases that have a merged image, newest version first (asked again every
  few minutes; offline, the ones downloaded before), and choosing one
  downloads and checks it; **Choose a file on this PC…** takes a merged image
  (`bx-tacho-vX.Y.Z-merged.bin`; it must have the bootloader, partition
  table and app description, and its version is read from it), kept in
  `firmware/local/`. The firmware can only be changed while automatic
  flashing or update is off. Each board gets the firmware chosen when it was
  plugged in; the page shows its version and SHA-256, and the records say
  `vX.Y.Z (local)` for a local file.
- **Records:** see [Database](#database) below. A board flashed again
  updates its row (flash count up by one) and loses its calibration there,
  since erasing the chip erased it.
- **Failures** (esptool error, firmware not answering, MAC mismatch) show in
  red with the reason; nothing is recorded. Unplug the board and plug it in
  again to start over. The log at the bottom has esptool's output.
- `--identify-only` records the IDs of boards that already run the firmware,
  without erasing or flashing them.

**Careful:** while automatic flashing is on, every ESP32-C3 plugged into this PC is erased,
including the factory partition with the battery calibration. Calibrate
(`cal vbat`, `cal batt`) after flashing, and don't plug in a board you want to
keep as it is.

If a blank chip isn't put into download mode automatically, hold BOOT while
plugging it in.

## Firmware update

The **Update** tab works like Flash, but only updates the firmware: settings,
shot history and the VBAT calibration stay. It writes two parts of the same
release image, without erasing the chip:

- `0x0`-`0x9000`: the bootloader and the partition table;
- `0x10000`-`0x110000`: the `ota_0` app slot and a blank `otadata`, so the new
  app boots also on a board that last ran `ota_1` after a Bluetooth update.

`nvs` and `phy_init` (`0x9000`-`0x10000`) and `fctry` (from `0x110000`, the
calibration) aren't touched. This is the same set the web app's USB update
writes, so a board that still has the older single-app partition table is
moved to the current one too.

Automatic update is off whenever the tab is opened, like automatic flashing;
turn it on with no board on USB, then plug in the boards. Each update goes to
the board's row (`fw_update_at`, `fw_update_count`, `firmware`). The tool
then reads the board's `status` as on the Database tab (calibrations, below),
and the page shows its VBAT calibration (PASS, FAIL, CAL, or empty for none
known). If the image isn't laid out
as above (a `--firmware` file of another kind), updating is unavailable and
the tab says why.

## VBAT calibration

The **VBAT calibration** tab calibrates the battery voltage reading with a
Nordic Power Profiler Kit II as the supply. In this tab nothing is flashed;
the tab can only be changed while no board is being flashed or calibrated.
Opening it turns automatic flashing off, so going back to Flash flashes
nothing until it is turned on again.

1. Connect the PPK2 over USB (the page shows it as connected). Its output
   leads go to the test clip.
2. Clip on the board's **VBAT+** and **VBAT-** (no battery, power switch on)
   and connect it with a **data-only** USB cable: with USB power the charger
   would drive VBAT.
3. Press **Start**. The tool then:
   - switches the PPK2 to source mode at 3600 mV and turns its output on;
   - waits for the board on USB, reads its IDs (`id`);
   - `ble off` (a steady current) and waits for the restart;
   - `cal vbat start`, then `cal vbat point` at 3200, 3400, 3600, 3800, 4000
     and 4200 mV, setting the PPK2 for each;
   - `cal vbat save` (the line fit). If the firmware rejects the fit ("Not
     saved: ..."), the calibration starts over from `cal vbat start`; this
     shares the 3 tries with failed checks below, and a third rejection ends
     the run as failed, with nothing recorded;
   - `cal vbat verify` at one random voltage between each two points (5 in
     all; `--verify-per-segment N` for more), each must be within ±15 mV (the firmware decides).
     The first failed check ends the checks and the calibration starts over
     from `cal vbat start` (new points, new fit), up to 3 tries in all; after
     the third the board is recorded as FAIL;
   - `cal vbat stop`, `ble on`, waits for the restart, checks Bluetooth is
     on, and turns the PPK2 off. Bluetooth is turned back on after a failure
     too.
4. **Done: remove the board** shows the fit, each point and each check.
   Unclip it, connect the next and press **Start the next board**.

Results go to the board's row in the [database](#database), replacing an
earlier calibration of it. A board whose check failed is recorded with
`cal_result` FAIL, and the page shows it in red.

**Accuracy:** the PPK2's set voltage is taken as the true voltage, so the
calibration is only as good as the PPK2 and the clip leads. Check it once with
a good multimeter at the clip while a board is on it (in `cal vbat` mode); if
the board sees, say, 6 mV less than the setting, start the tool with
`--ppk2-offset-mv -6`.

## Diagnose

The **Diagnose** tab shows a board's load sensor live. Nothing is flashed
or written. Plug in a board running the firmware and the tool:

- opens its console, reads `status` (firmware, launcher in use, both
  launchers' load levels) and turns on `diag on`, which pauses the launch
  state machine;
- streams `load` (a reading every 200 ms: LED on minus LED off, so ambient
  light cancels) to the page.

The page shows:

- the latest reading, and a chart of the last 10 s to 5 min;
- the empty and loaded levels and both thresholds (released below 40 %,
  loaded above 90 %) of the launcher in use, black, white or both;
- noise over the chart's window (min, max, peak to peak, mean, standard
  deviation);
- where the reading lies on each launcher's scale.

**Load LED On / Off** holds the LED (`load led on|off`); the readings are
then the raw LOAD_SENSE voltage, on a new chart. **Auto** goes back to normal
readings.

Leaving the tab (for the Database tab too) stops the readings and sends
`diag off`. Diag mode isn't saved, but a board unplugged while the tab is open
can't be sent `diag off`: on battery it stays in diag mode (not measuring
launches) until it restarts or is plugged in here again and the tab left. With
several boards on USB, only the first is read.

## Battery percentage calibration

The **Battery %** tab starts the firmware's battery percentage calibration
(`cal batt`, see the firmware README) on boards as soon as they're charged.
Fit the battery and do the VBAT calibration first. Then plug the boards in
with a USB cable that has power; several at once is fine. For each one the
tool:

- reads its `status` every 2 s and lists its power, VBAT and percentage;
- waits until the charger reports it charged (`power: USB, charged`), then
  runs `cal batt start`;
- shows **Unplug** in large type and sounds a tone (again every 15 s until it
  is unplugged).

Unplugging starts the run, on the battery, for several hours, until BX Tacho
shows CURVE SAVED and goes to sleep. Plugging it in before that cancels it;
the log notes it if a board started this session comes back. A board that
already has a measured curve is left alone, unless you press **Calibrate
again**. One without a VBAT calibration, in another mode, or without USB
power is shown with the reason. Each board's row in the database gets its
`status` as on the Database tab. Leaving the tab sends `cal batt stop` to
boards armed but still plugged in.

"Charged" comes from the charger's CHRG signal, not from the voltage. A board
whose CHRG pull-up is too weak (R7 100 kΩ instead of 4.7 kΩ) never shows
charged, so it is never armed.

## Database

Everything goes to one SQLite file, `production.db` in the folder you run the
tool from (`--db path` for another), table `boards`, one row per board with
the chip's eFuse **unique ID** as the primary key. A chip without one can't be
recorded: it fails with that reason (all ESP32-C3 v0.4 have one).

| Columns | |
|---|---|
| `unique_id`, `serial_mac`, `bt_mac`, `chip` | the IDs from `id`; `serial_mac` is the serial number |
| `flashed_at`, `flash_count` | the last full flash (erase and merged image), and how many there were |
| `fw_update_at`, `fw_update_count` | the last firmware update (Update tab), and how many there were |
| `firmware`, `firmware_sha256`, `flash_port` | what the last flash or update wrote, and where |
| `cal_at`, `cal_gain`, `cal_offset_mv`, `cal_points`, `cal_max_fit_error_mv`, `cal_result`, `cal_ppk2_offset_mv`, `cal_attempts` | the VBAT calibration (empty again after a flash). `cal_result` CAL: calibrated elsewhere (by hand with `cal vbat`), with only the gain, offset, points and fit error from `status` |
| `cal_verify`, `cal_detail` | the checks as `actual:error; ...`, and the points and checks as JSON |
| `cal_count` | calibrations so far |
| `batt_cal`, `batt_curve` | whether the battery percentage curve was measured (`cal batt`): 1, or 0 for the rough default; and its line from `status` |
| `black_load_empty_mv`, `black_load_loaded_mv`, `black_rpm_thr_mv` (and `white_...`) | each launcher's load sensor calibration (`cal load`; empty: not calibrated) and RPM threshold |
| `board_read_at` | when the last two rows were read from the board (Database tab); all of them are cleared by a flash, which erases them |
| `customer_name`, `customer_contact`, `warranty_since`, `notes` | entered by hand on the Database tab; kept when the board is flashed or calibrated again. `warranty_since` is a date (YYYY-MM-DD) |
| `updated_at` | the last change |

The **Database** tab lists the boards: search (every word must match
somewhere, e.g. `FAIL`, part of a MAC or a customer's name), click a column
to sort, click a row for all its fields with its calibration points and
checks, and a form for its customer, contact, warranty date and notes (**Save**
writes them). A database made before these columns existed gets them added
when the tool starts. **Download CSV**
exports the table. The tab only shows data: the mode stays as it was, so with
automatic flashing (or update) on, a board plugged in meanwhile is still
written.

**Black span check.** A board whose black launcher calibration is narrow
(loaded − empty under 50 mV, `BLACK_SPAN_MIN_MV` in `index.html`) shows its
**Black load** in red, with the reason on hover and in its details; the count
is under the table, and searching `low-span` lists them. Every board is
calibrated on the same set of launchers with the web app's "Several
launchers" guide, which saves two thirds of the room between their highest
empty and lowest loaded readings, so a span narrower than the other boards'
is the board: a less sensitive ITR8307, or one tilted or sitting too high. It
leaves less margin for a customer's launcher that reads differently. Reseat
or reflow the ITR8307 and calibrate again. (A board calibrated on one
launcher has a wider span, about 90 mV: the check only compares boards
calibrated the same way.)

**Delete…** in a board's details removes its row, after a second step that
names the board (and what goes with it: customer, calibration, notes) and
wants the last 4 characters of its serial typed. It can't be undone; the row
as it was goes to the log on the page, as the only trace. The board itself
isn't changed.

**Plugging in a board** while the tab is open looks it up: once its USB port
has settled, the tool reads its IDs from the firmware console (`id`). A board
in the database gets its row selected (clearing the search if it hid it); one
that isn't is offered with **Add to the database** (its IDs, and the firmware
version it runs, from `status`), or **Not now**. Either way its row gets
what `status` says about its battery curve and both launchers' load
calibration and RPM threshold (the **Batt %**, **Black load** and **White
load** columns; the thresholds and read time are in its details). A board
with a VBAT calibration this tool didn't make gets it recorded as **CAL**
(gain, offset, points, fit error), and loses it again once the board has
none; a calibration by this tool (PASS/FAIL) is left as it is. Reading
it again changes the last change time only if one of them changed; a
firmware that doesn't print one of them leaves that column as it was. Until a board answers, it is tried again every second (showing the
last reason: its console still starting, no firmware on it, automatic
flashing or update on), for as long as it stays plugged in and the tab stays
open. Several boards plugged in at once are read one after another.

Records from the CSV files of earlier versions are imported into a new
database with

```
python boarddb.py import records.csv vbat_cal.csv
```

in time order, so a flash after a calibration clears it as above. Their
calibration points weren't kept, only the checks.

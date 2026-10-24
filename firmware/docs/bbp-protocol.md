# BEYBATTLE PASS (BX-09) BLE protocol — research notes

What a device has to look like on BLE so that third-party apps written for
Takara Tomy's BEYBATTLE PASS (BBP, BX-09) accept it. Implemented in
`main/bbp.c` (see the firmware README); the choices made are in section 7.

There is no official documentation. Everything below comes from open-source
projects that either talk to a real BBP or emulate one; the confidence column
says how well each detail is backed up. Sources are listed at the end.

## 1. Summary

| Item | Value | Confidence |
|---|---|---|
| Advertised name | `BEYBLADE_TOOL01` | High: real-device client and two apps |
| Service UUID | `55c40000-f8eb-11ec-b939-0242ac120002` | High: every source |
| Notify characteristic | `55c4f002-f8eb-11ec-b939-0242ac120002` | High: every source |
| Write characteristic | `55c4f001-f8eb-11ec-b939-0242ac120002` | Medium, see 3.2 |
| Packet size | 17 bytes, byte 0 = packet type | High |
| Byte order | Little-endian 16-bit fields | High |
| Packet types | `A0` status; `B0`–`B6` history; `B7` checksum; `70`–`73` launch curve | High |
| Speed unit | "Shoot power" (SP), effectively RPM | High |
| Curve unit | Revolution period in 8 µs ticks: `SP = 7 500 000 / value` | High |

## 2. Advertising and connection

- **Name: `BEYBLADE_TOOL01`, exactly.** BeyMeter filters with
  `namePrefix: "BEYBLADE_TOOL01"` plus the service UUID; the Python client
  matches the name exactly. A different name (the BRD repo uses
  `BEYBLADE_TOOL_BRD_XXXX`) is **not** found by those apps.
- Advertise the service UUID as well: apps filter on name **and** service.
- The real BBP only advertises after its button is held ("pairing state",
  BeyMeter README). An emulator can advertise all the time.
- One connection at a time (BRD rejects a second central).
- No pairing/bonding. BeyMeter warns not to pair in the phone's Bluetooth
  settings; apps connect directly.
- MTU: the default 23 is enough (17-byte notifications).

## 3. GATT

### 3.1 Service `55c40000-f8eb-11ec-b939-0242ac120002`

### 3.2 Characteristics

| UUID | Properties | Use |
|---|---|---|
| `55c4f001-…-0242ac120002` | Write, Write Without Response | Commands from the app (1 byte) |
| `55c4f002-…-0242ac120002` | Notify | All data from the device (17 bytes) |

The real device has **two** characteristics. The Python client written against
a real BBP writes commands to characteristic handle 15 and subscribes to
handle 17, so they are separate attributes. Only one source names the write UUID
(`55c4f001`, MarkChan0225's tachometer); the BRD emulator instead puts
Write + Notify on `f002` alone.

**Recommendation:** expose both. Put Notify on `f002` and accept writes on both
`f001` and `f002`. That satisfies apps built against either layout, and
listen-only apps (BeyMeter) only need `f002`.

## 4. Packets (device → app, Notify on `f002`)

All packets are 17 bytes; byte 0 is the type, unused bytes are 0. `u16` =
16-bit little-endian.

### 4.1 `A0` — status

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `0xA0` |
| 1–2 | 2 | 0 |
| 3 | 1 | State, see below |
| 4 | 1 | Battery, raw 0–250 |
| 5–6 | 2 | 0 |
| 7–8 | u16 | Highest SP ever recorded (lifetime max) |
| 9–10 | u16 | Total launches ever (lifetime counter) |
| 11–16 | 6 | Device ID |

**State (byte 3).** These values are agreed by all sources:

| Value | Meaning |
|---|---|
| `0x00` | Detached / idle. Apps treat 1→0 as "released", used to place the launch marker on the curve |
| `0x04` | Attached (top loaded) |

`0x10` is set in some packets, and sources disagree on what it means:

- **BeyMeter:** saw `0x14` "occasionally" while attached and treats it as attached.
- **MarkChan simulator:** `0x10` is a button click, sent as `0x10` and then `0x00`.
- **BRD:** treats `0x10` as battery ≤ 20 % and `0x01` as battery ≤ 5 %.

Safe choice: send `0x04` and `0x00` only, and treat bit `0x04` as the
attached flag. Apps that recognise attached accept `0x04` or `0x14`.

**Battery (byte 4).** The BBP runs on 2× AAA; raw 0–250 maps 2.00–3.00 V
(0.004 V/step), and apps show `raw / 250 × 100 %`. An emulator should send its
own battery percentage as `percent × 2.5`.

**Device ID (bytes 11–16).** These six bytes are opaque. BRD sends the ESP32
MAC, the simulator sends `AA BB CC DD EE FF`, and one web parser saw `0x67`
padding. No app is known to check them. Send the ESP32 MAC so each unit stays
the same across boots.

### 4.2 `B0`–`B6` — shot history (last 50 shots)

- **Slots:** 50 shots × u16 SP, 8 per packet. Slot *i* is in packet
  `B0 + i / 8`, at offset `1 + (i % 8) × 2`. `B0`–`B5` hold slots 0–47; `B6`
  holds slots 48–49 at offsets 1–4.
- **`B6` extra fields:**

  | Offset | Size | Field |
  |---|---|---|
  | 5–6 | 2 | 0 |
  | 7–8 | u16 | Lifetime max SP (BRD and simulator; the parsers don't read it) |
  | 9–10 | u16 | Total launches |
  | 11 | 1 | List index *n*, 1–50: the latest shot is in slot *n − 1* |
  | 12–16 | 5 | 0 |

- **Latest shot:** BeyMeter takes it from slot `n − 1` and rejects `n = 0` or
  `n > 50`. So with no shots there is no valid history set.
- **Order of the 50 slots:** this is not fully settled. The simplest layout
  that every parser accepts is oldest first: fill slots 0, 1, 2, …; once all
  50 are full, keep them in age order with the newest at slot 49, and set
  `n` = count. BRD does this. MarkChan's dashboard treats `n` as a circular
  write pointer instead. Both readings agree on "latest = slot `n − 1`".

### 4.3 `B7` — history checksum

| Offset | Field |
|---|---|
| 0 | `0xB7` |
| 1–15 | 0 |
| 16 | `(sum of bytes 1–16 of B0…B6) & 0xFF` |

The checksum covers `B0`–`B6` only, not `B7` itself. BeyMeter discards the
whole shot on a mismatch.

### 4.4 `70`–`73` — launch curve (latest shot)

- 32 points × u16, 8 per packet. Point *i* is in packet `0x70 + i / 8`, at
  offset `1 + (i % 8) × 2`.
- **Value:** the period of one revolution in 8 µs units (`period_us / 8`,
  rounded); `SP = 7 500 000 / value`. A value of 0 means no point. Points end
  at the first 0.
- **Order:** consecutive revolutions in time order. They start once the top is
  loaded and turning (the pull), so the curve rises to the peak and then falls.
  An app's time axis is the running sum of `value / 125` ms.
- **App analysis (BeyMeter):** needs at least 7 points. It looks for the first
  peak among the first 14 points, extrapolates the "true" SP from that peak,
  and falls back to the history SP when the profile is short or odd. So the
  first ~14 revolutions of the pull carry most of the weight.

## 5. Commands (app → device, 1-byte writes)

| Byte | Meaning | Response |
|---|---|---|
| `0x51` | Request status | One `A0` |
| `0x74` | Request history and curve | `B0` … `B7`, `70` … `73` (12 packets) |
| `0x75` | **Unclear**: "clear history" (BRD) or "sensor test" (MarkChan) | BRD: none |
| `0x61` | Unknown; probed by the Python client against a real BBP | None |

- `0x51`, `0x74`, `0x75` and `0x61` are exactly the bytes the real-device client
  sends, so the real device does use them.
- The meaning of `0x75` isn't confirmed. Handling it as "clear history" risks
  wiping data on a probe, so ignore it at first.
- MarkChan's `0x76`/`0x77` (threshold and config) are its own extensions, not
  BBP.

## 6. Pushes the real device sends unprompted

Listen-only apps (BeyMeter never writes) work with a real BBP, so the real
device notifies without being asked:

| Event | Notify |
|---|---|
| Just after the app subscribes | `A0` (BRD waits 100 ms after the subscription) |
| Top attached / detached | `A0` with byte 3 = `0x04` / `0x00` |
| Shot finished | `B0` … `B7`, then `70` … `73` |

- **Order:** BeyMeter starts its analysis when `B7` or `73` arrives, once all
  of `B0`–`B7` and `70`–`73` are present. It doesn't depend on the order, but
  B0…B7 then 70…73 is the observed order.
- **Pacing:** BRD sends the packets about 10 ms apart. Never interleave an `A0`
  inside the 12-packet set.
- **After a shot:** BRD publishes about 600 ms after the first curve point.
  BeyMeter only places the launch marker if the detach `A0` (state `0x00`)
  arrives within 2 s before the shot set.

## 7. Mapping to BX Tacho (for later implementation)

| BBP | BX Tacho source |
|---|---|
| Attached (`A0` byte 3 `0x04`) | `LAUNCH_LOADED` (load sensor) |
| Detached (`0x00`) | Leaving LOADED (top released or removed) |
| Shot SP | `launch_result_t.peak_rpm` |
| Curve | Per-revolution periods from `rpm.c` edge timestamps, from the start of the pull; first 32 revolutions, `/ 8` |
| History, lifetime max, count | New NVS blob (50 × u16 + counters), kept apart from `settings` |
| Battery | `battery_status_t.percent × 2.5` (on USB: last valid value, or 250 when FULL) |
| Device ID | `esp_efuse_mac_get_default()` |

**As implemented:** BLE on by default (`ble on|off`, applied at boot); name
in the advertisement and the service UUID in the scan response; writes accepted
on both characteristics; `0x75` and `0x61` ignored; the shot set is sent when
the launch reaches RESULT (after the 250 ms end gap), then saved to NVS.

Commands and packets carry a session number that changes on every
connect, disconnect and subscription change, so a dump started for one session
never continues into the next; a packet the stack can't take for 1.5 s drops
the link so the app starts over.

**Points that were open before implementing:**

- **Power.** BLE advertising costs current and interacts with automatic light
  sleep. Advertising can run during light sleep (NimBLE with
  `CONFIG_BT_SLEEP_ENABLE`), but it needs testing against the RPM capture and
  the USB console locks. Decide whether BLE is always on, toggled by a
  setting, or on only while awake.
- **Deep sleep.** BLE disconnects on deep sleep; apps reconnect after a power
  cycle or USB wake.
- **CPU load.** The NimBLE host task during a launch must not disturb the edge
  ISR timestamps. Keep notifications out of the PULLING window by sending
  after RESULT, as the real device does about 600 ms later.
- **Flash.** NimBLE adds roughly 150–200 KB of flash; the 1 MB app partition
  has room (the app is about 370 KB now).
- **Licence.** BRD is GPL-3.0 and BeyMeter is MIT. Implement independently from
  these notes rather than copying code.

## 8. Sources

| Source | What it is | Used for |
|---|---|---|
| [printfChins/Beyblade-RPM-Detector-Device](https://github.com/printfChins/Beyblade-RPM-Detector-Device) @ `58bbc41` (2026-10-13), `firmware/BRD_BPX/brd_bbp*.cpp`, `brd_config.h` | ESP32-C3 BBP emulator (GPL-3.0) | UUIDs, packet builders, commands, auto-notify timing. Non-standard name, single characteristic |
| [bahamutonX/BeyMeter](https://github.com/bahamutonX/BeyMeter) @ `a700fc1`, `src/features/ble/bbpTypes.ts`, `bbpProtocol.ts` | App that reads a **real** BBP (MIT) | Name filter, listen-only behaviour, A0 states, checksum, list index, curve maths |
| [danielbyomujuni/beyblade-x-beybattlepass-python](https://github.com/danielbyomujuni/beyblade-x-beybattlepass-python) @ `a2882ac`, `main.py`, `discover.py` | bleak client for a **real** BBP | Name `BEYBLADE_TOOL01`, separate write/notify handles, command bytes 51/74/75/61 |
| [MarkChan0225/Beyblade-BattlePass-BLE-Simulator](https://github.com/MarkChan0225/Beyblade-BattlePass-BLE-Simulator) @ `29afef2` | Packet simulator | A0 layout, battery scale, B6 tail, checksum |
| [MarkChan0225/beyblade-tachometer-web](https://github.com/MarkChan0225/beyblade-tachometer-web) @ `3277f0f`, `README.md`, `protocol.mjs` | Dashboard for an ESP32-S3 BBP-style tachometer | Write UUID `f001`, history/curve parsing |
| [BX-09 product page (otakumart)](https://otakumart.co.nz/products/bx-09-beybattle-pass-beyblade-x) | Retail listing | 2× AAA batteries |

Not found: anything published by Takara Tomy, or any public source for the
"Beyblade Helper" app. To settle the open points (`0x10`, `0x75`, write UUID,
history order), capture a real BX-09 with nRF Connect: read the GATT table,
then log notifications through attach, launch and detach.

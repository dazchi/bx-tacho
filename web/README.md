# BX Tacho web app

A static web page for BX Tacho, run in the browser:

- **USB:** the console in a terminal, and firmware updates.
- **Bluetooth:** shot data from the Battle Pass service: latest and best shoot
  power, shot count, battery, the launch curve and the last 50 shots. Below
  it, the same console without a cable (for the VBAT calibration, which needs
  BX Tacho off USB power). Opening it asks for the 6-digit code BX Tacho
  shows on its display, on every connection; the shot data needs no code.

Only one connection at a time. Plain HTML, CSS and JavaScript modules, no build
step.

## Hosting on GitHub Pages

`.github/workflows/pages.yml` publishes this folder whenever `web/` changes on
`master`. Once, in the repository: **Settings › Pages › Build and deployment ›
Source: GitHub Actions**. The site is then at
`https://<user>.github.io/bx-tacho/` (for this repo,
`https://dazchi.github.io/bx-tacho/`).

Web Serial and Web Bluetooth only work on a secure page: GitHub Pages
(https) or `localhost`.

## Running it locally

```bash
python -m http.server 8731 --directory web
```

Then open `http://localhost:8731`. Add `?demo` to see the Bluetooth tab with
a recorded launch, without a device.

## Browsers

| | USB (Web Serial) | Bluetooth (Web Bluetooth) |
|---|---|---|
| Chrome, Edge (Windows, macOS, Linux) | Yes | Yes |
| Chrome on Android | No | Yes |
| Firefox, Safari, iOS | No | No |

## Languages

English, Traditional Chinese (`zh-Hant`), Simplified Chinese (`zh-Hans`) and
Japanese (`ja`). The page follows the browser's preferred languages: `zh-TW`,
`zh-HK`, `zh-MO` or a `Hant` script get Traditional, any other Chinese gets
Simplified, any `ja` gets Japanese, anything without a translation gets
English. The language menu in the footer
overrides that; the choice is remembered in this browser, and "Automatic"
goes back to following the browser.

To add a language:

1. Copy `js/locales/en.js` to `js/locales/<code>.js` (a BCP 47 code such as
   `ja` or `ko`) and translate the values. Keep the `{placeholders}`. Missing
   keys fall back to English.
2. Import it in `js/i18n.js` and add it to `LANGUAGES` with its own name for
   the menu.
3. If the browser's tags for it need more than the base code (as Chinese
   does), add a rule to `matchTag()` in `js/i18n.js`.
4. For a script that Archivo doesn't cover, add a `:root:lang(<code>)` font
   stack in `css/style.css`, as for Chinese.

In the markup, `data-i18n="key"` sets an element's text, `data-i18n-html` its
markup (our own strings only), and `data-i18n-attr="aria-label:key"` its
attributes. In code, `setText(el, key, params)` keeps a text translated when
the language changes.

## Firmware updates

Choose `bx-tacho.bin` from `firmware/build/`. It's written at `0x10000`, so
the shot history, load calibration and settings in NVS are kept. Every update
also writes the bootloader and partition table bundled in `firmware/`, and a
blank `otadata` so the board boots the app just written rather than one in the
other slot. That also moves a board that still has the older single-app table
to the two slots that Bluetooth updates need. The page
reads the file's header first and refuses anything else: the merged image
(`bx-tacho-merged.bin`), images without an ESP-IDF app description (such as
`bootloader.bin`), other projects, other chips. This prevents picking the
wrong file by mistake; it doesn't lock the firmware: any build of this
project (ESP-IDF project name `bx-tacho`), including your own changes, is
accepted.

**Advanced: erase the whole chip first.** For a chip that won't start or a
blank one. The page erases all of the flash before writing. This also deletes
the factory VBAT calibration (`fctry` partition), so it asks first.
`firmware/bootloader.bin` and `firmware/partition-table.bin` come from
`firmware/build/bootloader/` and `firmware/build/partition_table/`; copy them
again if the bootloader config or `partitions.csv` ever changes.

Flashing uses [esptool-js](https://github.com/espressif/esptool-js) 0.7.0,
loaded from jsDelivr only when you start an update. If the console was
connected, the page reconnects after the chip restarts.

If an update fails and BX Tacho no longer starts: hold BOOT (SW3), tap RESET
(SW2), release BOOT, and update again with **Erase the whole chip first**
ticked.

**Over Bluetooth** (Bluetooth tab, *Update firmware*): the same file check,
then the code from BX Tacho's display, then about a minute. See "Firmware
updates over Bluetooth" in `firmware/README.md`.

## Files

| File | Purpose |
|---|---|
| `index.html`, `css/style.css` | Page and styles |
| `js/app.js` | Tabs, connection rules, terminal, firmware and Bluetooth views |
| `js/serial.js` | Web Serial link to the USB console |
| `js/flasher.js` | Image check and flashing with esptool-js |
| `js/ble.js` | Web Bluetooth link |
| `js/bleconsole.js` | The console over Bluetooth, on that link (see `firmware/main/ble_console.h`) |
| `js/bleota.js` | Firmware updates over Bluetooth (see `firmware/main/ble_ota.h`) |
| `js/bbp.js` | Battle Pass packet parser (see `firmware/docs/bbp-protocol.md`) |
| `js/chart.js` | Launch curve and history strip (SVG) |
| `js/i18n.js`, `js/locales/*.js` | Language detection, translation lookup, the strings per language |
| `js/pixelfont.js`, `js/font5x7.js` | The OLED's 5×7 font, for the readout and wordmark |
| `img/icon-*.png` | Site icon, a square crop of `firmware/assets/icons/beyblade-reference.jpg` (512, 96 and 64 px with transparent corners; 180 px opaque for iOS) |
| `firmware/` | Bootloader and partition table, written with every USB update |

`js/font5x7.js` is generated from `firmware/main/font5x7.c`. If the firmware
font changes, run `python web/tools/gen-font.py` from the repository root.

## Third-party code (from CDNs)

- [xterm.js](https://xtermjs.org/) 6.0.0 and addon-fit 0.11.0 (MIT): terminal
- [esptool-js](https://github.com/espressif/esptool-js) 0.7.0 (Apache-2.0): flashing
- [Archivo](https://fonts.google.com/specimen/Archivo) and
  [JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) (OFL), from Google Fonts

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Firmware update over Web Serial with esptool-js (loaded only when needed).
import { t } from './i18n.js';

const ESPTOOL_URL = 'https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js';

export const APP_ADDRESS = 0x10000;
export const BOOTLOADER_ADDRESS = 0x0;
export const PART_TABLE_ADDRESS = 0x8000;
// Which of the two app slots boots (firmware/partitions.csv); blank boots ota_0,
// the slot at APP_ADDRESS
const OTADATA_ADDRESS = 0x10e000;
const OTADATA_SIZE = 0x2000;
const PART_TABLE = 0x8000;
const IMAGE_MAGIC = 0xe9;
const APP_DESC_MAGIC = 0xabcd5432;
const CHIP_ID_ESP32C3 = 0x0005;

function cString(bytes, start, len) {
  const end = bytes.indexOf(0, start);
  return new TextDecoder().decode(bytes.subarray(start, end < 0 || end > start + len ? start + len : end));
}

// esp_image_header_t (24 bytes), the first segment header (8), then esp_app_desc_t
function readApp(bytes, at) {
  if (bytes.length < at + 32 + 80 || bytes[at] !== IMAGE_MAGIC) return null;
  const chipId = bytes[at + 12] | (bytes[at + 13] << 8);
  const d = at + 32;
  const magic = (bytes[d] | (bytes[d + 1] << 8) | (bytes[d + 2] << 16) | (bytes[d + 3] << 24)) >>> 0;
  if (magic !== APP_DESC_MAGIC) return { chipId, hasDesc: false };
  return { chipId, hasDesc: true, version: cString(bytes, d + 16, 32), project: cString(bytes, d + 48, 32) };
}

// Checks that a .bin is a BX Tacho app image (bx-tacho-v<version>.bin, or bx-tacho.bin). kind: 'app' | 'invalid',
// with `reason` a translation key (and `project` for 'fw.bad.project').
// This guards against picking the wrong file, not against modified firmware: any
// ESP-IDF build of this project (project name "bx-tacho") is accepted.
export function analyzeImage(bytes) {
  const full = bytes.length > APP_ADDRESS && bytes[0] === IMAGE_MAGIC &&
    bytes[PART_TABLE] === 0xaa && bytes[PART_TABLE + 1] === 0x50;
  if (full) {
    return { kind: 'invalid', reason: 'fw.bad.full' };
  }
  const app = readApp(bytes, 0);
  if (!app) {
    return { kind: 'invalid', reason: 'fw.bad.notImage' };
  }
  if (app.chipId !== CHIP_ID_ESP32C3) {
    return { kind: 'invalid', reason: 'fw.bad.chip' };
  }
  // Without the ESP-IDF app description it isn't an app: e.g. bootloader.bin,
  // which would leave the chip unable to boot if written at 0x10000
  if (!app.hasDesc) {
    return { kind: 'invalid', reason: 'fw.bad.notApp' };
  }
  if (app.project !== 'bx-tacho') {
    return { kind: 'invalid', reason: 'fw.bad.project', project: app.project };
  }
  return { kind: 'app', address: APP_ADDRESS, version: app.version, size: bytes.length };
}

// The bootloader and partition table bundled with this page, written with every
// update: they match the firmware's partitions.csv, and bring a board that still
// has the older single-app table to the one with two slots (updates over
// Bluetooth). Plus a blank otadata, so the board boots what's written at
// APP_ADDRESS rather than an older image in the other slot.
export async function loadBootFiles() {
  const get = async (name) => new Uint8Array(await (await fetch(`firmware/${name}`, { cache: 'no-cache' })).arrayBuffer());
  const [boot, table] = await Promise.all([get('bootloader.bin'), get('partition-table.bin')]);
  if (boot[0] !== IMAGE_MAGIC || table[0] !== 0xaa || table[1] !== 0x50) {
    throw new Error('boot files damaged');
  }
  return [
    { data: boot, address: BOOTLOADER_ADDRESS },
    { data: table, address: PART_TABLE_ADDRESS },
    { data: new Uint8Array(OTADATA_SIZE).fill(0xff), address: OTADATA_ADDRESS },
  ];
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Restarts the chip into the new firmware. esptool-js 0.7.0's 'hard_reset' only
// releases RTS without pulsing it, so over the C3's USB Serial/JTAG the chip stayed
// in the flasher stub until the port closed. This is esptool.py's sequence: RTS is
// the reset line; DTR must be low when it's released, or the chip boots into the
// ROM download mode again.
async function resetChip(transport) {
  await transport.setDTR(false);
  await transport.setRTS(true);
  await sleep(200);
  await transport.setRTS(false);
  await sleep(200);
}

// Writes `files` ([{ data, address }]) through the already-granted `port` (which
// must be closed), erasing the whole chip first if `eraseAll`. Calls
// onProgress(0..1) and log(line). Resets the chip at the end.
export async function flashFirmware(port, files, { eraseAll = false, onProgress, log }) {
  const { ESPLoader, Transport } = await import(ESPTOOL_URL);
  const terminal = {
    clean() {},
    writeLine: (s) => log?.(s),
    write: (s) => log?.(s),
  };
  const transport = new Transport(port, false);
  try {
    const loader = new ESPLoader({ transport, baudrate: 460800, terminal, debugLogging: false });
    const chip = await loader.main();
    log?.(t('fw.log.connected', { chip }));
    const sizes = files.map((f) => f.data.length);
    const all = sizes.reduce((a, b) => a + b, 0);
    if (eraseAll) {
      log?.(t('fw.log.erasing'));
      onProgress?.(0, 'erase');
    }
    await loader.writeFlash({
      fileArray: files,
      flashMode: 'keep',
      flashFreq: 'keep',
      flashSize: 'keep',
      eraseAll,
      compress: true,
      reportProgress: (i, written, total) => {
        const before = sizes.slice(0, i).reduce((a, b) => a + b, 0);
        onProgress?.((before + (written / total) * sizes[i]) / all, 'write');
      },
    });
    await resetChip(transport);
  } finally {
    // The reset makes the USB device disappear, so this may fail
    try {
      await transport.disconnect();
    } catch { /* gone */ }
  }
}

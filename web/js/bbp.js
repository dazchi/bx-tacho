// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// BEYBATTLE PASS (BX-09) protocol, as BX Tacho speaks it.
// See firmware/docs/bbp-protocol.md for the packet layout and sources.

export const DEVICE_NAME = 'BEYBLADE_TOOL01';
export const SERVICE_UUID = '55c40000-f8eb-11ec-b939-0242ac120002';
export const CMD_UUID = '55c4f001-f8eb-11ec-b939-0242ac120002';    // app -> device
export const DATA_UUID = '55c4f002-f8eb-11ec-b939-0242ac120002';   // device -> app (notify)

export const CMD_STATUS = 0x51;
export const CMD_HISTORY = 0x74;

const PKT_LEN = 17;
const SET_HEADERS = [0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0x70, 0x71, 0x72, 0x73];

const u16 = (b, o) => b[o] | (b[o + 1] << 8);

export function parseStatus(b) {
  return {
    attached: (b[3] & 0x04) !== 0,
    batteryPct: Math.round(Math.min(b[4], 250) / 2.5),
    maxSp: u16(b, 7),
    total: u16(b, 9),
  };
}

// A curve value is one revolution in 8 us units: rpm = 7 500 000 / value
export function curveToPoints(values) {
  const points = [];
  let tMs = 0;
  for (const v of values) {
    if (!v) break;
    tMs += v / 125;
    points.push({ lap: points.length + 1, tMs, rpm: Math.round(7_500_000 / v) });
  }
  return points;
}

function parseSet(pk) {
  let sum = 0;
  for (let h = 0xb0; h <= 0xb6; h++) {
    for (let i = 1; i < PKT_LEN; i++) sum = (sum + pk.get(h)[i]) & 0xff;
  }
  if (sum !== pk.get(0xb7)[16]) {
    return { error: 'ble.checksum' };   // a translation key
  }
  const b6 = pk.get(0xb6);
  const count = Math.min(b6[11], 50);
  const history = [];
  for (let i = 0; i < count; i++) {
    history.push(u16(pk.get(0xb0 + Math.floor(i / 8)), 1 + (i % 8) * 2));
  }
  const curve = [];
  for (let i = 0; i < 32; i++) curve.push(u16(pk.get(0x70 + Math.floor(i / 8)), 1 + (i % 8) * 2));
  return {
    history,                       // oldest first
    latestSp: count ? history[count - 1] : 0,
    maxSp: u16(b6, 7),
    total: u16(b6, 9),
    points: curveToPoints(curve),
  };
}

// Feed it every notification; returns a parsed status, a complete shot set, or null.
export class Assembler {
  constructor() {
    this.packets = new Map();
  }

  feed(view) {
    const b = view instanceof DataView
      ? new Uint8Array(view.buffer, view.byteOffset, view.byteLength)
      : new Uint8Array(view);
    if (b.length < PKT_LEN) return null;
    const head = b[0];
    if (head === 0xa0) return { type: 'status', ...parseStatus(b) };
    if (!SET_HEADERS.includes(head)) return null;
    if (head === 0xb0) this.packets.clear();    // a new set starts
    this.packets.set(head, b.slice(0, PKT_LEN));
    if ((head === 0x73 || head === 0xb7) && SET_HEADERS.every((h) => this.packets.has(h))) {
      const set = parseSet(this.packets);
      this.packets.clear();
      return { type: 'set', ...set };
    }
    return null;
  }
}

export function peakOf(points) {
  let best = null;
  for (const p of points) if (!best || p.rpm > best.rpm) best = p;
  return best;
}

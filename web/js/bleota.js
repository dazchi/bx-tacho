// Firmware update over Bluetooth (firmware: main/ble_ota.h), on the link that
// BleLink opened. BX Tacho only accepts it while the console is unlocked with
// the code on its display; app.js takes care of that first.

export const OTA_SERVICE = 'fcf60011-7902-4817-adcc-c0380ba2609d';
const OTA_CHAR = 'fcf60012-7902-4817-adcc-c0380ba2609d';

const OP_BEGIN = 0x01;
const OP_DATA = 0x02;
const OP_END = 0x03;
const OP_ABORT = 0x04;

const WINDOW = 8192;           // bytes in flight beyond the last acknowledged count
const BEGIN_TIMEOUT_MS = 30000;   // erasing room for the image takes a few seconds
const ACK_TIMEOUT_MS = 10000;
const END_TIMEOUT_MS = 15000;     // checking the image

// Status codes from BX Tacho, as translation keys
const STATUS_KEYS = [null, 'bota.err.locked', 'bota.err.battery', 'bota.err.noSlot', 'bota.err.tooBig',
  'bota.err.flash', 'bota.err.badImage', 'bota.err.notOurs', 'bota.err.sequence'];

export class OtaError extends Error {
  constructor(key, detail = '') {
    super(detail || key);
    this.key = key;
  }
}

export class BleUpdater {
  constructor() {
    this.chr = null;
    this.waiter = null;      // { op, resolve, reject, timer }
    this.acked = 0;
    this.onAck = null;
    this.onValue = (e) => this.handle(e.target.value);
  }

  // Throws OtaError 'bota.err.noService' with firmware that can't update over Bluetooth
  async open(server) {
    let service;
    try {
      service = await server.getPrimaryService(OTA_SERVICE);
    } catch {
      throw new OtaError('bota.err.noService');
    }
    this.chr = await service.getCharacteristic(OTA_CHAR);
    this.chr.addEventListener('characteristicvaluechanged', this.onValue);
    await this.chr.startNotifications();
  }

  async close() {
    const chr = this.chr;
    this.chr = null;
    this.fail(new OtaError('bota.err.disconnected'));
    if (chr) {
      chr.removeEventListener('characteristicvaluechanged', this.onValue);
      await chr.stopNotifications().catch(() => {});
    }
  }

  handle(view) {
    if (view.byteLength < 6) return;
    const op = view.getUint8(0);
    const status = view.getUint8(1);
    const value = view.getUint32(2, true);
    if (op === OP_DATA && status === 0) {
      this.acked = value;
      this.onAck?.();
      return;
    }
    const w = this.waiter;
    if (status !== 0) {
      this.fail(new OtaError(STATUS_KEYS[status] || 'bota.err.sequence'));
    } else if (w && w.op === op) {
      this.waiter = null;
      clearTimeout(w.timer);
      w.resolve(value);
    }
  }

  fail(err) {
    const w = this.waiter;
    this.waiter = null;
    if (w) {
      clearTimeout(w.timer);
      w.reject(err);
    }
    this.error = this.error || err;
    this.onAck?.();
  }

  // Resolves with the reply's value to `op`
  expect(op, ms) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.fail(new OtaError('bota.err.timeout')), ms);
      this.waiter = { op, resolve, reject, timer };
    });
  }

  waitAck(below) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new OtaError('bota.err.timeout')), ACK_TIMEOUT_MS);
      this.onAck = () => {
        if (this.error || this.acked >= below) {
          clearTimeout(timer);
          this.onAck = null;
          if (this.error) reject(this.error);
          else resolve();
        }
      };
      this.onAck();
    });
  }

  // Sends `image` (Uint8Array); onProgress(0..1, phase) with phase 'erase',
  // 'write' or 'check'. Resolves once BX Tacho has accepted it and is
  // restarting into it.
  async update(image, onProgress) {
    if (!this.chr) throw new OtaError('bota.err.disconnected');
    this.error = null;
    this.acked = 0;
    try {
      onProgress?.(0, 'erase');
      const begin = new Uint8Array(5);
      begin[0] = OP_BEGIN;
      new DataView(begin.buffer).setUint32(1, image.length, true);
      const replied = this.expect(OP_BEGIN, BEGIN_TIMEOUT_MS);
      await this.chr.writeValueWithResponse(begin);
      const mtu = await replied;
      // One write: the op byte and as much data as fits in the MTU
      const chunk = Math.max(20, Math.min(512, mtu - 3) - 1);

      let sent = 0;
      while (sent < image.length) {
        if (this.error) throw this.error;
        if (sent - this.acked >= WINDOW) await this.waitAck(sent - WINDOW + chunk);
        const n = Math.min(chunk, image.length - sent);
        const msg = new Uint8Array(n + 1);
        msg[0] = OP_DATA;
        msg.set(image.subarray(sent, sent + n), 1);
        await this.chr.writeValueWithoutResponse(msg);
        sent += n;
        onProgress?.(sent / image.length, 'write');
      }
      await this.waitAck(image.length);

      onProgress?.(1, 'check');
      const done = this.expect(OP_END, END_TIMEOUT_MS);
      await this.chr.writeValueWithResponse(new Uint8Array([OP_END]));
      await done;
    } catch (e) {
      this.waiter = null;
      if (this.chr && !(e instanceof OtaError && e.key === 'bota.err.disconnected')) {
        await this.chr.writeValueWithResponse(new Uint8Array([OP_ABORT])).catch(() => {});
      }
      throw e instanceof OtaError ? e : new OtaError('bota.err.other', e.message);
    }
  }
}

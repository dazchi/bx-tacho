// BX Tacho's console over Bluetooth (firmware: main/ble_console.h), on the link
// that BleLink opened. On each connection it starts locked: BX Tacho shows a
// 6-digit code on its display, to be entered here. No Bluetooth pairing.

export const CONSOLE_SERVICE = 'fcf60001-7902-4817-adcc-c0380ba2609d';
const RX_UUID = 'fcf60002-7902-4817-adcc-c0380ba2609d';   // lines in: the code, then commands
const TX_UUID = 'fcf60003-7902-4817-adcc-c0380ba2609d';   // output, notified

const STOP = 0x03;     // stops the running command
const READY = 0x04;    // unlocked, and ready for the next command
const HELLO = 0x05;    // asks for the banner, or for the code
const CODE = 0x15;     // enter the code shown on the display

// "bx-tacho 0.7.1" (reading needs no code), or null for firmware without the console
export async function readVersion(server) {
  try {
    const service = await server.getPrimaryService(CONSOLE_SERVICE);
    const value = await (await service.getCharacteristic(TX_UUID)).readValue();
    return new TextDecoder().decode(value);
  } catch {
    return null;
  }
}

export class BleConsole {
  // onText(string); onReady() when ready for a command (also once unlocked);
  // onCode() when BX Tacho wants the code on its display
  constructor({ onText, onReady, onCode }) {
    this.cb = { onText, onReady, onCode };
    this.rx = null;
    this.tx = null;
    this.busy = false;     // waiting for BX Tacho to answer
    this.locked = true;    // the code isn't entered yet
    this.decoder = new TextDecoder();
    this.onValue = (e) => this.handle(e.target.value);
  }

  get open() {
    return !!this.tx;
  }

  // Throws an Error with code 'noConsole' if the firmware has no console service
  async start(server) {
    let service;
    try {
      service = await server.getPrimaryService(CONSOLE_SERVICE);
    } catch {
      throw Object.assign(new Error('no console service'), { code: 'noConsole' });
    }
    const rx = await service.getCharacteristic(RX_UUID);
    const tx = await service.getCharacteristic(TX_UUID);
    this.decoder = new TextDecoder();
    tx.addEventListener('characteristicvaluechanged', this.onValue);
    try {
      await tx.startNotifications();
      this.rx = rx;
      this.tx = tx;
      this.locked = true;
      this.busy = true;    // until BX Tacho asks for the code, or greets
      await rx.writeValueWithResponse(new Uint8Array([HELLO]));
    } catch (e) {
      this.reset();
      tx.removeEventListener('characteristicvaluechanged', this.onValue);
      await tx.stopNotifications().catch(() => {});
      throw e;
    }
  }

  handle(view) {
    const bytes = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
    let from = 0;
    for (let i = 0; i < bytes.length; i++) {
      if (bytes[i] !== READY && bytes[i] !== CODE) continue;
      this.emit(bytes.subarray(from, i));
      this.cb.onText?.(this.decoder.decode());   // the end of a split character
      from = i + 1;
      this.busy = false;
      this.locked = bytes[i] === CODE;
      if (this.locked) this.cb.onCode?.();
      else this.cb.onReady?.();
    }
    this.emit(bytes.subarray(from));
  }

  emit(bytes) {
    if (bytes.length) this.cb.onText?.(this.decoder.decode(bytes, { stream: true }));
  }

  // A command, or while locked the code
  async run(line) {
    if (!this.rx || this.busy) return false;
    this.busy = true;
    try {
      await this.rx.writeValueWithResponse(new TextEncoder().encode(`${line}\n`));
    } catch (e) {
      this.busy = false;
      throw e;
    }
    return true;
  }

  async stop() {
    if (this.rx) await this.rx.writeValueWithResponse(new Uint8Array([STOP]));
  }

  // Closes the console only; the Bluetooth link stays up (and stays unlocked)
  async close() {
    const tx = this.tx;
    this.reset();
    if (tx) {
      tx.removeEventListener('characteristicvaluechanged', this.onValue);
      await tx.stopNotifications().catch(() => {});
    }
  }

  // After the link dropped
  reset() {
    this.tx?.removeEventListener('characteristicvaluechanged', this.onValue);
    this.rx = this.tx = null;
    this.busy = false;
    this.locked = true;
  }
}

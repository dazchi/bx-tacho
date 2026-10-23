// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// USB serial over WebUSB, for Chrome on Android, which has Web Serial for
// Bluetooth ports only. BX Tacho's USB Serial/JTAG is a USB CDC-ACM device; this
// drives its two CDC interfaces directly and offers the same interface as Web
// Serial's SerialPort, as far as serial.js and esptool-js use it.

const CDC_CONTROL = 2;   // interface classes
const CDC_DATA = 10;
const SET_LINE_CODING = 0x20;
const SET_CONTROL_LINE_STATE = 0x22;

function findInterface(device, cls) {
  const iface = device.configurations[0]?.interfaces
    .find((i) => i.alternates[0].interfaceClass === cls);
  if (!iface) throw new TypeError(`no USB interface of class ${cls}`);
  return iface;
}

export class UsbSerialPort {
  constructor(device) {
    this.device = device;
    this.control = findInterface(device, CDC_CONTROL);
    this.data = findInterface(device, CDC_DATA);
    const eps = this.data.alternates[0].endpoints;
    this.inEp = eps.find((e) => e.direction === 'in');
    this.outEp = eps.find((e) => e.direction === 'out');
    this.signals = { dataTerminalReady: false, requestToSend: false };
    this.readable_ = null;
    this.writable_ = null;
  }

  // As in Web Serial, a closed or cancelled stream is replaced by a new one
  // (esptool-js closes the writable after each flush). An errored readable
  // stays, so its reader sees the error.
  get readable() {
    if (!this.readable_ && this.device.opened) {
      // One transfer in flight at a time (pull returns its promise)
      this.readable_ = new ReadableStream({
        pull: (c) => this.pull_(c),
        cancel: () => { this.readable_ = null; },
      });
    }
    return this.readable_;
  }

  get writable() {
    if (!this.writable_ && this.device.opened) {
      this.writable_ = new WritableStream({
        write: (chunk) => this.write_(chunk),
        close: () => { this.writable_ = null; },
        abort: () => { this.writable_ = null; },
      });
    }
    return this.writable_;
  }

  getInfo() {
    return { usbVendorId: this.device.vendorId, usbProductId: this.device.productId };
  }

  async open({ baudRate }) {
    const d = this.device;
    await d.open();
    try {
      if (d.configuration === null) await d.selectConfiguration(1);
      await d.claimInterface(this.control.interfaceNumber);
      await d.claimInterface(this.data.interfaceNumber);
      const coding = new DataView(new ArrayBuffer(7));   // 8N1 (CDC 6.2.12)
      coding.setUint32(0, baudRate, true);
      coding.setUint8(6, 8);
      await this.control_(SET_LINE_CODING, 0, coding.buffer);
      await this.setSignals({ dataTerminalReady: true });
    } catch (e) {
      await d.close().catch(() => {});
      throw e;
    }
  }

  async pull_(controller) {
    for (;;) {
      // Rejects when the port closes or the device goes away, which ends the stream
      const r = await this.device.transferIn(this.inEp.endpointNumber, this.inEp.packetSize * 8);
      if (r.status === 'stall') {
        await this.device.clearHalt('in', this.inEp.endpointNumber);
        continue;
      }
      if (r.status !== 'ok') throw new Error(`USB read: ${r.status}`);
      if (r.data?.byteLength) {
        controller.enqueue(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength));
        return;
      }
      // A zero-length packet ends a transfer: not data, read on
    }
  }

  async write_(chunk) {
    const r = await this.device.transferOut(this.outEp.endpointNumber, chunk);
    if (r.status !== 'ok') throw new Error(`USB write: ${r.status}`);
  }

  async setSignals(signals) {
    Object.assign(this.signals, signals);
    const value = (this.signals.dataTerminalReady ? 1 : 0) | (this.signals.requestToSend ? 2 : 0);
    await this.control_(SET_CONTROL_LINE_STATE, value);
  }

  control_(request, value, data) {
    const setup = { requestType: 'class', recipient: 'interface', request, value, index: this.control.interfaceNumber };
    return this.device.controlTransferOut(setup, data);
  }

  // Readers and writers are released by their owners first (serial.js, esptool-js)
  async close() {
    this.readable_ = null;
    this.writable_ = null;
    if (this.device.opened) await this.device.close();   // also ends a pending read
  }

  forget() {
    return this.device.forget();
  }
}

// The navigator.serial calls that serial.js and app.js use
export const usbSerial = {
  async requestPort({ filters = [] } = {}) {
    const usbFilters = filters.map((f) => ({ vendorId: f.usbVendorId, classCode: CDC_CONTROL }));
    return new UsbSerialPort(await navigator.usb.requestDevice({ filters: usbFilters }));
  },
  async getPorts() {
    return (await navigator.usb.getDevices()).flatMap((d) => {
      try {
        return [new UsbSerialPort(d)];
      } catch {
        return [];   // not a CDC-ACM device
      }
    });
  },
};

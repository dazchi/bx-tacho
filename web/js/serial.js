// Web Serial link to BX Tacho's USB console (USB Serial/JTAG, so the baud rate is nominal).

export const serialSupported = () => 'serial' in navigator;

export const ESPRESSIF_VID = 0x303a;

export class SerialLink {
  constructor({ onData, onClose }) {
    this.cb = { onData, onClose };
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.reading = null;
    this.encoder = new TextEncoder();
  }

  get connected() {
    return !!this.writer;
  }

  // Asks the browser for a port (shows the picker), unless one is given
  async connect(port) {
    this.port = port || await navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF_VID }] });
    await this.open();
  }

  async open() {
    await this.port.open({ baudRate: 115200 });
    this.writer = this.port.writable.getWriter();
    this.reading = this.readLoop();
  }

  async readLoop() {
    while (this.port?.readable) {
      this.reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value) this.cb.onData?.(value);
        }
      } catch {
        break;   // device unplugged or reset
      } finally {
        this.reader.releaseLock();
        this.reader = null;
      }
      if (this.closing) break;
    }
    if (!this.closing) {
      await this.closePort();
      this.cb.onClose?.('lost');
    }
  }

  write(text) {
    if (!this.writer) return Promise.resolve();
    return this.writer.write(this.encoder.encode(text));
  }

  // Closes the port but keeps the permission, so it can be reopened (flashing does)
  async release() {
    this.closing = true;
    try {
      await this.reader?.cancel();
      await this.reading;
    } catch { /* already gone */ }
    await this.closePort();
    this.closing = false;
    return this.port;
  }

  async disconnect() {
    await this.release();
    this.cb.onClose?.('user');
  }

  async closePort() {
    try {
      this.writer?.releaseLock();
    } catch { /* ignore */ }
    this.writer = null;
    // The ESP32-C3's USB Serial/JTAG resets the chip while RTS is set and DTR
    // isn't. Closing the port drops DTR first, which passes through that state
    // and restarts BX Tacho, so drop RTS first, by itself.
    try {
      await this.port?.setSignals({ requestToSend: false });
      await this.port?.setSignals({ dataTerminalReady: false });
    } catch { /* not open, or already gone */ }
    try {
      await this.port?.close();
    } catch { /* already closed */ }
  }
}

// After a reset the USB device re-enumerates; find it again among granted ports
export async function findGrantedPort(timeoutMs = 8000) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    const ports = await navigator.serial.getPorts();
    const port = ports.find((p) => p.getInfo().usbVendorId === ESPRESSIF_VID);
    if (port) return port;
    await new Promise((r) => setTimeout(r, 300));
  }
  return null;
}

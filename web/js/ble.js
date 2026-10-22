// Web Bluetooth link to BX Tacho's Battle Pass service.
import { Assembler, CMD_HISTORY, CMD_STATUS, CMD_UUID, DATA_UUID, DEVICE_NAME, SERVICE_UUID } from './bbp.js';
import { CONSOLE_SERVICE } from './bleconsole.js';
import { OTA_SERVICE } from './bleota.js';

export const bleSupported = () => 'bluetooth' in navigator;

export class BleLink {
  constructor({ onStatus, onSet, onError, onDisconnect }) {
    this.cb = { onStatus, onSet, onError, onDisconnect };
    this.device = null;
    this.data = null;
    this.cmd = null;
    this.assembler = new Assembler();
    this.onValue = (e) => this.handle(e.target.value);
    this.onGone = () => this.cleanup(true);
  }

  get connected() {
    return !!(this.device && this.device.gatt.connected);
  }

  // For the console (bleconsole.js), which runs on the same link
  get server() {
    return this.connected ? this.device.gatt : null;
  }

  async connect() {
    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: DEVICE_NAME }],
      optionalServices: [SERVICE_UUID, CONSOLE_SERVICE, OTA_SERVICE],
    });
    this.device.addEventListener('gattserverdisconnected', this.onGone);
    const server = await this.device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE_UUID);
    this.data = await service.getCharacteristic(DATA_UUID);
    // Commands go to the write characteristic; BX Tacho also accepts them on the data one
    this.cmd = await service.getCharacteristic(CMD_UUID).catch(() => this.data);
    this.data.addEventListener('characteristicvaluechanged', this.onValue);
    await this.data.startNotifications();
    await this.send(CMD_STATUS);
    await this.send(CMD_HISTORY);
  }

  async send(cmd) {
    if (!this.cmd) return;
    const v = new Uint8Array([cmd]);
    if (this.cmd.writeValueWithoutResponse) await this.cmd.writeValueWithoutResponse(v);
    else await this.cmd.writeValue(v);
  }

  requestHistory() {
    return this.send(CMD_HISTORY);
  }

  handle(view) {
    const msg = this.assembler.feed(view);
    if (!msg) return;
    if (msg.type === 'status') this.cb.onStatus?.(msg);
    else if (msg.error) this.cb.onError?.(msg.error);
    else this.cb.onSet?.(msg);
  }

  async disconnect() {
    if (this.device?.gatt.connected) this.device.gatt.disconnect();
    else this.cleanup(true);
  }

  cleanup(notify) {
    this.data?.removeEventListener('characteristicvaluechanged', this.onValue);
    this.device?.removeEventListener('gattserverdisconnected', this.onGone);
    this.device = this.data = this.cmd = null;
    if (notify) this.cb.onDisconnect?.();
  }
}

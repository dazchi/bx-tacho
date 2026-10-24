// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
import { Terminal } from 'https://cdn.jsdelivr.net/npm/@xterm/xterm@6.0.0/lib/xterm.mjs';
import { FitAddon } from 'https://cdn.jsdelivr.net/npm/@xterm/addon-fit@0.11.0/lib/addon-fit.mjs';
import { LANGUAGES, browserLanguage, choice, fmt, setAttr, setLanguage, setPlain, setText, t } from './i18n.js';
import { curveToPoints, peakOf } from './bbp.js';
import { BleLink, bleSupported } from './ble.js';
import { BleConsole, readInfo } from './bleconsole.js';
import { BleUpdater, OtaError } from './bleota.js';
import { drawCurve, drawHistory } from './chart.js';
import { analyzeImage, flashFirmware, loadBootFiles } from './flasher.js';
import { drawPixelText, pixelWidth, revealPixelText } from './pixelfont.js';
import { ESPRESSIF_VID, SerialLink, requestBxPort, serialApi, serialSupported } from './serial.js';

const $ = (id) => document.getElementById(id);

const state = {
  link: null,            // 'usb' | 'ble' | null: only one at a time
  flashing: false,
  axis: 'laps',
  set: null,             // last shot set from BX Tacho
  status: null,          // last status packet
  consoleOpening: false, // console over Bluetooth: starting
  consoleUsed: false,    // ...opened before: its output stays on screen
  usbUsed: false,        // the USB console was connected before (same)
  flip: null,            // display turned (cfg flip): true/false, null while not known
  bfw: null,             // firmware to send over Bluetooth: { bytes, info }
  updating: false,       // ...being sent
  fw: null,              // { bytes, info, name }
  latest: null,          // the latest release on this site (firmware/latest/latest.json)
};

// ---------------------------------------------------------------- brand

const PIXEL = getComputedStyle(document.documentElement).getPropertyValue('--pixel').trim() || '#cfe8ff';
drawPixelText($('wordmark'), 'BX TACHO', { scale: 3, color: PIXEL });

// ---------------------------------------------------------------- tabs

const tabs = [$('tab-usb'), $('tab-ble')];

function selectTab(tab) {
  for (const each of tabs) {
    const on = each === tab;
    each.setAttribute('aria-selected', String(on));
    each.tabIndex = on ? 0 : -1;
    $(each.getAttribute('aria-controls')).hidden = !on;
  }
  if (tab.id === 'tab-usb') fitTerminal();
  else {
    drawReadout(false);
    fitBleTerminal();
  }
  history.replaceState(null, '', location.pathname + location.search + (tab.id === 'tab-ble' ? '#bluetooth' : '#usb'));
}

tabs.forEach((tab, i) => {
  tab.addEventListener('click', () => selectTab(tab));
  tab.addEventListener('keydown', (e) => {
    if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
    const next = tabs[(i + (e.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length];
    next.focus();
    selectTab(next);
  });
});

// ---------------------------------------------------------------- one link at a time

// A message under a heading, from a translation key; no key hides it
function note(id, key, params = {}, tone = 'warn') {
  const n = $(id);
  n.hidden = !key;
  n.dataset.tone = tone;
  if (key) setText(n, key, params);
}

function setLink(kind) {
  state.link = kind;
  const ls = $('link-state');
  setText(ls, kind === 'usb' ? 'link.usb' : kind === 'ble' ? 'link.ble' : 'link.none');
  ls.dataset.state = kind ? 'on' : 'off';

  const usbBtn = $('usb-connect');
  setText(usbBtn, kind === 'usb' ? 'usb.disconnect' : 'usb.connect');
  usbBtn.classList.toggle('btn--primary', kind !== 'usb');
  usbBtn.disabled = !serialSupported() || kind === 'ble' || state.flashing;

  const bleBtn = $('ble-connect');
  setText(bleBtn, kind === 'ble' ? 'ble.disconnect' : 'ble.connect');
  bleBtn.classList.toggle('btn--primary', kind !== 'ble');
  bleBtn.disabled = !bleSupported() || kind === 'usb';
  $('ble-refresh').disabled = kind !== 'ble';
  renderConsole();

  for (const c of document.querySelectorAll('.quick .chip[data-cmd]')) c.disabled = kind !== 'usb';
  for (const id of ['usb-input', 'usb-run', 'usb-stop']) $(id).disabled = kind !== 'usb';
  if (kind === 'usb') state.usbUsed = true;
  $('term-empty').hidden = state.usbUsed;   // once used, the output stays readable
  updateFlashButton();
  updateBleUpdateButton();
  if (!kind) state.flip = null;   // not known for the next connection
  renderFlip();

  if (!serialSupported()) {
    note('usb-note', 'note.noSerial', {}, 'bad');
  } else if (kind === 'ble') {
    note('usb-note', 'note.usbBlocked', {}, 'info');
  } else {
    note('usb-note', null);
  }
  if (!bleSupported()) {
    note('ble-note', 'note.noBle', {}, 'bad');
  } else if (kind === 'usb') {
    note('ble-note', 'note.bleBlocked', {}, 'info');
  } else if (!isDemo()) {
    note('ble-note', null);
  }
}

const isDemo = () => new URLSearchParams(location.search).has('demo');

// ---------------------------------------------------------------- terminal

const TERM_THEME = {
  background: '#000000',
  foreground: '#cfe8ff',
  cursor: '#cfe8ff',
  cursorAccent: '#000000',
  selectionBackground: '#2b54e088',
  black: '#000000',
  red: '#ff7a70',
  green: '#86e3a8',
  yellow: '#f2c96b',
  blue: '#8fb3ff',
  magenta: '#d9a6ff',
  cyan: '#8fe0e8',
  white: '#cfe8ff',
  brightBlack: '#7d97b0',
  brightRed: '#ff9b93',
  brightGreen: '#a8efc1',
  brightYellow: '#f7dc95',
  brightBlue: '#b5ccff',
  brightMagenta: '#e7c4ff',
  brightCyan: '#b4eef2',
  brightWhite: '#ffffff',
};
const TERM_FONT = { fontFamily: "'JetBrains Mono', ui-monospace, monospace", fontSize: 14, lineHeight: 1.25 };

// xterm.js scrolls with the mouse wheel but not with a finger: on a phone a
// drag over the terminal moved the page instead. A vertical drag scrolls the
// output while there's more of it that way; past the top or bottom (or with
// nothing to scroll) it moves the page as usual.
function touchScroll(t, el) {
  let lastY = null;
  let capture = null;   // decided on the first move of each drag
  let rest = 0;         // fraction of a row not scrolled yet
  el.addEventListener('touchstart', (e) => {
    lastY = e.touches.length === 1 ? e.touches[0].clientY : null;
    capture = null;
    rest = 0;
  }, { passive: true });
  el.addEventListener('touchmove', (e) => {
    if (lastY === null || e.touches.length !== 1) return;
    const y = e.touches[0].clientY;
    const dy = lastY - y;   // > 0: finger up, later output
    lastY = y;
    const buf = t.buffer.active;
    if (capture === null) {
      capture = dy > 0 ? buf.viewportY < buf.baseY : buf.viewportY > 0;
    }
    if (!capture) return;
    e.preventDefault();
    rest += dy / (el.clientHeight / t.rows);
    const n = Math.trunc(rest);
    if (n) {
      t.scrollLines(n);
      rest -= n;
    }
  }, { passive: false });
  el.addEventListener('touchend', () => {
    lastY = null;
  });
}

const term = new Terminal({ ...TERM_FONT, cursorBlink: true, scrollback: 5000, theme: TERM_THEME });
const fit = new FitAddon();
term.loadAddon(fit);
term.open($('terminal'));
touchScroll(term, $('terminal'));

function fitTerminal() {
  if (!$('panel-usb').hidden) {
    try {
      fit.fit();
    } catch { /* not laid out yet */ }
  }
}
new ResizeObserver(fitTerminal).observe($('terminal'));

const serial = new SerialLink({
  onData: (bytes) => term.write(bytes),
  onClose: (reason, why) => {
    if (state.flashing) return;
    setLink(null);
    if (reason === 'lost') term.write(`\r\n\x1b[90m${t('term.disconnected')}${why ? ` (${why})` : ''}\x1b[0m\r\n`);
  },
});

term.onData((d) => {
  if (state.link === 'usb') serial.write(d);
});

async function connectUsb() {
  if (state.link === 'usb') {
    await serial.disconnect();
    return;
  }
  try {
    await serial.connect();
    setLink('usb');
    term.write(`\x1b[90m${t('term.connected')}\x1b[0m\r\n`);
    await serial.write('\r');   // ask for a fresh prompt
    term.focus();
  } catch (e) {
    if (e.name !== 'NotFoundError') note('usb-note', 'err.portOpen', { msg: e.message }, 'bad');
  }
}

$('usb-connect').addEventListener('click', connectUsb);

for (const c of document.querySelectorAll('.quick .chip[data-cmd]')) {
  c.addEventListener('click', () => {
    serial.write(`${c.dataset.cmd}\r`);
    term.focus();
  });
}
// A command line under the terminal as well: typing into the terminal itself
// works on a computer, but a phone's keyboard doesn't reliably come up for it.
// BX Tacho echoes what it gets, so nothing is echoed here.
const usbHistory = [];
let usbHistoryAt = 0;

$('usb-form').addEventListener('submit', (e) => {
  e.preventDefault();
  const input = $('usb-input');
  const line = input.value.trim();
  if (!line || state.link !== 'usb') return;
  if (usbHistory.at(-1) !== line) usbHistory.push(line);
  usbHistoryAt = usbHistory.length;
  input.value = '';
  serial.write(`${line}\r`);
});

$('usb-input').addEventListener('keydown', (e) => {
  const input = e.target;
  if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && usbHistory.length) {
    e.preventDefault();
    usbHistoryAt = Math.max(0, Math.min(usbHistory.length, usbHistoryAt + (e.key === 'ArrowUp' ? -1 : 1)));
    input.value = usbHistory[usbHistoryAt] ?? '';
  } else if (e.key === 'c' && e.ctrlKey && input.selectionStart === input.selectionEnd) {
    e.preventDefault();   // Ctrl-C stops the running command (unless copying)
    serial.write('\x03');
  }
});

$('usb-stop').addEventListener('click', () => serial.write('\x03'));

$('term-clear').addEventListener('click', () => {
  term.clear();
  term.focus();
});

// ---------------------------------------------------------------- firmware

function kb(n) {
  return t('fw.kb', { n: fmt(Math.round(n / 1024)) });
}

async function sha256(bytes) {
  const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return [...hash].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Shows the SHA-256 of a chosen firmware file in `el`, to compare with the
// SHA256SUMS.txt of a release, or that it matches `release` (the latest one,
// already checked). A newer file chosen meanwhile wins.
async function showChecksum(el, bytes, release) {
  el.hidden = true;
  el.pending = bytes;
  const sum = await sha256(bytes);
  if (el.pending !== bytes) return;
  setText(el, release ? 'fw.sha256Release' : 'fw.sha256', { sum, v: release?.version });
  el.hidden = false;
}

// The firmware of the latest release, which the Pages workflow copies to this
// site (GitHub's own downloads can't be read from another site), with
// latest.json describing it. Without it (a local copy, no release yet) the
// buttons stay hidden.
async function findLatest() {
  try {
    const res = await fetch('firmware/latest/latest.json', { cache: 'no-cache' });
    if (!res.ok) return;
    const latest = await res.json();
    latest.file ??= 'bx-tacho.bin';   // before releases named the file after the version
    if (!latest.version || !/^[0-9a-f]{64}$/.test(latest.sha256) || !/^bx-tacho[\w.-]*\.bin$/.test(latest.file)) return;
    state.latest = latest;
  } catch {
    return;
  }
  for (const id of ['fw-latest', 'bota-latest']) {
    setText($(id), 'fw.latest', { v: state.latest.version });
    $(id).hidden = false;
  }
}

// Downloads the latest release's firmware and checks it against the
// release's checksum, then loads it like a chosen file. `prefix` is 'fw' or
// 'bota', the block whose button was used.
async function useLatest(prefix, load) {
  const btn = $(`${prefix}-latest`);
  const { version, file, sha256: want } = state.latest;
  btn.disabled = true;
  setText(btn, 'fw.latestLoading', { v: version });
  try {
    let bytes;
    try {
      const res = await fetch(`firmware/latest/${file}?sha256=${want}`);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      bytes = new Uint8Array(await res.arrayBuffer());
    } catch {
      return latestFailed(prefix, 'fw.latestFailed');
    }
    if ((await sha256(bytes)) !== want) return latestFailed(prefix, 'fw.latestBad');
    await load(new File([bytes], file), state.latest);
  } finally {
    btn.disabled = false;
    setText(btn, 'fw.latest', { v: version });
  }
}

// The latest release couldn't be used: says why in the file box, and drops
// any file chosen before so it isn't sent by mistake
function latestFailed(prefix, key) {
  if (prefix === 'fw') state.fw = null;
  else state.bfw = null;
  const box = $(`${prefix}-info`);
  box.hidden = false;
  box.dataset.kind = 'invalid';
  setPlain($(`${prefix}-name`), state.latest.file);
  setText($(`${prefix}-detail`), key);
  $(`${prefix}-sum`).hidden = true;
  $(`${prefix}-sum`).pending = null;
  $(`${prefix}-progress`).hidden = true;
  if (prefix === 'fw') {
    describeFirmware();
    updateFlashButton();
  } else {
    updateBleUpdateButton();
  }
}

$('fw-latest').addEventListener('click', () => useLatest('fw', loadFirmware));
$('bota-latest').addEventListener('click', () => useLatest('bota', loadBleFirmware));
findLatest();

async function loadFirmware(file, release) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const info = analyzeImage(bytes);
  state.fw = info.kind === 'invalid' ? null : { bytes, info, name: file.name };
  const box = $('fw-info');
  box.hidden = false;
  box.dataset.kind = info.kind;
  if (info.kind === 'invalid') {
    setPlain($('fw-name'), file.name);
    setText($('fw-detail'), info.reason, { project: info.project });
  } else {
    setText($('fw-name'), info.version ? 'fw.version' : 'fw.unknownVersion', { v: info.version });
  }
  describeFirmware();
  $('fw-progress').hidden = true;
  updateFlashButton();
  showChecksum($('fw-sum'), bytes, release);
}

// What the update will do, which depends on the erase option
function describeFirmware() {
  const erase = $('fw-erase').checked;
  $('fw-info').dataset.erase = String(erase && !!state.fw);
  setText($('fw-flash'), erase ? 'fw.eraseUpdate' : 'fw.update');
  $('fw-flash').classList.toggle('btn--danger', erase);
  if (!state.fw) return;
  setText($('fw-detail'), erase ? 'fw.eraseDetail' : 'fw.keep', { size: kb(state.fw.info.size) });
}

$('fw-erase').addEventListener('change', describeFirmware);

function updateFlashButton() {
  $('fw-flash').disabled = !state.fw || !serialSupported() || state.link === 'ble' || state.flashing;
}

$('fw-file').addEventListener('change', (e) => {
  const f = e.target.files[0];
  if (f) loadFirmware(f);
});

const drop = $('drop');
drop.addEventListener('dragover', (e) => {
  e.preventDefault();
  drop.dataset.over = 'true';
});
drop.addEventListener('dragleave', () => {
  drop.dataset.over = 'false';
});
drop.addEventListener('drop', (e) => {
  e.preventDefault();
  drop.dataset.over = 'false';
  const f = e.dataTransfer.files[0];
  if (f) loadFirmware(f);
});

function fwLog(line) {
  const log = $('fw-log');
  log.hidden = false;
  log.textContent += `${line.replace(/\r?\n$/, '')}\n`;
  log.scrollTop = log.scrollHeight;
}

// The progress bar `id`-progress with `id`-fill and `id`-text. Tone 'ok' marks
// the success message (green, bold), 'bad' a failure.
function progress(id, k, key, params = {}, tone = '') {
  $(`${id}-progress`).hidden = false;
  $(`${id}-fill`).style.width = `${Math.round(k * 100)}%`;
  const text = $(`${id}-text`);
  setText(text, key, params);
  text.dataset.tone = tone;
}

const fwProgress = (...args) => progress('fw', ...args);

// The chip resets after flashing and comes back as a new USB device
async function reconnectAfterFlash() {
  for (let i = 0; i < 20; i++) {
    await new Promise((r) => setTimeout(r, 500));
    const ports = await serialApi().getPorts();
    for (const p of ports.filter((x) => x.getInfo().usbVendorId === ESPRESSIF_VID)) {
      try {
        await serial.connect(p);
        return true;
      } catch { /* not back yet */ }
    }
  }
  return false;
}

async function flash() {
  const { bytes, info } = state.fw;
  const eraseAll = $('fw-erase').checked;
  if (eraseAll && !confirm(t('fw.confirmErase'))) return;

  let files;
  try {
    files = [...await loadBootFiles(), { data: bytes, address: info.address }];
  } catch {
    fwProgress(0, 'fw.bootFilesBad');
    return;
  }

  let port;
  try {
    port = state.link === 'usb' ? await serial.release()
      : await requestBxPort();
  } catch (e) {
    if (e.name !== 'NotFoundError') note('usb-note', 'err.portOpenShort', { msg: e.message }, 'bad');
    return;
  }

  state.flashing = true;
  setLink('usb');
  $('fw-log').textContent = '';
  term.write(`\r\n\x1b[90m${t('term.flashing')}\x1b[0m\r\n`);
  fwProgress(0, 'fw.p.connecting');
  try {
    await flashFirmware(port, files, {
      eraseAll,
      onProgress: (k, phase) => fwProgress(k, phase === 'erase' ? 'fw.p.erasing' : 'fw.p.writing', { pct: Math.round(k * 100) }),
      log: fwLog,
    });
    fwProgress(1, 'fw.p.restarting', {}, 'ok');
    state.flashing = false;
    const back = await reconnectAfterFlash();
    setLink(back ? 'usb' : null);
    fwProgress(1, back ? 'fw.p.reconnected' : 'fw.p.reconnect', {}, 'ok');
    if (back) {
      term.write(`\x1b[90m${t('term.connected')}\x1b[0m\r\n`);
      // Let the new firmware finish booting, then ask for a prompt
      await new Promise((r) => setTimeout(r, 1500));
      if (state.link === 'usb') await serial.write('\r');
    }
  } catch (e) {
    state.flashing = false;
    setLink(null);
    fwProgress(0, 'fw.p.failed', { msg: e.message });
  }
  updateFlashButton();
}

$('fw-flash').addEventListener('click', flash);

// ---------------------------------------------------------------- Bluetooth

const ble = new BleLink({
  onStatus: (s) => {
    state.status = s;
    renderStats();
  },
  onSet: (set) => {
    const isNew = !state.set || set.total !== state.set.total;
    state.set = set;
    renderSet(isNew);
  },
  onError: (key) => note('ble-note', key, {}, 'bad'),
  onDisconnect: () => {
    if (bcon.open) bleTerm.write(`\r\n\x1b[90m${t('term.disconnected')}\x1b[0m\r\n`);
    bcon.reset();
    closeCodeDialog();
    settleUnlock(new OtaError('bota.err.disconnected'));
    updater.close();
    setPlain($('bota-current'), '');
    $('ble-device').hidden = true;
    state.consoleOpening = false;
    setLink(null);
  },
});

async function connectBle() {
  if (state.link === 'ble') {
    await ble.disconnect();
    return;
  }
  try {
    note('ble-note', null);
    await ble.connect();
    setLink('ble');
    // What's on BX Tacho now, for the firmware update
    const info = await readInfo(ble.server);
    if (info && state.link === 'ble') {
      setText($('bota-current'), 'bota.current', { v: info.version });
      const line = $('ble-device');
      setText(line, info.address ? 'ble.device' : 'ble.deviceNoAddr', { addr: info.address, v: info.version });
      line.hidden = false;
      state.flip = info.flip;
      renderFlip();
    }
  } catch (e) {
    await ble.disconnect().catch(() => {});
    if (e.name !== 'NotFoundError') {
      note('ble-note', 'err.bleConnect', { msg: e.message }, 'bad');
    }
  }
}

$('ble-connect').addEventListener('click', connectBle);
$('ble-refresh').addEventListener('click', () => ble.requestHistory());

function drawReadout(animate) {
  const canvas = $('readout');
  const sp = state.set?.latestSp;
  const text = sp ? String(sp) : '----';
  const room = canvas.parentElement.clientWidth - 70;
  const scale = Math.max(4, Math.min(16, Math.floor(room / pixelWidth('88888'))));
  const opts = { scale, color: PIXEL, minChars: 4 };
  if (animate) revealPixelText(canvas, text, opts);
  else drawPixelText(canvas, text, opts);
  setAttr(canvas, 'aria-label', sp ? 'readout.aria' : 'readout.ariaEmpty', { sp: sp ? fmt(sp) : '' });
}

function renderStats() {
  const s = state.status;
  const set = state.set;
  const total = set?.total ?? s?.total;
  const max = Math.max(set?.maxSp ?? 0, s?.maxSp ?? 0);
  $('stat-total').textContent = total != null ? fmt(total) : '–';
  $('stat-max').textContent = max ? fmt(max) : '–';
  const batt = $('stat-batt');
  batt.textContent = s ? `${s.batteryPct}%` : '–';
  batt.dataset.tone = s && s.batteryPct <= 20 ? 'warn' : '';
  const top = $('stat-top');
  if (s) setText(top, s.attached ? 'stat.loaded' : 'stat.emptyTop');
  else setPlain(top, '–');
  top.dataset.tone = s?.attached ? 'ok' : '';
}

function renderCurve() {
  const pts = state.set?.points || [];
  drawCurve($('curve'), pts, state.axis);
  const peak = peakOf(pts);
  if (peak) setText($('readout-foot'), 'readout.peak', { lap: peak.lap, ms: Math.round(peak.tMs) });
  else setText($('readout-foot'), 'readout.empty');
}

function renderSet(isNew) {
  drawReadout(isNew);
  renderStats();
  renderCurve();
  const h = state.set?.history || [];
  drawHistory($('history'), h);
  if (h.length) setText($('hist-range'), 'hist.range', { min: fmt(Math.min(...h)), max: fmt(Math.max(...h)) });
  else setPlain($('hist-range'), '');
}

for (const b of document.querySelectorAll('.seg__btn')) {
  b.addEventListener('click', () => {
    state.axis = b.dataset.axis;
    for (const o of document.querySelectorAll('.seg__btn')) o.setAttribute('aria-pressed', String(o === b));
    renderCurve();
  });
}

new ResizeObserver(() => drawReadout(false)).observe($('readout').parentElement);

// ---------------------------------------------------------------- console over Bluetooth

// Output only: commands are typed in the line below it, which suits phones too
const bleTerm = new Terminal({
  ...TERM_FONT,
  convertEol: true,
  disableStdin: true,
  cursorBlink: false,
  cursorInactiveStyle: 'none',
  scrollback: 5000,
  theme: { ...TERM_THEME, cursor: '#000000' },
});
const bleFit = new FitAddon();
bleTerm.loadAddon(bleFit);
bleTerm.open($('ble-terminal'));
touchScroll(bleTerm, $('ble-terminal'));

function fitBleTerminal() {
  if (!$('panel-ble').hidden) {
    try {
      bleFit.fit();
    } catch { /* not laid out yet */ }
  }
}
new ResizeObserver(fitBleTerminal).observe($('ble-terminal'));

const bcon = new BleConsole({
  onText: (text) => bleTerm.write(text),
  onReady: () => {
    renderConsole();
    settleUnlock(null);
    if (codeDialog.open) {
      closeCodeDialog();
      $('bcon-input').focus();
    }
  },
  onCode: () => {
    renderConsole();
    askCode();
  },
});

const cmdHistory = [];
let historyAt = 0;

function renderConsole() {
  const linked = state.link === 'ble';
  const open = bcon.open;
  const openBtn = $('bcon-open');
  setText(openBtn, state.consoleOpening ? 'bcon.opening' : open ? 'bcon.close' : 'bcon.open');
  openBtn.disabled = !linked || state.consoleOpening;
  openBtn.classList.toggle('btn--primary', linked && !open);
  // Locked until the code from BX Tacho's display is entered (askCode)
  const ready = open && !bcon.locked;
  $('bcon-input').disabled = !ready;
  $('bcon-run').disabled = !ready || bcon.busy;
  $('bcon-stop').disabled = !ready || !bcon.busy;
  for (const c of document.querySelectorAll('.quick .chip[data-bcmd]')) c.disabled = !ready || bcon.busy;
  $('bcon-empty').hidden = open || state.consoleUsed;
  renderFlip();
}

// ---- the code dialog

const codeDialog = $('code-dialog');
let codeSent = false;   // a code went out: asked again means it didn't work

function askCode() {
  const input = $('code-input');
  note('code-error', codeSent ? 'bcon.codeWrong' : null, {}, 'bad');
  codeSent = false;
  input.value = '';
  updateCodeButton();
  if (!codeDialog.open) codeDialog.showModal();
  input.focus();
}

function closeCodeDialog() {
  if (codeDialog.open) codeDialog.close();
}

const codeDigits = () => $('code-input').value.replace(/\D/g, '');

function updateCodeButton() {
  $('code-unlock').disabled = codeDigits().length !== 6 || bcon.busy;
}

$('code-input').addEventListener('input', updateCodeButton);

$('code-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  if (codeDigits().length !== 6 || bcon.busy) return;
  try {
    codeSent = await bcon.run(codeDigits());
  } catch (err) {
    codeSent = false;
    note('code-error', 'bcon.openFailed', { msg: err.message }, 'bad');
  }
  updateCodeButton();
});

// Cancel, or Esc: no console then
function cancelCode() {
  closeCodeDialog();
  settleUnlock(new OtaError('bota.err.cancelled'));
  if (bcon.open) toggleConsole();
}
codeDialog.addEventListener('cancel', (e) => {
  e.preventDefault();
  cancelCode();
});
$('code-cancel').addEventListener('click', cancelCode);

async function toggleConsole() {
  if (bcon.open) {
    await bcon.close();
    bleTerm.write(`\x1b[90m${t('bcon.closedLine')}\x1b[0m\r\n`);
    renderConsole();
    return;
  }
  const server = ble.server;
  if (!server) return;
  state.consoleOpening = true;
  note('bcon-note', null);
  renderConsole();
  try {
    await bcon.start(server);
    state.consoleUsed = true;
  } catch (e) {
    bcon.reset();
    if (e.code === 'noConsole') note('bcon-note', 'bcon.noConsole', {}, 'bad');
    else if (state.link === 'ble') note('bcon-note', 'bcon.openFailed', { msg: e.message }, 'bad');
    settleUnlock(new OtaError(e.code === 'noConsole' ? 'bota.err.noService' : 'bota.err.other', e.message));
  }
  state.consoleOpening = false;
  renderConsole();
}

$('bcon-open').addEventListener('click', toggleConsole);

async function runCommand(line) {
  if (!bcon.open || bcon.busy || bcon.locked) return;
  bleTerm.write(`\x1b[94mbxt>\x1b[0m ${line}\r\n`);
  try {
    await bcon.run(line);
  } catch (e) {
    bleTerm.write(`\x1b[91m${e.message}\x1b[0m\r\n`);
  }
  renderConsole();
}

$('bcon-form').addEventListener('submit', (e) => {
  e.preventDefault();
  const input = $('bcon-input');
  const line = input.value.trim();
  if (!line || bcon.busy) return;
  if (cmdHistory.at(-1) !== line) cmdHistory.push(line);
  historyAt = cmdHistory.length;
  input.value = '';
  runCommand(line);
});

$('bcon-input').addEventListener('keydown', (e) => {
  const input = e.target;
  if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
    if (!cmdHistory.length) return;
    e.preventDefault();
    historyAt = Math.max(0, Math.min(cmdHistory.length, historyAt + (e.key === 'ArrowUp' ? -1 : 1)));
    input.value = cmdHistory[historyAt] ?? '';
  } else if (e.key === 'c' && e.ctrlKey && bcon.busy && input.selectionStart === input.selectionEnd) {
    e.preventDefault();   // Ctrl-C stops the command, as in a terminal (unless copying)
    bcon.stop().catch(() => {});
  }
});

$('bcon-stop').addEventListener('click', () => bcon.stop().catch(() => {}));

for (const c of document.querySelectorAll('.quick .chip[data-bcmd]')) {
  c.addEventListener('click', () => runCommand(c.dataset.bcmd));
}
$('bcon-clear').addEventListener('click', () => {
  bleTerm.clear();
  renderConsole();
});

// ---------------------------------------------------------------- display orientation

// The Display switch under each console: cfg flip on|off, through that console.
// Over Bluetooth BX Tacho reports the setting on connecting (readInfo); over
// USB it's known once chosen here.
function renderFlip() {
  for (const [id, kind] of [['usb-flip', 'usb'], ['ble-flip', 'ble']]) {
    for (const b of $(id).querySelectorAll('.seg__btn')) {
      b.setAttribute('aria-pressed', String(state.flip !== null && state.flip === (b.dataset.flip === 'on')));
      b.disabled = state.link !== kind || (kind === 'ble' && (bcon.busy || state.updating));
    }
  }
}

for (const b of $('usb-flip').querySelectorAll('.seg__btn')) {
  b.addEventListener('click', () => {
    if (state.link !== 'usb') return;
    serial.write(`cfg flip ${b.dataset.flip}\r`);
    state.flip = b.dataset.flip === 'on';
    renderFlip();
  });
}

for (const b of $('ble-flip').querySelectorAll('.seg__btn')) {
  b.addEventListener('click', async () => {
    try {
      await unlockConsole();   // the code on the display, if the console is locked
    } catch {
      return;
    }
    await runCommand(`cfg flip ${b.dataset.flip}`);
    state.flip = b.dataset.flip === 'on';
    renderFlip();
  });
}

// ---------------------------------------------------------------- firmware over Bluetooth

const updater = new BleUpdater();
let unlockWaiters = [];

// Resolves once the console is unlocked with the code on the display, opening
// it (and so the code dialog) if needed: BX Tacho only takes updates then
function unlockConsole() {
  if (bcon.open && !bcon.locked) return Promise.resolve();
  return new Promise((resolve, reject) => {
    unlockWaiters.push({ resolve, reject });
    if (!bcon.open && !state.consoleOpening) toggleConsole();
  });
}

function settleUnlock(err) {
  const waiters = unlockWaiters;
  unlockWaiters = [];
  for (const w of waiters) {
    if (err) w.reject(err);
    else w.resolve();
  }
}

const botaProgress = (...args) => progress('bota', ...args);

async function loadBleFirmware(file, release) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const info = analyzeImage(bytes);
  state.bfw = info.kind === 'invalid' ? null : { bytes, info };
  const box = $('bota-info');
  box.hidden = false;
  box.dataset.kind = info.kind;
  if (info.kind === 'invalid') {
    setPlain($('bota-name'), file.name);
    setText($('bota-detail'), info.reason, { project: info.project });
  } else {
    setText($('bota-name'), info.version ? 'fw.version' : 'fw.unknownVersion', { v: info.version });
    setText($('bota-detail'), 'fw.keep', { size: kb(info.size) });
  }
  $('bota-progress').hidden = true;
  updateBleUpdateButton();
  showChecksum($('bota-sum'), bytes, release);
}

function updateBleUpdateButton() {
  $('bota-update').disabled = !state.bfw || state.link !== 'ble' || state.updating;
}

$('bota-file').addEventListener('change', (e) => {
  const f = e.target.files[0];
  if (f) loadBleFirmware(f);
});

const botaDrop = $('bota-drop');
botaDrop.addEventListener('dragover', (e) => {
  e.preventDefault();
  botaDrop.dataset.over = 'true';
});
botaDrop.addEventListener('dragleave', () => {
  botaDrop.dataset.over = 'false';
});
botaDrop.addEventListener('drop', (e) => {
  e.preventDefault();
  botaDrop.dataset.over = 'false';
  const f = e.dataTransfer.files[0];
  if (f) loadBleFirmware(f);
});

const BOTA_PHASES = { erase: 'bota.p.erasing', write: 'bota.p.writing', check: 'bota.p.checking' };

async function updateOverBle() {
  state.updating = true;
  updateBleUpdateButton();
  try {
    botaProgress(0, 'bota.p.unlock');
    await unlockConsole();
    if (!updater.chr) await updater.open(ble.server);
    note('bota-warn', 'bota.warn');
    await updater.update(state.bfw.bytes, (k, phase) => botaProgress(k, BOTA_PHASES[phase], { pct: Math.round(k * 100) }));
    botaProgress(1, 'bota.p.done', {}, 'ok');
  } catch (e) {
    const err = e instanceof OtaError ? e : new OtaError('bota.err.other', e.message);
    botaProgress(0, err.key, { msg: err.message }, 'bad');
  }
  note('bota-warn', null);
  state.updating = false;
  updateBleUpdateButton();
}

// Closing or reloading the page in the middle drops the update: ask first
window.addEventListener('beforeunload', (e) => {
  if (state.updating) e.preventDefault();
});

$('bota-update').addEventListener('click', updateOverBle);

// ---------------------------------------------------------------- demo data

// A real launch recorded on BX Tacho (revolution periods in us), for ?demo
const DEMO_PERIODS = [20152, 14587, 12100, 10439, 9622, 8799, 8112, 8008, 7962, 8098, 8200, 8161,
  8390, 8447, 8530, 8702, 8729, 8790, 8891, 9090, 8949, 9172, 9150, 9286, 9264, 9398, 9409, 9463,
  9739, 9788, 9963, 9957];
const DEMO_HISTORY = [6522, 7143, 5999, 6629, 7957, 7723, 8245, 7855, 6301, 7012, 8450, 7390, 6944,
  7688, 8102, 7531, 6875, 7905, 8317, 7535];

function loadDemo() {
  state.set = {
    history: DEMO_HISTORY,
    latestSp: DEMO_HISTORY.at(-1),
    maxSp: 9490,
    total: 138,
    points: curveToPoints(DEMO_PERIODS.map((p) => Math.round(p / 8))),
  };
  state.status = { attached: false, batteryPct: 86, maxSp: 9490, total: 138 };
  renderSet(true);
  note('ble-note', 'note.demo', {}, 'info');
}

// ---------------------------------------------------------------- language

function fillLanguages() {
  const sel = $('lang-select');
  const auto = LANGUAGES.find((l) => l.code === browserLanguage()).name;
  sel.replaceChildren(
    new Option(t('lang.auto', { name: auto }), 'auto'),
    ...LANGUAGES.map((l) => new Option(l.name, l.code)),
  );
  sel.value = choice();
}

$('lang-select').addEventListener('change', (e) => setLanguage(e.target.value));

// Texts set from keys follow by themselves; redraw what is built from numbers
document.addEventListener('langchange', () => {
  fillLanguages();
  renderSet(false);
});

// ---------------------------------------------------------------- start

fillLanguages();

setLink(null);
renderSet(false);
if (isDemo()) loadDemo();
selectTab(location.hash === '#bluetooth' || isDemo() ? $('tab-ble') : $('tab-usb'));

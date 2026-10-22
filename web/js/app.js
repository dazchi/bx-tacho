import { Terminal } from 'https://cdn.jsdelivr.net/npm/@xterm/xterm@6.0.0/lib/xterm.mjs';
import { FitAddon } from 'https://cdn.jsdelivr.net/npm/@xterm/addon-fit@0.11.0/lib/addon-fit.mjs';
import { LANGUAGES, browserLanguage, choice, fmt, setAttr, setLanguage, setPlain, setText, t } from './i18n.js';
import { curveToPoints, peakOf } from './bbp.js';
import { BleLink, bleSupported } from './ble.js';
import { drawCurve, drawHistory } from './chart.js';
import { analyzeImage, flashFirmware, loadBootFiles } from './flasher.js';
import { drawPixelText, pixelWidth, revealPixelText } from './pixelfont.js';
import { ESPRESSIF_VID, SerialLink, serialSupported } from './serial.js';

const $ = (id) => document.getElementById(id);

const state = {
  link: null,            // 'usb' | 'ble' | null: only one at a time
  flashing: false,
  axis: 'laps',
  set: null,             // last shot set from BX Tacho
  status: null,          // last status packet
  fw: null,              // { bytes, info, name }
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
  else drawReadout(false);
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

  for (const c of document.querySelectorAll('.quick .chip[data-cmd]')) c.disabled = kind !== 'usb';
  $('term-empty').hidden = kind === 'usb';
  updateFlashButton();

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

const term = new Terminal({
  fontFamily: "'JetBrains Mono', ui-monospace, monospace",
  fontSize: 14,
  lineHeight: 1.25,
  cursorBlink: true,
  scrollback: 5000,
  theme: {
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
  },
});
const fit = new FitAddon();
term.loadAddon(fit);
term.open($('terminal'));

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
  onClose: (reason) => {
    if (state.flashing) return;
    setLink(null);
    if (reason === 'lost') term.write(`\r\n\x1b[90m${t('term.disconnected')}\x1b[0m\r\n`);
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
$('term-clear').addEventListener('click', () => {
  term.clear();
  term.focus();
});

// ---------------------------------------------------------------- firmware

function kb(n) {
  return t('fw.kb', { n: fmt(Math.round(n / 1024)) });
}

async function loadFirmware(file) {
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

// tone 'ok' marks the success message (green, bold)
function fwProgress(k, key, params = {}, tone = '') {
  $('fw-progress').hidden = false;
  $('fw-fill').style.width = `${Math.round(k * 100)}%`;
  const text = $('fw-text');
  setText(text, key, params);
  text.dataset.tone = tone;
}

// The chip resets after flashing and comes back as a new USB device
async function reconnectAfterFlash() {
  for (let i = 0; i < 20; i++) {
    await new Promise((r) => setTimeout(r, 500));
    const ports = await navigator.serial.getPorts();
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

  let files = [{ data: bytes, address: info.address }];
  if (eraseAll) {
    try {
      files = [...await loadBootFiles(), ...files];
    } catch {
      fwProgress(0, 'fw.bootFilesBad');
      return;
    }
  }

  let port;
  try {
    port = state.link === 'usb' ? await serial.release()
      : await navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF_VID }] });
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
  onDisconnect: () => setLink(null),
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

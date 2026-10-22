// Launch curve and history strip, drawn as plain SVG.
import { peakOf } from './bbp.js';
import { fmt, t } from './i18n.js';

const NS = 'http://www.w3.org/2000/svg';

function el(name, attrs = {}, parent) {
  const n = document.createElementNS(NS, name);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  parent?.appendChild(n);
  return n;
}

function niceStep(range, ticks) {
  const raw = range / ticks;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const n = raw / mag;
  return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
}

// points: [{lap, tMs, rpm}], mode: 'laps' | 'time'
export function drawCurve(svg, points, mode) {
  svg.replaceChildren();
  const W = 720;
  const H = 320;
  // Room on the left for the y labels and the y-axis title, at the bottom for the
  // x labels and the x-axis title
  const m = { l: 76, r: 16, t: 16, b: 56 };
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  if (!points.length) return;

  const xs = points.map((p) => (mode === 'time' ? p.tMs : p.lap));
  const xMin = mode === 'time' ? 0 : 1;
  const xMax = Math.max(...xs);
  const yMax = Math.max(...points.map((p) => p.rpm));
  const yStep = niceStep(yMax, 4);
  const yTop = Math.ceil(yMax / yStep) * yStep;
  const px = (x) => m.l + ((x - xMin) / Math.max(1, xMax - xMin)) * (W - m.l - m.r);
  const py = (y) => H - m.b - (y / yTop) * (H - m.t - m.b);

  const grid = el('g', { class: 'chart-grid' }, svg);
  for (let y = 0; y <= yTop; y += yStep) {
    el('line', { x1: m.l, x2: W - m.r, y1: py(y), y2: py(y) }, grid);
    el('text', { x: m.l - 8, y: py(y) + 4, 'text-anchor': 'end' }, grid).textContent = fmt(y);
  }
  const xStep = niceStep(xMax - xMin, 6);
  for (let x = Math.ceil(xMin / xStep) * xStep; x <= xMax; x += xStep) {
    el('text', { x: px(x), y: H - m.b + 20, 'text-anchor': 'middle' }, grid).textContent = fmt(x);
  }
  // Axis titles with their units: x centred under the plot, y centred beside it
  el('text', { x: (m.l + W - m.r) / 2, y: H - 8, 'text-anchor': 'middle', class: 'chart-axis' }, grid)
    .textContent = t(mode === 'time' ? 'curve.axisTime' : 'curve.axisLap');
  const yMid = (m.t + H - m.b) / 2;
  el('text', { x: 16, y: yMid, 'text-anchor': 'middle', class: 'chart-axis', transform: `rotate(-90 16 ${yMid})` }, grid)
    .textContent = t('curve.axisRpm');

  const line = points.map((p, i) => `${i ? 'L' : 'M'}${px(xs[i]).toFixed(1)},${py(p.rpm).toFixed(1)}`).join('');
  el('path', { d: `${line}L${px(xs.at(-1)).toFixed(1)},${py(0)}L${px(xs[0]).toFixed(1)},${py(0)}Z`, class: 'chart-area' }, svg);
  el('path', { d: line, class: 'chart-line' }, svg);
  const dots = el('g', { class: 'chart-dots' }, svg);
  points.forEach((p, i) => {
    const c = el('circle', { cx: px(xs[i]), cy: py(p.rpm), r: 3 }, dots);
    el('title', {}, c).textContent = t('curve.point', { lap: p.lap, rpm: fmt(p.rpm), ms: Math.round(p.tMs) });
  });

  const peak = peakOf(points);
  const i = points.indexOf(peak);
  el('circle', { cx: px(xs[i]), cy: py(peak.rpm), r: 6, class: 'chart-peak' }, svg);
}

// values: shoot powers, oldest first. The newest bar is marked.
export function drawHistory(container, values) {
  container.replaceChildren();
  const max = Math.max(1, ...values);
  values.forEach((v, i) => {
    const bar = document.createElement('li');
    bar.className = i === values.length - 1 ? 'bar bar--latest' : 'bar';
    bar.style.setProperty('--h', `${Math.max(2, (v / max) * 100)}%`);
    bar.tabIndex = 0;
    bar.title = `${fmt(v)} rpm`;
    bar.setAttribute('aria-label', t('hist.bar', { i: i + 1, n: values.length, v: fmt(v) }));
    container.appendChild(bar);
  });
}

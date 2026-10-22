// Pixel text in the BX Tacho OLED font: 6 px per character cell, 7 px tall.
import { FONT, FONT_FIRST } from './font5x7.js';

function glyph(ch) {
  let c = ch.toUpperCase().charCodeAt(0);
  if (c < FONT_FIRST || c >= FONT_FIRST + FONT.length) c = '?'.charCodeAt(0);
  return FONT[c - FONT_FIRST];
}

export function pixelWidth(text) {
  return text.length ? text.length * 6 - 1 : 0;
}

// Draws `text` onto `canvas`, sized to fit. `scale` is CSS px per OLED pixel.
// `reveal` (0..1) draws only that share of the columns, for the refresh effect.
export function drawPixelText(canvas, text, { scale = 4, color = '#cfe8ff', reveal = 1, minChars = 0 } = {}) {
  const cols = Math.max(pixelWidth(text), pixelWidth(' '.repeat(minChars)));
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, cols) * scale;
  const h = 7 * scale;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    canvas.style.width = `${w}px`;
    canvas.style.height = `${h}px`;
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = color;
  // A hairline gap between pixels, as on the real panel
  const gap = scale >= 3 ? Math.max(0.5, scale * 0.12) : 0;
  const shown = Math.ceil(cols * reveal);
  const x0 = (cols - pixelWidth(text)) * scale;   // right-aligned when padded
  for (let i = 0; i < text.length; i++) {
    const g = glyph(text[i]);
    for (let c = 0; c < 5; c++) {
      const col = (x0 / scale) + i * 6 + c;
      if (col >= shown) return;
      for (let b = 0; b < 7; b++) {
        if (g[c] & (1 << b)) ctx.fillRect(col * scale, b * scale, scale - gap, scale - gap);
      }
    }
  }
}

// Redraws column by column, like the OLED refreshing. Skipped with reduced motion.
export function revealPixelText(canvas, text, opts = {}) {
  const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (reduce) {
    drawPixelText(canvas, text, opts);
    return;
  }
  const start = performance.now();
  const dur = 260;
  const step = (now) => {
    const k = Math.min(1, (now - start) / dur);
    drawPixelText(canvas, text, { ...opts, reveal: k });
    if (k < 1) requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}

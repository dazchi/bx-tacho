// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// One load calibration for several launchers of a colour: from each
// launcher's empty and loaded readings, the empty / loaded levels that tell
// loaded from empty on all of them with the most room on both sides.
//
// BX Tacho counts a top as loaded above LOADED_PCT of the way from the empty
// to the loaded level, and released below RELEASED_PCT (firmware: load.c). The
// room is the gap W between the highest empty reading and the lowest loaded
// one; the setting splits it in thirds: a margin above the highest empty, the
// band between the two thresholds, a margin below the lowest loaded.

export const LOADED_PCT = 90;
export const RELEASED_PCT = 40;
export const MIN_CONTRAST_MV = 25;   // the firmware's LOAD_MIN_CONTRAST_MV (load.h)
const SPIN_FACTOR = 0.91;            // a spinning top is read with short flashes: about 91 % of the level
const MIN_MARGIN_MV = 5;             // the least margin a setting may leave on each side
const TIGHT_MARGIN_MV = 10;

// With the smallest range the firmware takes (MIN_CONTRAST_MV), centred in the
// room W, the thresholds sit THRESHOLDS_PCT_SUM % of it apart from both ends
// together: each margin is (W - MIN_CONTRAST_MV * that / 100) / 2 around them,
// and released - emax = (W - MIN_CONTRAST_MV * (LOADED_PCT - RELEASED_PCT) / 100) / 2.
const THRESHOLDS_PCT_SUM = RELEASED_PCT + LOADED_PCT;
// Below this room, the margins would be under MIN_MARGIN_MV
export const MIN_WINDOW_MV = Math.ceil(2 * MIN_MARGIN_MV + MIN_CONTRAST_MV * (LOADED_PCT - RELEASED_PCT) / 100);
// Below this room, a range of two thirds of it would be under MIN_CONTRAST_MV
const THIRDS_MIN_WINDOW_MV = Math.ceil(MIN_CONTRAST_MV * 3 / 2);

const thresholds = (empty, loaded) => ({
  released: Math.round(empty + (loaded - empty) * RELEASED_PCT / 100),
  loadedAt: Math.round(empty + (loaded - empty) * LOADED_PCT / 100),
});

// rows: [{ empty, loaded }] in mV (null: not measured yet). Returns null with
// no launcher fully measured, else one of:
//   { verdict: 'contrast', at }  launcher `at` (index) reads loaded too close to empty
//   { verdict: 'impossible', emax, emaxAt, lmin, lminAt, w }
//   { verdict: 'ok' | 'tight', empty, loaded, released, loadedAt, emax, emaxAt, lmin, lminAt, w,
//     marginReleased, marginLoaded, spinOk, single }
export function suggest(rows) {
  const done = rows.map((r, i) => ({ ...r, i })).filter((r) => Number.isFinite(r.empty) && Number.isFinite(r.loaded));
  if (!done.length) return null;
  const flat = done.find((r) => r.loaded - r.empty < MIN_CONTRAST_MV);
  if (flat) return { verdict: 'contrast', at: flat.i, diff: flat.loaded - flat.empty };

  const top = done.reduce((a, b) => (b.empty > a.empty ? b : a));
  const low = done.reduce((a, b) => (b.loaded < a.loaded ? b : a));
  const base = { emax: top.empty, emaxAt: top.i, lmin: low.loaded, lminAt: low.i, w: low.loaded - top.empty };

  let empty;
  let loaded;
  if (done.length === 1) {
    // One launcher: its own levels, as 'cal load' measures them
    ({ empty, loaded } = done[0]);
  } else if (base.w >= THIRDS_MIN_WINDOW_MV) {
    empty = Math.round(base.emax + base.w / 15);
    loaded = Math.round(empty + base.w * 2 / 3);
  } else if (base.w >= MIN_WINDOW_MV) {
    // The smallest range the firmware takes, its thresholds centred in the room there is
    empty = Math.round(base.emax + (base.w - MIN_CONTRAST_MV * THRESHOLDS_PCT_SUM / 100) / 2);
    loaded = empty + MIN_CONTRAST_MV;
  } else {
    return { verdict: 'impossible', ...base };
  }
  const { released, loadedAt } = thresholds(empty, loaded);
  const marginReleased = released - base.emax;
  const marginLoaded = base.lmin - loadedAt;
  return {
    verdict: Math.min(marginReleased, marginLoaded) < TIGHT_MARGIN_MV ? 'tight' : 'ok',
    empty, loaded, released, loadedAt, ...base, marginReleased, marginLoaded,
    spinOk: base.lmin * SPIN_FACTOR > released,
    single: done.length === 1,
  };
}

// Whether BX Tacho can still tell the two colours apart with these settings:
// it switches to white above white's loaded threshold, and to black below
// black's released one. Returns the problems found ([] if none).
export function crossCheck(blackRows, black, whiteRows, white) {
  const out = [];
  if (!black?.loadedAt || !white?.loadedAt) return out;
  const vals = (rows, key) => rows.map((r) => r[key]).filter(Number.isFinite);
  const blackMax = Math.max(...vals(blackRows, 'empty'), ...vals(blackRows, 'loaded'));
  if (blackMax >= white.loadedAt) out.push({ problem: 'blackLooksWhite', max: blackMax, thr: white.loadedAt });
  const whiteMin = Math.min(...vals(whiteRows, 'empty'));
  if (whiteMin <= black.released) out.push({ problem: 'whiteLooksBlack', min: whiteMin, thr: black.released });
  return out;
}

// The readings in the output of `load <n>`: "LOAD_SENSE (LED on - off) 123 mV"
export function parseLoadReadings(text) {
  return [...text.matchAll(/LOAD_SENSE \(LED on - off\) (-?\d+) mV/g)]
    .map((m) => Number(m[1]))
    .filter((v) => v > -100000);   // an ADC error prints INT_MIN
}

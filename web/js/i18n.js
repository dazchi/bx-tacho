// Translations. To add a language: create js/locales/<code>.js (copy en.js),
// add it to LANGUAGES below, and, if its code needs it, a rule in matchTag().
import en from './locales/en.js';
import zhHant from './locales/zh-Hant.js';
import zhHans from './locales/zh-Hans.js';
import ja from './locales/ja.js';

export const LANGUAGES = [
  { code: 'en', name: 'English', strings: en },
  { code: 'zh-Hant', name: '繁體中文', strings: zhHant },
  { code: 'zh-Hans', name: '简体中文', strings: zhHans },
  { code: 'ja', name: '日本語', strings: ja },
];

const STORAGE_KEY = 'bxt-lang';   // the viewer's own choice; absent = follow the browser

// A browser language tag (e.g. "zh-TW", "en-GB") to one of LANGUAGES, or null
function matchTag(tag) {
  const t = tag.toLowerCase();
  if (t === 'zh' || t.startsWith('zh-')) {
    // An explicit script wins; otherwise Traditional for Taiwan, Hong Kong, Macau
    if (/-hans\b/.test(t)) return 'zh-Hans';
    return /-(hant|tw|hk|mo)\b/.test(t) ? 'zh-Hant' : 'zh-Hans';
  }
  const exact = LANGUAGES.find((l) => l.code.toLowerCase() === t);
  if (exact) return exact.code;
  const base = LANGUAGES.find((l) => l.code.split('-')[0] === t.split('-')[0]);
  return base ? base.code : null;
}

export function browserLanguage() {
  const tags = navigator.languages?.length ? navigator.languages : [navigator.language || 'en'];
  for (const tag of tags) {
    const code = matchTag(tag);
    if (code) return code;
  }
  return 'en';
}

function savedChoice() {
  try {
    const v = localStorage.getItem(STORAGE_KEY);
    return LANGUAGES.some((l) => l.code === v) ? v : null;
  } catch {
    return null;
  }
}

let current = savedChoice() || browserLanguage();
let strings = LANGUAGES.find((l) => l.code === current).strings;

export const lang = () => current;
export const choice = () => savedChoice() || 'auto';

export function t(key, params = {}) {
  const s = strings[key] ?? en[key] ?? key;
  return s.replace(/\{(\w+)\}/g, (_, k) => (k in params ? String(params[k]) : `{${k}}`));
}

export function fmt(n) {
  return new Intl.NumberFormat(current).format(n);
}

// Elements whose text or attributes come from a key, so they follow a change
const bound = new Map();   // element -> { text?: [key, params], html?: [...], attrs: { name: [key, params] } }

function render(el, b) {
  if (b.text) el.textContent = t(...b.text);
  if (b.html) el.innerHTML = t(...b.html);   // only our own strings
  for (const [name, args] of Object.entries(b.attrs)) el.setAttribute(name, t(...args));
}

function binding(el) {
  if (!bound.has(el)) bound.set(el, { attrs: {} });
  return bound.get(el);
}

// Sets an element's text from a key and keeps it translated
export function setText(el, key, params = {}) {
  const b = binding(el);
  b.text = [key, params];
  delete b.html;
  render(el, b);
}

// Sets text that isn't translated (a file name, a number), dropping any key
export function setPlain(el, text) {
  const b = bound.get(el);
  if (b) {
    delete b.text;
    delete b.html;
  }
  el.textContent = text;
}

export function setAttr(el, name, key, params = {}) {
  const b = binding(el);
  b.attrs[name] = [key, params];
  render(el, b);
}

// Static markup: data-i18n="key", data-i18n-html="key", data-i18n-attr="aria-label:key;title:key"
function bindStatic() {
  for (const el of document.querySelectorAll('[data-i18n]')) binding(el).text = [el.dataset.i18n, {}];
  for (const el of document.querySelectorAll('[data-i18n-html]')) binding(el).html = [el.dataset.i18nHtml, {}];
  for (const el of document.querySelectorAll('[data-i18n-attr]')) {
    for (const pair of el.dataset.i18nAttr.split(';')) {
      const [name, key] = pair.split(':').map((s) => s.trim());
      if (name && key) binding(el).attrs[name] = [key, {}];
    }
  }
}

function applyAll() {
  document.documentElement.lang = current;
  for (const [el, b] of bound) render(el, b);
  document.querySelector('meta[name="description"]')?.setAttribute('content', t('meta.description'));
}

// code: a language code, or 'auto' to follow the browser again
export function setLanguage(code) {
  try {
    if (code === 'auto') localStorage.removeItem(STORAGE_KEY);
    else localStorage.setItem(STORAGE_KEY, code);
  } catch { /* storage blocked: applies for this visit only */ }
  current = code === 'auto' ? browserLanguage() : code;
  strings = LANGUAGES.find((l) => l.code === current).strings;
  applyAll();
  document.dispatchEvent(new CustomEvent('langchange', { detail: current }));
}

bindStatic();
applyAll();
// Follow the system when it changes, unless the viewer picked a language
window.addEventListener('languagechange', () => {
  if (!savedChoice()) setLanguage('auto');
});

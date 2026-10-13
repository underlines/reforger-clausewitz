// Clausewitz HQ dashboard i18n (design: dev/notes/i18n-design.md, 6a).
// Flat dictionaries i18n/<lang>.json ("key": "text with {0} params"), English is the reference and the fallback.
// t(key, ...params) -> text; static HTML uses data-i18n / data-i18n-title / data-i18n-placeholder (applied on load and on switch).
// More languages = one more i18n/<code>.json + its code in LANGS (and a "lang.<code>" label key in every dictionary).
// Only the dashboard chrome is translated; game data (SITREPs, chat, missions, logs) is shown as stored.
export const LANGS = ["en", "de"];
const KEY = "clw.lang";
const dicts = {};
const warned = new Set();
const subs = new Set();
let lang = "en";

function detect() {
  try { const s = localStorage.getItem(KEY); if (LANGS.includes(s)) return s; } catch { /* storage unavailable */ }
  const n = String((typeof navigator !== "undefined" && navigator.language) || "en").slice(0, 2).toLowerCase();
  return LANGS.includes(n) ? n : "en";
}
async function load(l) {
  if (dicts[l]) return;
  try {
    const r = await fetch(`/i18n/${l}.json`, { cache: "no-store" });
    dicts[l] = r.ok ? await r.json() : {};
  } catch { dicts[l] = {}; }
}
function warnOnce(key, what) {
  const k = what + key;
  if (warned.has(k)) return;
  warned.add(k);
  console.warn(`[i18n] ${what} "${key}"`);
}

export const getLang = () => lang;
export const has = (key) => dicts[lang]?.[key] !== undefined || dicts.en?.[key] !== undefined;
export function t(key, ...p) {
  let s = dicts[lang]?.[key];
  if (s === undefined) {
    s = dicts.en?.[key];
    if (s === undefined) { warnOnce(key, "unknown key"); return key; }
    if (lang !== "en") warnOnce(key, `missing in ${lang}:`);
  }
  return p.length ? s.replace(/\{(\d+)\}/g, (m, i) => (p[+i] === undefined ? m : String(p[+i]))) : s;
}
// enum value -> label when "<prefix>.<value>" exists, else the raw value (game-supplied words stay as stored)
export const tk = (prefix, v) => (v !== undefined && v !== null && has(prefix + "." + v) ? t(prefix + "." + v) : v);

// static HTML: data-i18n="key" (text), data-i18n-title="key", data-i18n-placeholder="key"
export function applyStatic(root = document) {
  for (const [attr, set] of [["data-i18n", (el, s) => { el.textContent = s; }], ["data-i18n-title", (el, s) => el.setAttribute("title", s)], ["data-i18n-placeholder", (el, s) => el.setAttribute("placeholder", s)]]) {
    for (const el of root.querySelectorAll(`[${attr}]`)) set(el, t(el.getAttribute(attr)));
  }
}
export function onLang(fn) { subs.add(fn); return () => subs.delete(fn); }
export async function setLang(l) {
  if (!LANGS.includes(l)) l = "en";
  await load(l);
  lang = l;
  try { localStorage.setItem(KEY, l); } catch { /* storage unavailable */ }
  document.documentElement.lang = l;
  applyStatic();
  for (const fn of subs) fn(l);
}

// initial load (top-level await: t() works synchronously in every module that imports this one)
lang = detect();
await load("en");
await load(lang);
document.documentElement.lang = lang;
if (typeof document !== "undefined") { if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", () => applyStatic()); else applyStatic(); }

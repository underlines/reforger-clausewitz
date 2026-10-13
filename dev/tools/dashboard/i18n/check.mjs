// Dashboard i18n check: node dev/tools/dashboard/i18n/check.mjs
// Fails (exit 1) when
//   - the dictionaries (every i18n/*.json) do not have the same key set as en.json, or a {n} placeholder set differs,
//   - a t('key') / tk('prefix', ...) call, a data-i18n* attribute or an indirect key (tab.* / col.* / use.* literals) in
//     app.js, journal.js, index.html has no entry in en.json.
// Warns about en.json keys nothing seems to use (keys under a tk() prefix count as used).
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const dash = path.join(here, "..");
const read = (f) => fs.readFileSync(f, "utf8");
const errors = [], warns = [];

const dicts = {};
for (const f of fs.readdirSync(here).filter((n) => n.endsWith(".json"))) {
  try { dicts[path.basename(f, ".json")] = JSON.parse(read(path.join(here, f))); } catch (e) { errors.push(`${f}: invalid JSON (${e.message})`); }
}
const en = dicts.en;
if (!en) { console.error("i18n/en.json missing"); process.exit(1); }
const params = (s) => [...new Set([...String(s).matchAll(/\{(\d+)\}/g)].map((m) => m[1]))].sort().join(",");

for (const [lang, d] of Object.entries(dicts)) {
  if (lang === "en") continue;
  for (const k of Object.keys(en)) if (!(k in d)) errors.push(`${lang}.json: missing key "${k}"`);
  for (const k of Object.keys(d)) if (!(k in en)) errors.push(`${lang}.json: extra key "${k}" (not in en.json)`);
  for (const k of Object.keys(en)) if (k in d && params(en[k]) !== params(d[k])) errors.push(`${lang}.json: placeholders of "${k}" differ from en.json ({${params(en[k])}} vs {${params(d[k])}})`);
}
for (const [lang, d] of Object.entries(dicts)) for (const [k, v] of Object.entries(d)) if (typeof v !== "string" || v === "") errors.push(`${lang}.json: "${k}" is not a non-empty string`);

const used = new Set(), prefixes = new Set();
for (const f of ["app.js", "journal.js", "index.html"]) {
  const src = read(path.join(dash, f));
  const add = (key, what) => { used.add(key); if (!(key in en)) errors.push(`${f}: ${what} "${key}" is not in en.json`); };
  for (const m of src.matchAll(/(?<![\w.$])t\(\s*(["'`])([^"'`$]+)\1\s*[,)]/g)) add(m[2], "t()");
  for (const m of src.matchAll(/(?<![\w.$])tk\(\s*(["'`])([^"'`$]+)\1\s*,/g)) { prefixes.add(m[2] + "."); if (!Object.keys(en).some((k) => k.startsWith(m[2] + "."))) errors.push(`${f}: tk("${m[2]}") has no "${m[2]}.*" keys in en.json`); }
  for (const m of src.matchAll(/data-i18n(?:-title|-placeholder)?="([^"]+)"/g)) add(m[1], "data-i18n");
  if (f.endsWith(".js")) for (const m of src.matchAll(/["']((?:tab|col)\.[A-Za-z0-9_]+|use\.(?:today|thisWeek|thisMonth))["']/g)) add(m[1], "indirect key");
}
for (const k of Object.keys(en)) if (!used.has(k) && ![...prefixes].some((p) => k.startsWith(p)) && !/^(param|state|layer|use\.reset|j\.status)\./.test(k)) warns.push(`unused en.json key "${k}"`);

for (const w of warns) console.warn("warn: " + w);
for (const e of errors) console.error("ERROR: " + e);
const n = Object.keys(en).length;
if (errors.length) { console.error(`i18n check FAILED (${errors.length} error(s))`); process.exit(1); }
console.log(`i18n check OK: ${Object.keys(dicts).join(", ")}, ${n} keys each${warns.length ? `, ${warns.length} warning(s)` : ""}`);

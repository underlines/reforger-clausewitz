// i18n helpers for the sidecar (protocol v4, design: dev/notes/i18n-design.md). Pure functions + the translator; no HTTP, no config file.
//   - language codes: ISO 639-1 (two letters, lower case), "auto" only as setLanguage
//   - structured /hq reply:  {"reply": "...", "setLanguage": "de"|"auto"}  (one strong-LLM call, no translation)
//   - broadcast translation: createTranslator(...).translateFields({headline, situation, ...}, ["de", "fr"]) -> one weak-LLM call per language,
//     run in parallel, cached by (sha1 of the canonical text, lang), timeout -> that language is omitted and reported in `failed`.
import crypto from "node:crypto";

export const LANG_RE = /^[a-z]{2}$/;
const MAX_LANGS = 8;

/** "DE" / " de " -> "de"; anything else -> null. */
export function normLang(x) {
  if (typeof x !== "string") return null;
  const t = x.trim().toLowerCase();
  return LANG_RE.test(t) ? t : null;
}
/** setLanguage from the model: a 2-letter code or "auto", else null (dropped). */
export function normSetLanguage(x) {
  if (typeof x !== "string") return null;
  const t = x.trim().toLowerCase();
  return t === "auto" || LANG_RE.test(t) ? t : null;
}
/** langs[] from the game: valid, distinct, non-English codes (canonical English is always delivered), max 8. */
export function normLangs(arr) {
  if (!Array.isArray(arr)) return [];
  const out = [];
  for (const x of arr) { const l = normLang(x); if (l && l !== "en" && !out.includes(l)) out.push(l); if (out.length >= MAX_LANGS) break; }
  return out;
}
export function langName(code) {
  try { return new Intl.DisplayNames(["en"], { type: "language" }).of(code) || code; } catch { return code; }
}

// ---------- structured /hq reply ----------
/** Prompt block for a v4 /hq message: reply language + setLanguage + output format. */
export function hqLanguageRules(lang) {
  const name = langName(lang);
  return `- Language: write the radio reply in ${name} (ISO 639-1 "${lang}"), whatever language the player writes in.
- If the player asks, in any wording or language, to be answered in some language (or to go back to the default), set "setLanguage" to that language's two-letter ISO 639-1 code (or "auto" for the default) and write the reply in that language. Only then; never otherwise.
- Your final text is ONE JSON object and nothing else: {"reply": "<radio reply>", "setLanguage": "<code or auto, only when asked>"}.`;
}
/** Parses the final model text of a v4 /hq turn -> { reply, setLanguage|null }. Plain text (no JSON) is taken as the reply. */
export function parseStructuredReply(text) {
  const raw = String(text ?? "").trim();
  const a = raw.indexOf("{"), b = raw.lastIndexOf("}");
  if (a >= 0 && b > a) {
    try {
      const o = JSON.parse(raw.slice(a, b + 1));
      if (o && typeof o === "object" && !Array.isArray(o) && typeof o.reply === "string") return { reply: o.reply.trim(), setLanguage: normSetLanguage(o.setLanguage) };
    } catch { /* fall through: plain text */ }
  }
  return { reply: raw, setLanguage: null };
}

// ---------- translation ----------
export const translateSystem = (lang) => `You are a translator for a military tactical-war game. Translate the string VALUES of the given JSON object from English into ${langName(lang)} (${lang}). Keep the keys unchanged.
Keep unchanged: grid references, ids, numbers, unit/faction/place names and proper nouns. Keep the military radio tone and the length. Do not add, explain or decide anything.
Reply with the translated JSON object only.`;
export const translateUser = (fields) => JSON.stringify(fields);

const sha1 = (s) => crypto.createHash("sha1").update(s).digest("hex");

/**
 * deps:
 *   callModel({system, userMsg, maxTokens, timeoutSec}) -> Promise<{raw}>   (the sidecar maps the call to the weak slot, or strong without one)
 *   isFake() -> bool        translate slot is the fake provider: "[<lang>] <text>", no call
 *   timeoutMs() -> number   per-language, per-attempt budget (default 10 s)
 *   attempts() -> number    model attempts per language (2 with a backup model, the budget is per attempt; default 1)
 *   cacheMax                entries kept (default 500)
 */
export function createTranslator({ callModel, isFake = () => false, timeoutMs = () => 10000, attempts = () => 1, cacheMax = 500, log = () => {} }) {
  const cache = new Map(); // `${sha1}|${lang}` -> translated fields
  const stats = { calls: 0, hits: 0, failures: 0 };
  async function one(fields, lang, key) {
    if (isFake()) return Object.fromEntries(Object.entries(fields).map(([k, v]) => [k, `[${lang}] ${v}`]));
    stats.calls++;
    const ms = timeoutMs();
    let timer;
    const limit = new Promise((_, rej) => { timer = setTimeout(() => rej(Object.assign(new Error("timeout"), { timeout: true })), ms * Math.max(1, attempts())); });
    try {
      const p = await Promise.race([callModel({ system: translateSystem(lang), userMsg: translateUser(fields), maxTokens: 1200, timeoutSec: ms / 1000 }), limit]);
      const raw = String(p?.raw ?? ""), a = raw.indexOf("{"), b = raw.lastIndexOf("}");
      if (a < 0 || b < a) throw new Error("no JSON in translation reply");
      const o = JSON.parse(raw.slice(a, b + 1));
      const out = {};
      let any = false;
      for (const k of Object.keys(fields)) { if (typeof o[k] === "string" && o[k].trim()) { out[k] = o[k].trim(); any = true; } else out[k] = fields[k]; }
      if (!any) throw new Error("translation reply had none of the fields");
      return out;
    } finally { clearTimeout(timer); }
  }
  /** fields: {name: english text}; langs: normalised non-English codes. -> {by_lang:{en, <lang>:fields}, failed:[{lang,reason}], cached:[lang]} */
  async function translateFields(fields, langs, { fresh = false } = {}) {
    const clean = {};
    for (const [k, v] of Object.entries(fields ?? {})) if (typeof v === "string" && v) clean[k] = v;
    const res = { by_lang: { en: clean }, failed: [], cached: [] };
    if (!Object.keys(clean).length) return res;
    const h = sha1(JSON.stringify(clean));
    await Promise.all(langs.map(async (lang) => {
      const key = h + "|" + lang;
      const hit = fresh ? null : cache.get(key);
      if (hit) { cache.delete(key); cache.set(key, hit); stats.hits++; res.cached.push(lang); res.by_lang[lang] = hit; return; }
      try {
        const out = await one(clean, lang, key);
        cache.set(key, out);
        if (cache.size > cacheMax) cache.delete(cache.keys().next().value);
        res.by_lang[lang] = out;
      } catch (e) {
        stats.failures++;
        const reason = e?.timeout || e?.name === "AbortError" ? "timeout" : String(e?.message ?? e).slice(0, 160);
        res.failed.push({ lang, reason });
        log(`translate ${lang} FAILED (${reason}): omitted, canonical English only`);
      }
    }));
    return res;
  }
  return { translateFields, stats, cacheSize: () => cache.size, clear: () => cache.clear() };
}

// i18n sidecar checks (protocol v4), fake provider, no API key, no game:   node dev/tools/test-i18n-sidecar.mjs
// Starts a second sidecar instance (own port + scratch config) and a stub OpenAI-compatible server that plays the "weak" LLM.
// Covers: v3 request unchanged, v4 lang reply, setLanguage validation, langs[] -> text_by_lang, cache hit, strong<->weak slot routing,
// weak failure / timeout -> language omitted + flagged in the exchange log, config API (strong/weak slots, backward compatible config).
import http from "node:http";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { manifest, situation } from "./agent/fixtures/hq_fixture.mjs";
import * as I18N from "./agent/i18n.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const PORT = 18791, STUB_PORT = 18792, BASE = `http://127.0.0.1:${PORT}`;
let pass = 0, fail = 0;
const ok = (cond, name, extra = "") => { if (cond) { pass++; console.log("  ok   " + name); } else { fail++; console.log("  FAIL " + name + (extra ? "  -> " + extra : "")); } };
const eq = (a, b, name) => ok(JSON.stringify(a) === JSON.stringify(b), name, `got ${JSON.stringify(a)} want ${JSON.stringify(b)}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------- 1. pure helpers ----------
console.log("helpers");
eq(I18N.normLang(" DE "), "de", "normLang lower-cases and trims");
eq([I18N.normLang("deu"), I18N.normLang("d"), I18N.normLang("d1"), I18N.normLang(5), I18N.normLang("auto")], [null, null, null, null, null], "normLang rejects non ^[a-z]{2}$");
eq([I18N.normSetLanguage("JA"), I18N.normSetLanguage("auto"), I18N.normSetLanguage("xyz"), I18N.normSetLanguage("j"), I18N.normSetLanguage("1a"), I18N.normSetLanguage(null), I18N.normSetLanguage({})], ["ja", "auto", null, null, null, null, null], "setLanguage validation (^[a-z]{2}$ | auto, else dropped)");
eq(I18N.normLangs(["de", "DE", "en", "fr", "xx1", 3, "es"]), ["de", "fr", "es"], "normLangs: distinct, valid, no en");
eq(I18N.parseStructuredReply('{"reply":"Roger.","setLanguage":"Klingon"}'), { reply: "Roger.", setLanguage: null }, "structured reply: invalid setLanguage dropped");
eq(I18N.parseStructuredReply('Sure: {"reply":"Roger.","setLanguage":"ja"} done'), { reply: "Roger.", setLanguage: "ja" }, "structured reply: JSON embedded in text");
eq(I18N.parseStructuredReply("Roger, on it."), { reply: "Roger, on it.", setLanguage: null }, "structured reply: plain text is the reply");

// ---------- 2. translator unit: cache + timeout (stub model, no HTTP) ----------
console.log("translator");
let calls = 0;
const tr = I18N.createTranslator({ callModel: async ({ userMsg }) => { calls++; const o = JSON.parse(userMsg); return { raw: JSON.stringify(Object.fromEntries(Object.entries(o).map(([k, v]) => [k, "~" + v]))) }; }, timeoutMs: () => 200 });
const f1 = { headline: "We hold A.", enemy: "None." };
const r1 = await tr.translateFields(f1, ["de", "fr"]);
eq([calls, r1.by_lang.de.headline, r1.by_lang.en.headline, r1.cached], [2, "~We hold A.", "We hold A.", []], "one call per language, en canonical");
const r2 = await tr.translateFields(f1, ["de", "fr"]);
eq([calls, r2.cached.sort(), r2.by_lang.fr.enemy], [2, ["de", "fr"], "~None."], "cache hit by (sha1 text, lang): no new call");
const r3 = await tr.translateFields({ ...f1, headline: "Changed." }, ["de"]);
eq([calls, r3.cached], [3, []], "changed text misses the cache");
const slow = I18N.createTranslator({ callModel: ({ userMsg }) => new Promise((res) => { if (!/slow/.test(userMsg)) res({ raw: userMsg }); }), timeoutMs: () => 80 });
const t0 = Date.now();
const r4 = await slow.translateFields({ headline: "slow text" }, ["de"]);
eq([r4.failed.map((x) => x.reason), Object.keys(r4.by_lang)], [["timeout"], ["en"]], "timeout -> language omitted, flagged");
ok(Date.now() - t0 < 1000, "timeout does not hang");

// ---------- stub weak LLM (OpenAI-compatible) ----------
let stubMode = "ok", stubCalls = 0;
const stub = http.createServer((req, res) => {
  let body = "";
  req.on("data", (d) => (body += d));
  req.on("end", () => {
    stubCalls++;
    if (stubMode === "hang") return; // never answers
    if (stubMode === "error") { res.writeHead(500); return res.end("boom"); }
    const j = JSON.parse(body);
    const lang = /\((\w{2})\)/.exec(j.messages[0].content)?.[1] ?? "??";
    const o = JSON.parse(j.messages[1].content);
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ choices: [{ message: { content: JSON.stringify(Object.fromEntries(Object.entries(o).map(([k, v]) => [k, `<${lang}> ${v}`]))) } }], usage: { prompt_tokens: 1, completion_tokens: 1 } }));
  });
}).listen(STUB_PORT, "127.0.0.1");

// ---------- sidecar instance ----------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-i18n-"));
const cfgFile = path.join(tmp, "cfg.json");
fs.writeFileSync(cfgFile, JSON.stringify({ provider: "fake", model: "x", timeoutSec: 20 })); // an OLD single-provider config: no weak, no i18n group
const proc = spawn(process.execPath, [path.join(DIR, "llm_sidecar.mjs")], { env: { ...process.env, CLW_PORT: String(PORT), CLW_LLM: "fake", CLW_CONFIG: cfgFile }, stdio: ["ignore", "pipe", "pipe"] });
let log = "";
proc.stdout.on("data", (d) => (log += d)); proc.stderr.on("data", (d) => (log += d));
const post = async (p, body) => { const r = await fetch(BASE + p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }); return { status: r.status, json: await r.json() }; };
const get = async (p) => (await fetch(BASE + p)).json();
async function up() { for (let i = 0; i < 50; i++) { try { if ((await fetch(BASE + "/health")).ok) return true; } catch {} await sleep(100); } return false; }

let msgId = 100;
// Plays the game: re-sends the pending hq message every 200 ms until the reply carries hqReplies.
async function hq(text, lang, faction = "US") {
  const msg = { id: ++msgId, player: "Alice", role: "soldier", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text, ageSec: 1, ...(lang ? { lang } : {}) };
  for (let i = 0; i < 50; i++) {
    const { json } = await post("/orders", { ...situation, v: lang ? 4 : 3, faction, manifest: manifest.hash, hq: [msg] });
    if (json.hqReplies?.length) return json.hqReplies[0];
    await sleep(200);
  }
  return null;
}
const sitrepCall = (faction, extra = {}) => post("/orders", { ...situation, v: 4, faction, manifest: manifest.hash, hq: [], ...extra });

try {
  ok(await up(), "sidecar started");
  await post("/manifest", manifest);

  console.log("config (old single-provider config)");
  let c = await get("/api/config");
  eq([c.provider, c.strong.provider, c.weak.provider, c.translateSlot, c.i18n.translateTimeoutSec], ["fake", "fake", "none", "strong", 10], "old config loads: top-level = strong, no weak -> strong translates");
  ok(!JSON.stringify(c).includes('"apiKey"'), "no apiKey in the public config");
  eq((await post("/api/config", { weak: { provider: "bogus" } })).status, 400, "weak.provider validated");
  eq((await post("/api/config", { weak: { timeoutSec: 0 } })).status, 400, "weak.timeoutSec validated");
  eq((await post("/api/config", { weak: { nope: 1 } })).status, 400, "unknown weak field rejected");

  console.log("v3 unchanged");
  const v3 = await hq("HQ, we need a truck at the camp", null);
  ok(v3 && /^Roger, HQ is working on your request/.test(v3.text), "v3 /hq reply has no language marker", JSON.stringify(v3));
  ok(v3 && !("setLanguage" in v3), "v3 reply has no setLanguage");
  const o3 = await sitrepCall("NATO", { v: 3 });
  ok(o3.json.sitrep && !o3.json.sitrep.text_by_lang, "v3 /orders sitrep has no text_by_lang", JSON.stringify(Object.keys(o3.json.sitrep ?? {})));

  console.log("v4 lang reply + setLanguage");
  const v4 = await hq("HQ, we need a truck at the camp", "de");
  ok(v4 && v4.text.startsWith("[de] Roger"), "v4 lang=de -> reply written in de (fake marker)", JSON.stringify(v4));
  ok(v4 && !("setLanguage" in v4), "no setLanguage unless the player asked");
  const en = await hq("HQ, we need a truck at the camp", "en");
  ok(en && !en.text.startsWith("["), "v4 lang=en -> plain reply", JSON.stringify(en));
  const sl = await hq("please respond in Japanese from now on", "de");
  ok(sl && sl.setLanguage === "ja" && sl.text.startsWith("[ja] "), "setLanguage ja + reply already in ja", JSON.stringify(sl));
  const sa = await hq("please answer in the default language again", "ja");
  ok(sa && sa.setLanguage === "auto" && sa.text.startsWith("[ja] "), "setLanguage auto accepted", JSON.stringify(sa));
  const st0 = await get("/api/state");
  const lg = st0.hq.find((h) => h.msg.text.includes("Japanese"));
  eq([lg?.lang, lg?.setLanguage], ["de", "ja"], "exchange log records lang + setLanguage");

  console.log("langs[] -> text_by_lang (fake provider)");
  const o4 = await sitrepCall("USSR", { langs: ["de", "FR", "en", "xx1", "de"] });
  const tbl = o4.json.sitrep?.text_by_lang;
  eq(Object.keys(tbl ?? {}).sort(), ["de", "en", "fr"], "text_by_lang has en + each valid non-en language once");
  ok(tbl && tbl.de.situation === "[de] " + tbl.en.situation && tbl.fr.headline === "[fr] " + tbl.en.headline, "fake translation = [<lang>] <text>, en canonical");
  ok(tbl && o4.json.sitrep.situation === tbl.en.situation, "sitrep fields stay the canonical English");
  const ex = (await get("/api/state")).exchanges.find((e) => e.faction === "USSR");
  eq([ex?.i18n?.ok.sort(), ex?.i18n?.failed, ex?.i18n?.slot], [["de", "fr"], [], "strong"], "exchange log has the i18n block");

  console.log("cache");
  const t1 = await post("/translate", { texts: { msg: "Enemy armor at the bridge." }, langs: ["de", "fr"] });
  const t2 = await post("/translate", { texts: { msg: "Enemy armor at the bridge." }, langs: ["de", "fr"] });
  eq([t1.json.cached, t2.json.cached.sort(), t2.json.text_by_lang.de.msg], [[], ["de", "fr"], "[de] Enemy armor at the bridge."], "second identical /translate is served from the cache");

  console.log("strong <-> weak routing");
  const tt = await post("/api/test", { slot: "translate" });
  eq([tt.json.ok, tt.json.slot, tt.json.text], [true, "strong", "[de] Enemy infantry sighted near the bridge."], "no weak slot: strong translates");
  const w = await post("/api/config", { weak: { provider: "openai-compatible", model: "weak-model", baseUrl: `http://127.0.0.1:${STUB_PORT}`, apiKey: "sk-weaksecret1234", timeoutSec: 5 }, i18n: { translateTimeoutSec: 1 } });
  eq([w.status, w.json.config.translateSlot, w.json.config.weak.apiKeySet, w.json.config.weak.apiKeyLast4, "apiKey" in w.json.config.weak], [200, "weak", true, "1234", false], "weak slot saved, key never returned");
  ok(!JSON.stringify(w.json).includes("sk-weaksecret"), "weak key not leaked");
  stubCalls = 0;
  const tw = await post("/translate", { texts: { msg: "Weak routed text." }, langs: ["de"] });
  eq([tw.json.text_by_lang.de.msg, stubCalls], ["<de> Weak routed text.", 1], "translate goes to the weak slot");
  stubCalls = 0;
  const hqw = await hq("HQ, we need a truck at the camp", "de");
  ok(hqw && hqw.text.startsWith("[de] ") && stubCalls === 0, "/hq (role hq) stays on the strong slot, never the weak one", `stubCalls=${stubCalls}`);

  console.log("weak failure / timeout -> language omitted, flagged");
  stubMode = "error";
  const te = await post("/translate", { texts: { msg: "Will fail with 500." }, langs: ["de"] });
  ok(te.json.failed.length === 1 && !te.json.text_by_lang.de && te.json.text_by_lang.en.msg, "weak HTTP error -> de omitted, en kept", JSON.stringify(te.json));
  stubMode = "hang";
  const t00 = Date.now();
  const o5 = await sitrepCall("EU", { langs: ["de"], objectives: [{ id: "Z", name: "Zulu Ridge", pos: [800, 800], owner: "EU" }] });
  const dt = Date.now() - t00;
  const ex5 = (await get("/api/state")).exchanges.find((e) => e.faction === "EU");
  ok(o5.json.sitrep && Object.keys(o5.json.sitrep.text_by_lang).join() === "en", "timeout -> text_by_lang only has en", JSON.stringify(Object.keys(o5.json.sitrep?.text_by_lang ?? {})));
  eq(ex5?.i18n?.failed.map((x) => x.lang + ":" + x.reason), ["de:timeout"], "timeout flagged in the exchange log");
  ok(dt < 4000, "orders reply returned after the translate timeout, not hung", dt + " ms");
  stubMode = "ok";

  console.log("weak removed -> strong again");
  const wn = await post("/api/config", { weak: null });
  eq(wn.json.config.translateSlot, "strong", "weak: null clears the slot");
  const tn = await post("/translate", { texts: { msg: "Back on strong." }, langs: ["de"] });
  eq(tn.json.text_by_lang.de.msg, "[de] Back on strong.", "fallback to strong translates");
} catch (e) {
  fail++; console.log("  FAIL exception: " + (e?.stack ?? e));
} finally {
  proc.kill(); stub.close();
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) { console.log("--- sidecar log tail ---\n" + log.split("\n").slice(-25).join("\n")); process.exit(1); }
process.exit(0);

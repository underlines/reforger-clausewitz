// Backup (fallback) model checks, no API key, no game:   node dev/tools/test-fallback.mjs
// Starts a sidecar instance (own port + scratch config + short cooldown) and a stub OpenAI-compatible server whose behaviour is chosen per model.
// Covers: retryable failures (429 / 5xx / hang / network) fall back once, 400/401 and invalid output do not, per-(slot, model) cooldown,
// weak slot, HQ agent turn, exchange log + counters, config API round trip (key never leaked, key inheritance), /api/test slot+which.
import http from "node:http";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { loginFetch } from "./test-login.mjs"; // dashboard API needs the admin session
import { fileURLToPath } from "node:url";
import { manifest, situation } from "./agent/fixtures/hq_fixture.mjs";
import * as FB from "./agent/fallback.mjs";
import { createFallbackModel } from "./agent/agentTurn.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const PORT = 18793, STUB_PORT = 18794, BASE = `http://127.0.0.1:${PORT}`, STUB = `http://127.0.0.1:${STUB_PORT}`;
const COOL_MS = 1500;
let pass = 0, fail = 0;
const ok = (cond, name, extra = "") => { if (cond) { pass++; console.log("  ok   " + name); } else { fail++; console.log("  FAIL " + name + (extra ? "  -> " + extra : "")); } };
const eq = (a, b, name) => ok(JSON.stringify(a) === JSON.stringify(b), name, `got ${JSON.stringify(a)} want ${JSON.stringify(b)}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------- 1. pure helpers ----------
console.log("helpers");
const err = (o) => Object.assign(new Error(o.message ?? "x"), o);
eq([429, 500, 502, 503, 408].map((s) => FB.isRetryable(err({ status: s }))), [true, true, true, true, true], "429 / 5xx / 408 are retryable");
eq([400, 401, 403, 404].map((s) => FB.isRetryable(err({ status: s }))), [false, false, false, false], "400 / 401 / 403 / 404 are not");
eq([FB.isRetryable(err({ statusCode: 429 })), FB.isRetryable(err({ lastError: { statusCode: 503 } })), FB.isRetryable(err({ name: "AbortError" })), FB.isRetryable(err({ name: "TimeoutError" })), FB.isRetryable(err({ cooling: true }))], [true, true, true, true, true], "SDK status shapes, timeouts, cooling down");
eq([FB.isRetryable(Object.assign(new TypeError("fetch failed"), { cause: { code: "ECONNRESET" } })), FB.isRetryable(new Error("no JSON object in reply")), FB.isRetryable(new Error("API key not set")), FB.isRetryable(null)], [true, false, false, false], "network error yes; invalid output / config error / null no");
const base = { provider: "openrouter", model: "free/m:free", baseUrl: "", apiKey: "sk-main-1234", timeoutSec: 20, fallback: { model: "x" }, weak: {} };
let rf = FB.resolveFallback(base, { model: "paid/m" });
eq([rf.model, rf.provider, rf.apiKey, rf.timeoutSec, "fallback" in rf, "weak" in rf], ["paid/m", "openrouter", "sk-main-1234", 20, false, false], "fallback inherits provider / key / timeout from the slot");
rf = FB.resolveFallback(base, { model: "m", baseUrl: "https://other.example/v1" });
eq(rf.apiKey, "", "main key is NOT sent to a different baseUrl");
rf = FB.resolveFallback(base, { model: "m", provider: "anthropic" });
eq(rf.apiKey, "", "main key is NOT sent to a different provider");
rf = FB.resolveFallback(base, { model: "m", baseUrl: "https://other.example/v1", apiKey: "sk-own", timeoutSec: 5 });
eq([rf.apiKey, rf.timeoutSec, rf.baseUrl], ["sk-own", 5, "https://other.example/v1"], "own key / timeout / baseUrl win");
eq([FB.resolveFallback(base, null), FB.resolveFallback(base, { model: "" }), FB.resolveFallback(base, { model: "free/m:free" })], [null, null, null], "no model / identical to the primary = no fallback");
eq(FB.publicFallback({ model: "m", apiKey: "sk-secret-abcd" }).apiKeyLast4, "abcd", "publicFallback shows last4 only");
ok(!JSON.stringify(FB.publicFallback({ model: "m", apiKey: "sk-secret-abcd" })).includes("sk-secret"), "publicFallback never has the key");
eq([FB.cooldownFor(err({ status: 429 }), 100, false), FB.cooldownFor(err({ name: "AbortError" }), 100, false), FB.cooldownFor(err({ name: "AbortError" }), 100, true), FB.cooldownFor(err({ status: 400 }), 100, true)], [100, 0, 100, 0], "cooldown: 429 always, timeout only with a backup, 400 never");

// agent loop model wrapper: primary fails on step 2 -> backup for the rest, step 1 stays
console.log("agent model wrapper");
{
  const log = [];
  const mk = (name, failAt = []) => { let n = 0; return { modelId: name, specificationVersion: "v4", doGenerate: async () => { n++; log.push(name + n); if (failAt.includes(n)) throw err({ status: 429, message: "rate limited" }); return { text: name }; }, doStream: async () => ({ stream: name }) }; };
  const w = createFallbackModel(mk("P", [2]), mk("B"), { names: { primary: "P", backup: "B" } });
  const a = await w.model.doGenerate({}), b = await w.model.doGenerate({}), c = await w.model.doGenerate({});
  eq([a.text, b.text, c.text, log], ["P", "B", "B", ["P1", "P2", "B1", "B2"]], "step 1 on primary, failed step 2 and the rest on the backup (sticky)");
  eq([w.state.fellBack, w.state.modelUsed, /rate limited/.test(w.state.primaryError), w.model.modelId], [true, "B", true, "P"], "state reports fellBack / modelUsed / primaryError");
  const w2 = createFallbackModel(mk("P", [1]), mk("B"), { names: { primary: "P", backup: "B" } });
  let threw = null; try { await w2.model.doGenerate({ abortSignal: AbortSignal.abort() }); } catch (e) { threw = e; }
  eq(threw?.status, 429, "a cancelled call (abortSignal) is not retried on the backup");
  const w3 = createFallbackModel({ ...mk("P"), doGenerate: async () => { throw err({ status: 400 }); } }, mk("B"), {});
  let t3 = null; try { await w3.model.doGenerate({}); } catch (e) { t3 = e; }
  eq(t3?.status, 400, "400 does not fall back");
  const w4 = createFallbackModel(mk("P"), mk("B"), { primaryCooling: () => true, names: { primary: "P", backup: "B" } });
  eq([(await w4.model.doGenerate({})).text, w4.state.fellBack], ["B", true], "primary cooling down: straight to the backup");
}

// ---------- stub provider (OpenAI-compatible); behaviour per model ----------
const modes = {}; // model -> "ok" | "429" | "500" | "400" | "401" | "hang" | "net" | "garbage"
const calls = []; // {model, auth, kind}
const stub = http.createServer((req, res) => {
  let body = "";
  req.on("data", (d) => (body += d));
  req.on("end", () => {
    const j = JSON.parse(body || "{}");
    const sys = j.messages?.[0]?.content ?? "", usr = j.messages?.[1]?.content ?? j.messages?.at(-1)?.content ?? "";
    const kind = /translator for a military/.test(body) ? "translate" : /HQ radio operator/.test(body) ? "hq" : "commander";
    calls.push({ model: j.model, auth: req.headers.authorization ?? null, kind });
    const mode = modes[j.model] ?? "ok";
    if (mode === "hang") return;
    if (mode === "net") return req.socket.destroy();
    if (/^\d+$/.test(mode)) { res.writeHead(Number(mode), { "content-type": "application/json" }); return res.end(JSON.stringify({ error: { message: "stub " + mode } })); }
    let content;
    if (mode === "garbage") content = "I cannot help with that.";
    else if (kind === "translate") { const lang = /\((\w{2})\)/.exec(sys)?.[1] ?? "??"; content = JSON.stringify(Object.fromEntries(Object.entries(JSON.parse(usr)).map(([k, v]) => [k, `<${j.model}:${lang}> ${v}`]))); }
    else if (kind === "hq") content = JSON.stringify({ reply: "Roger from " + j.model });
    else content = /"v":/.test(usr) ? JSON.stringify({ keep: true }) : JSON.stringify({ orders: [] }); // the sidecar's own /api/test situation has no protocol version
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ id: "x", object: "chat.completion", created: 1, model: j.model, choices: [{ index: 0, message: { role: "assistant", content }, finish_reason: "stop" }], usage: { prompt_tokens: 3, completion_tokens: 2, total_tokens: 5 } }));
  });
}).listen(STUB_PORT, "127.0.0.1");

// ---------- sidecar instance ----------
const MAIN_KEY = "sk-mainsecret9999";
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-fb-"));
const cfgFile = path.join(tmp, "cfg.json");
fs.writeFileSync(cfgFile, JSON.stringify({ provider: "openai-compatible", model: "primary-m", baseUrl: STUB, apiKey: MAIN_KEY, timeoutSec: 1, memory: { enabled: false }, sitrep: { enabled: false } })); // an OLD config (memory/sitrep off: no background journal calls to count): no fallback anywhere
const proc = spawn(process.execPath, [path.join(DIR, "llm_sidecar.mjs")], { env: { ...process.env, CLW_PORT: String(PORT), CLW_LLM: "fake", CLW_CONFIG: cfgFile, CLW_DATA_DIR: path.join(tmp, "data"), CLW_RATE_COOLDOWN_MS: String(COOL_MS) }, stdio: ["ignore", "pipe", "pipe"] });
let log = "";
proc.stdout.on("data", (d) => (log += d)); proc.stderr.on("data", (d) => (log += d));
const post = async (p, body) => { const r = await fetch(BASE + p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }); return { status: r.status, json: await r.json() }; };
const getText = async (p) => (await fetch(BASE + p)).text();
const get = async (p) => JSON.parse(await getText(p));
async function up() { for (let i = 0; i < 50; i++) { try { if ((await fetch(BASE + "/health")).ok) return true; } catch {} await sleep(100); } return false; }
let fn = 0;
const newFaction = () => "F" + ++fn;
const callsFor = (from) => calls.slice(from).map((c) => c.model);
// One commander call (the faction's first request always calls the LLM). Returns the exchange log entry.
async function commander(faction = newFaction()) {
  const r = await post("/orders", { ...situation, v: 3, faction, manifest: manifest.hash, hq: [] });
  const ex = (await get("/api/state")).exchanges.find((e) => e.faction === faction);
  return { reply: r.json, ex };
}
let msgId = 500;
async function hq(text, faction = newFaction()) {
  const msg = { id: ++msgId, player: "Alice", role: "soldier", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text, ageSec: 1 };
  for (let i = 0; i < 60; i++) {
    const { json } = await post("/orders", { ...situation, v: 3, faction, manifest: manifest.hash, hq: [msg] });
    if (json.hqReplies?.length) return (await get("/api/state")).hq.find((h) => h.msg.id === msg.id);
    await sleep(200);
  }
  return null;
}
const counters = async () => (await get("/api/state")).counters.modelFallbacks;

try {
  ok(await up(), "sidecar started");
  await loginFetch(BASE);
  await post("/manifest", manifest);

  console.log("no fallback configured: unchanged");
  let c = await get("/api/config");
  eq([c.fallback, c.strong.fallback, c.weak.fallback], [null, null, null], "old config loads: no fallback anywhere");
  modes["primary-m"] = "429";
  let from = calls.length;
  let r = await commander();
  eq([r.ex.ok, r.ex.fellBack, r.ex.modelUsed, r.ex.primaryError, /429/.test(r.ex.reason)], [false, false, "primary-m", null, true], "429 without a backup: plain failure, as before", JSON.stringify(r.ex.reason));
  eq(callsFor(from), ["primary-m"], "only the primary was called");
  r = await commander();
  ok(!r.ex.ok && /cooling down/.test(r.ex.reason), "old cooldown still applies without a backup", r.ex.reason);
  eq((await counters()), { strong: 0, weak: 0 }, "no model fallbacks counted");
  await sleep(COOL_MS + 200);

  console.log("config API round trip");
  let w = await post("/api/config", { fallback: { model: "backup-m" } });
  eq([w.status, w.json.config.fallback.model, w.json.config.fallback.apiKeySet, w.json.config.strong.fallback.model, w.json.config.weak.fallback], [200, "backup-m", false, "backup-m", null], "top-level fallback = strong fallback; only model set");
  w = await post("/api/config", { strong: { fallback: { baseUrl: `http://localhost:${STUB_PORT}`, apiKey: "sk-backupsecret5678", timeoutSec: 7, provider: "openai-compatible" } } });
  eq([w.status, w.json.config.fallback.model, w.json.config.fallback.baseUrl, w.json.config.fallback.timeoutSec, w.json.config.fallback.apiKeyLast4, "apiKey" in w.json.config.fallback], [200, "backup-m", `http://localhost:${STUB_PORT}`, 7, "5678", false], "strong.fallback partial update merges, key only as last4");
  w = await post("/api/config", { fallback: { apiKey: "" } });
  eq(w.json.config.fallback.apiKeyLast4, "5678", "empty apiKey keeps the stored key");
  ok(![JSON.stringify(w.json), await getText("/api/config"), await getText("/api/state")].some((t) => t.includes("sk-backupsecret") || t.includes("sk-mainsecret")), "no key in /api/config, /api/state or the POST reply");
  eq((await post("/api/config", { fallback: {} })).status, 200, "empty partial update on an existing fallback is fine");
  eq([(await post("/api/config", { fallback: { model: "x", provider: "bogus" } })).status, (await post("/api/config", { fallback: { timeoutSec: 0 } })).status, (await post("/api/config", { fallback: { nope: 1 } })).status, (await post("/api/config", { fallback: { baseUrl: "ftp://x" } })).status, (await post("/api/config", { fallback: 5 })).status], [400, 400, 400, 400, 400], "validated like the primary (provider, timeout, unknown field, baseUrl, type)");
  w = await post("/api/config", { fallback: null });
  eq([w.json.config.fallback, w.json.config.strong.fallback], [null, null], "fallback: null clears");
  eq((await post("/api/config", { fallback: { provider: "openrouter" } })).status, 400, "a new fallback needs a model");
  w = await post("/api/config", { strong: { fallback: { model: "backup-m" } }, weak: { provider: "openai-compatible", model: "weak-m", baseUrl: STUB, apiKey: "sk-weaksecret1111", timeoutSec: 1, fallback: { model: "weak-b" } } });
  eq([w.json.config.weak.fallback.model, w.json.config.fallback.model, w.json.config.translateSlot], ["weak-b", "backup-m", "weak"], "weak.fallback and strong.fallback set together");
  const saved = JSON.parse(fs.readFileSync(cfgFile, "utf8"));
  eq([saved.fallback?.model, saved.weak?.fallback?.model], ["backup-m", "weak-b"], "persisted to the config file");

  console.log("primary 429 -> backup answers (key inherited)");
  modes["primary-m"] = "429"; modes["backup-m"] = "ok";
  from = calls.length;
  r = await commander();
  eq([r.ex.ok, r.ex.fellBack, r.ex.modelUsed, r.ex.model, /429/.test(r.ex.primaryError)], [true, true, "backup-m", "primary-m", true], "exchange: modelUsed backup, fellBack, primaryError has the 429", JSON.stringify([r.ex.reason, r.ex.primaryError]));
  eq(callsFor(from), ["primary-m", "backup-m"], "primary tried once, then the backup");
  eq(calls.slice(from).map((x) => x.auth), ["Bearer " + MAIN_KEY, "Bearer " + MAIN_KEY], "backup with only a model inherits the slot key");
  eq((await counters()).strong, 1, "counter modelFallbacks.strong = 1");
  eq(r.reply.keep, true, "the game still gets a normal reply");

  console.log("primary cooling down -> straight to the backup, then recovery");
  from = calls.length;
  r = await commander();
  eq([r.ex.ok, r.ex.fellBack, r.ex.modelUsed, /cooling down/.test(r.ex.primaryError)], [true, true, "backup-m", true], "cooling primary skipped without a call");
  eq(callsFor(from), ["backup-m"], "only the backup was called while the primary cools down");
  eq(Object.keys((await get("/api/state")).modelCooldowns), ["strong|primary-m"], "/api/state modelCooldowns lists (slot|model)");
  modes["primary-m"] = "ok";
  await sleep(COOL_MS + 200);
  from = calls.length;
  r = await commander();
  eq([r.ex.fellBack, r.ex.modelUsed, callsFor(from)], [false, "primary-m", ["primary-m"]], "cooldown over: the primary answers again");

  console.log("primary hangs -> backup");
  modes["primary-m"] = "hang";
  from = calls.length;
  const t0 = Date.now();
  r = await commander();
  const dt = Date.now() - t0;
  eq([r.ex.ok, r.ex.fellBack, r.ex.modelUsed, callsFor(from)], [true, true, "backup-m", ["primary-m", "backup-m"]], "timeout (timeoutSec 1) falls back", r.ex.primaryError);
  ok(dt >= 900 && dt < 5000, "took about one timeout, not hung", dt + " ms");
  r = await commander();
  ok(r.ex.fellBack && /cooling down/.test(r.ex.primaryError), "a timed-out primary also cools down (with a backup configured)", r.ex.primaryError);
  await sleep(COOL_MS + 200);

  console.log("5xx and network errors fall back");
  for (const m of ["500", "503", "net"]) {
    modes["primary-m"] = m; from = calls.length;
    r = await commander();
    eq([m, r.ex.ok, r.ex.fellBack, r.ex.modelUsed], [m, true, true, "backup-m"], `primary ${m} -> backup`, r.ex.primaryError);
    await sleep(COOL_MS + 200);
  }

  console.log("non-retryable: no fallback");
  for (const m of ["400", "401", "garbage"]) {
    modes["primary-m"] = m; from = calls.length;
    r = await commander();
    eq([m, r.ex.ok, r.ex.fellBack, callsFor(from)], [m, false, false, ["primary-m"]], `primary ${m} -> failure, backup never called`, r.ex.reason);
  }

  console.log("both fail");
  modes["primary-m"] = "429"; modes["backup-m"] = "500";
  r = await commander();
  eq([r.ex.ok, r.ex.fellBack, /429/.test(r.ex.primaryError), /500/.test(r.ex.reason)], [false, true, true, true], "primary 429 + backup 500: failure, both errors in the log", JSON.stringify([r.ex.reason, r.ex.primaryError]));
  r = await commander();
  ok(!r.ex.ok && /cooling down/.test(r.ex.reason), "both cooling: plain 'cooling down' failure (rule commander takes over)", r.ex.reason);
  modes["primary-m"] = "ok"; modes["backup-m"] = "ok";
  await sleep(COOL_MS + 200);

  console.log("cooldown is per (slot, model)");
  modes["primary-m"] = "429";
  await commander(); // cools primary-m
  const cd = (await get("/api/state")).modelCooldowns;
  ok("strong|primary-m" in cd && !("strong|backup-m" in cd), "a rate-limited primary does not cool the backup", JSON.stringify(cd));
  modes["primary-m"] = "ok";
  await sleep(COOL_MS + 200);

  console.log("weak slot (translate)");
  modes["weak-m"] = "429"; modes["weak-b"] = "ok";
  from = calls.length;
  const f0 = await counters();
  let tw = await post("/translate", { texts: { msg: "Weak routed text." }, langs: ["de"] });
  eq([tw.json.text_by_lang.de?.msg, tw.json.failed.length, callsFor(from)], ["<weak-b:de> Weak routed text.", 0, ["weak-m", "weak-b"]], "weak 429 -> weak backup translates", JSON.stringify(tw.json));
  eq([(await counters()).weak - f0.weak, (await counters()).strong - f0.strong], [1, 0], "counter modelFallbacks.weak +1, strong untouched");
  from = calls.length;
  tw = await post("/translate", { texts: { msg: "Second weak text." }, langs: ["de"] });
  eq(callsFor(from), ["weak-b"], "weak primary cooling: straight to the weak backup");
  from = calls.length;
  r = await commander();
  eq([r.ex.fellBack, callsFor(from)], [false, ["primary-m"]], "the strong slot is not affected by the weak cooldown");
  modes["weak-m"] = "ok";
  await sleep(COOL_MS + 200);
  tw = await post("/translate", { texts: { msg: "Third weak text." }, langs: ["de"] });
  eq(tw.json.text_by_lang.de?.msg, "<weak-m:de> Third weak text.", "weak primary recovers");
  // a slow weak primary must still leave the backup time inside the translate budget
  modes["weak-m"] = "hang";
  await post("/api/config", { i18n: { translateTimeoutSec: 1 } });
  const t1 = Date.now();
  tw = await post("/translate", { texts: { msg: "Hanging weak text." }, langs: ["de"] });
  eq([tw.json.text_by_lang.de?.msg, tw.json.failed.length], ["<weak-b:de> Hanging weak text.", 0], "hanging weak primary -> weak backup within the budget", JSON.stringify(tw.json));
  ok(Date.now() - t1 < 4500, "...in about one attempt timeout", Date.now() - t1 + " ms");
  modes["weak-m"] = "ok";
  await sleep(COOL_MS + 200);
  // weak without a backup: unchanged behaviour (failure -> language omitted)
  await post("/api/config", { weak: { fallback: null } });
  modes["weak-m"] = "500";
  tw = await post("/translate", { texts: { msg: "No weak backup." }, langs: ["de"] });
  ok(tw.json.failed.length === 1 && !tw.json.text_by_lang.de, "weak without a backup: language omitted as before", JSON.stringify(tw.json));
  modes["weak-m"] = "ok";
  await post("/api/config", { weak: { fallback: { model: "weak-b" } } });

  console.log("HQ agent turn (AI SDK path)");
  modes["primary-m"] = "429"; modes["backup-m"] = "ok";
  await sleep(COOL_MS + 200);
  const h0 = (await counters()).strong;
  const hqe = await hq("hello HQ, radio check");
  ok(hqe && hqe.ok && hqe.fellBack && hqe.modelUsed === "backup-m" && /429/.test(hqe.primaryError ?? ""), "hq: primary 429 -> backup answered, logged", JSON.stringify(hqe && { ok: hqe.ok, error: hqe.error, fellBack: hqe.fellBack, modelUsed: hqe.modelUsed, primaryError: hqe.primaryError }));
  ok(hqe && /Roger from backup-m/.test(hqe.reply), "hq reply came from the backup model", hqe?.reply);
  eq((await counters()).strong - h0, 2, "hq fallback counted (the hq turn + the faction's first commander call in the same POST)");
  eq(calls.filter((x) => x.kind === "hq").slice(0, 2).map((x) => x.model), ["primary-m", "backup-m"], "the hq turn itself went primary -> backup");
  const hq2 = await hq("another radio check");
  ok(hq2 && hq2.ok && hq2.fellBack && /cooling down/.test(hq2.primaryError ?? ""), "hq: primary cooling -> straight to the backup", JSON.stringify(hq2 && { error: hq2.error, primaryError: hq2.primaryError }));
  modes["primary-m"] = "400";
  await sleep(COOL_MS + 200);
  const hq3 = await hq("third radio check");
  ok(hq3 && !hq3.ok && !hq3.fellBack, "hq: 400 does not fall back", JSON.stringify(hq3 && { ok: hq3.ok, error: hq3.error }));
  modes["primary-m"] = "ok";

  console.log("/api/test slot + which");
  modes["primary-m"] = "429"; modes["backup-m"] = "ok"; modes["weak-m"] = "429"; modes["weak-b"] = "ok";
  let tp = await post("/api/test", { slot: "strong", which: "primary" });
  eq([tp.json.ok, tp.json.which, tp.json.model, /429/.test(tp.json.error)], [false, "primary", "primary-m", true], "test primary: fails on its own, no fallback routing", JSON.stringify(tp.json));
  let tb = await post("/api/test", { slot: "strong", which: "fallback" });
  eq([tb.json.ok, tb.json.which, tb.json.model, tb.json.orders !== undefined], [true, "fallback", "backup-m", true], "test fallback: ok on its own", JSON.stringify(tb.json));
  tp = await post("/api/test", {});
  eq([tp.json.ok, tp.json.which, tp.json.slot], [false, "primary", "strong"], "no body = strong primary, as before");
  let tt = await post("/api/test", { slot: "translate", which: "primary" });
  eq([tt.json.ok, tt.json.model, tt.json.slot, tt.json.which], [false, "weak-m", "weak", "primary"], "translate primary tested alone", JSON.stringify(tt.json));
  tt = await post("/api/test", { slot: "weak", which: "fallback" });
  eq([tt.json.ok, tt.json.model, tt.json.text], [true, "weak-b", "<weak-b:de> Enemy infantry sighted near the bridge."], "weak fallback tested alone", JSON.stringify(tt.json));
  // overrides in the body are used for the test only
  tb = await post("/api/test", { slot: "strong", which: "fallback", fallback: { model: "other-b" } });
  eq([tb.json.model], ["other-b"], "body override of the fallback applies to the test only");
  eq((await get("/api/config")).fallback.model, "backup-m", "...and is not saved");
  eq([(await post("/api/test", { slot: "bogus" })).status, (await post("/api/test", { which: "x" })).status], [400, 400], "bad slot / which rejected");
  await post("/api/config", { fallback: null, weak: { fallback: null } });
  eq([(await post("/api/test", { slot: "strong", which: "fallback" })).status, (await post("/api/test", { slot: "weak", which: "fallback" })).status], [400, 400], "which=fallback without a fallback -> 400");
  await post("/api/config", { weak: null });
  eq((await post("/api/test", { slot: "weak" })).status, 400, "slot=weak without a weak slot -> 400");
  modes["primary-m"] = "ok";
  tt = await post("/api/test", { slot: "translate" });
  eq([tt.json.ok, tt.json.slot, tt.json.model], [true, "strong", "primary-m"], "slot=translate without a weak slot tests strong");
} catch (e) {
  fail++; console.log("  FAIL exception: " + (e?.stack ?? e));
} finally {
  proc.kill(); stub.close(); stub.closeAllConnections?.();
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) { console.log("--- sidecar log tail ---\n" + log.split("\n").slice(-30).join("\n")); process.exit(1); }
process.exit(0);

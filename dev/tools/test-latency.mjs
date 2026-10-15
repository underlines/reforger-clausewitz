// LLM-latency work (B1) checks, no API key, no game:   node dev/tools/test-latency.mjs
// Starts two sidecar instances (own ports, scratch config/data) and a mock OpenAI-compatible server that streams SSE.
//  1. fake provider: the SITREP rides on the same reply, GET /api/state has llmCalls + latency, exchanges carry metrics, config groups are validated
//  2. provider "openai-compatible" -> mock: TTFT / tokens / cached tokens in llmCalls, terse reply -> valid orders, response_format per output.mode,
//     system prompt without a SITREP request, the SITREP is its own call (weak slot role) delivered on a LATER gated keep reply exactly once,
//     or on the current reply when it finished first.
import http from "node:http";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { loginFetch } from "./test-login.mjs";
import { manifest, situation } from "./agent/fixtures/hq_fixture.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const FAKE_PORT = 18795, LLM_PORT = 18796, MOCK_PORT = 18797;
const FAKE = `http://127.0.0.1:${FAKE_PORT}`, REAL = `http://127.0.0.1:${LLM_PORT}`, MOCK = `http://127.0.0.1:${MOCK_PORT}`;
let pass = 0, fail = 0;
const ok = (cond, name, extra = "") => { if (cond) { pass++; console.log("  ok   " + name); } else { fail++; console.log("  FAIL " + name + (extra ? "  -> " + extra : "")); } };
const eq = (a, b, name) => ok(JSON.stringify(a) === JSON.stringify(b), name, `got ${JSON.stringify(a)} want ${JSON.stringify(b)}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------- mock OpenAI-compatible server (SSE) ----------
const SITREP_JSON = { headline: "We hold the crossing.", situation: "We hold Highway Crossing and the enemy presses it.", enemy: "Squad-sized element near the crossing, just now.", mission: "Hold the crossing.", execution: "Main effort: the rifle element holds the crossing.", logistics: "Supplies healthy." };
const LONG_REASON = Array.from({ length: 30 }, (_, i) => "word" + i).join(" ");
let mock = { commanderDelay: 80, sitrepDelay: 0, commanderReply: null };
const seen = []; // {kind:"commander"|"sitrep"|"other", body}
const mockServer = http.createServer((req, res) => {
  let b = "";
  req.on("data", (d) => (b += d));
  req.on("end", async () => {
    const body = JSON.parse(b || "{}");
    const sys = String(body.messages?.[0]?.content ?? "");
    const kind = /write the SITREP/.test(sys) ? "sitrep" : /HQ commander/.test(sys) ? "commander" : "other";
    seen.push({ kind, body });
    const text = kind === "sitrep" ? JSON.stringify(SITREP_JSON)
      : kind === "commander" ? (mock.commanderReply ?? JSON.stringify({ o: [["US_Squad2", "ATTACK", "B", LONG_REASON]], n: [90, "quiet"] }))
      : "{}";
    await sleep(kind === "sitrep" ? mock.sitrepDelay : mock.commanderDelay);
    res.writeHead(200, { "content-type": "text/event-stream" });
    res.write(": OPENROUTER PROCESSING\n\n");
    const chunk = (o) => res.write("data: " + JSON.stringify(o) + "\n\n");
    const mid = Math.floor(text.length / 2);
    chunk({ choices: [{ delta: { role: "assistant", content: "" } }] });
    chunk({ choices: [{ delta: { content: text.slice(0, mid) } }] });
    await sleep(30);
    chunk({ choices: [{ delta: { content: text.slice(mid) } }] });
    chunk({ choices: [], usage: { prompt_tokens: 1234, completion_tokens: 56, prompt_tokens_details: { cached_tokens: 1000 } } });
    res.write("data: [DONE]\n\n");
    res.end();
  });
}).listen(MOCK_PORT, "127.0.0.1");

// ---------- sidecar instances ----------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-latency-"));
const procs = [];
let log = "";
function start(port, name, cfg) {
  const cfgFile = path.join(tmp, name + ".json");
  fs.writeFileSync(cfgFile, JSON.stringify(cfg));
  const p = spawn(process.execPath, [path.join(DIR, "llm_sidecar.mjs")], { env: { ...process.env, CLW_PORT: String(port), CLW_LLM: "fake", CLW_CONFIG: cfgFile, CLW_DATA_DIR: path.join(tmp, name + "-data") }, stdio: ["ignore", "pipe", "pipe"] });
  p.stdout.on("data", (d) => (log += `[${name}] ` + d)); p.stderr.on("data", (d) => (log += `[${name}] ` + d));
  procs.push(p);
}
const api = (base) => ({
  post: async (p, body) => { const r = await fetch(base + p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }); return { status: r.status, json: await r.json() }; },
  get: async (p) => (await fetch(base + p)).json(),
});
async function up(base) { for (let i = 0; i < 60; i++) { try { if ((await fetch(base + "/health")).ok) return true; } catch {} await sleep(100); } return false; }
const sit = (faction = "US", extra = {}) => ({ ...JSON.parse(JSON.stringify(situation)), v: 3, faction, manifest: manifest.hash, hq: [], ...extra });

try {
  start(FAKE_PORT, "fake", { provider: "fake", model: "x", memory: { enabled: false } });
  start(LLM_PORT, "real", { provider: "fake", model: "x", memory: { enabled: false } });
  ok(await up(FAKE) && await up(REAL), "both sidecars started");
  await loginFetch(FAKE); await loginFetch(REAL);

  // ---------- 1. fake provider ----------
  console.log("fake provider");
  const F = api(FAKE);
  eq((await F.post("/manifest", manifest)).status, 200, "manifest accepted");
  const cf = await F.get("/api/config");
  eq([cf.output, cf.prompt], [{ mode: "json_object", terse: true, reasonWords: 12 }, { lean: true, staticBlock: true, cache: true, contactMax: 25, contactMaxAgeSec: 600 }], "default output + prompt groups");
  eq(cf.roles.sitrep, "weak", "role sitrep maps to the weak slot");
  const o1 = await F.post("/orders", sit("US", { langs: ["de"] }));
  ok(o1.json.sitrep?.headline && o1.json.sitrep.text_by_lang?.de, "fake: sitrep (+ translation) is in the same reply");
  ok(Array.isArray(o1.json.orders), "fake: orders in the reply");
  let st = await F.get("/api/state");
  const roles = new Set(st.llmCalls.map((e) => e.role));
  ok(roles.has("commander") && roles.has("sitrep"), "llmCalls has a commander and a sitrep entry", [...roles].join());
  const e0 = st.llmCalls[0];
  ok(["ts", "faction", "role", "slot", "model", "ok", "error", "inTok", "outTok", "cachedTok", "ttftMs", "totalMs", "promptChars", "sections", "outputMode", "fellBack"].every((k) => k in e0), "llmCalls entry has the contract fields", Object.keys(e0).join());
  ok(Date.now() - e0.ts < 60000 && e0.ts > 1.6e12, "llmCalls[].ts is epoch ms");
  const ce = st.llmCalls.find((e) => e.role === "commander");
  ok(ce.promptChars > 1000 && ce.sections.rules?.tokens > 0 && ce.sections.commands && ce.sections.situation, "commander entry: promptChars + measured sections", Object.keys(ce.sections).join());
  ok(st.latency.commander?.n >= 1 && st.latency.sitrep?.n >= 1 && "p50Ms" in st.latency.commander && "lastAt" in st.latency.commander, "latency aggregate per role", JSON.stringify(st.latency));
  const ex = st.exchanges[0];
  eq([ex.metrics?.role, ex.metrics?.slot, typeof ex.metrics?.outputMode], ["commander", "strong", "string"], "exchange.metrics = the commander llmCalls entry");
  ok(!JSON.stringify(ex.request).includes("SITREP"), "no SITREP in the stored request");
  const hqr = await F.post("/orders", sit("US", { hq: [{ id: 501, player: "Al", role: "soldier", squad: "US_Squad1", grid: "009-009", pos: [900, 950], text: "HQ, we need a truck", ageSec: 1 }] }));
  for (let i = 0; i < 30 && !(await F.get("/api/state")).llmCalls.some((e) => e.role === "hq"); i++) await sleep(100);
  const hqe = (await F.get("/api/state")).llmCalls.find((e) => e.role === "hq");
  ok(hqe && hqe.ok && hqe.ttftMs === null && hqe.slot === "strong", "hq agent turn recorded (usage + latency, ttft null)", JSON.stringify(hqe));
  // gated poll: nothing pending any more, keep reply without a sitrep
  const o2 = await F.post("/orders", sit("US"));
  eq([o2.json.keep, o2.json.sitrep], [true, undefined], "gated keep reply carries no (already delivered) sitrep");
  // config validation
  eq((await F.post("/api/config", { output: { mode: "xml" } })).status, 400, "output.mode enum validated");
  eq((await F.post("/api/config", { output: { reasonWords: 3 } })).status, 400, "output.reasonWords 4..30");
  eq((await F.post("/api/config", { output: { reasonWords: 31 } })).status, 400, "output.reasonWords upper bound");
  eq((await F.post("/api/config", { output: { terse: "yes" } })).status, 400, "output.terse boolean");
  eq((await F.post("/api/config", { prompt: { contactMax: 4 } })).status, 400, "prompt.contactMax 5..60");
  eq((await F.post("/api/config", { prompt: { contactMaxAgeSec: 30 } })).status, 400, "prompt.contactMaxAgeSec 60..1800");
  eq((await F.post("/api/config", { prompt: { nope: 1 } })).status, 400, "unknown prompt key rejected");
  const sv = await F.post("/api/config", { output: { mode: "json_schema", reasonWords: 20 }, prompt: { cache: false, contactMax: 40 } });
  eq([sv.status, sv.json.config.output, sv.json.config.prompt.cache, sv.json.config.prompt.contactMax], [200, { mode: "json_schema", terse: true, reasonWords: 20 }, false, 40], "groups merge onto the current values");
  await F.post("/api/config", { output: { mode: "json_object", reasonWords: 12 }, prompt: { cache: true, contactMax: 25 } });

  // ---------- 2. mock provider ----------
  console.log("openai-compatible mock (SSE)");
  const R = api(REAL);
  await R.post("/manifest", manifest);
  const set = await R.post("/api/config", { provider: "openai-compatible", model: "mock-m", baseUrl: MOCK, apiKey: "sk-test", timeoutSec: 5 });
  eq(set.status, 200, "provider switched to the mock");
  mock = { commanderDelay: 80, sitrepDelay: 700, commanderReply: null };
  const m1 = await R.post("/orders", sit("US"));
  eq(m1.json.orders?.map((o) => [o.squad, o.command, o.target]), [["US_Squad2", "ATTACK", "B"]], "terse reply became a valid order");
  ok(m1.json.orders[0].reason.split(" ").length <= 18 && m1.json.orders[0].reason.length <= 120, "30-word reason cut to ~reasonWords x 1.5", m1.json.orders[0].reason);
  ok(!m1.json.sitrep, "slow SITREP is not in the first reply (commander call is not delayed by it)");
  const cmd1 = seen.find((s) => s.kind === "commander");
  ok(cmd1 && !/SITREP/.test(cmd1.body.messages[0].content + cmd1.body.messages[1].content), "commander prompt has no SITREP request");
  ok(cmd1.body.messages[0].content.includes("Compact tuples") && cmd1.body.messages[1].content.startsWith("Current situation:"), "terse reply spec in system, situation in user");
  eq(cmd1.body.response_format, { type: "json_object" }, "response_format json_object");
  eq([cmd1.body.stream, cmd1.body.stream_options], [true, { include_usage: true }], "streaming with usage");
  ok(!cmd1.body.messages[0].content.includes("WAKE") && cmd1.body.messages[1].content.includes("WAKE (why you are consulted now)"), "wake in the user message");
  eq(cmd1.body.max_tokens, 1024, "maxTokens stays the config value (no SITREP extra tokens)");
  st = await R.get("/api/state");
  const mc = st.llmCalls.find((e) => e.role === "commander");
  ok(mc && mc.ok && mc.ttftMs >= 60 && mc.ttftMs < mc.totalMs, "commander: ttft measured", JSON.stringify(mc));
  eq([mc.inTok, mc.outTok, mc.cachedTok, mc.model, mc.outputMode, mc.slot], [1234, 56, 1000, "mock-m", "json_object", "strong"], "commander: tokens, cached tokens, model, mode");
  ok(st.latency.commander.avgTtftMs >= 60 && st.latency.commander.avgInTok === 1234 && st.latency.commander.avgCachedTok === 1000, "latency aggregates tokens + ttft", JSON.stringify(st.latency.commander));
  eq([st.exchanges[0].metrics?.ttftMs === mc.ttftMs, st.exchanges[0].usage?.input, st.exchanges[0].nextCheck?.sec], [true, 1234, 90], "exchange: metrics, usage, next check from the terse n key");
  const inflight = st.llmCalls.find((e) => e.role === "sitrep");
  ok(!inflight, "the SITREP call has not finished yet (running in the background)");

  // gated polls while the SITREP is still running: keep, no sitrep
  const g1 = await R.post("/orders", sit("US"));
  eq([g1.json.keep, g1.json.orders, g1.json.sitrep], [true, [], undefined], "gated reply while the SITREP is in flight: plain keep");
  await sleep(900);
  const g2 = await R.post("/orders", sit("US"));
  ok(g2.json.keep === true && g2.json.orders.length === 0 && g2.json.sitrep?.headline === SITREP_JSON.headline, "finished SITREP rides on the next gated keep reply", JSON.stringify(g2.json).slice(0, 200));
  const g3 = await R.post("/orders", sit("US"));
  eq([g3.json.keep, g3.json.sitrep], [true, undefined], "delivered exactly once");
  const sr = seen.find((s) => s.kind === "sitrep");
  ok(sr && sr.body.messages[0].content.includes("headline, situation, enemy, mission, execution, logistics") && sr.body.messages[1].content.includes("HQ orders in force"), "SITREP call: own prompt with situation + orders");
  eq(sr.body.response_format, { type: "json_object" }, "SITREP call: json_object");
  st = await R.get("/api/state");
  const sc = st.llmCalls.find((e) => e.role === "sitrep");
  ok(sc && sc.ok && sc.ttftMs !== null && sc.slot === "strong" && sc.sections.rules, "sitrep llmCalls entry (strong slot answers: no weak slot configured)", JSON.stringify(sc));
  eq(st.factions.US.sitrep.source, "llm", "dashboard sitrep source = llm");
  eq(seen.filter((s) => s.kind === "sitrep").length, 1, "exactly one SITREP call so far");

  // output.mode json_schema: the terse schema goes out; the SITREP finishes before the commander reply -> same reply
  await R.post("/api/config", { output: { mode: "json_schema" } });
  mock = { commanderDelay: 400, sitrepDelay: 0, commanderReply: JSON.stringify({ o: [["US_Squad2", "ATTACK", "B", "short reason"]], k: 0 }) };
  await R.post("/api/wake", { faction: "US" });
  const n0 = seen.length;
  const m2 = await R.post("/orders", sit("US"));
  const cmd2 = seen.slice(n0).find((s) => s.kind === "commander");
  eq([cmd2.body.response_format?.type, cmd2.body.response_format?.json_schema?.name, !!cmd2.body.response_format?.json_schema?.schema?.properties?.o], ["json_schema", "clausewitz_orders", true], "response_format json_schema with the terse schema");
  ok(m2.json.sitrep?.headline && m2.json.orders.length === 1, "SITREP finished first: delivered on the current reply", JSON.stringify(m2.json).slice(0, 200));
  eq(m2.json.orders[0].reason, "short reason", "short reason untouched");

  // weak slot: the SITREP (role sitrep) and its translation (role translate) go to it, the commander stays on the strong slot
  await R.post("/api/config", { weak: { provider: "openai-compatible", model: "weak-m", baseUrl: MOCK, apiKey: "sk-w", timeoutSec: 5 } });
  mock = { commanderDelay: 20, sitrepDelay: 0, commanderReply: null };
  await R.post("/api/wake", { faction: "US" });
  await R.post("/orders", sit("US", { langs: ["de"] }));
  await sleep(300);
  st = await R.get("/api/state");
  const wsr = st.llmCalls.find((e) => e.role === "sitrep"), wtr = st.llmCalls.find((e) => e.role === "translate"), wcm = st.llmCalls.find((e) => e.role === "commander");
  eq([wsr?.slot, wsr?.model, wcm?.slot, wcm?.model], ["weak", "weak-m", "strong", "mock-m"], "sitrep -> weak slot, commander stays strong");
  eq([wtr?.slot, wtr?.model, wtr?.ok], ["weak", "weak-m", true], "translate call is recorded (weak slot)");
  await R.post("/api/config", { weak: null });

  // output.mode text: no response_format; a verbose reply (model ignoring the terse spec) still works
  await R.post("/api/config", { output: { mode: "text", terse: false } });
  mock = { commanderDelay: 50, sitrepDelay: 0, commanderReply: JSON.stringify({ orders: [{ squad: "US_Squad2", command: "ATTACK", target: "B", reason: "verbose" }], next_check_sec: 45, next_check_reason: "x" }) };
  await R.post("/api/wake", { faction: "US" });
  const n1 = seen.length;
  const m3 = await R.post("/orders", sit("US"));
  const cmd3 = seen.slice(n1).find((s) => s.kind === "commander");
  ok(cmd3.body.response_format === undefined, "text mode: no response_format");
  ok(cmd3.body.messages[0].content.includes('"orders":[{"squad":"<id>"') && !cmd3.body.messages[0].content.includes("Compact tuples"), "terse off: verbose REPLY spec");
  eq([m3.json.orders?.[0]?.reason, st.exchanges.length > 0], ["verbose", true], "verbose reply still valid");

  // terse reply with a verbose-only model answer is covered above; a garbage reply is an error exchange with metrics
  mock = { commanderDelay: 20, sitrepDelay: 0, commanderReply: "no json at all" };
  await R.post("/api/wake", { faction: "US" });
  const m4 = await R.post("/orders", sit("US"));
  eq(m4.json.orders, [], "invalid reply -> empty orders (rule fallback)");
  st = await R.get("/api/state");
  ok(st.exchanges[0].ok === false && st.exchanges[0].metrics?.role === "commander" && st.exchanges[0].metrics.ok === true, "invalid output: the call itself was ok, the exchange failed", JSON.stringify(st.exchanges[0].metrics));
  ok(st.llmCalls.length <= 200, "ring is capped");
} catch (e) {
  fail++; console.log("  FAIL exception: " + (e?.stack ?? e));
} finally {
  for (const p of procs) p.kill();
  mockServer.close();
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) { console.log("--- sidecar log tail ---\n" + log.split("\n").slice(-40).join("\n")); process.exit(1); }
process.exit(0);

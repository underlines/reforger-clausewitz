// LLM-latency work (B2) checks, no API key, no game:   node dev/tools/test-planner.mjs
// Planner split (buy + FOB as their own background call), role -> slot tiering, escalation, roleTimeouts, HQ own cooldown.
// Starts two sidecar instances (own ports, scratch config/data) and a mock OpenAI-compatible server (SSE and JSON) that answers per call kind and model.
//  1. fake provider: planner runs inline (buy / fobs ride on the same reply), planner disabled = same result, config groups validated
//  2. mock provider: ORDERS prompt without buy / FOB text + PLAN block, planner call with its own prompt, delivery on a later gated keep reply (once),
//     re-validation against the newest situation, no delivery on a failure / rule-fallback reply, planner triggers, roles routing (planner -> weak model),
//     escalation to escalate.slot on an important trigger, roleTimeouts, planner disabled = old combined prompt, /hq cooldown independent of the commander's.
import http from "node:http";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { loginFetch } from "./test-login.mjs";
import { messages } from "./agent/fixtures/hq_fixture.mjs";
import { DEFAULT_MANIFEST } from "./agent/commanderPrompt.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const FAKE_PORT = 18811, REAL_PORT = 18812, MOCK_PORT = 18813;
const FAKE = `http://127.0.0.1:${FAKE_PORT}`, REAL = `http://127.0.0.1:${REAL_PORT}`, MOCK = `http://127.0.0.1:${MOCK_PORT}`;
let pass = 0, fail = 0;
const ok = (cond, name, extra = "") => { if (cond) { pass++; console.log("  ok   " + name); } else { fail++; console.log("  FAIL " + name + (extra ? "  -> " + extra : "")); } };
const eq = (a, b, name) => ok(JSON.stringify(a) === JSON.stringify(b), name, `got ${JSON.stringify(a)} want ${JSON.stringify(b)}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------- mock OpenAI-compatible server ----------
const COMMANDER_OK = JSON.stringify({ o: [["US_Squad2", "ATTACK", "B", "go"]], n: [90, "quiet"] });
const PLANNER_OK = JSON.stringify({ b: [["rifle_squad", 1, "ATTACK", "B", "reinforce B"]] });
const mock = { delay: { commander: 60, planner: 0, hq: 0 }, reply: { commander: COMMANDER_OK, planner: PLANNER_OK }, fail: {} }; // fail: "<model>|<kind>" -> HTTP status
const seen = []; // {kind, model, body}
const mockServer = http.createServer((req, res) => {
  let b = "";
  req.on("data", (d) => (b += d));
  req.on("end", async () => {
    const body = JSON.parse(b || "{}");
    const sys = String(body.messages?.[0]?.content ?? "");
    const kind = /HQ radio operator/.test(b) ? "hq" : /economy and logistics planner/.test(sys) ? "planner" : /HQ commander/.test(sys) ? "commander" : "other";
    seen.push({ kind, model: body.model, body });
    const status = mock.fail[body.model + "|" + kind];
    await sleep(mock.delay[kind] ?? 0);
    if (status) { res.writeHead(status, { "content-type": "application/json" }); return res.end(JSON.stringify({ error: { message: "mock " + status } })); }
    const text = kind === "hq" ? JSON.stringify({ reply: "Roger from " + body.model }) : (mock.reply[kind] ?? "{}");
    if (!body.stream) {
      res.writeHead(200, { "content-type": "application/json" });
      return res.end(JSON.stringify({ id: "x", object: "chat.completion", created: 1, model: body.model, choices: [{ index: 0, message: { role: "assistant", content: text }, finish_reason: "stop" }], usage: { prompt_tokens: 10, completion_tokens: 5, total_tokens: 15 } }));
    }
    res.writeHead(200, { "content-type": "text/event-stream" });
    const chunk = (o) => res.write("data: " + JSON.stringify(o) + "\n\n");
    chunk({ choices: [{ delta: { role: "assistant", content: "" } }] });
    chunk({ choices: [{ delta: { content: text } }] });
    chunk({ choices: [], usage: { prompt_tokens: 100, completion_tokens: 20 } });
    res.write("data: [DONE]\n\n");
    res.end();
  });
}).listen(MOCK_PORT, "127.0.0.1");
const seenOf = (kind, from = 0) => seen.slice(from).filter((s) => s.kind === kind);
const sysOf = (s) => String(s.body.messages[0].content), usrOf = (s) => String(s.body.messages[1].content);

// ---------- sidecar instances ----------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-planner-"));
const procs = [];
let log = "";
function start(port, name, cfg) {
  const cfgFile = path.join(tmp, name + ".json");
  fs.writeFileSync(cfgFile, JSON.stringify(cfg));
  const p = spawn(process.execPath, [path.join(DIR, "llm_sidecar.mjs")], { env: { ...process.env, CLW_PORT: String(port), CLW_LLM: "fake", CLW_CONFIG: cfgFile, CLW_DATA_DIR: path.join(tmp, name + "-data"), CLW_RATE_COOLDOWN_MS: "1500", CLW_PLANNER_MIN_GAP_MS: "150" }, stdio: ["ignore", "pipe", "pipe"] });
  p.stdout.on("data", (d) => (log += `[${name}] ` + d)); p.stderr.on("data", (d) => (log += `[${name}] ` + d));
  procs.push(p);
}
const api = (base) => ({
  post: async (p, body) => { const r = await fetch(base + p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }); return { status: r.status, json: await r.json() }; },
  get: async (p) => (await fetch(base + p)).json(),
});
async function up(base) { for (let i = 0; i < 60; i++) { try { if ((await fetch(base + "/health")).ok) return true; } catch {} await sleep(100); } return false; }
async function waitFor(fn, ms = 4000) { for (let t = 0; t < ms; t += 50) { const v = await fn(); if (v) return v; await sleep(50); } return null; }

// ---------- fixtures ----------
const ent = (name, channel, params = []) => ({ name, layer: "intent", channel, authority: "hq", cost: 0, tags: [], requires: [], desc: `${name} desc.`, params });
const manifest = { ...DEFAULT_MANIFEST, hash: "plan-1", faction: "US", mode: "conquest", builtin: undefined, primitives: [],
  intents: [...DEFAULT_MANIFEST.intents, ent("ESTABLISH_FOB", "fob", [{ name: "site", type: "string", required: true }]), ent("UPGRADE_FOB", "fob", [{ name: "fob", type: "string", required: true }]),
    ent("RESUPPLY", "fob", [{ name: "fob", type: "string", required: true }]), ent("CONVOY", "fob", [{ name: "origin", type: "base" }, { name: "dest", type: "base" }])] };
const objective = (o = {}) => ({ command: "ATTACK", target: "B", status: "active", ageSec: 30, ...o });
const baseSit = () => ({
  v: 3, faction: "US", manifest: "plan-1", time: 1300, hq: [],
  war: { supplies: 400, tickets: 500, phase: "running", aiCap: 60, liveAI: 12, vehicles: 1, vehicleCap: 6 },
  catalog: [{ type: "rifle_squad", kind: "infantry", cost: 100 }, { type: "truck", kind: "motorized", cost: 150 }],
  objectives: [{ id: "A", name: "Alpha", pos: [500, 500], radius: 80, value: 1, owner: "US" }, { id: "B", name: "Bravo", pos: [1750, 1620], radius: 80, value: 2, owner: "USSR" }],
  bases: [{ faction: "US", pos: [100, 100] }],
  squads: [{ id: "US_Squad1", kind: "infantry", pos: [900, 0, 950], agents: 9, startAgents: 9, humanLed: true },
    { id: "US_Squad2", kind: "infantry", pos: [1000, 0, 1000], agents: 8, startAgents: 8, humanLed: false, objective: objective() },
    { id: "US_Hmv1", kind: "light", pos: [1100, 0, 1000], agents: 3, startAgents: 3, humanLed: false, objective: objective() }],
  contacts: [{ pos: [1750, 0, 1620], desc: "5x USSR infantry", kind: "infantry", ageSec: 12 }],
});
const clone = (o) => JSON.parse(JSON.stringify(o));

try {
  start(FAKE_PORT, "fake", { provider: "fake", model: "x", memory: { enabled: false }, sitrep: { enabled: false } });
  start(REAL_PORT, "real", { provider: "openai-compatible", model: "strong-m", baseUrl: MOCK, apiKey: "sk-test", timeoutSec: 5,
    weak: { provider: "openai-compatible", model: "weak-m", baseUrl: MOCK, apiKey: "sk-w", timeoutSec: 5 },
    memory: { enabled: false }, sitrep: { enabled: false }, triggers: { cooldownSec: 0 } });
  ok(await up(FAKE) && await up(REAL), "both sidecars started");
  await loginFetch(FAKE); await loginFetch(REAL);

  // ---------- 1. fake provider ----------
  console.log("fake provider: planner inline");
  const F = api(FAKE);
  eq((await F.post("/manifest", manifest)).status, 200, "manifest accepted");
  const cf = await F.get("/api/config");
  eq([cf.planner, cf.roles, cf.escalate, cf.roleTimeouts], [{ enabled: true, intervalSec: 180 },
    { commander: "strong", planner: "strong", sitrep: "weak", journal: "weak", hq: "strong", translate: "weak" }, { enabled: false, slot: "strong" },
    { commander: 0, planner: 0, sitrep: 0, journal: 0, hq: 0 }], "defaults of planner / roles / escalate / roleTimeouts");
  const f1 = await F.post("/orders", baseSit());
  ok(f1.json.orders.length > 0 && f1.json.buy.length > 0, "fake: orders AND buy in the same reply (planner inline)", JSON.stringify(f1.json).slice(0, 300));
  ok(f1.json.buy.every((x) => x.command === "ATTACK" && x.target === "B"), "fake: buys carry their assignment");
  let st = await F.get("/api/state");
  const ex = st.exchanges[0];
  eq([ex.buy.length > 0, ex.planner?.delivered, ex.planner?.buy], [true, true, f1.json.buy.length], "exchange.planner = the delivery");
  ok(st.llmCalls.some((e) => e.role === "planner" && e.model === "fake") && st.latency.planner?.n >= 1, "llmCalls + latency have the planner role");
  ok(st.planner?.US?.runs >= 1 && st.factions.US.buy.length === f1.json.buy.length, "GET /api/state planner view + faction buy");
  // disabled: same reply, no planner call
  const n0 = st.llmCalls.filter((e) => e.role === "planner").length;
  eq((await F.post("/api/config", { planner: { enabled: false } })).status, 200, "planner off");
  await F.post("/api/wake", { faction: "US" });
  const f2 = await F.post("/orders", baseSit());
  eq(f2.json.buy.length, f1.json.buy.length, "planner off: buy in the same reply as before");
  st = await F.get("/api/state");
  eq([st.llmCalls.filter((e) => e.role === "planner").length, st.exchanges[0].planner], [n0, null], "planner off: no planner call, exchange.planner null");
  await F.post("/api/config", { planner: { enabled: true } });
  // validation
  for (const [body, name] of [[{ planner: { intervalSec: 59 } }, "planner.intervalSec >= 60"], [{ planner: { intervalSec: 901 } }, "planner.intervalSec <= 900"], [{ planner: { enabled: "yes" } }, "planner.enabled boolean"],
    [{ roles: { commander: "medium" } }, "roles value enum"], [{ roles: { boss: "strong" } }, "unknown role rejected"], [{ escalate: { slot: "x" } }, "escalate.slot enum"], [{ escalate: { enabled: 1 } }, "escalate.enabled boolean"],
    [{ roleTimeouts: { hq: 301 } }, "roleTimeouts <= 300"], [{ roleTimeouts: { hq: -1 } }, "roleTimeouts >= 0"], [{ roleTimeouts: { translate: 5 } }, "roleTimeouts has no translate"]]) {
    eq((await F.post("/api/config", body)).status, 400, "400: " + name);
  }
  const sv = await F.post("/api/config", { planner: { intervalSec: 240 }, roles: { journal: "strong" }, escalate: { enabled: true }, roleTimeouts: { commander: 12 } });
  eq([sv.json.config.planner, sv.json.config.roles.journal, sv.json.config.roles.planner, sv.json.config.escalate, sv.json.config.roleTimeouts.commander, sv.json.config.roleTimeouts.hq], [{ enabled: true, intervalSec: 240 }, "strong", "strong", { enabled: true, slot: "strong" }, 12, 0], "groups merge onto the current values");
  await F.post("/api/config", { planner: { intervalSec: 180 }, roles: { journal: "weak" }, escalate: { enabled: false }, roleTimeouts: { commander: 0 } });

  // ---------- 2. mock provider ----------
  console.log("openai-compatible mock: planner split");
  const R = api(REAL);
  eq((await R.post("/manifest", manifest)).status, 200, "manifest accepted (real)");
  const cur = baseSit();
  const poll = async () => (await R.post("/orders", clone(cur))).json;
  const wake = () => R.post("/api/wake", { faction: "US" });
  const orderTypes = (j) => (j.buy ?? []).map((x) => x.type);

  // -- 2a. first poll: orders prompt, planner prompt, delivery later on a gated keep reply
  mock.delay = { commander: 80, planner: 400, hq: 0 };
  const m0 = seen.length;
  const p1 = await poll();
  eq(p1.orders?.map((o) => [o.squad, o.command, o.target]), [["US_Squad2", "ATTACK", "B"]], "orders call: terse reply became a valid order");
  ok((p1.buy ?? []).length === 0, "planner still running: no buy in the first reply", JSON.stringify(p1.buy));
  const cmd = seenOf("commander", m0)[0], pln = seenOf("planner", m0)[0];
  ok(cmd && pln, "one commander call and one planner call (parallel)");
  ok(!/\bbuy\b/i.test(sysOf(cmd)) && !/FOB|convoy|RESUPPLY/i.test(sysOf(cmd)), "ORDERS system prompt has no BUY / FOB text");
  ok(sysOf(cmd).includes("Compact tuples") && sysOf(cmd).includes('"o":[') && !sysOf(cmd).includes('"b":[') && !sysOf(cmd).includes('"f":['), "ORDERS reply spec: o / m only");
  ok(usrOf(cmd).includes("PLAN (") && usrOf(cmd).includes("working on it now"), "ORDERS user message has the PLAN block (planner running)", usrOf(cmd).slice(usrOf(cmd).indexOf("PLAN"), usrOf(cmd).indexOf("PLAN") + 200));
  ok(sysOf(pln).includes("SPAWN ON ASSIGNMENT") && sysOf(pln).includes("BUY:") && sysOf(pln).includes("ESTABLISH_FOB") && !sysOf(pln).includes("COMMANDS (orders") && sysOf(pln).includes('"b":[') && !sysOf(pln).includes('"o":['), "PLANNER prompt: economy doctrine, buy / fob entries, b / f spec");
  ok(usrOf(pln).includes("ORDERS IN FORCE") && usrOf(pln).includes("PLANNER WAKE:") && usrOf(pln).startsWith("Current situation:"), "PLANNER user message: situation, orders in force, wake line");
  eq([pln.body.response_format, pln.body.stream], [{ type: "json_object" }, true], "planner: response_format json_object, streaming");
  eq(pln.model, "strong-m", "roles.planner = strong -> strong model");
  st = await R.get("/api/state");
  eq([st.exchanges[0].planner?.delivered, st.exchanges[0].slot, st.exchanges[0].escalated], [false, "strong", false], "exchange: planner not delivered yet, slot strong, not escalated");
  ok(await waitFor(async () => (await R.get("/api/state")).llmCalls.some((e) => e.role === "planner" && e.ok)), "planner call finished (llmCalls)");
  st = await R.get("/api/state");
  const pc = st.llmCalls.find((e) => e.role === "planner");
  ok(pc.slot === "strong" && pc.model === "strong-m" && pc.outputMode === "json_object" && pc.sections.rules && pc.sections.commands && pc.sections.orders && pc.sections.situation && pc.inTok === 100, "planner llmCalls entry: role, slot, sections, tokens", JSON.stringify(pc));
  ok(/planner faction=US buy=1 fobs=0 \d+ms/.test(log), "log line: planner faction=US buy=1 fobs=0 <ms>");
  const g1 = await poll();
  eq([g1.keep, g1.orders, orderTypes(g1), g1.buy?.[0]?.target], [true, [], ["rifle_squad"], "B"], "planner result rides on the next gated keep reply", JSON.stringify(g1));
  const g2 = await poll();
  eq([g2.keep, g2.buy], [true, undefined], "delivered exactly once");
  st = await R.get("/api/state");
  eq(st.factions.US.buy.map((x) => x.type), ["rifle_squad"], "dashboard faction state shows the delivered buy");

  // -- 2b. re-validation against the newest situation
  mock.reply.planner = JSON.stringify({ b: [["rifle_squad", 1, "ATTACK", "B", "a"], ["truck", 1, "ATTACK", "B", "b"]] });
  mock.delay.planner = 150;
  await wake(); await poll();
  await waitFor(async () => (await R.get("/api/state")).planner.US.pending);
  cur.war.supplies = 120; // the truck (150) is no longer affordable after the rifle squad (100)
  const g3 = await poll();
  eq([g3.keep, orderTypes(g3)], [true, ["rifle_squad"]], "re-validated at delivery: the unaffordable truck is dropped", JSON.stringify(g3.buy));
  cur.war.supplies = 400;

  // -- 2c. not attached to a failure / rule-fallback reply; stays pending; delivered on the next good reply
  mock.reply.planner = PLANNER_OK; mock.delay = { commander: 300, planner: 0, hq: 0 };
  mock.reply.commander = "no json at all";
  await wake();
  const bad = await poll();
  eq([bad.orders, orderTypes(bad), bad.keep], [[], [], undefined], "failed commander call: rule-fallback reply without buy");
  ok(await waitFor(async () => (await R.get("/api/state")).planner.US.pending), "planner result is still pending after the failure reply");
  const badGate = await poll();
  eq([badGate.orders, badGate.keep, badGate.buy], [[], undefined, undefined], "gated poll after a failure: still {orders:[]} (rules), nothing attached");
  ok((await R.get("/api/state")).planner.US.pending, "result still pending");
  mock.reply.commander = COMMANDER_OK; mock.delay.commander = 60;
  await wake();
  const good = await poll();
  eq(orderTypes(good), ["rifle_squad"], "next good reply carries the pending planner result", JSON.stringify(good).slice(0, 250));
  await sleep(200);
  const goodGate = await poll();
  eq([goodGate.keep, orderTypes(goodGate).length <= 1], [true, true], "(the manual wake started one more run; nothing is delivered twice)");

  // -- 2d. planner triggers (min gap 150 ms in this test)
  console.log("planner triggers");
  await sleep(300); await poll(); await sleep(300); // drain pending results, let the gap pass
  const trig = async (name, re, mutate) => {
    await sleep(250);
    const mark = log.length;
    mutate();
    await poll();
    ok(await waitFor(() => re.test(log.slice(mark))), "trigger: " + name, log.slice(mark).split("\n").filter((l) => /planner/.test(l)).join(" | "));
  };
  cur.war.supplies = 120; await wake(); await poll(); await sleep(300); // baseline planner run with 120 supplies
  await trig("supplies grew by the cheapest cost", /planner faction=US buy=\d+ fobs=\d+ \d+ms.*\(trigger: supplies 120 -> 400/, () => { cur.war.supplies = 400; });
  await trig("fobRules.ready turned true", /\(trigger: fobs ready/, () => { cur.fobRules = { ready: true, cost: 100, max: 2, live: 0, inProgress: 0 }; });
  await trig("a fob became active", /\(trigger: fob F1 active/, () => { cur.fobs = [{ id: "F1", state: "active", tier: 1, stock: 80 }]; cur.logisticsRules = { lowStock: 50, truckCargo: 100, convoyMax: 2, inFlight: 0 }; });
  await trig("fob stock below lowStock", /\(trigger: fob F1 stock 30 below 50/, () => { cur.fobs[0].stock = 30; });
  await trig("objective owner change", /\(trigger: objective B changed owner \(USSR -> US\)/, () => { cur.objectives[1].owner = "US"; });
  await trig("an own squad lost", /\(trigger: US_Hmv1 lost/, () => { cur.squads = cur.squads.filter((q) => q.id !== "US_Hmv1"); });
  const mark2 = log.length; await poll(); await sleep(400);
  ok(!/planner faction=US buy/.test(log.slice(mark2)), "no trigger, no run on an unchanged poll");
  cur.objectives[1].owner = "USSR"; cur.squads.push({ id: "US_Hmv1", kind: "light", pos: [1100, 0, 1000], agents: 3, startAgents: 3, humanLed: false, objective: objective() }); cur.fobs = []; delete cur.fobRules; delete cur.logisticsRules;
  await sleep(300); await wake(); await poll(); await sleep(400);

  // -- 2e. roles routing: planner -> weak model, commander stays strong
  console.log("roles / escalate / timeouts");
  eq((await R.post("/api/config", { roles: { planner: "weak" } })).status, 200, "roles.planner = weak");
  mock.delay = { commander: 60, planner: 0, hq: 0 };
  const m5 = seen.length;
  await wake(); await poll(); await sleep(300);
  eq([seenOf("planner", m5)[0]?.model, seenOf("commander", m5)[0]?.model], ["weak-m", "strong-m"], "planner -> weak model, commander -> strong model");
  st = await R.get("/api/state");
  eq([st.llmCalls.find((e) => e.role === "planner").slot, st.llmCalls.find((e) => e.role === "planner").model], ["weak", "weak-m"], "planner llmCalls entry: slot weak");
  await R.post("/api/config", { roles: { planner: "strong" } });

  // -- 2f. escalation
  await R.post("/api/config", { roles: { commander: "weak" }, escalate: { enabled: true, slot: "strong" }, planner: { enabled: false } });
  mock.reply.commander = COMMANDER_OK;
  const m6 = seen.length;
  await wake(); await poll();
  const routine = seenOf("commander", m6)[0];
  st = await R.get("/api/state");
  eq([routine?.model, st.exchanges[0].slot, st.exchanges[0].escalated, st.exchanges[0].wake.reason], ["weak-m", "weak", false, "manual"], "routine tick -> roles.commander (weak)");
  cur.squads.find((q) => q.id === "US_Squad2").agents = 2; // 6 casualties
  const m7 = seen.length;
  await poll();
  const esc = seenOf("commander", m7)[0];
  st = await R.get("/api/state");
  ok(st.exchanges[0].wake.triggers.some((t) => /casualt/.test(t)), "the wake has a casualties trigger", JSON.stringify(st.exchanges[0].wake));
  eq([esc?.model, st.exchanges[0].slot, st.exchanges[0].escalated, st.llmCalls.find((e) => e.role === "commander").slot], ["strong-m", "strong", true, "strong"], "important trigger -> escalate.slot (strong), recorded in the exchange + llmCalls");
  await R.post("/api/config", { escalate: { enabled: false } });
  cur.squads.find((q) => q.id === "US_Squad2").agents = 8;
  await wake(); await poll();
  cur.squads.find((q) => q.id === "US_Squad2").agents = 1;
  const m8 = seen.length;
  await poll();
  eq(seenOf("commander", m8)[0]?.model, "weak-m", "escalate disabled: the important trigger stays on roles.commander");
  await R.post("/api/config", { roles: { commander: "strong" } });
  cur.squads.find((q) => q.id === "US_Squad2").agents = 8;

  // -- 2g. planner disabled = old combined prompt
  const m9 = seen.length;
  mock.reply.commander = JSON.stringify({ o: [["US_Squad2", "ATTACK", "B", "go"]], b: [["rifle_squad", 1, "ATTACK", "B", "combined"]] });
  await wake();
  const comb = await poll();
  const cc = seenOf("commander", m9)[0];
  ok(sysOf(cc).includes("SPAWN ON ASSIGNMENT") && sysOf(cc).includes('"b":[') && !usrOf(cc).includes("PLAN ("), "planner off: combined commander prompt (buy rules + spec, no plan block)");
  eq([orderTypes(comb), seenOf("planner", m9).length], [["rifle_squad"], 0], "planner off: buy in the commander reply, no planner call");
  await R.post("/api/config", { planner: { enabled: true } });
  // a model that ignores the split and answers buy in the orders call: dropped
  mock.reply.commander = JSON.stringify({ o: [["US_Squad2", "ATTACK", "B", "go"]], b: [["rifle_squad", 1, "ATTACK", "B", "ignored"]] });
  mock.reply.planner = "{}"; // planner returns nothing usable
  await wake();
  const ign = await poll();
  eq([ign.orders.length, orderTypes(ign)], [1, []], "split mode: buy in the orders reply is ignored (planner owns it)");
  st = await R.get("/api/state");
  ok(st.exchanges[0].dropped.some((d) => /planner/.test(d.reason)), "…and listed under dropped", JSON.stringify(st.exchanges[0].dropped));
  mock.reply.commander = COMMANDER_OK; mock.reply.planner = PLANNER_OK;
  await sleep(300);

  // -- 2h. roleTimeouts
  await R.post("/api/config", { roleTimeouts: { commander: 1, planner: 1 } });
  mock.delay = { commander: 2500, planner: 2500, hq: 0 };
  await wake();
  const t0 = Date.now();
  const to = await poll();
  const took = Date.now() - t0;
  st = await R.get("/api/state");
  ok(took < 2200 && st.exchanges[0].ok === false && st.exchanges[0].reason === "timeout", `commander roleTimeout 1 s applied (slot default is 5 s): ${took} ms`, JSON.stringify(st.exchanges[0].reason));
  ok(await waitFor(() => /planner faction=US FAILED \(timeout\)/.test(log), 3000), "planner roleTimeout applied");
  eq([to.orders, st.llmCalls.find((e) => e.role === "commander").error], [[], "timeout"], "timeout = rule fallback reply, llmCalls error timeout");
  await R.post("/api/config", { roleTimeouts: { commander: 0, planner: 0 } });
  mock.delay = { commander: 60, planner: 0, hq: 0 };
  await sleep(3000); // let the pending planner/commander sockets of the hung mock calls finish

  // -- 2i. hq cooldown independent of the commander's
  console.log("hq own cooldown");
  await R.post("/api/config", { fallback: { model: "backup-m" } });
  mock.fail["strong-m|commander"] = 429;
  const m10 = seen.length;
  await wake(); const hb = await poll();
  ok(hb.orders.length === 1 && seenOf("commander", m10).map((s) => s.model).join() === "strong-m,backup-m", "commander: strong-m 429 -> backup-m answered", seenOf("commander", m10).map((s) => s.model).join());
  st = await R.get("/api/state");
  ok(st.modelCooldowns["strong|strong-m"] > 0 && !Object.keys(st.modelCooldowns).some((k) => k.startsWith("hq:")), "cooldown key strong|strong-m, no hq: key", JSON.stringify(st.modelCooldowns));
  const msg1 = { ...messages.vehicle, id: 7001 };
  const h1 = await R.post("/hq", { faction: "US", msg: msg1 });
  st = await R.get("/api/state");
  const hq1 = st.hq.find((h) => h.msg.id === 7001);
  ok(h1.json.handled && hq1 && hq1.ok && !hq1.fellBack && hq1.modelUsed === "strong-m" && /Roger from strong-m/.test(hq1.reply), "commander cooldown does not block /hq: the hq turn ran on strong-m", JSON.stringify(hq1 && { ok: hq1.ok, fellBack: hq1.fellBack, modelUsed: hq1.modelUsed, err: hq1.error, pe: hq1.primaryError }));
  eq(seenOf("hq", m10).map((s) => s.model).slice(0, 1), ["strong-m"], "mock saw the hq turn on strong-m");
  await sleep(1700); // commander cooldown (1.5 s) over
  delete mock.fail["strong-m|commander"];
  mock.fail["strong-m|hq"] = 429;
  const h2 = await R.post("/hq", { faction: "US", msg: { ...messages.vehicle, id: 7002 } });
  st = await R.get("/api/state");
  const hq2 = st.hq.find((h) => h.msg.id === 7002);
  ok(hq2 && hq2.ok && hq2.fellBack && hq2.modelUsed === "backup-m", "hq: strong-m 429 -> backup-m", JSON.stringify(hq2 && { ok: hq2.ok, fellBack: hq2.fellBack, modelUsed: hq2.modelUsed, err: hq2.error }));
  ok(st.modelCooldowns["hq:strong|strong-m"] > 0 && !st.modelCooldowns["strong|strong-m"], "cooldown key hq:strong|strong-m only", JSON.stringify(st.modelCooldowns));
  const m11 = seen.length;
  await wake(); const hc = await poll();
  eq([hc.orders.length, seenOf("commander", m11).map((s) => s.model)], [1, ["strong-m"]], "an /hq cooldown does not move the commander to the backup");
  delete mock.fail["strong-m|hq"];
} catch (e) {
  fail++; console.log("  FAIL exception: " + (e?.stack ?? e));
} finally {
  for (const p of procs) p.kill();
  mockServer.close();
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) { console.log("--- sidecar log tail ---\n" + log.split("\n").slice(-60).join("\n")); process.exit(1); }
process.exit(0);

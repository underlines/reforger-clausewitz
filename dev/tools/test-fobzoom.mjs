// M4.6 FOB tool through the sidecar, no API key, no game:   node dev/tools/test-fobzoom.mjs
// Step 1 (area pick on the fobs channel of the commander reply, validateV2) and step 2 (POST /fobzoom, one small LLM call), fake provider and a mock
// OpenAI-compatible provider. Starts two sidecar instances (own ports, scratch config / data).
import http from "node:http";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { loginFetch } from "./test-login.mjs";
import { DEFAULT_MANIFEST } from "./agent/commanderPrompt.mjs";
import { macroFixture, zoomFixture } from "./fixtures/fobmap_fixture.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const FAKE_PORT = 18821, REAL_PORT = 18822, MOCK_PORT = 18823;
const FAKE = `http://127.0.0.1:${FAKE_PORT}`, REAL = `http://127.0.0.1:${REAL_PORT}`, MOCK = `http://127.0.0.1:${MOCK_PORT}`;
let pass = 0, fail = 0;
const ok = (cond, name, extra = "") => { if (cond) { pass++; console.log("  ok   " + name); } else { fail++; console.log("  FAIL " + name + (extra ? "  -> " + extra : "")); } };
const eq = (a, b, name) => ok(JSON.stringify(a) === JSON.stringify(b), name, `got ${JSON.stringify(a)} want ${JSON.stringify(b)}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------- mock OpenAI-compatible server: commander call and fobzoom call ----------
const mock = { commander: JSON.stringify({ f: [["ESTABLISH_FOB", "D6", "cover the south flank"]] }), fobzoom: JSON.stringify({ cell: "f8", reason: "road, open ground" }) };
const seen = [];
const mockServer = http.createServer((req, res) => {
  let b = "";
  req.on("data", (d) => (b += d));
  req.on("end", () => {
    const body = JSON.parse(b || "{}");
    const sys = String(body.messages?.[0]?.content ?? "");
    const kind = /You already decided to build/.test(sys) ? "fobzoom" : /HQ commander/.test(sys) ? "commander" : "other";
    seen.push({ kind, body });
    const text = mock[kind] ?? "{}";
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

// ---------- sidecar instances ----------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-fobzoom-"));
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

// ---------- fixtures ----------
const ent = (name, channel, params = []) => ({ name, layer: "intent", channel, authority: "hq", cost: 0, tags: [], requires: [], desc: `${name} desc.`, params });
const manifest = { ...DEFAULT_MANIFEST, hash: "fz-1", faction: "US", mode: "conquest", builtin: undefined, primitives: [],
  intents: [...DEFAULT_MANIFEST.intents, ent("ESTABLISH_FOB", "fob", [{ name: "area", type: "string" }, { name: "intent", type: "string" }, { name: "site", type: "string" }, { name: "reason", type: "string" }]),
    ent("UPGRADE_FOB", "fob", [{ name: "fob", type: "string", required: true }])] };
const baseSit = (over = {}) => ({
  v: 4, faction: "US", manifest: "fz-1", time: 90, hq: [],
  war: { supplies: 600, tickets: 500, phase: "running", aiCap: 60, liveAI: 12, vehicles: 1, vehicleCap: 6 },
  catalog: [{ type: "rifle_squad", kind: "infantry", cost: 100 }],
  objectives: [{ id: "C", name: "Morton", pos: [5750, 7250], radius: 60, value: 1, owner: "none" }],
  bases: [{ faction: "US", pos: [1500, 0, 1500] }],
  squads: [{ id: "US_Squad2", kind: "infantry", pos: [1600, 0, 1600], agents: 8, startAgents: 8, humanLed: false }],
  contacts: [{ pos: [6120, 0, 7390], desc: "3x infantry", kind: "infantry", ageSec: 15 }],
  fobs: [], fobRules: { ready: true, cost: 300, max: 2, live: 0, inProgress: 0, earliestSec: 60, elapsedSec: 90, upgradeCost: 150 },
  fobMap: macroFixture(),
  ...over,
});
const clone = (o) => JSON.parse(JSON.stringify(o));

try {
  start(FAKE_PORT, "fake", { provider: "fake", model: "x", memory: { enabled: false }, sitrep: { enabled: false } });
  start(REAL_PORT, "real", { provider: "openai-compatible", model: "strong-m", baseUrl: MOCK, apiKey: "sk-test", timeoutSec: 5, planner: { enabled: false },
    memory: { enabled: false }, sitrep: { enabled: false }, triggers: { cooldownSec: 0 } });
  ok(await up(FAKE) && await up(REAL), "both sidecars started");
  await loginFetch(FAKE); await loginFetch(REAL);

  // ---------- 1. fake provider ----------
  console.log("fake provider");
  const F = api(FAKE);
  eq((await F.post("/manifest", manifest)).status, 200, "manifest accepted");
  const o1 = await F.post("/orders", baseSit());
  eq(o1.json.fobs?.length, 1, "step 1: the fake commander places one FOB", JSON.stringify(o1.json.fobs));
  eq([o1.json.fobs?.[0]?.command, o1.json.fobs?.[0]?.area], ["ESTABLISH_FOB", "D6"], "step 1: area = one cell from the front (E5) toward the own HQ (B9)");
  ok(typeof o1.json.fobs?.[0]?.intent === "string" && o1.json.fobs[0].intent.length > 0 && !("site" in o1.json.fobs[0]), "step 1: area + intent, no site");

  // step 2
  const z1 = await F.post("/fobzoom", { v: 1, faction: "US", area: "E5", intent: "cover Morton", attempt: 1, zoom: zoomFixture() });
  eq([z1.status, z1.json.ok, z1.json.cell], [200, true, "f8"], "step 2: fake pick = cell beside a road nearest to the front", JSON.stringify(z1.json));
  ok(/fobzoom faction=US area=E5 attempt=1 -> cell f8/.test(log), "log line: fobzoom faction=US area=E5 attempt=1 -> cell f8");
  const noRoad = zoomFixture({ rows: zoomFixture().rows.map((r) => r.replace(/[=-]/g, "_")) });
  const z2 = await F.post("/fobzoom", { v: 1, faction: "US", area: "E5", intent: "x", attempt: 1, zoom: noRoad });
  eq([z2.json.ok, z2.json.decline], [true, true], "step 2: no road anywhere -> decline");
  eq((await F.post("/fobzoom", { faction: "US", area: "E5" })).status, 400, "step 2: a body without a digest is a 400");
  const st = await F.get("/api/state");
  ok(st.llmCalls.some((e) => e.role === "fobzoom" && e.model === "fake"), "llmCalls has the fobzoom role");
  eq([st.fobZoom?.length, st.fobZoom?.map((e) => e.result)], [3, ["fobzoom needs a zoom digest with rows", "decline", "f8"]], "api/state fobZoom ring: newest first");
  await F.post("/api/pause", { paused: true });
  const z3 = await F.post("/fobzoom", { v: 1, faction: "US", area: "E5", attempt: 1, zoom: zoomFixture() });
  eq([z3.json.ok, z3.json.error], [false, "HQ paused"], "paused: ok=false, the game falls back to its rules");
  await F.post("/api/pause", { paused: false });

  // ---------- 2. mock provider ----------
  console.log("openai-compatible mock");
  const R = api(REAL);
  eq((await R.post("/manifest", manifest)).status, 200, "manifest accepted (real)");
  const poll = async (sit = baseSit()) => { await R.post("/api/wake", { faction: "US" }); return (await R.post("/orders", clone(sit))).json; };

  let m0 = seen.length;
  const p1 = await poll();
  eq(p1.fobs?.[0], { command: "ESTABLISH_FOB", area: "D6", intent: "cover the south flank", reason: "cover the south flank" }, "terse tuple [ESTABLISH_FOB, D6, why] -> area + intent + reason", JSON.stringify(p1.fobs));
  const cmd = seenOf("commander", m0)[0];
  ok(cmd && /STEP 1/.test(String(cmd.body.messages[0].content)) && String(cmd.body.messages[0].content).includes('"fobMap"'), "commander prompt has the two-step FOB paragraph and the fobMap in the static block");
  ok(!String(cmd.body.messages[1].content).includes(macroFixture().rows[0]), "terrain rows are not repeated in the user message");

  mock.commander = JSON.stringify({ f: [["ESTABLISH_FOB", "none", "front still unclear"]] });
  const p2 = await poll();
  eq(p2.fobs?.[0], { command: "ESTABLISH_FOB", area: "none", reason: "front still unclear" }, "explicit decline passes through", JSON.stringify(p2.fobs));

  mock.commander = JSON.stringify({ f: [["ESTABLISH_FOB", "Z9", "x"]], k: 1 });
  const p3 = await poll();
  ok(!p3.fobs?.length, "an area outside the grid is dropped", JSON.stringify(p3));
  ok(/dropped fob: unknown fob area Z9/.test(log), "drop reason names the area");

  mock.commander = JSON.stringify({ f: [["ESTABLISH_FOB", "D6", "cover"]], k: 1 });
  const p4 = await poll(baseSit({ fobRules: { ready: false, readyReason: "opening phase", cost: 300, max: 2, live: 0, inProgress: 0, earliestSec: 360, elapsedSec: 90 } }));
  ok(!p4.fobs?.length && /too early for a FOB/.test(log), "not ready: dropped as before");

  mock.commander = JSON.stringify({ o: [["US_Squad2", "ATTACK", "C", "go"]], f: [["ESTABLISH_FOB", [5750, 7050], "direct spot"]] });
  const p5 = await poll();
  eq(p5.fobs?.[0]?.pos, [5750, 7050], "a position instead of an area is accepted", JSON.stringify(p5.fobs));

  // zoom call
  m0 = seen.length;
  const zr = await R.post("/fobzoom", { v: 1, faction: "US", area: "E5", intent: "cover Morton", attempt: 1, zoom: zoomFixture() });
  eq([zr.json.ok, zr.json.cell, zr.json.reason], [true, "f8", "road, open ground"], "zoom through the mock model", JSON.stringify(zr.json));
  const zc = seenOf("fobzoom", m0)[0];
  ok(zc && String(zc.body.messages[1].content).includes("cover Morton") && String(zc.body.messages[1].content).includes('"rows"'), "zoom prompt carries the intent and the digest");
  eq(zc?.body.response_format, { type: "json_object" }, "zoom: response_format json_object");
  mock.fobzoom = JSON.stringify({ cell: "c3", reason: "better" });
  m0 = seen.length;
  const zr2 = await R.post("/fobzoom", { v: 1, faction: "US", area: "E5", intent: "cover Morton", attempt: 2, retry: { reason: "no road within 60 m", pos: [5750, 7050] }, zoom: zoomFixture() });
  eq([zr2.json.ok, zr2.json.cell], [true, "c3"], "retry answer");
  ok(/RETRY \(attempt 2 of 2\)/.test(String(seenOf("fobzoom", m0)[0].body.messages[1].content)), "retry prompt names the refusal");
  mock.fobzoom = JSON.stringify({ decline: true, reason: "everything is exposed" });
  const zr3 = await R.post("/fobzoom", { v: 1, faction: "US", area: "E5", attempt: 1, zoom: zoomFixture() });
  eq([zr3.json.ok, zr3.json.decline, zr3.json.reason], [true, true, "everything is exposed"], "model declines in the zoom");
  mock.fobzoom = "I cannot decide.";
  const zr4 = await R.post("/fobzoom", { v: 1, faction: "US", area: "E5", attempt: 1, zoom: zoomFixture() });
  eq(zr4.json.ok, false, "garbage reply -> ok=false with an error", JSON.stringify(zr4.json));
  const rs = await R.get("/api/state");
  ok(rs.llmCalls.some((e) => e.role === "fobzoom" && e.model === "strong-m" && e.ok) && rs.latency.fobzoom?.n >= 3, "llmCalls + latency have the fobzoom role");
} finally {
  if (process.env.CLW_TEST_LOG) console.log(log.slice(-5000));
  for (const p of procs) p.kill();
  mockServer.close();
}
console.log(`\ntest-fobzoom: ${pass} passed, ${fail} failed`);
if (fail) { console.log("--- sidecar log ---\n" + log.slice(-3000)); process.exit(1); }

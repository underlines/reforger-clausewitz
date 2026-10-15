// LLM-latency work (B3) checks, no API key, no game:   node dev/tools/test-lean.mjs
// Lean situation in the real call path + situation recording. Starts sidecars (own ports, scratch config/data) and a mock OpenAI-compatible server that records
// every request body. Posts the LARGE fixture (fixtures/large_situation.json, v4) as the game would.
//  1. config prompt.lean on (default): commander / planner / SITREP requests carry tables + the static block in the SYSTEM prompt, no raw arrays, contacts keep place/grid/size for the SITREP;
//     the rules still work on the raw situation (orders validated against raw squads, reply applied)
//  2. prompt.lean off: the same calls carry the raw situation (no lean marker); prompt.staticBlock off: static block heads the user message
//  3. CLW_RECORD_DIR: the raw situation of the faction is written to <dir>/<faction>.json once per 60 s
import http from "node:http";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { loginFetch } from "./test-login.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const PORTS = { lean: 18821, off: 18822, noStatic: 18823, mock: 18824 };
const URL_ = (k) => `http://127.0.0.1:${PORTS[k]}`;
let pass = 0, fail = 0;
const ok = (cond, name, extra = "") => { if (cond) { pass++; console.log("  ok   " + name); } else { fail++; console.log("  FAIL " + name + (extra ? "  -> " + extra : "")); } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const load = (f) => JSON.parse(fs.readFileSync(path.join(DIR, "fixtures", f), "utf8"));
const sit = load("large_situation.json"), manifest = load("large_manifest.json");
sit.manifest = manifest.hash;
const firstSquad = sit.squads.find((s) => !s.garrison && !s.humanLed && !s.reserved).id;

// ---------- mock OpenAI-compatible server ----------
const seen = []; // {kind, body}
const REPLY = { commander: JSON.stringify({ o: [[firstSquad, "ATTACK", "D", "go"]], n: [90, "quiet"] }), planner: JSON.stringify({ k: 1 }),
  sitrep: JSON.stringify({ headline: "Holding.", situation: "s", enemy: "e", mission: "m", execution: "x", logistics: "l" }) };
const mockServer = http.createServer((req, res) => {
  let b = "";
  req.on("data", (d) => (b += d));
  req.on("end", () => {
    const body = JSON.parse(b || "{}");
    const sys = String(body.messages?.[0]?.content ?? "");
    const kind = /HQ radio operator/.test(sys) ? "hq" : /economy and logistics planner/.test(sys) ? "planner" : /staff officer/.test(sys) ? "sitrep" : /HQ commander/.test(sys) ? "commander" : "other";
    seen.push({ kind, body });
    const text = REPLY[kind] ?? "{}";
    res.writeHead(200, { "content-type": "text/event-stream" });
    const chunk = (o) => res.write("data: " + JSON.stringify(o) + "\n\n");
    chunk({ choices: [{ delta: { role: "assistant", content: "" } }] });
    chunk({ choices: [{ delta: { content: text } }] });
    chunk({ choices: [], usage: { prompt_tokens: 100, completion_tokens: 20 } });
    res.write("data: [DONE]\n\n");
    res.end();
  });
}).listen(PORTS.mock, "127.0.0.1");
const sysOf = (s) => String(s.body.messages[0].content), usrOf = (s) => String(s.body.messages[1].content);
const of = (kind, from = 0) => seen.slice(from).filter((s) => s.kind === kind);

// ---------- sidecar instances ----------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-lean-"));
const recDir = path.join(tmp, "rec");
const procs = [];
let log = "";
function start(port, name, cfg, env = {}) {
  const cfgFile = path.join(tmp, name + ".json");
  fs.writeFileSync(cfgFile, JSON.stringify(cfg));
  const p = spawn(process.execPath, [path.join(DIR, "llm_sidecar.mjs")], { env: { ...process.env, CLW_PORT: String(port), CLW_LLM: "fake", CLW_CONFIG: cfgFile, CLW_DATA_DIR: path.join(tmp, name + "-data"), CLW_PLANNER_MIN_GAP_MS: "150", ...env }, stdio: ["ignore", "pipe", "pipe"] });
  p.stdout.on("data", (d) => (log += `[${name}] ` + d)); p.stderr.on("data", (d) => (log += `[${name}] ` + d));
  procs.push(p);
}
const api = (base) => ({
  post: async (p, body) => { const r = await fetch(base + p, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }); return { status: r.status, json: await r.json() }; },
  get: async (p) => (await fetch(base + p)).json(),
});
async function up(base) { for (let i = 0; i < 60; i++) { try { if ((await fetch(base + "/health")).ok) return true; } catch {} await sleep(100); } return false; }
async function waitFor(fn, ms = 6000) { for (let t = 0; t < ms; t += 50) { const v = await fn(); if (v) return v; await sleep(50); } return null; }
const provider = { provider: "openai-compatible", model: "strong-m", baseUrl: `http://127.0.0.1:${PORTS.mock}`, apiKey: "sk-test", timeoutSec: 8, memory: { enabled: false }, sitrep: { enabled: true, intervalSec: 60 }, triggers: { cooldownSec: 0 } };

/** Posts the large situation to a sidecar and returns the requests it caused: {commander, planner, sitrep}. */
async function round(name, base) {
  const A = api(base), from = seen.length;
  ok((await A.post("/manifest", manifest)).status === 200, name + ": manifest accepted");
  const r = await A.post("/orders", JSON.parse(JSON.stringify(sit)));
  ok(r.json.orders?.length === 1 && r.json.orders[0].squad === firstSquad, name + ": reply applied against the RAW squads", JSON.stringify(r.json).slice(0, 200));
  await waitFor(() => of("planner", from).length && of("sitrep", from).length);
  return { commander: of("commander", from)[0], planner: of("planner", from)[0], sitrep: of("sitrep", from)[0] };
}

try {
  start(PORTS.lean, "lean", provider, { CLW_RECORD_DIR: recDir });
  start(PORTS.off, "off", { ...provider, prompt: { lean: false } });
  start(PORTS.noStatic, "nostatic", { ...provider, prompt: { staticBlock: false } });
  ok(await up(URL_("lean")) && await up(URL_("off")) && await up(URL_("noStatic")), "three sidecars started");
  for (const k of ["lean", "off", "noStatic"]) await loginFetch(URL_(k));

  console.log("lean on (default)");
  const L = await round("lean", URL_("lean"));
  ok(L.commander && L.planner && L.sitrep, "commander, planner and SITREP calls happened");
  for (const [k, s] of Object.entries(L)) {
    if (!s) continue;
    ok(sysOf(s).includes("SITUATION ENCODING") && sysOf(s).trimEnd().endsWith("}") && sysOf(s).includes("MAP AND CATALOG (static)\n{"), `${k}: encoding note + static block at the end of the system prompt`);
    ok(!usrOf(s).includes("MAP AND CATALOG") && usrOf(s).includes('"squads":{"cols":["id"') && !usrOf(s).includes('"squads":[{') && !usrOf(s).includes('"structures"'), `${k}: user message = lean dynamic situation (tables, no structures)`);
    ok(usrOf(s).length < JSON.stringify(sit).length * 0.6, `${k}: user message is much smaller than the raw situation (${usrOf(s).length} vs ${JSON.stringify(sit).length})`);
  }
  const contactCols = (s) => JSON.parse(usrOf(s).split("\n")[1]).contacts.cols;
  ok(["place", "grid", "size"].every((c) => contactCols(L.sitrep).includes(c)), "SITREP call: contacts keep place / grid / size");
  ok(!/"catalog"/.test(sysOf(L.commander)) && /"catalog"/.test(sysOf(L.planner)), "orders call has no catalog, planner call has it in the static block");
  ok(L.commander.body.messages[0].role === "system" && L.planner.body.stream === true, "request shape unchanged (system + user, streaming)");

  console.log("lean off");
  const O = await round("off", URL_("off"));
  for (const [k, s] of Object.entries(O)) {
    if (!s) continue;
    ok(!/SITUATION ENCODING|MAP AND CATALOG/.test(sysOf(s) + usrOf(s)) && usrOf(s).startsWith('Current situation:\n{') && usrOf(s).includes('"squads":[{'), `${k}: raw situation, no lean markers`);
  }
  ok(usrOf(L.commander).length < usrOf(O.commander).length, "lean commander request is smaller");

  console.log("staticBlock off");
  const N = await round("nostatic", URL_("noStatic"));
  ok(N.commander && N.planner && N.sitrep, "calls happened");
  for (const [k, s] of Object.entries(N)) {
    if (!s) continue;
    ok(!sysOf(s).includes("MAP AND CATALOG (static)\n{") && usrOf(s).startsWith("MAP AND CATALOG (static)\n{") && usrOf(s).includes("\nCurrent situation:\n"), `${k}: static block heads the user message`);
  }

  console.log("situation recording");
  const f = path.join(recDir, "RHS_USAF.json");
  ok(await waitFor(() => fs.existsSync(f)), "CLW_RECORD_DIR/<faction>.json written");
  const rec = JSON.parse(fs.readFileSync(f, "utf8"));
  ok(rec.v === 4 && rec.faction === "RHS_USAF" && rec.squads.length === sit.squads.length && Array.isArray(rec.structures) && rec.catalog.length === sit.catalog.length && !rec.wake, "recorded the RAW situation as posted (structures + catalog descriptions, no wake)");
  const mt = fs.statSync(f).mtimeMs;
  await sleep(50);
  await api(URL_("lean")).post("/api/wake", { faction: "RHS_USAF" });
  await api(URL_("lean")).post("/orders", { ...JSON.parse(JSON.stringify(sit)), time: sit.time + 99 });
  await sleep(300);
  ok(fs.statSync(f).mtimeMs === mt && JSON.parse(fs.readFileSync(f, "utf8")).time === sit.time, "throttled: a second commander call within 60 s does not rewrite it");
  ok(!fs.existsSync(path.join(tmp, "off-data", "RHS_USAF.json")), "no recording without CLW_RECORD_DIR");
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

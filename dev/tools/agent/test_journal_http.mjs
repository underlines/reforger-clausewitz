// Second-instance HTTP test of the HQ journal with the fake provider. node agent/test_journal_http.mjs [port]
// Starts llm_sidecar.mjs from this checkout on another port (own config file in the OS temp dir), posts situations, checks GET /api/state "journals",
// then stops ONLY the child it started.
import { spawn } from "node:child_process";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const PORT = Number(process.argv[2]) || 8799;
const DIR = path.dirname(fileURLToPath(import.meta.url));
const cfgFile = path.join(os.tmpdir(), `clw-journal-test-${process.pid}.json`);
fs.writeFileSync(cfgFile, JSON.stringify({ provider: "fake", memory: { enabled: true, reviewMin: 5 } }));
const child = spawn(process.execPath, [path.join(DIR, "..", "llm_sidecar.mjs")], { env: { ...process.env, CLW_PORT: String(PORT), CLW_CONFIG: cfgFile, CLW_LLM: "fake" }, stdio: ["ignore", "pipe", "pipe"] });
let out = "";
child.stdout.on("data", (d) => (out += d));
child.stderr.on("data", (d) => (out += d));
const base = `http://127.0.0.1:${PORT}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const post = async (u, b) => (await fetch(base + u, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(b) })).json();
const get = async (u) => (await fetch(base + u)).json();
const sit = (time, events = [], extra = {}) => ({ v: 2, faction: "US", side: 0, time, timeLeftSec: 3000, war: { phase: "running", tickets: 300, enemyTickets: 300, supplies: 50 },
  objectives: [{ id: "C", name: "Timber Hub", pos: [1000, 1000], radius: 80, owner: "none", progress: 0, side: "centre" }],
  bases: [{ faction: "US", pos: [100, 100] }], catalog: [], contacts: [], events,
  squads: [{ id: "US_Inf1", kind: "infantry", type: "rifle", pos: [200, 0, 200], agents: 8, startAgents: 8, humanLed: false, objective: null }], ...extra });
let ok = false;
try {
  for (let i = 0; i < 40; i++) { try { await get("/health"); break; } catch { await sleep(250); } }
  await post("/orders", sit(100));
  await sleep(300);
  let st = await get("/api/state");
  const j = st.journals.US;
  assert.ok(j, "journal exists");
  assert.equal(j.reviews, 1, "first review ran");
  assert.ok(j.intent.startsWith("Fake HQ"));
  assert.equal(j.goals.length, 2);
  // ATTACK order from the fake provider -> try; then outcome events
  const r = await post("/orders", sit(110, [], { contacts: [{ pos: [1000, 0, 1000], desc: "x", kind: "infantry", ageSec: 5 }] }));
  console.log("orders reply:", JSON.stringify(r.orders));
  await post("/orders", sit(300, ["outcome US_Inf1 ATTACK C failed: unit lost (after 190 s)"]));
  await post("/orders", sit(400, ["outcome US_Inf1 ATTACK C failed: stuck (after 90 s)"]));
  st = await get("/api/state");
  const t = st.journals.US.tries.find((x) => x.id === "C");
  console.log("tries C:", JSON.stringify(t), "block:\n" + st.journals.US.block);
  assert.ok(t && t.failed >= 2, "two failures counted");
  assert.match(st.journals.US.block, /^HQ JOURNAL/);
  // toggle off via config
  const c = await post("/api/config", { memory: { enabled: false } });
  assert.equal(c.config.memory.enabled, false);
  // reset: setup phase after running
  await post("/orders", sit(500, [], { war: { phase: "setup" } }));
  st = await get("/api/state");
  assert.equal(st.journals.US.reviews, 0, "journal reset at war start");
  assert.match(st.journals.US.resetReason, /setup/);
  ok = true;
  console.log("journal http test ok");
} catch (e) {
  console.log("FAILED:", e.message);
} finally {
  child.kill();
  fs.rmSync(cfgFile, { force: true });
  console.log("--- sidecar log ---\n" + out.split("\n").filter((l) => /journal|error/i.test(l)).join("\n"));
  process.exit(ok ? 0 : 1);
}

// HQ chat FAST PATH checks (POST /hq) against a scratch sidecar instance with the fake provider (no key, no game):
//   node dev/tools/agent/test_hq_fast.mjs
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { manifest, situation, messages } from "./fixtures/hq_fixture.mjs";
import { loginFetch } from "../test-login.mjs";

const DIR = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const PORT = 8790 + Math.floor(Math.random() * 90), BASE = `http://127.0.0.1:${PORT}`, TOKEN = "fast-test-token";
let pass = 0, fail = 0;
const ok = (c, n, x = "") => { if (c) { pass++; console.log("  ok   " + n); } else { fail++; console.log("  FAIL " + n + (x ? " -> " + x : "")); } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-hqfast-"));
const cfgFile = path.join(tmp, "cfg.json");
fs.writeFileSync(cfgFile, JSON.stringify({ gameToken: TOKEN, memory: { enabled: false }, sitrep: { enabled: false } }));
const proc = spawn(process.execPath, [path.join(DIR, "llm_sidecar.mjs")], { env: { ...process.env, CLW_PORT: String(PORT), CLW_LLM: "fake", CLW_CONFIG: cfgFile, CLW_DATA_DIR: path.join(tmp, "data") }, stdio: ["ignore", "pipe", "pipe"] });
let log = "";
proc.stdout.on("data", (d) => (log += d)); proc.stderr.on("data", (d) => (log += d));
const post = async (p, body, token = TOKEN) => { const r = await fetch(BASE + p, { method: "POST", headers: { "Content-Type": "application/json", ...(token ? { "X-CLW-Token": token } : {}) }, body: typeof body === "string" ? body : JSON.stringify(body) }); return { status: r.status, json: await r.json() }; };
const state = async () => (await fetch(BASE + "/api/state")).json();
let msgId = 900;
const mk = (key, extra = {}) => ({ ...messages[key], id: ++msgId, ...extra });

try {
  for (let i = 0; i < 60; i++) { try { if ((await fetch(BASE + "/health", { headers: { "X-CLW-Token": TOKEN } })).ok) break; } catch {} await sleep(100); }
  await loginFetch(BASE);
  await post("/manifest", manifest);

  ok((await post("/hq", mk("vehicle"), "")).status === 401, "/hq needs the game token");
  ok((await post("/hq", "not json")).status === 400, "invalid JSON -> 400");
  ok((await post("/hq", { faction: "US", msg: { id: 1 } })).status === 400, "message without text -> 400");

  const cold = await post("/hq", { faction: "US", msg: mk("vehicle") });
  ok(cold.status === 200 && cold.json.handled === false && /no situation/.test(cold.json.reason), "no cached situation -> handled:false", JSON.stringify(cold.json));

  const sit = { ...situation, manifest: manifest.hash, hq: [] };
  await post("/orders", sit); // the situation is cached on arrival

  // 1) fast path answers on the request itself
  const m1 = mk("vehicle");
  const t0 = Date.now();
  const f1 = await post("/hq", { faction: "US", msg: m1 });
  ok(f1.json.handled === true && f1.json.hqReplies?.length === 1 && f1.json.hqReplies[0].msgId === m1.id, "fast: handled with the reply for the message", JSON.stringify(f1.json));
  ok(f1.json.hqActions?.some((a) => a.msgId === m1.id && a.action === "REQUEST_VEHICLE"), "fast: hqActions carry the REQUEST_VEHICLE action", JSON.stringify(f1.json.hqActions));
  ok(Date.now() - t0 < 5000, "fast: answered within seconds (" + (Date.now() - t0) + " ms)");

  // 2) dedupe: neither the same fast request nor the situation path answers it again
  const again = await post("/hq", { faction: "US", msg: m1 });
  ok(again.json.handled === false && /already/.test(again.json.reason), "fast again: handled:false (dedupe)", JSON.stringify(again.json));
  const sp = await post("/orders", { ...sit, hq: [m1] });
  await sleep(300);
  const sp2 = await post("/orders", { ...sit, hq: [m1] });
  ok(!sp.json.hqReplies && !sp2.json.hqReplies && !sp.json.hqActions && !sp2.json.hqActions, "situation path after fast: no second answer", JSON.stringify([sp.json.hqReplies, sp2.json.hqReplies]));

  // 3) reverse: started by the situation path -> fast declines, the /orders reply still delivers it
  const m2 = mk("ammo");
  await post("/orders", { ...sit, hq: [m2] });
  const decl = await post("/hq", { faction: "US", msg: m2 });
  ok(decl.json.handled === false && /already/.test(decl.json.reason), "situation first: fast declines", JSON.stringify(decl.json));
  let got = null;
  for (let i = 0; i < 40 && !got; i++) { const r = await post("/orders", { ...sit, hq: [m2] }); if (r.json.hqReplies?.length) got = r.json; else await sleep(100); }
  ok(got?.hqReplies?.[0]?.msgId === m2.id, "situation path still delivers its message", JSON.stringify(got));

  // 4) fast result is not queued for the next /orders reply
  const m3 = mk("recon");
  const f3 = await post("/hq", { faction: "US", msg: m3 });
  ok(f3.json.handled === true && f3.json.hqReplies?.[0]?.msgId === m3.id, "second fast message handled");
  const after = await post("/orders", { ...sit, hq: [] });
  ok(!after.json.hqReplies && !after.json.hqActions, "no leftover hqReplies on the next /orders reply", JSON.stringify(after.json.hqReplies));

  // 5) per faction: another faction without a cached situation is declined
  const other = await post("/hq", { faction: "USSR", msg: mk("contacts") });
  ok(other.json.handled === false, "other faction without situation -> handled:false");

  const st = await state();
  const e1 = st.hq.find((h) => h.msg.id === m1.id), e2 = st.hq.find((h) => h.msg.id === m2.id);
  ok(e1?.path === "fast" && e1.state === "done" && e1.ok, "dashboard log: fast entry done", JSON.stringify(e1 && { path: e1.path, state: e1.state }));
  ok(e2?.path === "situation", "dashboard log: situation entry marked", e2?.path);
  ok(/hq fast faction=US msg=\d+ .* \d+ms/.test(log), "log line 'hq fast faction=... msg=... ms'");
  ok(st.hq.filter((h) => h.msg.id === m1.id).length === 1, "message ran exactly once");
} finally {
  proc.kill();
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);

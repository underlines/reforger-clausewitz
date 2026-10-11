// Clausewitz LLM bridge sidecar + read-only web dashboard. Plain Node 18+, no npm deps.
//   node dev/tools/llm_sidecar.mjs
// Env (initial defaults only; saved settings in dev/tools/sidecar-config.json win):
//   CLW_PORT (default 8788), CLW_LLM=fake|anthropic|openai-compatible (default fake),
//   ANTHROPIC_API_KEY, CLW_MODEL (default claude-sonnet-5-5)
// Listens on 127.0.0.1 only.
//
// Game API:
//   GET  /health -> {"ok":true,"mode":"<provider>"}
//   POST /orders body:
//     {"faction":"US","time":123,
//      "squads":[{"id":"US_Squad1","pos":[x,y,z],"agents":8,"startAgents":8,
//                 "mode":"NONE|SEARCH|ATTACK|DEFEND","humanLed":false}],
//      "contacts":[{"pos":[x,y,z],"desc":"2x infantry","ageSec":12}]}
//     -> 200 {"orders":[{"squad":"US_Squad1","command":"MOVE|ATTACK|DEFEND","pos":[x,z],"reason":"..."}]}
//   Any LLM/validation failure, or the panel's "enabled" switch off -> 200 {"orders":[]}
//   (game falls back to its rule commander). Unparsable request body -> 400.
//   Cost control (real providers only, not "fake"): settings minIntervalSec (at most one provider call per
//   faction per N s, 0 = off) and skipUnchanged (skip the call when squads/contacts are materially unchanged:
//   same ids/modes/agents, every squad and contact moved < 75 m, same contact count). A skipped request
//   answers 200 {"orders":[],"keep":true}: the game keeps its current orders (no rule fallback).
//
// Dashboard (http://127.0.0.1:8788/, static files from dev/tools/dashboard/):
//   GET  /api/state   -> factions, latest situations/orders, recent exchanges (ring of 100), counters, config (no secrets)
//   GET  /api/config  -> current settings; apiKey is reported only as {apiKeySet, apiKeyLast4}
//   POST /api/config  -> change settings (JSON, localhost Origin/Host only), persisted to sidecar-config.json
//   POST /api/test    -> send a tiny canned situation through the current provider -> {ok,orders,error,latencyMs}
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const DASH_DIR = path.join(DIR, "dashboard");
const CONFIG_FILE = path.join(DIR, "sidecar-config.json");
const PORT = Number(process.env.CLW_PORT) || 8788;
const COMMANDS = new Set(["MOVE", "ATTACK", "DEFEND"]);
const PROVIDERS = ["fake", "anthropic", "openai-compatible"];
const RING_MAX = 100;
const isNum = (n) => typeof n === "number" && Number.isFinite(n);

const SYSTEM = `You are the HQ commander of one faction in a tactical war simulation (Arma Reforger).
You see only what your units have reported (fog of war): the contact list may be old or incomplete.
You command squads by giving at most one order per squad. Allowed commands, and only these:
- MOVE: redeploy the squad to pos [x, z]
- ATTACK: assault pos [x, z]
- DEFEND: hold pos [x, z]
Squads with humanLed=true are led by players; avoid ordering them unless clearly useful.
Weak squads (agents well below startAgents) should not attack. Squad positions are [x, y, z]; order positions are [x, z].
Reply with ONLY a JSON object, no prose, no code fences:
{"orders":[{"squad":"<squad id from the situation>","command":"MOVE|ATTACK|DEFEND","pos":[x,z],"reason":"short text"}]}
Use an empty orders array if no action is needed.`;

// ---------- config ----------
const cfg = {
  provider: PROVIDERS.includes(process.env.CLW_LLM) ? process.env.CLW_LLM : "fake",
  model: process.env.CLW_MODEL || "claude-sonnet-5-5",
  baseUrl: "",
  apiKey: process.env.ANTHROPIC_API_KEY || "",
  timeoutSec: 20,
  maxTokens: 1024,
  enabled: true,
  minIntervalSec: 0,
  skipUnchanged: true,
};

function validateConfig(inp) {
  if (!inp || typeof inp !== "object" || Array.isArray(inp)) return { error: "body must be a JSON object" };
  const out = {};
  for (const [k, v] of Object.entries(inp)) {
    switch (k) {
      case "provider":
        if (!PROVIDERS.includes(v)) return { error: `provider must be one of ${PROVIDERS.join(", ")}` };
        out.provider = v; break;
      case "model":
        if (typeof v !== "string" || v.length > 200) return { error: "model must be a string (max 200)" };
        out.model = v.trim(); break;
      case "baseUrl": {
        if (typeof v !== "string" || v.length > 300) return { error: "baseUrl must be a string" };
        const t = v.trim();
        if (t) {
          let u; try { u = new URL(t); } catch { return { error: "baseUrl is not a valid URL" }; }
          if (u.protocol !== "http:" && u.protocol !== "https:") return { error: "baseUrl must be http(s)" };
        }
        out.baseUrl = t.replace(/\/+$/, ""); break;
      }
      case "apiKey":
        if (typeof v !== "string" || v.length > 500 || /[\r\n]/.test(v)) return { error: "apiKey must be a single-line string" };
        out.apiKey = v.trim(); break;
      case "timeoutSec":
        if (!isNum(v) || v < 1 || v > 300) return { error: "timeoutSec must be a number 1..300" };
        out.timeoutSec = v; break;
      case "maxTokens":
        if (!Number.isInteger(v) || v < 16 || v > 32000) return { error: "maxTokens must be an integer 16..32000" };
        out.maxTokens = v; break;
      case "enabled":
        if (typeof v !== "boolean") return { error: "enabled must be boolean" };
        out.enabled = v; break;
      case "minIntervalSec":
        if (!isNum(v) || v < 0 || v > 600) return { error: "minIntervalSec must be a number 0..600" };
        out.minIntervalSec = v; break;
      case "skipUnchanged":
        if (typeof v !== "boolean") return { error: "skipUnchanged must be boolean" };
        out.skipUnchanged = v; break;
      default:
        return { error: `unknown setting ${k}` };
    }
  }
  return { value: out };
}

try {
  const saved = validateConfig(JSON.parse(fs.readFileSync(CONFIG_FILE, "utf8")));
  if (saved.value) Object.assign(cfg, saved.value);
  else console.log(`ignoring ${CONFIG_FILE}: ${saved.error}`);
} catch (e) {
  if (e.code !== "ENOENT") console.log(`could not read ${CONFIG_FILE}: ${e.message}`);
}

function saveConfig() {
  try { fs.writeFileSync(CONFIG_FILE, JSON.stringify(cfg, null, 2)); return true; }
  catch (e) { console.log(`could not save config: ${e.message}`); return false; }
}

function publicConfig() {
  const { apiKey, ...rest } = cfg;
  return { ...rest, apiKeySet: !!apiKey, apiKeyLast4: apiKey ? apiKey.slice(-4) : "" };
}

// ---------- state ----------
const state = {
  startedAt: Date.now(),
  factions: {},   // name -> {ts, situation, orders}
  exchanges: [],  // newest first, max RING_MAX
  counters: { requests: 0, skipped: 0, errors: 0, fallbacks: 0, inputTokens: 0, outputTokens: 0 },
  lastRequestTs: 0,
};

// ---------- providers ----------
function fakeOrders(sit) {
  const orders = [];
  const contacts = Array.isArray(sit.contacts) ? sit.contacts.filter((c) => Array.isArray(c?.pos) && isNum(c.pos[0]) && isNum(c.pos[2])) : [];
  if (!contacts.length) return orders;
  for (const s of sit.squads) {
    if (!Array.isArray(s?.pos) || !isNum(s.pos[0]) || !isNum(s.pos[2])) continue;
    if (s.agents < s.startAgents * 0.5) {
      orders.push({ squad: s.id, command: "DEFEND", pos: [s.pos[0], s.pos[2]], reason: "weak squad, holding position" });
      continue;
    }
    let best = null, bd = Infinity;
    for (const c of contacts) {
      const d = Math.hypot(c.pos[0] - s.pos[0], c.pos[2] - s.pos[2]);
      if (d < bd) { bd = d; best = c; }
    }
    orders.push({ squad: s.id, command: "ATTACK", pos: [best.pos[0], best.pos[2]], reason: `nearest contact: ${best.desc ?? "unknown"}` });
  }
  return orders;
}

// Shared by both providers: pull the JSON object out of the reply text.
function extractOrders(text) {
  const a = text.indexOf("{"), b = text.lastIndexOf("}");
  if (a < 0 || b < a) throw new Error("no JSON object in reply");
  const obj = JSON.parse(text.slice(a, b + 1));
  if (!Array.isArray(obj.orders)) throw new Error("reply has no orders array");
  return obj.orders;
}

// Returns {raw, usage:{input,output}}. Throws Error on transport/HTTP failure.
async function callProvider(c, sit) {
  const userMsg = "Current situation:\n" + JSON.stringify(sit);
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), c.timeoutSec * 1000);
  try {
    let url, headers, body;
    if (c.provider === "anthropic") {
      if (!c.apiKey) throw new Error("API key not set");
      url = (c.baseUrl || "https://api.anthropic.com") + "/v1/messages";
      headers = { "x-api-key": c.apiKey, "anthropic-version": "2023-06-01", "content-type": "application/json" };
      body = { model: c.model, max_tokens: c.maxTokens, system: SYSTEM, messages: [{ role: "user", content: userMsg }] };
    } else {
      if (!c.baseUrl) throw new Error("baseUrl not set");
      url = c.baseUrl + "/chat/completions";
      headers = { "content-type": "application/json" };
      if (c.apiKey) headers.authorization = "Bearer " + c.apiKey;
      body = { model: c.model, max_tokens: c.maxTokens, messages: [{ role: "system", content: SYSTEM }, { role: "user", content: userMsg }] };
    }
    const r = await fetch(url, { method: "POST", signal: ac.signal, headers, body: JSON.stringify(body) });
    if (!r.ok) {
      const err = new Error(`HTTP ${r.status}: ${(await r.text()).slice(0, 200)}`);
      if (r.status === 429 || r.status >= 500) err.cooldownMs = Math.max(RATE_COOLDOWN_MS, (Number(r.headers.get("retry-after")) || 0) * 1000);
      throw err;
    }
    const j = await r.json();
    let text, usage;
    if (c.provider === "anthropic") {
      text = (j.content ?? []).filter((b) => b.type === "text").map((b) => b.text).join("");
      usage = { input: j.usage?.input_tokens ?? 0, output: j.usage?.output_tokens ?? 0 };
    } else {
      text = j.choices?.[0]?.message?.content ?? "";
      usage = { input: j.usage?.prompt_tokens ?? 0, output: j.usage?.completion_tokens ?? 0 };
    }
    return { raw: text, usage };
  } finally {
    clearTimeout(timer);
  }
}

function validate(raw, sit, dropped = []) {
  const ids = new Set(sit.squads.map((s) => s?.id));
  const seen = new Set(), out = [];
  for (const o of raw) {
    const why = !o || typeof o !== "object" ? "not an object"
      : !ids.has(o.squad) ? `unknown squad ${o.squad}`
      : seen.has(o.squad) ? `duplicate order for ${o.squad}`
      : !COMMANDS.has(o.command) ? `bad command ${o.command}`
      : !Array.isArray(o.pos) || o.pos.length !== 2 || !isNum(o.pos[0]) || !isNum(o.pos[1]) ? "bad pos"
      : typeof o.reason !== "string" ? "bad reason" : null;
    if (why) { console.log(`  dropped order: ${why}`); dropped.push({ order: o, reason: why }); continue; }
    seen.add(o.squad);
    out.push({ squad: o.squad, command: o.command, pos: [o.pos[0], o.pos[1]], reason: o.reason.slice(0, 120) });
  }
  return out;
}

// After a 429/5xx the provider is skipped for a while; the game falls back to the rule commander meanwhile.
const RATE_COOLDOWN_MS = 20000;
let cooldownUntil = 0;

// Runs one situation through the given config. Never throws.
async function computeOrders(c, sit, isTest = false) {
  const r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: null };
  try {
    if (!isTest && c.provider !== "fake" && Date.now() < cooldownUntil) throw new Error(`provider cooling down after rate limit/server error (${Math.ceil((cooldownUntil - Date.now()) / 1000)}s left)`);
    if (!sit || typeof sit !== "object" || !Array.isArray(sit.squads)) throw new Error("no squads array");
    let raw;
    if (c.provider === "fake") raw = fakeOrders(sit);
    else {
      const p = await callProvider(c, sit);
      r.rawReply = p.raw; r.usage = p.usage;
      raw = extractOrders(p.raw);
    }
    r.orders = validate(raw, sit, r.dropped);
  } catch (e) {
    if (e?.cooldownMs && !isTest) cooldownUntil = Date.now() + e.cooldownMs;
    r.ok = false;
    r.reason = e?.name === "AbortError" ? "timeout" : String(e?.message ?? e);
    r.orders = [];
    console.log(`  error: ${r.reason}`);
  }
  return r;
}

// Materially unchanged = same squad ids/modes/agent counts, same contact count, nothing moved >= 75 m.
const MOVE_EPS = 75;
function moved(a, b) { return !(Array.isArray(a) && Array.isArray(b)) || Math.hypot((a[0] ?? 0) - (b[0] ?? 0), (a[2] ?? 0) - (b[2] ?? 0)) >= MOVE_EPS; }
function unchanged(a, b) {
  if (!a || !b || !Array.isArray(a.squads) || !Array.isArray(b.squads) || a.squads.length !== b.squads.length) return false;
  const old = new Map(a.squads.map((s) => [s?.id, s]));
  for (const s of b.squads) {
    const o = old.get(s?.id);
    if (!o || o.mode !== s.mode || o.agents !== s.agents || !!o.humanLed !== !!s.humanLed || moved(o.pos, s.pos)) return false;
  }
  const ca = Array.isArray(a.contacts) ? a.contacts : [], cb = Array.isArray(b.contacts) ? b.contacts : [];
  if (ca.length !== cb.length) return false;
  const used = new Set();
  for (const c of cb) { // each new contact must match an unused old one within 75 m
    const i = ca.findIndex((o, k) => !used.has(k) && !moved(o?.pos, c?.pos));
    if (i < 0) return false;
    used.add(i);
  }
  return true;
}
const lastCall = {}; // faction -> {ts of last real attempt, sit of last successful real call}

// ---------- http helpers ----------
function send(res, code, obj) {
  res.writeHead(code, { "Content-Type": "application/json", "Cache-Control": "no-store" });
  res.end(JSON.stringify(obj));
}

const MIME = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".mjs": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".woff2": "font/woff2", ".png": "image/png", ".json": "application/json",
};
function serveStatic(res, name) {
  fs.readFile(path.join(DASH_DIR, name), (err, data) => {
    if (err) return send(res, 404, { error: "not found" });
    res.writeHead(200, { "Content-Type": MIME[path.extname(name)] ?? "application/octet-stream", "Cache-Control": "no-store" });
    res.end(data);
  });
}

const LOCAL_HOST = /^(localhost|127\.0\.0\.1)(:\d+)?$/i;
const LOCAL_ORIGIN = /^https?:\/\/(localhost|127\.0\.0\.1)(:\d+)?$/i;
const hostOk = (req) => LOCAL_HOST.test(req.headers.host ?? "");
const originOk = (req) => !req.headers.origin || LOCAL_ORIGIN.test(req.headers.origin);

function readBody(req, limit = 2_000_000) {
  return new Promise((resolve, reject) => {
    let body = "";
    req.on("data", (c) => { body += c; if (body.length > limit) { reject(new Error("body too large")); req.destroy(); } });
    req.on("end", () => resolve(body));
    req.on("error", reject);
  });
}

// ---------- handlers ----------
async function handleOrders(req, res) {
  let sit;
  try { sit = JSON.parse(await readBody(req)); } catch { return send(res, 400, { error: "invalid JSON" }); }
  const t0 = Date.now();
  const snapshot = { ...cfg };
  const faction = typeof sit?.faction === "string" ? sit.faction : "?";
  const lc = (lastCall[faction] ??= { ts: 0, sit: null });
  let skip = null;
  if (snapshot.enabled && snapshot.provider !== "fake" && sit && Array.isArray(sit.squads)) {
    if (snapshot.minIntervalSec > 0 && Date.now() - lc.ts < snapshot.minIntervalSec * 1000) skip = "skipped: interval";
    else if (snapshot.skipUnchanged && lc.sit && unchanged(lc.sit, sit)) skip = "skipped: unchanged";
  }
  let r;
  if (!snapshot.enabled) r = { orders: [], dropped: [], rawReply: null, usage: null, ok: false, reason: "disabled" };
  else if (skip) r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: skip, skipped: true };
  else {
    if (snapshot.provider !== "fake") lc.ts = Date.now();
    r = await computeOrders(snapshot, sit);
    if (r.ok && snapshot.provider !== "fake") lc.sit = sit;
  }
  const latencyMs = Date.now() - t0;

  state.counters.requests++;
  if (r.skipped) state.counters.skipped++;
  if (!r.ok && r.reason !== "disabled") state.counters.errors++;
  if (!r.skipped && (!r.ok || r.orders.length === 0)) state.counters.fallbacks++;
  if (r.usage) { state.counters.inputTokens += r.usage.input; state.counters.outputTokens += r.usage.output; }
  state.lastRequestTs = Date.now();
  if (sit && typeof sit === "object") {
    // Unknown squad fields are passed through untouched (situation is stored as received).
    // prevPos / orderTs are derived helpers for the Squads view (movement delta, order age).
    const old = state.factions[faction];
    const prevPos = {}, orderTs = { ...(old?.orderTs ?? {}) };
    for (const s of old?.situation?.squads ?? []) {
      if (s && Array.isArray(s.pos)) prevPos[s.id] = { pos: s.pos, ts: old.ts, time: old.situation.time };
    }
    for (const o of r.orders) orderTs[o.squad] = state.lastRequestTs;
    state.factions[faction] = { ts: state.lastRequestTs, situation: sit, orders: r.skipped ? (old?.orders ?? []) : r.orders, prevPos, orderTs };
  }
  state.exchanges.unshift({
    ts: state.lastRequestTs, faction, provider: snapshot.enabled ? snapshot.provider : "disabled",
    model: snapshot.provider === "fake" ? null : snapshot.model,
    request: sit, rawReply: r.rawReply, orders: r.orders, dropped: r.dropped,
    latencyMs, ok: r.ok, reason: r.reason, usage: r.usage,
  });
  if (state.exchanges.length > RING_MAX) state.exchanges.length = RING_MAX;

  const nContacts = Array.isArray(sit?.contacts) ? sit.contacts.length : 0;
  console.log(`orders faction=${faction} squads=${sit?.squads?.length ?? 0} contacts=${nContacts} orders=${r.orders.length} ${latencyMs}ms${r.skipped ? " (" + r.reason + ")" : ""}`);
  send(res, 200, r.skipped ? { orders: [], keep: true } : { orders: r.orders });
}

async function handleApi(req, res, url) {
  if (!hostOk(req)) return send(res, 403, { error: "forbidden host" });
  if (req.method === "GET" && url === "/api/state") {
    return send(res, 200, {
      now: Date.now(), startedAt: state.startedAt, lastRequestTs: state.lastRequestTs,
      factions: state.factions, exchanges: state.exchanges, counters: state.counters, config: publicConfig(),
    });
  }
  if (req.method === "GET" && url === "/api/config") return send(res, 200, publicConfig());
  if (req.method === "POST" && (url === "/api/config" || url === "/api/test")) {
    if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) return send(res, 400, { error: "Content-Type must be application/json" });
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    let inp;
    try { inp = JSON.parse((await readBody(req, 100_000)) || "{}"); } catch { return send(res, 400, { error: "invalid JSON" }); }
    const v = validateConfig(inp);
    if (v.error) return send(res, 400, { error: v.error });
    if (v.value.apiKey === "") delete v.value.apiKey; // empty = keep stored key
    if (url === "/api/config") {
      Object.assign(cfg, v.value);
      const saved = saveConfig();
      return send(res, 200, { ok: true, saved, config: publicConfig() });
    }
    // /api/test: optional overrides in the body are used for this test only (not saved)
    const tcfg = { ...cfg, ...v.value };
    const sit = {
      faction: "TEST", time: 0,
      squads: [{ id: "TEST_Squad1", pos: [1000, 0, 1000], agents: 8, startAgents: 8, mode: "NONE", humanLed: false }],
      contacts: [{ pos: [1500, 0, 1200], desc: "2x infantry", ageSec: 10 }],
    };
    const t0 = Date.now();
    const r = await computeOrders(tcfg, sit, true);
    return send(res, 200, { ok: r.ok, orders: r.orders, dropped: r.dropped, rawReply: r.rawReply, error: r.reason, latencyMs: Date.now() - t0, provider: tcfg.provider });
  }
  return send(res, 404, { error: "not found" });
}

http.createServer((req, res) => {
  const url = (req.url ?? "/").split("?")[0];
  if (req.method === "GET" && url === "/health") return send(res, 200, { ok: true, mode: cfg.provider });
  if (req.method === "POST" && url === "/orders") {
    return void handleOrders(req, res).catch((e) => { console.log(`  handler error: ${e?.message ?? e}`); if (!res.headersSent) send(res, 500, { error: "internal" }); });
  }
  if (url.startsWith("/api/")) {
    return void handleApi(req, res, url).catch((e) => { if (!res.headersSent) send(res, 500, { error: String(e?.message ?? e) }); });
  }
  if (req.method === "GET") {
    if (!hostOk(req)) return send(res, 403, { error: "forbidden host" });
    if (url === "/" || url === "/index.html") return serveStatic(res, "index.html");
    if (url === "/app.js") return serveStatic(res, "app.js");
    if (url === "/style.css") return serveStatic(res, "style.css");
    // vendored libraries: flat names only (no slashes, no dot-dot), known extensions
    const m = /^\/vendor\/([A-Za-z0-9][A-Za-z0-9._-]*)$/.exec(url);
    if (m && !m[1].includes("..") && MIME[path.extname(m[1]).toLowerCase()]) return serveStatic(res, "vendor/" + m[1]);
  }
  send(res, 404, { error: "not found" });
}).listen(PORT, "127.0.0.1", () => console.log(`clausewitz llm sidecar (${cfg.provider}${cfg.enabled ? "" : ", DISABLED"}) on http://127.0.0.1:${PORT}  (dashboard at /)`));

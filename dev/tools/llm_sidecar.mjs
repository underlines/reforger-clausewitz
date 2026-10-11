// Clausewitz LLM bridge sidecar + read-only web dashboard. Plain Node 18+, no npm deps.
//   node dev/tools/llm_sidecar.mjs
// Env (initial defaults only; saved settings in dev/tools/sidecar-config.json win):
//   CLW_PORT (default 8788), CLW_LLM=fake|anthropic|openai-compatible|openrouter (default fake),
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
// HQ scheduler (all providers, per faction): the game polls every 10 s but the LLM is only called on the first request, at the
//   scheduled time (model-chosen next_check_sec, clamped to wakeup.minSec..maxSec, else wakeup.defaultSec; x budget stretch factor),
//   when a trigger fires (config "triggers": new contact, contact near squad, casualties, squad lost, strength drop, objective
//   reached, squad change; min gap triggers.cooldownSec) or after POST /api/wake {faction?}. minIntervalSec is the hard floor
//   between any two calls. Other polls answer {"orders":[],"keep":true} without an exchange entry (counted as counters.gated).
//   The model sees a "wake" object {reason,triggers,sinceLastSec}; /api/state has "scheduler" per faction.
//
// OpenRouter provider ("openrouter", baseUrl defaults to https://openrouter.ai/api/v1; a saved "openai-compatible" config
//   pointing at openrouter.ai is migrated automatically): each request adds usage:{include:true} and records usage.cost (USD).
//   Local metrics (session cost/requests/tokens, cost by model and by faction; reset on restart) are in /api/state "usage".
//   Key budget: GET <baseUrl>/key is polled at startup, after a config save, every 10 min (60 s while the limit is reached)
//   and on POST /api/usage/refresh. Warnings at 80 % / 95 % of the key limit (polled value minus local cost since the poll);
//   at 100 % and setting stopAtLimit (default true) real provider calls stop: /orders answers {"orders":[]} (reason
//   "budget limit reached", counted as fallback) until a refresh shows budget again.
//
// World / map (see "world info" below): game POSTs /world {worldFile,mapPrefab,mapOffset,mapSize,boundsMin,boundsMax} -> {ok,worldKey,needRoads,needLocations},
//   /world/roads (chunks) and /world/locations. Dashboard: GET /api/world, /api/map.png, /api/vector; POST /api/map/reload, /api/vector/reset.
//   Cache in dev/tools/.cache/ (world.json, vector/<worldKey>.json, maps/). Overrides: dev/tools/maps/<World>.png + .json.
//
// Dashboard (http://127.0.0.1:8788/, static files from dev/tools/dashboard/):
//   GET  /api/state   -> factions, latest situations/orders, recent exchanges (ring of 100), counters, config (no secrets)
//   GET  /api/config  -> current settings; apiKey is reported only as {apiKeySet, apiKeyLast4}
//   POST /api/config  -> change settings (JSON, localhost Origin/Host only), persisted to sidecar-config.json
//   POST /api/usage/refresh -> re-poll the OpenRouter key budget now -> {ok,usage}
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
const PROVIDERS = ["fake", "anthropic", "openai-compatible", "openrouter"];
// HQ engine: llm = LLM provider decides; rules = sidecar answers {"orders":[]} so the game's rule commander drives (no LLM calls);
// model = future trained decision-model tier (not implemented: treated like rules, logged once).
const ENGINES = ["llm", "rules", "model"];
let paused = false; // runtime only (not persisted): no LLM calls, every poll answers {"orders":[],"keep":true}
let modelEngineLogged = false;
const OR_BASE = "https://openrouter.ai/api/v1";
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
Use an empty orders array if no action is needed.
The situation may contain a "wake" object telling you why you are being consulted now: reason first|scheduled|trigger|manual,
triggers (short descriptions of what happened since your last decision) and sinceLastSec. React to the triggers first.`;

// System prompt incl. the wakeup paragraph (only when the model may choose its next wakeup).
function systemPrompt(c) {
  if (!c.wakeup?.dynamic) return SYSTEM;
  const w = c.wakeup;
  return SYSTEM + `
You are not polled continuously: you also choose when you are consulted next. Add to the JSON object the optional fields
"next_check_sec" (integer seconds, allowed range ${w.minSec}..${w.maxSec}, default ${w.defaultSec}) and "next_check_reason" (short text), e.g.
{"orders":[...],"next_check_sec":120,"next_check_reason":"quiet, no contacts"}.
You are woken early anyway when something important happens (new contact, casualties, lost squad, objective reached), so choose
long intervals in quiet phases and short ones during fights or while an assault is developing.`;
}

// ---------- config ----------
const cfg = {
  provider: PROVIDERS.includes(process.env.CLW_LLM) ? process.env.CLW_LLM : "fake",
  model: process.env.CLW_MODEL || "claude-sonnet-5-5",
  baseUrl: "",
  apiKey: process.env.ANTHROPIC_API_KEY || "",
  timeoutSec: 20,
  maxTokens: 1024,
  enabled: true,
  engine: "llm",
  minIntervalSec: 0,
  skipUnchanged: true,
  stopAtLimit: true,
  gameDir: "",       // Arma Reforger install dir for the map image; empty = auto-detect
  mapMaxSize: 2048,  // longest side of the served map PNG
  // HQ scheduler: how often the LLM is called (see "HQ scheduler" below)
  wakeup: { dynamic: true, defaultSec: 60, minSec: 20, maxSec: 300, budgetStretch: true },
  // events that wake the HQ before its scheduled time (each tunable; 0 / false = off)
  triggers: { enabled: true, newContact: true, contactNearSquadM: 300, casualties: 2, squadLost: true, strengthDropPct: 25, objectiveReached: true, squadChange: true, cooldownSec: 15 },
};

// Group settings: "bool" or [min, max] number range per key. Partial objects are merged onto the current group.
const GROUP_SPECS = {
  wakeup: { dynamic: "bool", defaultSec: [5, 3600], minSec: [5, 3600], maxSec: [5, 7200], budgetStretch: "bool" },
  triggers: { enabled: "bool", newContact: "bool", contactNearSquadM: [0, 5000], casualties: [0, 1000], squadLost: "bool", strengthDropPct: [0, 100], objectiveReached: "bool", squadChange: "bool", cooldownSec: [0, 600] },
};
function validateGroup(name, v) {
  if (!v || typeof v !== "object" || Array.isArray(v)) return { error: `${name} must be an object` };
  const spec = GROUP_SPECS[name], out = { ...cfg[name] };
  for (const [k, x] of Object.entries(v)) {
    const s = spec[k];
    if (!s) return { error: `unknown setting ${name}.${k}` };
    if (s === "bool") { if (typeof x !== "boolean") return { error: `${name}.${k} must be boolean` }; }
    else if (!isNum(x) || x < s[0] || x > s[1]) return { error: `${name}.${k} must be a number ${s[0]}..${s[1]}` };
    out[k] = x;
  }
  if (name === "wakeup" && out.minSec > out.maxSec) return { error: "wakeup.minSec must be <= wakeup.maxSec" };
  return { value: out };
}

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
      case "engine":
        if (!ENGINES.includes(v)) return { error: `engine must be one of ${ENGINES.join(", ")}` };
        out.engine = v; break;
      case "minIntervalSec":
        if (!isNum(v) || v < 0 || v > 600) return { error: "minIntervalSec must be a number 0..600" };
        out.minIntervalSec = v; break;
      case "skipUnchanged":
        if (typeof v !== "boolean") return { error: "skipUnchanged must be boolean" };
        out.skipUnchanged = v; break;
      case "stopAtLimit":
        if (typeof v !== "boolean") return { error: "stopAtLimit must be boolean" };
        out.stopAtLimit = v; break;
      case "gameDir":
        if (typeof v !== "string" || v.length > 400 || /[\r\n]/.test(v)) return { error: "gameDir must be a single-line string" };
        out.gameDir = v.trim(); break;
      case "mapMaxSize":
        if (!Number.isInteger(v) || v < 256 || v > 8192) return { error: "mapMaxSize must be an integer 256..8192" };
        out.mapMaxSize = v; break;
      case "wakeup":
      case "triggers": {
        const g = validateGroup(k, v);
        if (g.error) return g;
        out[k] = g.value; break;
      }
      default:
        return { error: `unknown setting ${k}` };
    }
  }
  return { value: out };
}

// A saved openai-compatible config that points at openrouter.ai is the openrouter provider (key is kept).
function migrateOpenRouter(c) {
  if (c.provider === "openai-compatible" && /openrouter\.ai/i.test(c.baseUrl)) { c.provider = "openrouter"; c.baseUrl = ""; console.log("migrated provider openai-compatible (openrouter.ai) -> openrouter"); }
}

try {
  const saved = validateConfig(JSON.parse(fs.readFileSync(CONFIG_FILE, "utf8")));
  if (saved.value) { Object.assign(cfg, saved.value); migrateOpenRouter(cfg); }
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
  counters: { requests: 0, gated: 0, skipped: 0, errors: 0, fallbacks: 0, inputTokens: 0, outputTokens: 0 },
  lastRequestTs: 0,
};

// ---------- OpenRouter usage / budget ----------
const POLL_MS = 10 * 60 * 1000, POLL_REACHED_MS = 60 * 1000;
const session = { cost: 0, requests: 0, tokensIn: 0, tokensOut: 0, byModel: {}, byFaction: {} };
const keyState = { data: null, ts: 0, error: null, polling: false, costSincePoll: 0 };
let budgetLevel = "ok"; // last logged level: ok | warn | crit | reached

function recordUsage(model, faction, u) {
  const cost = isNum(u?.cost) ? u.cost : 0;
  session.cost += cost; session.tokensIn += u?.input ?? 0; session.tokensOut += u?.output ?? 0;
  keyState.costSincePoll += cost;
  const add = (m, k) => { const e = (m[k] ??= { cost: 0, requests: 0 }); e.cost += cost; e.requests++; };
  add(session.byModel, model || "?"); add(session.byFaction, faction || "?");
  checkBudget();
}

// Budget of the active key limit: polled limit_remaining minus local cost since that poll. null = no limit known.
function budget() {
  const d = keyState.data;
  if (cfg.provider !== "openrouter" || !d || !isNum(d.limit) || d.limit <= 0 || !isNum(d.limit_remaining)) return null;
  const remaining = d.limit_remaining - keyState.costSincePoll;
  const pct = Math.max(0, Math.min(100, 100 * (d.limit - remaining) / d.limit));
  const level = remaining <= 0 ? "reached" : pct >= 95 ? "crit" : pct >= 80 ? "warn" : "ok";
  return { limit: d.limit, remaining, pct, level, resetting: d.limit_reset ?? null };
}
// Spending pace. Assumption: OpenRouter resets on UTC boundaries (daily 00:00, weekly Monday 00:00, monthly 1st 00:00).
// Period limit: projected = u / elapsedFraction (needs f >= 0.05 and u > 0). No reset: session burn rate (needs >= 5 min). No limit: $/h only.
function periodBounds(reset, now) {
  const d = new Date(now), y = d.getUTCFullYear(), m = d.getUTCMonth(), day = d.getUTCDate();
  if (reset === "daily") { const s = Date.UTC(y, m, day); return [s, s + 86400000]; }
  if (reset === "weekly") { const s = Date.UTC(y, m, day - ((d.getUTCDay() + 6) % 7)); return [s, s + 7 * 86400000]; }
  if (reset === "monthly") return [Date.UTC(y, m, 1), Date.UTC(y, m + 1, 1)];
  return null;
}
function pace() {
  const d = keyState.data;
  if (cfg.provider !== "openrouter" || !d) return null;
  const now = Date.now(), hours = (now - state.startedAt) / 3600000;
  const rate = hours * 60 >= 5 && session.cost > 0 ? session.cost / hours : null;
  const hasLimit = isNum(d.limit) && d.limit > 0;
  if (!hasLimit) return { kind: "rate", ratePerHour: rate };
  const bounds = periodBounds(d.limit_reset, now);
  if (!bounds) {
    const remaining = (d.limit_remaining ?? (d.limit - (d.usage ?? 0))) - keyState.costSincePoll;
    return { kind: "rate", ratePerHour: rate, hoursToLimit: rate && remaining > 0 ? remaining / rate : null };
  }
  const [start, end] = bounds, f = (now - start) / (end - start);
  const u = (d[{ daily: "usage_daily", weekly: "usage_weekly", monthly: "usage_monthly" }[d.limit_reset]] ?? 0) + keyState.costSincePoll;
  const out = { kind: "period", reset: d.limit_reset, elapsed: f, expected: d.limit * f, used: u, resetsInMs: end - now, projected: null, delta: null };
  if (f >= 0.05 && u > 0) { out.projected = u / f; out.delta = out.projected - d.limit; }
  return out;
}
const LEVELS = ["ok", "warn", "crit", "reached"];
function checkBudget() {
  const b = budget(), level = b ? b.level : "ok";
  if (level !== budgetLevel) {
    if (LEVELS.indexOf(level) > LEVELS.indexOf(budgetLevel)) {
      console.log(`BUDGET ${level === "reached" ? "LIMIT REACHED" : level === "crit" ? "95% used" : "80% used"}: $${(b.limit - b.remaining).toFixed(4)} of $${b.limit} (${b.pct.toFixed(1)}%)${level === "reached" && cfg.stopAtLimit ? " - provider calls stopped" : ""}`);
    } else console.log(`budget back to ${level}`);
    budgetLevel = level;
  }
}

const KEY_FIELDS = ["usage", "usage_daily", "usage_weekly", "usage_monthly", "limit", "limit_remaining"];
async function pollKey() {
  if (cfg.provider !== "openrouter" || !cfg.apiKey || keyState.polling) return;
  keyState.polling = true;
  const ac = new AbortController(), timer = setTimeout(() => ac.abort(), 10000);
  try {
    const r = await fetch((cfg.baseUrl || OR_BASE) + "/key", { signal: ac.signal, headers: { authorization: "Bearer " + cfg.apiKey } });
    if (!r.ok) throw new Error(`HTTP ${r.status}: ${(await r.text()).slice(0, 150)}`);
    const d = (await r.json())?.data ?? {};
    const k = { label: typeof d.label === "string" ? d.label : "", limit_reset: typeof d.limit_reset === "string" ? d.limit_reset : null, is_free_tier: !!d.is_free_tier };
    for (const f of KEY_FIELDS) k[f] = isNum(d[f]) ? d[f] : null;
    if (k.limit !== null && k.limit_remaining === null) k.limit_remaining = k.limit - (k.usage ?? 0); // tolerate a missing field
    keyState.data = k; keyState.error = null; keyState.costSincePoll = 0;
  } catch (e) {
    keyState.error = e?.name === "AbortError" ? "timeout" : String(e?.message ?? e);
    console.log(`  key poll failed: ${keyState.error}`);
  } finally { clearTimeout(timer); keyState.polling = false; keyState.ts = Date.now(); checkBudget(); }
}
setInterval(() => {
  const age = Date.now() - keyState.ts;
  if (age >= POLL_MS || (budgetLevel === "reached" && age >= POLL_REACHED_MS)) void pollKey();
}, 15000).unref();

function usageView() {
  const on = cfg.provider === "openrouter";
  return {
    enabled: on, session, stopAtLimit: cfg.stopAtLimit,
    key: on ? { data: keyState.data, ts: keyState.ts, error: keyState.error, polling: keyState.polling, costSincePoll: keyState.costSincePoll, keySet: !!cfg.apiKey } : null,
    budget: on ? (budget() ? { ...budget(), pace: pace() } : null) : null,
    pace: on ? pace() : null, // also present without a limit (rate only)
  };
}
const budgetStopped = (c) => c.provider === "openrouter" && c.stopAtLimit && budget()?.level === "reached";

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
  return obj;
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
      body = { model: c.model, max_tokens: c.maxTokens, system: systemPrompt(c), messages: [{ role: "user", content: userMsg }] };
    } else {
      const base = c.provider === "openrouter" ? (c.baseUrl || OR_BASE) : c.baseUrl;
      if (!base) throw new Error("baseUrl not set");
      url = base + "/chat/completions";
      headers = { "content-type": "application/json" };
      if (c.apiKey) headers.authorization = "Bearer " + c.apiKey;
      body = { model: c.model, max_tokens: c.maxTokens, messages: [{ role: "system", content: systemPrompt(c) }, { role: "user", content: userMsg }] };
      if (c.provider === "openrouter") body.usage = { include: true };
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
      if (isNum(j.usage?.cost)) usage.cost = j.usage.cost;
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
      session.requests++;
      const p = await callProvider(c, sit);
      if (c.provider === "openrouter") recordUsage(c.model, typeof sit.faction === "string" ? sit.faction : "?", p.usage);
      r.rawReply = p.raw; r.usage = p.usage;
      const obj = extractOrders(p.raw);
      raw = obj.orders;
      if (isNum(obj.next_check_sec)) r.nextCheck = { sec: Math.round(obj.next_check_sec), reason: typeof obj.next_check_reason === "string" ? obj.next_check_reason.slice(0, 120) : "" };
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
const lastCall = {}; // faction -> {ts of last provider attempt}

// ---------- HQ scheduler ----------
// Per faction the LLM is called on: first request, scheduled due time (default/model-chosen interval), a trigger
// (something happened since the baseline = situation at the last call) or a manual wake. Otherwise the poll is gated
// (answered {"orders":[],"keep":true}, counted only). minIntervalSec is the hard floor between two calls.
const sched = {}; // faction -> scheduler state
function schedFor(f) {
  return (sched[f] ??= { baseline: null, lastLlmAt: 0, nextDueAt: 0, chosenSec: null, chosenReason: "", intervalSec: null, stretch: 1,
    lastWakeReason: null, lastWakeTriggers: [], gated: 0, manual: false, failed: false });
}
const d2 = (a, b) => Math.hypot((a[0] ?? 0) - (b[0] ?? 0), (a[a.length > 2 ? 2 : 1] ?? 0) - (b[b.length > 2 ? 2 : 1] ?? 0));
const okPos = (p) => Array.isArray(p) && p.length >= 2 && p.every(isNum);
const IDLE_MODES = new Set(["none", "idle"]);
const NEW_CONTACT_M = 150, OBJECTIVE_M = 50;

// Descriptions of what changed between the baseline situation and now, per the enabled triggers.
function evalTriggers(t, base, sit) {
  const out = [];
  const bs = new Map((base.squads ?? []).map((s) => [s?.id, s]));
  const cs = new Map((sit.squads ?? []).map((s) => [s?.id, s]));
  const bc = (Array.isArray(base.contacts) ? base.contacts : []).filter((c) => okPos(c?.pos));
  const cc = (Array.isArray(sit.contacts) ? sit.contacts : []).filter((c) => okPos(c?.pos));
  if (t.newContact) {
    for (const c of cc) if (!bc.some((o) => d2(o.pos, c.pos) <= NEW_CONTACT_M)) out.push(`new contact: ${c.desc ?? "unknown"}`);
  }
  if (t.contactNearSquadM > 0) {
    for (const [id, s] of cs) {
      if (!okPos(s?.pos)) continue;
      let best = null;
      for (const c of cc) {
        const d = d2(s.pos, c.pos);
        if (d > t.contactNearSquadM || (best && d >= best.d)) continue;
        const b = bs.get(id);
        const closeBefore = okPos(b?.pos) && bc.some((o) => d2(o.pos, c.pos) <= NEW_CONTACT_M && d2(b.pos, o.pos) <= t.contactNearSquadM);
        if (!closeBefore) best = { d, c };
      }
      if (best) out.push(`contact ${Math.round(best.d)} m from ${id}`);
    }
  }
  let lost = 0;
  for (const [id, b] of bs) {
    const s = cs.get(id), now = s && isNum(s.agents) ? s.agents : 0;
    if (isNum(b?.agents)) lost += Math.max(0, b.agents - now);
    if (t.squadLost && isNum(b?.agents) && b.agents > 0 && now <= 0) out.push(`${id} lost`);
    if (!s) continue;
    if (t.strengthDropPct > 0 && isNum(b.agents) && isNum(s.agents)) {
      const start = s.startAgents > 0 ? s.startAgents : b.startAgents;
      const pct = start > 0 ? 100 * (b.agents - s.agents) / start : 0;
      if (pct >= t.strengthDropPct && s.agents > 0) out.push(`${id} lost ${Math.round(pct)}% strength`);
    }
    if (t.objectiveReached) {
      const reached = (x) => okPos(x?.objective?.pos) && okPos(x?.pos) && d2(x.pos, x.objective.pos) <= OBJECTIVE_M;
      const idle = (x) => IDLE_MODES.has(String(x?.mode ?? "").toLowerCase());
      if ((reached(s) && !reached(b)) || (idle(s) && !idle(b))) out.push(`${id} reached objective`);
    }
    if (t.squadChange && !!b.humanLed !== !!s.humanLed) out.push(`${id} ${s.humanLed ? "now player-led" : "no longer player-led"}`);
  }
  if (t.casualties > 0 && lost >= t.casualties) out.push(`${lost} casualties`);
  if (t.squadChange) for (const id of cs.keys()) if (!bs.has(id)) out.push(`${id} appeared`);
  return out.slice(0, 8);
}

// Interval multiplier from the OpenRouter pace estimate: projected spend over the key limit stretches the interval (max x3).
function stretchFactor(c) {
  if (!c.wakeup.budgetStretch) return 1;
  const p = pace(), limit = keyState.data?.limit;
  if (p?.kind !== "period" || !isNum(p.projected) || !isNum(limit) || limit <= 0 || p.projected <= limit) return 1;
  return Math.min(p.projected / limit, 3);
}

// Next wakeup after a call (or a skipped scheduled check). chosen = model's next_check_sec or null.
function scheduleNext(c, sc, chosen, reason, now) {
  const w = c.wakeup;
  let sec = w.defaultSec;
  if (w.dynamic && isNum(chosen)) sec = Math.max(w.minSec, Math.min(w.maxSec, chosen));
  sc.chosenSec = sec; sc.chosenReason = w.dynamic && isNum(chosen) ? reason : "";
  sc.stretch = stretchFactor(c);
  sc.intervalSec = Math.round(sec * sc.stretch);
  sc.nextDueAt = now + sc.intervalSec * 1000;
}

// Decide for one poll: {call, reason, triggers} or {gate:true} or {skip:"..."}.
function decideWake(c, sc, lc, sit, now) {
  if (c.minIntervalSec > 0 && now - lc.ts < c.minIntervalSec * 1000) return { gate: true };
  if (sc.manual) return { call: true, reason: "manual", triggers: [] };
  if (!sc.baseline) return { call: true, reason: "first", triggers: [] };
  if (c.triggers.enabled && now - lc.ts >= c.triggers.cooldownSec * 1000) {
    const fired = evalTriggers(c.triggers, sc.baseline, sit);
    if (fired.length) return { call: true, reason: "trigger", triggers: fired };
  }
  if (now >= sc.nextDueAt) {
    if (c.skipUnchanged && unchanged(sc.baseline, sit)) return { skip: "skipped: unchanged" };
    return { call: true, reason: "scheduled", triggers: [] };
  }
  return { gate: true };
}

function schedulerView() {
  const now = Date.now(), out = {};
  for (const [f, sc] of Object.entries(sched)) {
    out[f] = {
      nextDueInSec: sc.baseline || sc.nextDueAt ? Math.max(0, Math.round((sc.nextDueAt - now) / 1000)) : 0,
      chosenSec: sc.chosenSec, intervalSec: sc.intervalSec, chosenReason: sc.chosenReason, stretch: sc.stretch,
      lastWakeReason: sc.lastWakeReason, lastWakeTriggers: sc.lastWakeTriggers, lastLlmAgoSec: sc.lastLlmAt ? Math.round((now - sc.lastLlmAt) / 1000) : null,
      gated: sc.gated, manual: sc.manual, failed: sc.failed,
    };
  }
  return out;
}

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
  const valid = !!sit && Array.isArray(sit.squads);
  const sc = schedFor(faction);
  let r, wake = null, interval = null;
  if (snapshot.enabled && snapshot.engine !== "llm") {
    // rules/model engine: game's rule commander drives; not an exchange, only counted as gated
    if (snapshot.engine === "model" && !modelEngineLogged) { modelEngineLogged = true; console.log("engine 'model' is not implemented yet: behaving like 'rules'"); }
    r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: "engine: " + snapshot.engine, gated: true, rules: true };
  } else if (snapshot.enabled && paused) {
    sc.gated++;
    r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: "paused", skipped: true, gated: true, paused: true };
  } else if (snapshot.enabled && valid && budgetStopped(snapshot)) r = { orders: [], dropped: [], rawReply: null, usage: null, ok: false, reason: "budget limit reached" };
  else if (!snapshot.enabled) r = { orders: [], dropped: [], rawReply: null, usage: null, ok: false, reason: "disabled" };
  else {
    const now = Date.now();
    const dec = valid ? decideWake(snapshot, sc, lc, sit, now) : { call: true, reason: "first", triggers: [] }; // invalid body: computeOrders reports the error
    if (dec.gate) {
      sc.gated++; // not an exchange: only counted; after a failed call the game keeps its rule fallback instead of "keep"
      r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: "gated", skipped: true, gated: true };
    } else if (dec.skip) {
      scheduleNext(snapshot, sc, sc.chosenSec, sc.chosenReason, now);
      r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: dec.skip, skipped: true };
    } else {
      wake = { reason: dec.reason, triggers: dec.triggers, sinceLastSec: lc.ts ? Math.round((now - lc.ts) / 1000) : 0 };
      lc.ts = now;
      if (valid) { sc.manual = false; sc.baseline = sit; sc.lastLlmAt = now; sc.lastWakeReason = wake.reason; sc.lastWakeTriggers = wake.triggers; }
      r = await computeOrders(snapshot, valid ? { ...sit, wake } : sit);
      if (valid) {
        sc.failed = !r.ok;
        if (r.ok) scheduleNext(snapshot, sc, r.nextCheck?.sec, r.nextCheck?.reason ?? "", Date.now());
        else { sc.chosenSec = snapshot.wakeup.minSec; sc.chosenReason = "retry after error"; sc.intervalSec = sc.chosenSec; sc.nextDueAt = Date.now() + sc.intervalSec * 1000; }
        interval = sc.intervalSec;
      }
    }
  }
  const latencyMs = Date.now() - t0;

  if (r.gated) state.counters.gated++;
  else {
    state.counters.requests++;
    if (r.skipped) state.counters.skipped++;
    if (!r.ok && r.reason !== "disabled" && r.reason !== "budget limit reached") state.counters.errors++;
    if (!r.skipped && (!r.ok || r.orders.length === 0)) state.counters.fallbacks++;
    if (r.usage) { state.counters.inputTokens += r.usage.input; state.counters.outputTokens += r.usage.output; }
  }
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
  if (!r.gated) state.exchanges.unshift({
    ts: state.lastRequestTs, faction, provider: snapshot.enabled ? snapshot.provider : "disabled",
    model: snapshot.provider === "fake" ? null : snapshot.model,
    request: sit, rawReply: r.rawReply, orders: r.orders, dropped: r.dropped,
    latencyMs, ok: r.ok, reason: r.reason, usage: r.usage,
    wake, nextCheck: r.nextCheck ?? null, intervalSec: interval,
  });
  if (state.exchanges.length > RING_MAX) state.exchanges.length = RING_MAX;

  const nContacts = Array.isArray(sit?.contacts) ? sit.contacts.length : 0;
  if (!r.gated) console.log(`orders faction=${faction} squads=${sit?.squads?.length ?? 0} contacts=${nContacts} orders=${r.orders.length} ${latencyMs}ms${r.skipped ? " (" + r.reason + ")" : ""}${wake ? " wake=" + wake.reason + (wake.triggers.length ? " [" + wake.triggers.join("; ") + "]" : "") : ""}${r.nextCheck ? " next=" + r.nextCheck.sec + "s" : ""}`);
  send(res, 200, r.rules ? { orders: [] } : r.paused ? { orders: [], keep: true } : r.gated && sc.failed ? { orders: [] } : r.skipped ? { orders: [], keep: true } : { orders: r.orders });
}

// ---------- world info, map image, vector overlay (roads, locations) ----------
// Game -> POST /world (every 60 s), /world/roads (chunks), /world/locations. The map image is resolved in the background by
// ./map/index.mjs from the user's own game install (never committed); vector data is cached in .cache/vector/<worldKey>.json.
const CACHE_DIR = path.join(DIR, ".cache");
const WORLD_FILE = path.join(CACHE_DIR, "world.json");
const VECTOR_DIR = path.join(CACHE_DIR, "vector");
const worldState = { info: null, key: "", ts: 0 };
const mapState = { status: "none", key: "", pngPath: null, width: 0, height: 0, source: "", texturePath: "", calibration: null, error: null, ts: 0, detectedGameDir: "" };
const vectors = new Map();   // worldKey -> {roads: [...]|null, locations: [...]|null}
const roadAsm = new Map();   // worldKey -> {chunks: Map(idx -> roads), total: -1}
const numArr = (a, n) => Array.isArray(a) && a.length >= n && a.slice(0, n).every(isNum);

function worldKeyOf(info) {
  const base = (s) => String(s ?? "").replace(/\\/g, "/").split("/").pop().replace(/\.[^.]*$/, "");
  const k = (base(info.mapPrefab).replace(/^MapEntity_/i, "") || base(info.worldFile) || "world").replace(/[^A-Za-z0-9_-]/g, "_").slice(0, 60);
  return k || "world";
}

function cleanWorld(b) {
  if (!b || typeof b !== "object") return null;
  const str = (v) => (typeof v === "string" ? v.slice(0, 300) : "");
  return {
    worldFile: str(b.worldFile), mapPrefab: str(b.mapPrefab),
    mapOffset: numArr(b.mapOffset, 2) ? b.mapOffset.slice(0, 2) : [0, 0], mapSize: numArr(b.mapSize, 2) ? b.mapSize.slice(0, 2) : [0, 0],
    boundsMin: numArr(b.boundsMin, 3) ? b.boundsMin.slice(0, 3) : [0, 0, 0], boundsMax: numArr(b.boundsMax, 3) ? b.boundsMax.slice(0, 3) : [0, 0, 0],
  };
}

function loadVectors(key) {
  if (!vectors.has(key)) {
    let v = { roads: null, locations: null };
    try { const j = JSON.parse(fs.readFileSync(path.join(VECTOR_DIR, key + ".json"), "utf8")); v = { roads: Array.isArray(j.roads) ? j.roads : null, locations: Array.isArray(j.locations) ? j.locations : null }; } catch { /* none cached */ }
    vectors.set(key, v);
  }
  return vectors.get(key);
}
function saveVectors(key) {
  try { fs.mkdirSync(VECTOR_DIR, { recursive: true }); fs.writeFileSync(path.join(VECTOR_DIR, key + ".json"), JSON.stringify(vectors.get(key))); }
  catch (e) { console.log(`could not save vector cache: ${e.message}`); }
}

// Image resolver: lazy import so the sidecar still starts if ./map/index.mjs is missing or broken.
async function resolveMap(force = false) {
  const info = worldState.info;
  if (!info) return;
  const key = worldState.key;
  if (!force && mapState.key === key && mapState.status !== "none" && mapState.status !== "error") return;
  Object.assign(mapState, { status: "resolving", key, pngPath: null, width: 0, height: 0, source: "", texturePath: "", calibration: null, error: null, ts: Date.now() });
  try {
    const mod = await import("./map/index.mjs");
    let gameDir = cfg.gameDir;
    try { mapState.detectedGameDir = (await mod.findGameDir?.()) || ""; } catch { mapState.detectedGameDir = ""; }
    if (!gameDir) gameDir = mapState.detectedGameDir;
    const r = await mod.resolveMapImage({
      mapPrefab: info.mapPrefab, worldFile: info.worldFile, gameDir, extraDirs: [], maxSize: cfg.mapMaxSize,
      cacheDir: path.join(CACHE_DIR, "maps"), overrideDir: path.join(DIR, "maps"),
    });
    if (mapState.key !== key) return; // world changed meanwhile
    if (!r || !r.pngPath) mapState.status = "notfound";
    else Object.assign(mapState, { status: "ready", pngPath: r.pngPath, width: r.width, height: r.height, source: r.source ?? "", texturePath: r.texturePath ?? "", calibration: r.calibration ?? null });
    console.log(`map ${key}: ${mapState.status}${r ? ` (${r.source}, ${r.width}x${r.height})` : ""}`);
  } catch (e) {
    mapState.status = "error"; mapState.error = String(e?.message ?? e);
    console.log(`map ${key}: resolve failed: ${mapState.error}`);
  }
  mapState.ts = Date.now();
}

function mapExtent() {
  const c = mapState.calibration, i = worldState.info;
  if (c && isNum(c.x0) && isNum(c.z0) && isNum(c.x1) && isNum(c.z1)) return { x0: c.x0, z0: c.z0, x1: c.x1, z1: c.z1 };
  if (i && i.mapSize[0] > 0 && i.mapSize[1] > 0) return { x0: i.mapOffset[0], z0: i.mapOffset[1], x1: i.mapOffset[0] + i.mapSize[0], z1: i.mapOffset[1] + i.mapSize[1] };
  if (i && i.boundsMax[0] > i.boundsMin[0] && i.boundsMax[2] > i.boundsMin[2]) return { x0: i.boundsMin[0], z0: i.boundsMin[2], x1: i.boundsMax[0], z1: i.boundsMax[2] };
  return null;
}

function worldView() {
  const v = worldState.key ? loadVectors(worldState.key) : { roads: null, locations: null };
  return {
    worldKey: worldState.key, info: worldState.info, ts: worldState.ts, extent: mapExtent(),
    map: { status: mapState.status, width: mapState.width, height: mapState.height, source: mapState.source, texturePath: mapState.texturePath, flipY: !!mapState.calibration?.flipY, error: mapState.error, ts: mapState.ts, extent: mapExtent() },
    vector: { roads: v.roads ? v.roads.length : null, locations: v.locations ? v.locations.length : null },
    gameDir: cfg.gameDir, detectedGameDir: mapState.detectedGameDir, mapMaxSize: cfg.mapMaxSize,
  };
}

function setWorld(info) {
  const key = worldKeyOf(info), changed = key !== worldState.key || JSON.stringify(info) !== JSON.stringify(worldState.info);
  worldState.info = info; worldState.key = key; worldState.ts = Date.now();
  if (changed) {
    try { fs.mkdirSync(CACHE_DIR, { recursive: true }); fs.writeFileSync(WORLD_FILE, JSON.stringify(info, null, 2)); } catch (e) { console.log(`could not save world.json: ${e.message}`); }
    console.log(`world ${key}: mapPrefab=${info.mapPrefab || "-"} worldFile=${info.worldFile || "-"} mapSize=${info.mapSize.join("x")}`);
  }
  void resolveMap(false);
}
try { const j = cleanWorld(JSON.parse(fs.readFileSync(WORLD_FILE, "utf8"))); if (j) setWorld(j); } catch { /* no cached world yet */ }

function validRoads(arr) {
  const out = [];
  for (const r of Array.isArray(arr) ? arr : []) {
    if (r && isNum(r.w) && Array.isArray(r.p) && r.p.length >= 4 && r.p.length % 2 === 0 && r.p.every(isNum)) out.push({ w: r.w, p: r.p });
  }
  return out;
}

// Game-facing POST /world, /world/roads, /world/locations (no Origin check: the game sends none; localhost host only).
async function handleWorldPost(req, res, url) {
  if (!hostOk(req)) return send(res, 403, { error: "forbidden host" });
  let b, raw = "";
  try { raw = await readBody(req); b = JSON.parse(raw); } catch (e) {
    console.log(`world post ${url}: invalid JSON (${e.message}) len=${raw.length}`);
    try { fs.mkdirSync(path.join(path.dirname(fileURLToPath(import.meta.url)), ".cache"), { recursive: true }); fs.writeFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), ".cache", "bad-world-body.txt"), raw); } catch {}
    return send(res, 400, { error: "invalid JSON" });
  }
  if (url === "/world") {
    const info = cleanWorld(b);
    if (!info) return send(res, 400, { error: "bad world info" });
    setWorld(info);
    const v = loadVectors(worldState.key);
    return send(res, 200, { ok: true, worldKey: worldState.key, needRoads: !v.roads, needLocations: !v.locations });
  }
  const key = typeof b?.worldKey === "string" ? b.worldKey.replace(/[^A-Za-z0-9_-]/g, "_").slice(0, 60) : "";
  if (!key) return send(res, 400, { error: "worldKey missing" });
  const v = loadVectors(key);
  if (url === "/world/locations") {
    const items = (Array.isArray(b.items) ? b.items : []).filter((i) => i && typeof i.name === "string" && numArr(i.pos, 2))
      .map((i) => ({ name: i.name.slice(0, 80), type: typeof i.type === "string" ? i.type.slice(0, 30) : "", pos: [i.pos[0], i.pos[1]] })).slice(0, 2000);
    v.locations = items; saveVectors(key);
    console.log(`world ${key}: locations n=${items.length}`);
    return send(res, 200, { ok: true, n: items.length });
  }
  // /world/roads
  if (!Number.isInteger(b.chunk) || b.chunk < 0) return send(res, 400, { error: "bad chunk" });
  if (b.chunk === 0) roadAsm.set(key, { chunks: new Map(), total: -1 }); // a restart of the upload discards a partial one
  const asm = roadAsm.get(key) ?? (roadAsm.set(key, { chunks: new Map(), total: -1 }), roadAsm.get(key));
  asm.chunks.set(b.chunk, validRoads(b.roads));
  if (b.last === true && Number.isInteger(b.total)) asm.total = b.total;
  if (asm.total >= 0 && asm.chunks.size >= asm.total) {
    const roads = [];
    for (let i = 0; i < asm.total; i++) roads.push(...(asm.chunks.get(i) ?? []));
    v.roads = roads; roadAsm.delete(key); saveVectors(key);
    console.log(`world ${key}: roads n=${roads.length} chunks=${asm.total}`);
  }
  return send(res, 200, { ok: true });
}

function handleWorldApi(req, res, url) {
  if (req.method === "GET" && url === "/api/world") return send(res, 200, worldView());
  if (req.method === "GET" && url === "/api/vector") {
    const v = worldState.key ? loadVectors(worldState.key) : { roads: null, locations: null };
    return send(res, 200, { roads: v.roads ?? [], locations: v.locations ?? [] });
  }
  if (req.method === "GET" && url === "/api/map.png") {
    if (mapState.status !== "ready" || !mapState.pngPath) return send(res, 404, { error: "no map image" });
    return fs.readFile(mapState.pngPath, (err, data) => {
      if (err) return send(res, 404, { error: "map image missing" });
      res.writeHead(200, { "Content-Type": "image/png", "Cache-Control": "public, max-age=3600" });
      res.end(data);
    });
  }
  if (req.method === "POST" && (url === "/api/map/reload" || url === "/api/vector/reset")) {
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    if (url === "/api/map/reload") { void resolveMap(true); return send(res, 200, { ok: true }); }
    const key = worldState.key;
    if (key) { vectors.set(key, { roads: null, locations: null }); roadAsm.delete(key); try { fs.rmSync(path.join(VECTOR_DIR, key + ".json"), { force: true }); } catch { /* ignore */ } }
    return send(res, 200, { ok: true });
  }
  return false;
}

async function handleApi(req, res, url) {
  if (!hostOk(req)) return send(res, 403, { error: "forbidden host" });
  if (handleWorldApi(req, res, url) !== false) return;
  if (req.method === "GET" && url === "/api/state") {
    return send(res, 200, {
      now: Date.now(), startedAt: state.startedAt, lastRequestTs: state.lastRequestTs,
      factions: state.factions, exchanges: state.exchanges, counters: state.counters, config: publicConfig(), usage: usageView(), scheduler: schedulerView(), paused,
    });
  }
  if (req.method === "POST" && url === "/api/pause") {
    if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) return send(res, 400, { error: "Content-Type must be application/json" });
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    let inp;
    try { inp = JSON.parse((await readBody(req, 10_000)) || "{}"); } catch { return send(res, 400, { error: "invalid JSON" }); }
    if (typeof inp?.paused !== "boolean") return send(res, 400, { error: "paused must be boolean" });
    if (paused && !inp.paused) for (const sc of Object.values(sched)) sc.manual = true; // resume: consult the HQ on the next poll
    paused = inp.paused;
    console.log(paused ? "HQ paused: no LLM calls" : "HQ resumed");
    return send(res, 200, { ok: true, paused });
  }
  if (req.method === "POST" && url === "/api/wake") {
    if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) return send(res, 400, { error: "Content-Type must be application/json" });
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    let inp;
    try { inp = JSON.parse((await readBody(req, 10_000)) || "{}"); } catch { return send(res, 400, { error: "invalid JSON" }); }
    const f = inp?.faction;
    if (f !== undefined && typeof f !== "string") return send(res, 400, { error: "faction must be a string" });
    const names = f ? [f] : Object.keys(state.factions);
    for (const n of names) schedFor(n).manual = true;
    return send(res, 200, { ok: true, woken: names });
  }
  if (req.method === "POST" && url === "/api/usage/refresh") {
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    await pollKey();
    return send(res, 200, { ok: !keyState.error, usage: usageView() });
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
      const wasEngine = cfg.engine;
      Object.assign(cfg, v.value);
      if (wasEngine !== "llm" && cfg.engine === "llm") for (const sc of Object.values(sched)) sc.manual = true; // back to LLM: call on the next poll
      migrateOpenRouter(cfg);
      keyState.data = null; keyState.error = null; keyState.costSincePoll = 0; checkBudget();
      void pollKey();
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
  if (req.method === "POST" && (url === "/world" || url === "/world/roads" || url === "/world/locations")) {
    return void handleWorldPost(req, res, url).catch((e) => { if (!res.headersSent) send(res, 500, { error: String(e?.message ?? e) }); });
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
}).listen(PORT, "127.0.0.1", () => {
  console.log(`clausewitz llm sidecar (${cfg.provider}${cfg.enabled ? "" : ", DISABLED"}) on http://127.0.0.1:${PORT}  (dashboard at /)`);
  void pollKey();
});

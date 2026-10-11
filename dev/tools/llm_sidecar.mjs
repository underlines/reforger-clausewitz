// Clausewitz M1 LLM bridge sidecar. Plain Node 18+, no npm deps.
//   node dev/tools/llm_sidecar.mjs
// Env: CLW_PORT (default 8788), CLW_LLM=fake|anthropic (default fake),
//      ANTHROPIC_API_KEY, CLW_MODEL (default claude-sonnet-5-5)
//
// GET  /health -> {"ok":true,"mode":"fake|anthropic"}
// POST /orders  body:
//   {"faction":"US","time":123,
//    "squads":[{"id":"US_Squad1","pos":[x,y,z],"agents":8,"startAgents":8,
//               "mode":"NONE|SEARCH|ATTACK|DEFEND","humanLed":false}],
//    "contacts":[{"pos":[x,y,z],"desc":"2x infantry","ageSec":12}]}
//   -> 200 {"orders":[{"squad":"US_Squad1","command":"MOVE|ATTACK|DEFEND","pos":[x,z],"reason":"..."}]}
// Any LLM/validation failure -> 200 {"orders":[]} (game falls back to its rule commander).
// Unparsable request body -> 400.
import http from "node:http";

const PORT = Number(process.env.CLW_PORT) || 8788;
const MODE = process.env.CLW_LLM === "anthropic" ? "anthropic" : "fake";
const MODEL = process.env.CLW_MODEL || "claude-sonnet-5-5";
const COMMANDS = new Set(["MOVE", "ATTACK", "DEFEND"]);
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

async function llmOrders(sit) {
  const key = process.env.ANTHROPIC_API_KEY;
  if (!key) throw new Error("ANTHROPIC_API_KEY not set");
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), 20000);
  try {
    const r = await fetch("https://api.anthropic.com/v1/messages", {
      method: "POST",
      signal: ac.signal,
      headers: { "x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json" },
      body: JSON.stringify({
        model: MODEL, max_tokens: 1024, system: SYSTEM,
        messages: [{ role: "user", content: "Current situation:\n" + JSON.stringify(sit) }],
      }),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}: ${(await r.text()).slice(0, 200)}`);
    const j = await r.json();
    const text = (j.content ?? []).filter((b) => b.type === "text").map((b) => b.text).join("");
    const a = text.indexOf("{"), b = text.lastIndexOf("}");
    if (a < 0 || b < a) throw new Error("no JSON object in reply");
    const obj = JSON.parse(text.slice(a, b + 1));
    if (!Array.isArray(obj.orders)) throw new Error("reply has no orders array");
    return obj.orders;
  } finally {
    clearTimeout(timer);
  }
}

function validate(raw, sit) {
  const ids = new Set(sit.squads.map((s) => s?.id));
  const seen = new Set(), out = [];
  for (const o of raw) {
    const why = !o || typeof o !== "object" ? "not an object"
      : !ids.has(o.squad) ? `unknown squad ${o.squad}`
      : seen.has(o.squad) ? `duplicate order for ${o.squad}`
      : !COMMANDS.has(o.command) ? `bad command ${o.command}`
      : !Array.isArray(o.pos) || o.pos.length !== 2 || !isNum(o.pos[0]) || !isNum(o.pos[1]) ? "bad pos"
      : typeof o.reason !== "string" ? "bad reason" : null;
    if (why) { console.log(`  dropped order: ${why}`); continue; }
    seen.add(o.squad);
    out.push({ squad: o.squad, command: o.command, pos: [o.pos[0], o.pos[1]], reason: o.reason.slice(0, 120) });
  }
  return out;
}

function send(res, code, obj) {
  res.writeHead(code, { "Content-Type": "application/json" });
  res.end(JSON.stringify(obj));
}

http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/health") return send(res, 200, { ok: true, mode: MODE });
  if (req.method !== "POST" || req.url !== "/orders") return send(res, 404, { error: "not found" });
  let body = "";
  req.on("data", (c) => (body += c));
  req.on("end", async () => {
    let sit;
    try { sit = JSON.parse(body); } catch { return send(res, 400, { error: "invalid JSON" }); }
    const t0 = Date.now();
    let orders = [];
    try {
      if (!sit || typeof sit !== "object" || !Array.isArray(sit.squads)) throw new Error("no squads array");
      const raw = MODE === "anthropic" ? await llmOrders(sit) : fakeOrders(sit);
      orders = validate(raw, sit);
    } catch (e) {
      console.log(`  error: ${e?.name === "AbortError" ? "timeout" : e?.message ?? e}`);
      orders = [];
    }
    const nContacts = Array.isArray(sit?.contacts) ? sit.contacts.length : 0;
    console.log(`orders faction=${sit?.faction} squads=${sit?.squads?.length ?? 0} contacts=${nContacts} orders=${orders.length} ${Date.now() - t0}ms`);
    send(res, 200, { orders });
  });
}).listen(PORT, "127.0.0.1", () => console.log(`clausewitz llm sidecar (${MODE}) on http://127.0.0.1:${PORT}`));

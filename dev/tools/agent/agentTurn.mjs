// HQ chat agent turn (M3 phase A): a player's free-text request (/hq <text>) is answered by a small tool-using agent.
// Design: dev/notes/decision-command-tools.md. Built on the Vercel AI SDK core (ai 7.x, ToolLoopAgent) with the OpenRouter provider.
//
//   runAgentTurn({ manifest, situation, message, config }) -> { replyText, actions, steps, usage, latencyMs, ... }   (never throws)
//
// The SDK is hidden behind callModel(): swap that one function (and createModel) to change framework / transport.
// Tools given to the model:
//   - one tool per manifest entry with authority "player" (zod schema generated from the entry params). execute() validates and QUEUES
//     the action (returns {accepted, id}); the game runs it through its action registry as the requesting player on its next poll
//     (so the grant rules - role, cooldown, ledger, caps - apply there) and tells the player the result.
//     Beyond CORE_TOOLS_MAX entries the rest is deferred (deferLoading) behind find_tools (AI SDK toolSearch + our own ranker).
//   - read-only, fog-limited: query_own_forces, query_bases, query_contacts (computed from the situation the HQ already has).
// The final model text is the radio reply to the player. No MCP, no raw game commands, nothing the manifest does not list.
import { ToolLoopAgent, tool, stepCountIs, toolSearch } from "ai";
import { createOpenRouter } from "@openrouter/ai-sdk-provider";
import { z } from "zod";

export const DEFAULTS = { maxSteps: 4, timeoutSec: 40, maxActions: 3, coreToolsMax: 6, maxOutputTokens: 700 };
const isNum = (n) => typeof n === "number" && Number.isFinite(n);
const num = (v, d) => (isNum(v) ? v : d);

// ---------- small helpers on the situation ----------
const posXZ = (p) => (Array.isArray(p) && p.length >= 2 ? (p.length > 2 ? [p[0], p[2]] : [p[0], p[1]]) : null);
const pad3 = (n) => String(Math.max(0, Math.min(999, Math.floor(n / 100)))).padStart(3, "0");
export const gridOf = (xz) => (xz ? `${pad3(xz[0])}-${pad3(xz[1])}` : "");
const distM = (a, b) => (a && b ? Math.round(Math.hypot(a[0] - b[0], a[1] - b[1])) : null);
const msgPos = (m) => posXZ(m?.pos);

// ---------- tool schemas from manifest entries ----------
function zodFor(p) {
  let z0;
  switch (p.type) {
    case "int": z0 = z.number().int(); break;
    case "enum": z0 = Array.isArray(p.enum) && p.enum.length ? z.enum(p.enum) : z.string(); break;
    case "position": z0 = z.array(z.number()).length(2); break; // [x, z] (no tuple: prefixItems is rejected by some upstream providers)
    default: z0 = z.string(); // string / unit / base
  }
  const hint = [p.hint, p.type === "position" ? "world [x, z]" : ""].filter(Boolean).join(" - ");
  if (hint) z0 = z0.describe(hint);
  return p.required ? z0 : z0.optional();
}
export function inputSchemaOf(entry) {
  const shape = {};
  for (const p of entry.params ?? []) shape[p.name] = zodFor(p);
  return z.object(shape);
}
// action args on the wire: [{name, value}], positions as "x,z" (the game's DTO has no maps)
function wireArgs(entry, input) {
  const out = [];
  for (const p of entry.params ?? []) {
    const v = input?.[p.name];
    if (v === undefined || v === null || v === "") continue;
    out.push({ name: p.name, value: Array.isArray(v) ? v.map((n) => Math.round(n)).join(",") : String(v) });
  }
  return out;
}

// ---------- tool ranking (find_tools) ----------
const tok = (s) => String(s ?? "").toLowerCase().split(/[^a-z0-9]+/).filter((t) => t.length > 1);
export function rankTools({ query, tools }) {
  const q = new Set(tok(query));
  if (!q.size) return [];
  return tools
    .map((t) => {
      const nameT = new Set(tok(t.name)), descT = new Set(tok(t.description));
      let s = 0;
      for (const w of q) { if (nameT.has(w)) s += 3; else if ([...nameT].some((n) => n.startsWith(w) || w.startsWith(n))) s += 1.5; if (descT.has(w)) s += 1; }
      return { name: t.name, s };
    })
    .filter((x) => x.s > 0).sort((a, b) => b.s - a.s).map((x) => x.name);
}

// ---------- read-only tools ----------
function readTools(situation, message) {
  const me = msgPos(message);
  const lim = (arr, n) => arr.slice(0, n);
  const near = (a, b) => (a.distM ?? 1e9) - (b.distM ?? 1e9);
  return {
    query_own_forces: tool({
      description: "Own AI/human squads of the faction with type, strength, state and grid, nearest to the requesting player first. Use to check what HQ has available near the player.",
      inputSchema: z.object({ kind: z.string().optional().describe("filter by unit kind, e.g. infantry, motorized, armor") }),
      execute: async ({ kind }) => {
        const rows = (situation?.squads ?? []).filter((s) => s && (!kind || s.kind === kind)).map((s) => {
          const xz = posXZ(s.pos);
          return { id: s.id, type: s.type, kind: s.kind, men: s.agents, state: s.state ?? "idle", humanLed: !!s.humanLed, grid: gridOf(xz), distM: distM(me, xz) };
        }).sort(near);
        return { count: rows.length, squads: lim(rows, 20) };
      },
    }),
    query_bases: tool({
      description: "Own bases, FOBs and the objectives (owner, grid) with the faction supplies. Use for questions about where to go or what exists.",
      inputSchema: z.object({}),
      execute: async () => ({
        supplies: situation?.war?.supplies ?? null,
        bases: (situation?.bases ?? []).map((b) => ({ faction: b.faction, grid: gridOf(posXZ(b.pos)), distM: distM(me, posXZ(b.pos)), services: b.services, stock: b.stock, state: b.state })).slice(0, 12),
        fobs: (situation?.fobs ?? []).map((f) => ({ id: f.id, state: f.state, tier: f.tier, grid: f.grid ?? gridOf(posXZ(f.pos)) })).slice(0, 8),
        objectives: (situation?.objectives ?? []).map((o) => ({ id: o.id, name: o.name, owner: o.owner, contested: !!o.contested, grid: o.grid ?? gridOf(posXZ(o.pos)), distM: distM(me, posXZ(o.pos)) })).slice(0, 12),
      }),
    }),
    query_contacts: tool({
      description: "Known enemy contacts (reports only, may be old: see ageSec), nearest to the requesting player first. Fog of war: HQ knows nothing else.",
      inputSchema: z.object({ maxAgeSec: z.number().optional().describe("ignore contacts older than this") }),
      execute: async ({ maxAgeSec }) => {
        const rows = (situation?.contacts ?? []).filter((c) => c && (maxAgeSec === undefined || num(c.ageSec, 0) <= maxAgeSec)).map((c) => {
          const xz = posXZ(c.pos);
          return { desc: c.desc, kind: c.kind, ageSec: c.ageSec, grid: c.grid ?? gridOf(xz), distM: distM(me, xz), place: c.place };
        }).sort(near);
        return { count: rows.length, contacts: lim(rows, 12) };
      },
    }),
  };
}

// ---------- action tools ----------
function actionTools(manifest, ctx) {
  const entries = [...(manifest?.primitives ?? []), ...(manifest?.intents ?? [])].filter((e) => e && e.authority === "player");
  const seen = new Set();
  const uniq = entries.filter((e) => !seen.has(e.name) && seen.add(e.name));
  // request-tagged entries are the core; the rest is discovered with find_tools
  uniq.sort((a, b) => Number(b.tags?.includes("request")) - Number(a.tags?.includes("request")));
  const tools = {};
  uniq.forEach((entry, i) => {
    const deferred = i >= ctx.coreToolsMax;
    tools[entry.name] = tool({
      description: `${entry.desc}${entry.cost ? ` Cost ${entry.cost} supplies.` : ""} Queues the request; HQ rules decide and the game reports the result to the player.`,
      inputSchema: inputSchemaOf(entry),
      ...(deferred ? { deferLoading: true } : {}),
      execute: async (input) => {
        const args = wireArgs(entry, input);
        const dup = ctx.actions.find((a) => a.action === entry.name && JSON.stringify(a.args) === JSON.stringify(args));
        if (dup) return { accepted: true, id: dup.id, note: "already queued" };
        if (ctx.actions.length >= ctx.maxActions) return { accepted: false, error: `at most ${ctx.maxActions} actions per message` };
        const id = `${ctx.message.id}-${ctx.actions.length + 1}`;
        ctx.actions.push({ id, action: entry.name, args });
        return { accepted: true, id, note: "queued, NOT yet decided: the game rules grant or refuse it and tell the player the result. Only acknowledge, do not promise delivery." };
      },
    });
  });
  return { tools, names: uniq.map((e) => e.name), deferred: uniq.slice(ctx.coreToolsMax).map((e) => e.name) };
}

export function buildTools(manifest, situation, message, limits = {}) {
  const ctx = { message, actions: [], maxActions: limits.maxActions ?? DEFAULTS.maxActions, coreToolsMax: limits.coreToolsMax ?? DEFAULTS.coreToolsMax };
  const act = actionTools(manifest, ctx);
  const tools = { ...act.tools, ...readTools(situation, message) };
  if (act.deferred.length) tools.find_tools = toolSearch({ search: rankTools, maxResults: 4 });
  return { tools, ctx, actionNames: act.names, deferred: act.deferred };
}

// ---------- prompt ----------
export function systemPrompt(situation, message, tools) {
  return `You are the HQ radio operator of one faction in a tactical war game (Arma Reforger). A player talks to HQ over the radio and you answer in a short radio style.
Player: ${message.player} (${message.role}, squad ${message.squad || "none"}, grid ${message.grid || "?"}). Faction ${situation?.faction ?? "?"}, supplies ${situation?.war?.supplies ?? "?"}.
Rules:
- If the player asks for something (a vehicle, a squad, ammo/resupply, reinforcements, a recon), call the matching tool ONCE with the parameters the player gave. The game's rules then grant or refuse it and tell the player the result, so never promise delivery.
- Soldiers may only get resupply and vehicles; squad, reinforce and recon are for squad leaders. If the role cannot do it, say so and offer the alternative instead of calling.
- Use the query_* tools only when the player asks about forces, bases or enemies. HQ knows only what units reported (fog of war).
- Do not invent ids, types, grids or numbers. If something is unclear, ask one short question instead of guessing.
- Your final text is the radio reply: 1-2 short sentences, in the language the player wrote. After calling a tool, only acknowledge (for example "Roger, requesting a truck for you.").
${tools.find_tools ? "- Tools not shown are found with find_tools.\n" : ""}`;
}

// ---------- model seam ----------
export function createModel(config) {
  const provider = config.provider;
  if (provider === "openrouter" || provider === "openai-compatible") {
    if (!config.apiKey && provider === "openrouter") throw new Error("API key not set");
    const or = createOpenRouter({ apiKey: config.apiKey || undefined, ...(config.baseUrl ? { baseURL: config.baseUrl } : {}) });
    return or.chat(config.model, { usage: { include: true } });
  }
  throw new Error(`agent turns support openrouter / openai-compatible, not ${provider}`);
}

// Runs the tool loop. Everything the rest of the sidecar needs from the SDK is normalised here:
// -> { text, steps:[{n, text, finishReason, toolCalls:[{name,input,output,error}], usage}], usage:{input,output,cost,calls} }
export async function callModel({ model, system, prompt, tools, maxSteps, maxOutputTokens, timeoutSec }) {
  const agent = new ToolLoopAgent({
    model, instructions: system, tools, stopWhen: stepCountIs(maxSteps), maxOutputTokens, temperature: 0.2,
  });
  const res = await agent.generate({ prompt, abortSignal: AbortSignal.timeout(timeoutSec * 1000) });
  const steps = res.steps.map((s, i) => {
    const results = new Map((s.toolResults ?? []).map((r) => [r.toolCallId, r]));
    const orCost = s.providerMetadata?.openrouter?.usage?.cost;
    return {
      n: i + 1, text: s.text ?? "", finishReason: s.finishReason,
      toolCalls: (s.toolCalls ?? []).map((c) => ({ name: c.toolName, input: c.input, output: results.get(c.toolCallId)?.output ?? null })),
      usage: { input: s.usage?.inputTokens ?? 0, output: s.usage?.outputTokens ?? 0, cost: isNum(orCost) ? orCost : null },
    };
  });
  const usage = steps.reduce((a, s) => ({ input: a.input + s.usage.input, output: a.output + s.usage.output, cost: a.cost + (s.usage.cost ?? 0), calls: a.calls + 1 }), { input: 0, output: 0, cost: 0, calls: 0 });
  return { text: res.text ?? "", steps, usage };
}

// ---------- fake provider (deterministic, no key) ----------
const FAKE_RULES = [
  [/\b(vehicle|truck|car|jeep|apc|transport|ride|fahrzeug|lkw)\b/i, "REQUEST_VEHICLE"],
  [/\b(squad|team|fireteam|men|infantry|truppe|trupp)\b/i, "REQUEST_SQUAD"],
  [/\b(ammo|ammunition|resupply|supplies|munition|nachschub|box)\b/i, "REQUEST_RESUPPLY"],
  [/\b(reinforce|reinforcement|backup|help|support|verst[aä]rkung)\b/i, "REQUEST_REINFORCE"],
  [/\b(recon|scout|look|observe|aufkl[aä]r)\b/i, "REQUEST_RECON"],
];
async function fakeTurn({ tools, message, manifest }) {
  const steps = [];
  const text = String(message.text ?? "");
  const hit = FAKE_RULES.find(([re, name]) => re.test(text) && tools[name]);
  let reply;
  if (hit) {
    const name = hit[1], entry = [...(manifest.primitives ?? []), ...(manifest.intents ?? [])].find((e) => e.name === name);
    const input = {};
    for (const p of entry?.params ?? []) {
      if (p.name === "where") { const g = /\b(\d{3})[- ]?(\d{3})\b/.exec(text); input.where = g ? `${g[1]}-${g[2]}` : "here"; }
      if (p.name === "type") { const m = /\b(truck|apc|jeep|rifle|at|mg|sniper|medic)\b/i.exec(text); if (m) input.type = m[1].toLowerCase(); }
    }
    const output = await tools[name].execute(input, {});
    steps.push({ n: 1, text: "", finishReason: "tool-calls", toolCalls: [{ name, input, output }], usage: { input: 0, output: 0, cost: null } });
    reply = `Roger, HQ is working on your request (${name.replace(/^REQUEST_/, "").toLowerCase()}).`;
  } else if (/\b(enemy|contact|where|status|forces?)\b/i.test(text) && tools.query_contacts) {
    const output = await tools.query_contacts.execute({}, {});
    steps.push({ n: 1, text: "", finishReason: "tool-calls", toolCalls: [{ name: "query_contacts", input: {}, output }], usage: { input: 0, output: 0, cost: null } });
    reply = output.count ? `HQ knows ${output.count} contact(s), nearest at grid ${output.contacts[0].grid}.` : "No enemy contacts on record.";
  } else reply = "HQ here. Request a vehicle, squad, ammo, reinforcements or recon.";
  steps.push({ n: steps.length + 1, text: reply, finishReason: "stop", toolCalls: [], usage: { input: 0, output: 0, cost: null } });
  return { text: reply, steps, usage: { input: 0, output: 0, cost: 0, calls: 0 } };
}

// ---------- entry point ----------
export async function runAgentTurn({ manifest, situation, message, config, agentConfig = {} }) {
  const t0 = Date.now();
  const lim = { ...DEFAULTS, ...agentConfig };
  const out = { msgId: message.id, ok: true, replyText: "", actions: [], steps: [], usage: { input: 0, output: 0, cost: 0, calls: 0 }, latencyMs: 0, provider: config.provider, model: config.provider === "fake" ? null : config.model, tools: [], error: null };
  try {
    const built = buildTools(manifest, situation, message, lim);
    out.tools = Object.keys(built.tools);
    const system = systemPrompt(situation, message, built.tools);
    const prompt = String(message.text ?? "").slice(0, 500);
    let r;
    if (config.provider === "fake") r = await fakeTurn({ tools: built.tools, message, manifest });
    else r = await callModel({ model: createModel(config), system, prompt, tools: built.tools, maxSteps: lim.maxSteps, maxOutputTokens: lim.maxOutputTokens, timeoutSec: lim.timeoutSec });
    out.steps = r.steps; out.usage = r.usage; out.actions = built.ctx.actions;
    out.replyText = String(r.text ?? "").trim().slice(0, 300);
    if (!out.replyText) out.replyText = out.actions.length ? "Roger, request passed on." : "HQ copies, nothing to do.";
  } catch (e) {
    out.ok = false;
    out.error = e?.name === "TimeoutError" || e?.name === "AbortError" ? "timeout" : String(e?.message ?? e).slice(0, 300);
    out.actions = [];
  }
  out.latencyMs = Date.now() - t0;
  return out;
}

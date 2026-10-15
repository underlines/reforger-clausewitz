// Prompt budget test (LLM-latency plan step 8). No network, no game.   node dev/tools/test-prompt-budget.mjs
// Builds every LLM prompt of the sidecar from a recorded-shape LARGE situation (fixtures/large_situation.json + large_manifest.json: 30 squads, 25 contacts,
// 70 catalog entries, 8 objectives, FOBs, convoys, help calls, env ...) and fails when a prompt SECTION or a prompt TOTAL exceeds its token budget
// (promptKit.approxTokens = chars / 4), so a new feature has to fit into the budget or justify more. Prints prompt / section / tokens / budget, plus the
// lean-off (raw situation) totals for comparison. Also asserts that the SYSTEM prompt prefix is byte-identical between two situations that differ only in
// dynamic data (the cacheable prefix). The /hq agent prompt is measured only. Refresh the fixture: fixtures/make_large_fixture.mjs, or a real recording
// (sidecar env CLW_RECORD_DIR).
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import * as P from "./agent/commanderPrompt.mjs";
import * as J from "./agent/journal.mjs";
import { sectionStats, approxTokens } from "./agent/promptKit.mjs";
import { systemPrompt as hqSystemPrompt } from "./agent/agentTurn.mjs";

// ---------------------------------------------------------------- BUDGETS (tokens)
// One table. Lean build, large fixture. Raise a budget only with a reason in the commit message (what grew and why it is worth the tokens).
// Measured values were rounded up by ~15-20 %. `total` = system + user. A section that is not listed here fails the test (a new section needs a budget).
const BUDGET = {
  orders:   { total: 5800, rules: 1125, commands: 440, persona: 85, helpCalls: 105, static: 270, situation: 3350, plan: 110, wake: 45, env: 70, journal: 195 },
  combined: { total: 9250, rules: 1210, commands: 1090, persona: 85, fobs: 360, logistics: 240, helpCalls: 105, static: 2320, situation: 3550, wake: 45, env: 70, journal: 195 },
  planner:  { total: 8900, rules: 790, commands: 725, fobs: 360, logistics: 240, static: 2320, situation: 3550, orders: 730, trigger: 15, journal: 195 },
  sitrep:   { total: 5440, rules: 850, persona: 30, help: 32, static: 270, situation: 3530, orders: 730 },
};

const DIR = path.dirname(fileURLToPath(import.meta.url));
const load = (f) => JSON.parse(fs.readFileSync(path.join(DIR, "fixtures", f), "utf8"));
const sitA = { ...load("large_situation.json"), wake: { reason: "trigger", triggers: ["contact 380 m from RHS_USAF_Inf4", "objective D contested"], sinceLastSec: 42 } };
const manifest = load("large_manifest.json");

// sample journal block (at the journal caps), orders in force (one per non-garrison squad) and PLAN block input
const jr = J.emptyJournal();
jr.intent = "Hold the right flank at Chapel Hill and take Saint Philippe with armor support before the enemy reinforces from Meaux; keep the FOB supplied.";
jr.goals = [{ text: "Capture Saint Philippe", status: "open" }, { text: "Secure FOB Quarry Road", status: "open" }, { text: "Kill the enemy armor at Chapel Hill", status: "done" }];
jr.lessons = ["Frontal pushes on Le Moule bleed squads; use the left side road.", "Keep AT near objectives where vehicles were seen.", "Do not leave FOB builders without escort."];
jr.tries = { D: { id: "D", name: "Saint Philippe", tried: 3, failed: 2, last: "repulsed", lastAt: 2300 }, F: { id: "F", name: "Meaux", tried: 2, failed: 2, last: "repulsed", lastAt: 1900 } };
const journal = J.renderBlock(jr, sitA);
const orders = sitA.squads.filter((s) => !s.garrison && s.objective).map((s) => ({ squad: s.id, command: s.objective.command, target: s.objective.target, reason: s.objective.reason }));
const plan = { buy: [{ type: "RHS_USAF_M2A3_Bradley", count: 1, command: "ATTACK", target: "D", reason: "armor support for the Saint Philippe push" }], fobs: [{ command: "RESUPPLY", fob: "FOB_2", reason: "FOB stock empty" }], ageSec: 95, nextInSec: 85 };

// a second situation that differs only in dynamic data: positions, contacts, supplies, stocks, events, env, ownership/progress, time, wake
const sitB = JSON.parse(JSON.stringify(sitA));
sitB.time += 340; sitB.timeLeftSec -= 340; sitB.war.supplies += 77; sitB.war.supplyTotal += 77; sitB.war.tickets -= 9; sitB.war.liveAI -= 6; sitB.war.engineAI.headroom -= 20;
for (const s of sitB.squads) { s.pos = [s.pos[0] + 83, s.pos[1], s.pos[2] - 61]; s.agents = Math.max(1, s.agents - 1); s.health = 0.5; s.state = "moving"; if (s.objective) s.objective.ageSec += 340; }
for (const c of sitB.contacts) { c.pos = [c.pos[0] - 120, c.pos[1], c.pos[2] + 90]; c.ageSec += 340; }
sitB.contacts = sitB.contacts.slice(0, 12);
for (const o of sitB.objectives) { o.progress = -o.progress; if (o.owner !== "none") o.owner = o.owner === "RHS_USAF" ? "RHS_AFRF" : "RHS_USAF"; o.contested = !o.contested; }
sitB.bases[0].stock += 120; sitB.fobs[0].stock -= 90; sitB.events = ["objective D captured by RHS_AFRF", "RHS_USAF_Inf9 lost"]; sitB.env = { ...sitB.env, time: "20:25", daylight: "night", weather: "Rain", next: "Rain", nextInMin: 40 };
sitB.requests = sitB.requests.slice(0, 2); sitB.wake = { reason: "scheduled", triggers: [], sinceLastSec: 120 };

const cfg = (lean, planner, over = {}) => ({
  wakeup: { dynamic: true, defaultSec: 60, minSec: 20, maxSec: 300, budgetStretch: true }, memory: { enabled: true, reviewMin: 5 },
  output: { mode: "json_object", terse: true, reasonWords: 12 }, planner: { enabled: planner, intervalSec: 180 },
  prompt: { lean, staticBlock: true, cache: true, contactMax: 25, contactMaxAgeSec: 600 }, ...over,
});
const build = {
  orders: (lean, sit = sitA) => P.buildCommanderPrompt({ config: cfg(lean, true), sit, manifest, journal, plan }),
  combined: (lean, sit = sitA) => P.buildCommanderPrompt({ config: cfg(lean, false), sit, manifest, journal }),
  planner: (lean, sit = sitA) => P.buildPlannerPrompt({ config: cfg(lean, true), sit, manifest, journal, orders, trigger: "supplies 640 -> 717" }),
  sitrep: (lean, sit = sitA) => P.buildSitrepPrompt({ config: cfg(lean, true), sit, orders }),
};
const total = (p) => approxTokens(p.system) + approxTokens(p.user);

// ---------------------------------------------------------------- budgets
const rows = [];
let failures = 0;
const check = (cond, msg) => { if (!cond) { failures++; console.log("  FAIL " + msg); } };
for (const name of Object.keys(build)) {
  const lean = build[name](true), off = build[name](false), st = sectionStats(lean.sections), B = BUDGET[name];
  for (const [sec, v] of Object.entries(st)) {
    rows.push([name, sec, v.tokens, B[sec] ?? "-"]);
    check(B[sec] !== undefined, `${name}: section "${sec}" (${v.tokens} tokens) has no budget`);
    check(B[sec] === undefined || v.tokens <= B[sec], `${name}: section "${sec}" ${v.tokens} tokens > budget ${B[sec]}`);
  }
  const t = total(lean);
  rows.push([name, "TOTAL (lean)", t, B.total]);
  rows.push([name, "total (lean off)", total(off), "-"]);
  check(t <= B.total, `${name}: total ${t} tokens > budget ${B.total}`);
  check(t < total(off), `${name}: lean (${t}) is not smaller than the raw situation prompt (${total(off)})`);
  for (const sec of Object.keys(B)) if (sec !== "total" && !(sec in st)) check(false, `${name}: budget for "${sec}" but the prompt has no such section (stale table entry)`);
}

// /hq agent system prompt: measured only (its tools carry the data on demand)
const hqMessage = { player: "Underlines", role: "leader", squad: "Alpha 1-1", grid: "058-072", history: [{ q: "status?", a: "Roger, Delta is contested." }] };
const hqSys = hqSystemPrompt(sitA, hqMessage, {}, journal, null);
rows.push(["hq (measure only)", "system", approxTokens(hqSys), "-"]);

const w = [Math.max(...rows.map((r) => String(r[0]).length)), Math.max(...rows.map((r) => String(r[1]).length))];
console.log("\n" + "prompt".padEnd(w[0]) + "  " + "section".padEnd(w[1]) + "  tokens  budget");
for (const r of rows) console.log(String(r[0]).padEnd(w[0]) + "  " + String(r[1]).padEnd(w[1]) + "  " + String(r[2]).padStart(6) + "  " + String(r[3]).padStart(6));

// ---------------------------------------------------------------- cacheable prefix
for (const name of Object.keys(build)) {
  const a = build[name](true, sitA), b = build[name](true, sitB);
  check(a.system === b.system, `${name}: system prompt differs between two situations that differ only in dynamic data`);
  check(a.user !== b.user, `${name}: the user message should differ (sanity)`);
  check(a.system.length > 2000, `${name}: system prompt suspiciously short`);
}
// the static block really is the tail of the system prompt (stable rules first), and the volatile data is only in the user message
{
  const a = build.orders(true);
  check(a.system.endsWith(a.sections.static), "orders: static block is the end of the system prompt");
  check(a.system.lastIndexOf("MAP AND CATALOG") > a.system.indexOf("COMMANDS"), "orders: static block after rules + commands");
  check(!a.system.includes("2710") && !a.system.includes('"owner"'), "orders: no volatile values in the system prompt");
}

if (failures) { console.log(`\nprompt budget: ${failures} FAILED`); process.exit(1); }
assert.ok(rows.length > 20);
console.log("\nprompt budget: all passed");

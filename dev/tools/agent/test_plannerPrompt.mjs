// Offline unit test of the planner split prompts (agent/commanderPrompt.mjs: buildPlannerPrompt, the ORDERS call, planBlock). node agent/test_plannerPrompt.mjs
import assert from "node:assert/strict";
import * as P from "./commanderPrompt.mjs";
import { sectionStats, terseSchema } from "./promptKit.mjs";

const cfg = (over = {}) => ({
  wakeup: { dynamic: true, defaultSec: 60, minSec: 20, maxSec: 300, budgetStretch: true },
  memory: { enabled: true, reviewMin: 5 },
  output: { mode: "json_object", terse: true, reasonWords: 12 },
  planner: { enabled: true, intervalSec: 180 },
  ...over,
});
const ent = (name, channel, params = []) => ({ name, layer: "intent", channel, authority: "hq", cost: 0, tags: [], requires: [], desc: `${name} desc.`, params });
const fobManifest = {
  ...P.DEFAULT_MANIFEST, hash: "fob-1", builtin: false,
  intents: [...P.DEFAULT_MANIFEST.intents,
    ent("ESTABLISH_FOB", "fob", [{ name: "site", type: "string", required: true, hint: "fobSites id" }]),
    ent("UPGRADE_FOB", "fob", [{ name: "fob", type: "string", required: true, hint: "fobs id" }]),
    ent("RESUPPLY", "fob", [{ name: "fob", type: "string", required: true, hint: "fobs id" }]),
    ent("CONVOY", "fob", [{ name: "origin", type: "base", required: true }, { name: "dest", type: "base", required: true }])],
};
const sitA = (over = {}) => ({
  v: 3, faction: "US", time: 1000, war: { supplies: 300, tickets: 500, phase: "running" },
  objectives: [{ id: "A", name: "Alpha", pos: [100, 100], owner: "US" }, { id: "C", name: "Charlie", pos: [900, 900], owner: "none" }],
  catalog: [{ type: "rifle_squad", kind: "infantry", cost: 100 }],
  squads: [{ id: "US_1", kind: "infantry", pos: [100, 0, 100], agents: 8, startAgents: 8 }],
  contacts: [], hq: { trait: "aggressive" },
  structures: [{ base: "HQ", kind: "arsenal", pos: [1, 2] }],
  fobs: [{ id: "F1", state: "active", tier: 1, stock: 10 }], fobSites: [{ id: "S1", pos: [5, 5] }], fobRules: { ready: true, cost: 100, max: 2, live: 1 },
  convoys: [], logisticsRules: { lowStock: 50, truckCargo: 100 },
  wake: { reason: "scheduled", triggers: [], sinceLastSec: 60 },
  ...over,
});
const JOURNAL = "HQ JOURNAL:\nIntent: take Charlie with armor\nGoals: - [open] hold Alpha";

// --- ORDERS call (split)
const o = P.buildCommanderPrompt({ config: cfg(), sit: sitA(), manifest: fobManifest, journal: JOURNAL, plan: { buy: [{ type: "rifle_squad", count: 1, command: "ATTACK", target: "C", reason: "x" }], fobs: [{ command: "ESTABLISH_FOB", site: "S1", reason: "y" }], ageSec: 40, nextInSec: 140 } });
assert.ok(!/\bbuy\b/i.test(o.system) && !/\bBUY\b/.test(o.system) && !/FOB/i.test(o.system) && !/convoy|RESUPPLY|logistic/i.test(o.system), "orders system prompt: no buy / FOB / logistics text\n" + o.system.match(/.{40}(buy|fob).{40}/i));
assert.ok(!o.sections.fobs && !o.sections.logistics, "no FOB / LOGISTICS sections in the orders call");
assert.ok(o.system.includes("COMMANDS (orders") && o.system.includes("Offer up to 3 missions") && o.system.includes("Compact tuples"), "orders + missions stay");
assert.ok(!o.system.includes('"b":') && !o.system.includes('"f":') && o.system.includes('"o":') && o.system.includes('"m":'), "terse spec: o / m only");
assert.deepEqual(o.spec.fobs, []); assert.equal(o.spec.buy, false);
assert.equal(terseSchema(o.spec).properties.b, undefined);
assert.ok(o.user.includes("PLAN (") && o.user.includes('"nextRunInSec":140') && o.user.includes("ESTABLISH_FOB") && o.user.includes('"target":"C"'), "plan block in the user message", o.user);
assert.ok(o.sections.plan && Object.keys(o.sections).join() === "rules,commands,persona,situation,plan,wake,journal", "section order: plan after the situation: " + Object.keys(o.sections).join());
assert.ok(!o.system.includes(", FOB") && o.system.includes("HQ JOURNAL:"), "journal rule without the FOB word");
const ov = P.buildCommanderPrompt({ config: cfg({ output: { mode: "text", terse: false, reasonWords: 12 } }), sit: sitA(), manifest: fobManifest, plan: null });
assert.ok(ov.system.includes('"orders":[{"squad":"<id>"') && !ov.system.includes('"buy":[') && !ov.system.includes('"fobs":['), "verbose spec: orders / missions only");
assert.ok(ov.user.includes("PLAN (") && ov.user.includes("none yet"), "no plan yet");
assert.ok(P.planBlock({ buy: [], fobs: [], running: true }).includes("working on it now"));
assert.ok(P.planBlock({ buy: [], fobs: [], ageSec: 12, nextInSec: 100 }).includes('"buy":[]'), "a run that decided nothing is shown");

// --- planner disabled / no buy+fob intents = the old combined prompt
const off = P.buildCommanderPrompt({ config: cfg({ planner: { enabled: false, intervalSec: 180 } }), sit: sitA(), manifest: fobManifest, journal: "" });
assert.ok(off.system.includes("SPAWN ON ASSIGNMENT") && off.system.includes("FOBS:") && off.system.includes('"b":') && off.system.includes('"f":') && !off.user.includes("PLAN ("), "planner off: combined prompt as before");
const ordersOnly = { ...P.DEFAULT_MANIFEST, intents: P.DEFAULT_MANIFEST.intents.filter((e) => e.channel === "orders") };
assert.equal(P.plannerSplit(cfg(), ordersOnly), false, "no buy / fob intents: no planner");
assert.equal(P.plannerSplit(cfg(), fobManifest), true);
assert.equal(P.plannerSplit({}, fobManifest), false, "no planner group = off");
const noPl = P.buildCommanderPrompt({ config: cfg(), sit: sitA(), manifest: ordersOnly });
assert.ok(!noPl.user.includes("PLAN ("), "manifest without buy/fob: the orders prompt has no plan block");

// --- PLANNER call
const pl = P.buildPlannerPrompt({ config: cfg(), sit: sitA({ helpCalls: [{ id: 1, player: "x", grid: "1-1", pos: [1, 1], note: "help", ageSec: 3, answeredBy: "" }], env: { weather: "rain" } }), manifest: fobManifest, journal: JOURNAL, trigger: "objective C changed owner (none -> US)",
  orders: [{ squad: "US_1", command: "ATTACK", target: "C", reason: "take it" }] });
assert.ok(pl.system.includes("SPAWN ON ASSIGNMENT") && pl.system.includes("engineAI.headroom") && pl.system.includes("BUY:") && pl.system.includes("ESTABLISH_FOB"), "economy doctrine + buy / fob entries");
assert.ok(pl.system.includes("FOBS: a FOB") && pl.system.includes("LOGISTICS: stock"), "FOB / LOGISTICS paragraphs when they apply");
assert.ok(!pl.system.includes("COMMANDS (orders") && !pl.system.includes("Offer up to 3 missions") && !pl.system.includes("HUMAN-LED SQUADS") && !pl.system.includes("HELP CALLS"), "no order / mission / help-call text");
assert.ok(pl.system.includes('"b":') && pl.system.includes('"f":') && !pl.system.includes('"o":') && !pl.system.includes('"m":') && !pl.system.includes('"n":['), "terse spec: b / f only, no next-check");
assert.ok(pl.system.includes("nothing to buy or build now") && !pl.system.includes("keep all current orders"), "keep sentence adapted");
assert.deepEqual(Object.keys(pl.sections), ["rules", "commands", "fobs", "logistics", "situation", "orders", "journal", "trigger"], "sections");
assert.equal(pl.system, ["rules", "commands", "fobs", "logistics"].map((k) => pl.sections[k] ?? "").join(""), "system = its sections");
assert.ok(pl.user.startsWith("Current situation:\n") && !pl.user.includes('"structures"') && !pl.user.includes("WAKE (why"), "situation only, no structures, no wake object");
assert.ok(pl.user.includes("ORDERS IN FORCE") && pl.user.includes('"squad":"US_1"') && pl.user.includes("take it"), "orders in force");
assert.ok(pl.user.includes("HQ JOURNAL:") && pl.user.includes("Intent: take Charlie"), "journal intent + goals");
assert.ok(pl.user.endsWith("PLANNER WAKE: objective C changed owner (none -> US)"), "own trigger line");
assert.ok(!/weather|rain/i.test(pl.system) && !pl.user.includes("ENVIRONMENT"), "no environment paragraph");
assert.deepEqual(pl.spec.orders, []); assert.equal(pl.spec.buy, true); assert.deepEqual(pl.spec.fobs, ["ESTABLISH_FOB", "UPGRADE_FOB", "RESUPPLY", "CONVOY"]); assert.equal(pl.spec.wake, null);
const sc = terseSchema(pl.spec);
assert.ok(sc.properties.b && sc.properties.f && !sc.properties.o && !sc.properties.n, "schema: b / f / k");
const st = sectionStats(pl.sections);
assert.ok(st.rules.tokens > 0 && st.commands.tokens > 0);
// smaller than the combined prompt
const combined = P.buildCommanderPrompt({ config: cfg({ planner: { enabled: false } }), sit: sitA(), manifest: fobManifest, journal: JOURNAL });
assert.ok(pl.system.length < combined.system.length && o.system.length < combined.system.length, "each split prompt is smaller than the combined one");
// conditional paragraphs
const plMin = P.buildPlannerPrompt({ config: cfg(), sit: sitA({ fobs: [], convoys: [], fobRules: { ready: false } }), manifest: P.DEFAULT_MANIFEST, orders: [] });
assert.ok(!plMin.system.includes("FOBS:") && !plMin.system.includes("LOGISTICS:") && plMin.user.includes("ORDERS IN FORCE") && plMin.user.includes("none yet") && plMin.user.endsWith("PLANNER WAKE: scheduled run"), "no FOB / logistics manifest: only buy");
assert.ok(!plMin.sections.journal, "no journal block when there is none");
// verbose planner reply spec
const plV = P.buildPlannerPrompt({ config: cfg({ output: { mode: "text", terse: false, reasonWords: 12 } }), sit: sitA(), manifest: fobManifest });
assert.ok(plV.system.includes('"buy":[{"type"') && plV.system.includes('"fobs":[{"command":"ESTABLISH_FOB"') && !plV.system.includes('"orders":[{') && plV.spec === null, "verbose planner spec: buy / fobs");
console.log("test_plannerPrompt: all passed");

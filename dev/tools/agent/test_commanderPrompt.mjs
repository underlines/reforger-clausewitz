// Offline unit test of agent/commanderPrompt.mjs (no network, no game). node agent/test_commanderPrompt.mjs
import assert from "node:assert/strict";
import * as P from "./commanderPrompt.mjs";
import { sectionStats, terseSchema, approxTokens } from "./promptKit.mjs";

const cfg = (over = {}) => ({
  wakeup: { dynamic: true, defaultSec: 60, minSec: 20, maxSec: 300, budgetStretch: true },
  memory: { enabled: true, reviewMin: 5 },
  output: { mode: "json_object", terse: true, reasonWords: 12 },
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
const sitA = () => ({
  v: 3, faction: "US", time: 1000, war: { supplies: 300, tickets: 500, phase: "running" },
  objectives: [{ id: "A", name: "Alpha", pos: [100, 100], owner: "US" }, { id: "C", name: "Charlie", pos: [900, 900], owner: "none" }],
  catalog: [{ type: "rifle_squad", kind: "infantry", cost: 100 }],
  squads: [{ id: "US_1", kind: "infantry", pos: [100, 0, 100], agents: 8, startAgents: 8 }],
  contacts: [], hq: { trait: "aggressive" },
  structures: [{ base: "HQ", kind: "arsenal", pos: [1, 2] }],
  wake: { reason: "scheduled", triggers: [], sinceLastSec: 60 },
});
const build = (over = {}) => P.buildCommanderPrompt({ config: cfg(), manifest: P.DEFAULT_MANIFEST, journal: "", ...over });
const partsOf = (r) => ["rules", "commands", "persona", "fobs", "logistics", "helpCalls"].map((k) => r.sections[k] ?? "").join("");

// --- shape + invariants
const r0 = build({ sit: sitA() });
assert.deepEqual(Object.keys(r0).sort(), ["sections", "spec", "system", "user"]);
assert.equal(partsOf(r0), r0.system, "system = concatenation of its sections");
assert.ok(r0.user.startsWith("Current situation:\n"));
assert.ok(!r0.user.includes('"structures"'), "structures never go to the model");
assert.ok(!/\bSITREP\b/.test(r0.system + r0.user), "no SITREP request in the commander prompt");
const stats = sectionStats(r0.sections);
for (const k of Object.keys(r0.sections)) assert.ok(stats[k].tokens > 0, `section ${k} measurable`);
assert.ok(stats.rules.tokens > stats.persona.tokens);
assert.deepEqual(Object.keys(r0.sections), ["rules", "commands", "persona", "situation", "wake"]);

// --- terse vs verbose reply spec
assert.ok(r0.system.includes("Compact tuples"), "terse spec in the system prompt");
assert.ok(!r0.system.includes('"orders":[{"squad":"<id>"'), "no verbose REPLY block when terse");
assert.ok(r0.system.includes("COMMANDS (orders") && r0.system.includes("BUY:"), "command descriptions stay");
assert.ok(r0.system.includes('"n":[120,"why"]'), "dynamic wakeup: next-check in the terse spec");
assert.ok(!r0.system.includes("You are not polled continuously"), "wakeupText replaced by the terse n key");
assert.equal(r0.spec.reasonWords, 12);
assert.deepEqual(r0.spec.orders, ["MOVE", "ATTACK", "DEFEND", "RECON", "FALL_BACK"], "channels from the manifest");
assert.equal(r0.spec.buy, true);
assert.deepEqual(r0.spec.wake, { minSec: 20, maxSec: 300, defaultSec: 60 });
assert.ok(terseSchema(r0.spec).properties.o);
const rv = build({ sit: sitA(), config: cfg({ output: { mode: "json_object", terse: false, reasonWords: 12 } }) });
assert.ok(rv.system.includes('"orders":[{"squad":"<id>"'), "verbose REPLY block when terse is off");
assert.ok(rv.system.includes("You are not polled continuously"), "verbose: wakeup paragraph");
assert.equal(rv.spec, null);
const rw = build({ sit: sitA(), config: cfg({ output: { mode: "text", terse: true, reasonWords: 7 } }) });
assert.ok(rw.system.includes("Reasons max 7 words"));
const rn = build({ sit: sitA(), config: cfg({ wakeup: { dynamic: false, defaultSec: 60, minSec: 20, maxSec: 300 } }) });
assert.ok(!rn.system.includes('"n":[') && rn.spec.wake === null, "static wakeup: no n key");
assert.ok(P.SYSTEM_V2.length + 500 < r0.system.length);

// --- stable system prefix: situations that differ only in squads / contacts / env / wake give a byte-identical system prompt
const sitB = { ...sitA(), time: 2400, squads: [{ id: "US_9", kind: "armor", pos: [5, 0, 5], agents: 3, startAgents: 4 }], contacts: [{ pos: [9, 0, 9], desc: "x", ageSec: 3 }],
  env: { time: "21:30", daylight: "night", weather: "Rain", next: "Clear", nextInMin: 20 }, wake: { reason: "trigger", triggers: ["new contact"], sinceLastSec: 10 }, events: ["C captured by USSR"] };
const rb = build({ sit: sitB, journal: "HQ JOURNAL\nintent: hold" });
assert.equal(rb.system, r0.system, "stable system prefix");
assert.notEqual(rb.user, r0.user);
const iSit = rb.user.indexOf("Current situation:"), iWake = rb.user.indexOf("WAKE"), iEnv = rb.user.indexOf("ENVIRONMENT:"), iJ = rb.user.indexOf("HQ JOURNAL");
assert.ok(iSit === 0 && iSit < iWake && iWake < iEnv && iEnv < iJ, "user = situation, wake, environment, journal");
assert.ok(!rb.system.includes("ENVIRONMENT") && !rb.system.includes("HQ JOURNAL\nintent"), "volatile env / journal live in the user message");
assert.ok(!rb.user.slice(0, iWake).includes('"wake"'), "wake object is not inside the situation JSON");
assert.deepEqual(Object.keys(rb.sections), ["rules", "commands", "persona", "situation", "wake", "env", "journal"]);
assert.ok(rb.system.includes("AGGRESSIVE"), "persona (per faction, stable) stays in the system prompt");
assert.equal(build({ sit: { ...sitA(), hq: undefined } }).sections.persona, undefined, "no trait: no persona section");
// the journal rule is config-stable; with memory off it is not in the rules
assert.ok(r0.system.includes("HQ JOURNAL:") && !build({ sit: sitA(), config: cfg({ memory: { enabled: false } }) }).system.includes("HQ JOURNAL:"));

// --- conditional paragraphs
const f = (sit, manifest = fobManifest) => build({ sit, manifest });
const base = sitA();
assert.ok(!build({ sit: base }).system.includes("FOBS:"), "builtin manifest has no FOB intents: no FOB paragraph");
assert.ok(f(base).system.includes("FOBS:"), "FOB intents + no fobRules: paragraph");
assert.ok(f({ ...base, fobRules: { ready: true } }).system.includes("FOBS:"), "ready: paragraph");
assert.ok(!f({ ...base, fobRules: { ready: false, readyReason: "no front" } }).system.includes("FOBS:"), "ready=false and no fobs: no paragraph");
assert.ok(f({ ...base, fobRules: { ready: false }, fobs: [{ id: "F1", state: "building" }] }).system.includes("FOBS:"), "ready=false but a FOB exists: paragraph");
assert.ok(f(base).system.includes("ESTABLISH_FOB: ESTABLISH_FOB desc"), "FOB command lines stay even without the paragraph");
assert.ok(!f(base).system.includes("LOGISTICS:"), "no active fob / convoy: no LOGISTICS");
assert.ok(!f({ ...base, fobs: [{ id: "F1", state: "building" }] }).system.includes("LOGISTICS:"), "building fob only: no LOGISTICS");
assert.ok(f({ ...base, fobs: [{ id: "F1", state: "active" }] }).system.includes("LOGISTICS:"), "active fob: LOGISTICS");
assert.ok(f({ ...base, convoys: [{ id: "K1", state: "en_route" }] }).system.includes("LOGISTICS:"), "convoy: LOGISTICS");
const noResupply = { ...fobManifest, intents: fobManifest.intents.filter((e) => e.name !== "RESUPPLY") };
assert.ok(!f({ ...base, fobs: [{ id: "F1", state: "active" }] }, noResupply).system.includes("LOGISTICS:"), "no RESUPPLY intent: no LOGISTICS");
assert.ok(!build({ sit: base }).system.includes("HELP CALLS:") && !build({ sit: { ...base, helpCalls: [] } }).system.includes("HELP CALLS:"), "no help calls: no paragraph");
const hc = build({ sit: { ...base, helpCalls: [{ id: 1, player: "Al", ageSec: 5, answeredBy: "" }] } });
assert.ok(hc.system.includes("HELP CALLS:") && hc.sections.helpCalls, "help calls: paragraph");
assert.ok(hc.system.startsWith(r0.system.slice(0, r0.system.length)), "conditional paragraphs are appended after the stable part");
const all = f({ ...base, fobs: [{ id: "F1", state: "active" }], helpCalls: [{ id: 1 }] });
assert.ok(all.system.indexOf("FOBS:") < all.system.indexOf("LOGISTICS:") && all.system.indexOf("LOGISTICS:") < all.system.indexOf("HELP CALLS:"));
assert.ok(all.spec.fobs.includes("CONVOY") && all.system.includes('["CONVOY","<origin>","<dest>"]'), "terse spec lists the manifest's fob channels");
assert.equal(partsOf(all), all.system);

// --- protocol v1 is unchanged: wake stays inside the situation, no sections beyond rules/situation
const v1 = P.buildCommanderPrompt({ config: cfg(), sit: { faction: "US", squads: [], contacts: [], wake: { reason: "first" } } });
assert.ok(v1.system.startsWith(P.SYSTEM) && v1.user.includes('"wake":{"reason":"first"}') && v1.spec === null);

// --- SITREP prompt
const orders = [{ squad: "US_1", command: "ATTACK", target: "C", reason: "take the mill" }, { squad: "US_2", command: "MOVE", pos: [5, 6], reason: "reposition" }];
const s1 = P.buildSitrepPrompt({ sit: sitA(), orders });
assert.ok(s1.system.includes("headline, situation, enemy, mission, execution, logistics") && s1.system.includes("Example (invented)"));
assert.ok(s1.system.includes("Tone:"), "persona tone from the trait");
assert.ok(s1.user.includes('"reason":"take the mill"') && s1.user.includes('"pos":[5,6]') && !s1.user.includes("structures") && !s1.user.includes('"wake"'));
assert.ok(P.buildSitrepPrompt({ sit: sitA(), orders: [] }).user.includes("none yet"));
assert.ok(!s1.system.includes("Open help calls") && P.buildSitrepPrompt({ sit: { ...sitA(), helpCalls: [{ id: 1 }] }, orders }).system.includes("Open help calls"));
assert.deepEqual(Object.keys(s1.sections), ["rules", "persona", "situation", "orders"]);
assert.equal(s1.sections.rules + s1.sections.persona, s1.system);
assert.ok(approxTokens(s1.system) > 200);

console.log("test_commanderPrompt: all passed");

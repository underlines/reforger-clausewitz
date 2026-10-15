// Offline test of the lean situation wiring in agent/commanderPrompt.mjs (config prompt {lean, staticBlock, contactMax, contactMaxAgeSec}). No network, no game.
//   node agent/test_leanPrompt.mjs
//  - lean off (no prompt group, or lean:false) = the raw-situation prompts, no lean marker anywhere; protocol v1 never lean
//  - lean on: dynamic-only tables in the user message, static block at the END of the system prompt (staticBlock) or at the head of the user message, ONE encoding note
//  - every field name that the rule texts mention still exists in the lean output of a realistic fixture
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import * as P from "./commanderPrompt.mjs";
import { leanSituation } from "./promptKit.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
const load = (f) => JSON.parse(fs.readFileSync(path.join(DIR, "..", "fixtures", f), "utf8"));
const sit0 = { ...load("large_situation.json"), wake: { reason: "trigger", triggers: ["new contact"], sinceLastSec: 30 } };
const manifest = load("large_manifest.json");
const orders = sit0.squads.filter((s) => s.objective).slice(0, 5).map((s) => ({ squad: s.id, command: s.objective.command, target: s.objective.target, reason: s.objective.reason }));
const base = { wakeup: { dynamic: true, defaultSec: 60, minSec: 20, maxSec: 300 }, memory: { enabled: true }, output: { mode: "json_object", terse: true, reasonWords: 12 } };
const cfg = (planner, prompt) => ({ ...base, planner: { enabled: planner, intervalSec: 180 }, ...(prompt ? { prompt } : {}) });
const LEAN = { lean: true, staticBlock: true, cache: true, contactMax: 25, contactMaxAgeSec: 600 };
const builders = {
  orders: (config, sit = sit0) => P.buildCommanderPrompt({ config: { ...config, planner: { enabled: true, intervalSec: 180 } }, sit, manifest, journal: "" }),
  combined: (config, sit = sit0) => P.buildCommanderPrompt({ config: { ...config, planner: { enabled: false, intervalSec: 180 } }, sit, manifest, journal: "" }),
  planner: (config, sit = sit0) => P.buildPlannerPrompt({ config, sit, manifest, journal: "", orders, trigger: "" }),
  sitrep: (config, sit = sit0) => P.buildSitrepPrompt({ config, sit, orders }),
};
const count = (s, sub) => s.split(sub).length - 1;
const { structures: _s, wake: _w, ...rawNoWake } = sit0;

// ---- lean off: raw situation, no markers, same with or without the prompt group
for (const [name, b] of Object.entries(builders)) {
  const none = b(cfg(true)), off = b(cfg(true, { ...LEAN, lean: false }));
  assert.equal(none.system, off.system, `${name}: lean:false = no prompt group (system)`);
  assert.equal(none.user, off.user, `${name}: lean:false = no prompt group (user)`);
  assert.ok(!/SITUATION ENCODING|MAP AND CATALOG/.test(none.system + none.user), `${name}: no lean markers when off`);
  assert.ok(!("static" in none.sections), `${name}: no static section when off`);
  assert.ok(none.user.startsWith("Current situation:\n{"), `${name}: raw JSON situation`);
}
assert.equal(builders.combined(cfg(false)).user, "Current situation:\n" + JSON.stringify(rawNoWake) + "\n\nWAKE (why you are consulted now):\n" + JSON.stringify(sit0.wake) + "\n\n" + builders.combined(cfg(false)).sections.env, "off: user = raw situation (minus structures, wake), wake, env");
assert.ok(builders.planner(cfg(true)).user.startsWith("Current situation:\n" + JSON.stringify(rawNoWake)), "off: planner raw situation");
assert.ok(builders.sitrep(cfg(true)).user.startsWith("Current situation:\n" + JSON.stringify(rawNoWake)), "off: sitrep raw situation");
// protocol v1 is never lean
{
  const v1 = { faction: "US", time: 5, war: { supplies: 1 }, squads: [{ id: "US_1", kind: "infantry", pos: [1, 2, 3], agents: 4, startAgents: 4 }], objectives: [{ id: "A", pos: [1, 2] }] };
  const a = P.buildCommanderPrompt({ config: cfg(false, LEAN), sit: v1, manifest }), b = P.buildCommanderPrompt({ config: cfg(false), sit: v1, manifest });
  assert.equal(a.system, b.system); assert.equal(a.user, b.user);
  assert.ok(a.user.includes('"squads":[{'));
}

// ---- lean on
for (const [name, b] of Object.entries(builders)) {
  const r = b(cfg(true, LEAN));
  assert.equal(count(r.system + r.user, "SITUATION ENCODING"), 1, `${name}: encoding note exactly once`);
  assert.ok(r.sections.rules.includes("SITUATION ENCODING"), `${name}: note is in the rules`);
  assert.ok(!/\[x, ?y, ?z\]/.test(r.sections.rules), `${name}: rules no longer say [x,y,z]`);
  assert.ok(r.system.endsWith(r.sections.static), `${name}: static block ends the system prompt`);
  assert.ok(r.sections.static.startsWith("MAP AND CATALOG (static)\n") && r.sections.static.endsWith("}\n"), `${name}: static section shape`);
  assert.ok(!r.user.includes("MAP AND CATALOG"), `${name}: static block not in the user message`);
  const dyn = r.user.split("\n")[1];
  assert.ok(dyn.includes('"squads":{"cols":["id"') && !dyn.includes('"squads":[{'), `${name}: squads are a table`);
  assert.ok(!dyn.includes('"structures"') && !dyn.includes('"fobTasks"') && !dyn.includes('"dormant"') && !dyn.includes('"heading"'), `${name}: structures / fobTasks / dormant / heading never sent`);
  assert.ok(r.sections.static.includes('"warMode"') && !dyn.includes('"mode":{'), `${name}: war mode is in the static block`);
  assert.ok(r.system.length + r.user.length < b(cfg(true)).system.length + b(cfg(true)).user.length, `${name}: smaller than raw`);
  // staticBlock off: the same block heads the user message
  const u = b(cfg(true, { ...LEAN, staticBlock: false }));
  assert.ok(!u.system.includes("MAP AND CATALOG (static)\n{"), `${name}: staticBlock:false keeps it out of the system prompt`);
  assert.ok(u.user.startsWith(r.sections.static + "\nCurrent situation:\n"), `${name}: staticBlock:false puts it first in the user message`);
  assert.equal(u.sections.static, r.sections.static, `${name}: same static section either way`);
}
// system = concatenation of its sections (lean + static in system)
{
  const r = builders.combined(cfg(true, LEAN)), s = r.sections;
  assert.equal(r.system, ["rules", "commands", "persona", "fobs", "logistics", "helpCalls", "static"].map((k) => s[k] ?? "").join(""));
  const o = builders.orders(cfg(true, LEAN));
  assert.deepEqual(Object.keys(o.sections), ["rules", "commands", "persona", "helpCalls", "static", "situation", "plan", "wake", "env", "journal"].filter((k) => k in o.sections));
  assert.ok(Object.keys(o.sections).indexOf("static") < Object.keys(o.sections).indexOf("situation"));
  const pl = builders.planner(cfg(true, LEAN));
  assert.equal(pl.system, ["rules", "commands", "fobs", "logistics", "static"].map((k) => pl.sections[k] ?? "").join(""));
  const sr = builders.sitrep(cfg(true, LEAN));
  assert.equal(sr.system, sr.sections.rules + (sr.sections.persona ?? "") + "\n" + sr.sections.help + sr.sections.static);
}

// ---- profiles: what each prompt gets
{
  const o = builders.orders(cfg(true, LEAN)), c = builders.combined(cfg(true, LEAN)), p = builders.planner(cfg(true, LEAN)), s = builders.sitrep(cfg(true, LEAN));
  assert.ok(!/"catalog"|"fobSites"/.test(o.sections.static) && !/"fobRules"|"logisticsRules"|"convoys"/.test(o.user), "orders call: no catalog / FOB / logistics data");
  assert.ok(o.sections.static.includes('"objectives"') && o.user.includes('"fobs"'), "orders call keeps objectives and own fobs");
  for (const [n, r] of [["combined", c], ["planner", p]]) {
    assert.ok(r.sections.static.includes('"catalog"') && r.sections.static.includes("(crew"), `${n}: catalog with descriptions in the static block`);
    assert.ok(r.sections.static.includes('"fobSites"'), `${n}: fobSites in the static block`);
    assert.ok(r.user.includes('"fobRules":{') && r.user.includes('"convoys":{"cols"'), `${n}: fobRules / convoys in the situation`);
  }
  assert.ok(!/"catalog"/.test(s.sections.static) && !/"fobRules"/.test(s.user), "sitrep: no catalog / FOB rules");
  // contacts: sitrep keeps place / grid / size; orders + planner drop only the redundant grid
  const cols = (r) => JSON.parse(r.user.split("\n")[1]).contacts.cols;
  for (const col of ["place", "grid", "size", "kind", "ageSec", "pos"]) assert.ok(cols(s).includes(col), `sitrep contacts keep ${col}`);
  for (const r of [o, c, p]) { assert.ok(cols(r).includes("place") && cols(r).includes("size") && !cols(r).includes("grid"), "orders / planner contacts: place + size, no grid"); }
  // fobSites order: the dynamic table keeps the game's order (frontline first), the static one is sorted by id
  assert.ok(/first fobSites row of the situation table/.test(c.sections.fobs) && !/first fobSites entry/.test(c.sections.fobs));
  // contact limits
  const few = JSON.parse(builders.orders(cfg(true, { ...LEAN, contactMax: 5 })).user.split("\n")[1]).contacts;
  assert.equal(few.rows.length, 5);
  const fresh = JSON.parse(builders.orders(cfg(true, { ...LEAN, contactMaxAgeSec: 60 })).user.split("\n")[1]).contacts;
  assert.ok(fresh.rows.length < 25 && fresh.rows.length > 0);
}

// ---- static block is stable against dynamic data; bases stock is dynamic
{
  const L1 = leanSituation(sit0), sit2 = JSON.parse(JSON.stringify(sit0));
  sit2.bases[0].stock += 500; sit2.objectives[0].owner = "RHS_AFRF"; sit2.objectives[0].progress = -100; sit2.objectives[0].contested = true;
  assert.equal(JSON.stringify(leanSituation(sit2).static), JSON.stringify(L1.static), "static unchanged by stock / owner / progress");
  assert.ok(L1.dynamic.bases.cols.includes("stock") && L1.dynamic.bases.cols.includes("faction"), "bases stock + join key are dynamic");
  assert.ok(!JSON.stringify(L1.static.bases).includes("640"));
}

// ---- every field name the rules mention exists in the lean output
function names(lean) {
  const bare = new Set(), paths = new Set();
  const walk = (v, pre) => {
    if (Array.isArray(v)) { for (const x of v) walk(x, pre); return; }
    if (!v || typeof v !== "object") return;
    if (Array.isArray(v.cols) && Array.isArray(v.rows)) {
      for (const c of v.cols) { paths.add(pre ? `${pre}.${c}` : c); paths.add(c); for (const seg of c.split(".")) bare.add(seg); }
      return;
    }
    for (const [k, x] of Object.entries(v)) { const p = pre ? `${pre}.${k}` : k; paths.add(p); bare.add(k); walk(x, p); }
  };
  walk(lean.static, ""); walk(lean.dynamic, "");
  // war.mode lives in the static block as warMode (the note says so)
  for (const p of [...paths]) if (p === "warMode" || p.startsWith("warMode.")) paths.add("war.mode" + p.slice("warMode".length));
  return { bare, paths };
}
// identifiers in rule texts that are NOT situation fields (wake object, reply keys, api names)
const NOT_FIELDS = new Set(["sinceLastSec", "a.b"]); // wake object field; the note's own placeholder
const camelDotted = /\b(?:[a-z][a-z0-9]*[A-Z][A-Za-z0-9]*|[a-z]+(?=\.[a-zA-Z]))(?:\.[A-Za-z][A-Za-z0-9]*)*/g;
const PLAIN = { // plain lower-case field words the rules lean on (each must appear in the rule text AND resolve)
  orders: ["owner", "contested", "progress", "garrison", "reserved", "kind", "agents", "humanLed", "idleSec", "pos", "radius", "value", "side", "objective", "mission", "vehicle", "health", "status", "ageSec"],
  combined: ["owner", "contested", "progress", "garrison", "reserved", "cost", "kind", "desc", "agents", "humanLed", "idleSec", "supplies", "tickets", "enemyTickets", "income", "fobs", "convoys", "stock", "tier", "state", "build", "underAttack", "side", "front", "toFront", "place", "grid"],
  planner: ["owner", "contested", "progress", "cost", "kind", "desc", "agents", "humanLed", "supplies", "tickets", "income", "stock", "side", "front", "toFront", "place", "grid"],
  sitrep: ["place", "grid", "size", "vehicles", "infantry"],
};
for (const [name, b] of Object.entries(builders)) {
  const r = b(cfg(true, LEAN));
  const rules = ["rules", "commands", "persona", "fobs", "logistics", "helpCalls", "help"].map((k) => r.sections[k] ?? "").join("\n");
  const role = { orders: "orders", combined: "commander", planner: "planner", sitrep: "sitrep" }[name];
  const { bare, paths } = names(P.leanFor(Object.fromEntries(Object.entries(sit0).filter(([k]) => k !== "structures" && k !== "wake")), cfg(true, LEAN), role));
  const unresolved = [];
  for (const tok of new Set(rules.match(camelDotted) ?? [])) {
    if (NOT_FIELDS.has(tok)) continue;
    const ok = tok.includes(".") ? paths.has(tok) || [...paths].some((p) => p.endsWith("." + tok)) : bare.has(tok); // dotted: full path or relative to its table / group
    if (!ok) unresolved.push(tok);
  }
  assert.deepEqual(unresolved, [], `${name}: field names in the rules missing from the lean output: ${unresolved.join(", ")}`);
  for (const w of PLAIN[name]) {
    assert.ok(new RegExp(`\\b${w}\\b`).test(rules), `${name}: (test table stale) rules do not mention ${w}`);
    assert.ok(bare.has(w) || w === "size" && bare.has("size"), `${name}: rules mention "${w}" but the lean output has no such field`);
  }
  // spot checks of flags the rules depend on, from real rows
  if (name !== "sitrep") {
    const dyn = JSON.parse(r.user.split("\n")[1]);
    const sq = dyn.squads, ix = (c) => sq.cols.indexOf(c);
    assert.ok(ix("garrison") >= 0 && ix("reserved") >= 0 && ix("idleSec") >= 0 && ix("humanLed") >= 0 && ix("objective.status") >= 0 && ix("vehicle.health") >= 0, `${name}: squad flags present`);
    assert.ok(sq.rows.some((row) => row[ix("garrison")] === true) && sq.rows.some((row) => row[ix("reserved")]), `${name}: garrison / reserved values survive`);
    assert.equal(dyn.war.engineAI.headroom, 84, `${name}: war.engineAI.headroom`);
    assert.ok(dyn.baseRules.idleDespawnMin === 6 && dyn.baseRules.recentlyDespawned === 2, `${name}: baseRules`);
    assert.ok(dyn.helpCalls.cols.includes("answeredBy"), `${name}: helpCalls.answeredBy`);
  }
  if (name === "combined" || name === "planner") {
    const dyn = JSON.parse(r.user.split("\n")[1]), st = JSON.parse(r.sections.static.split("\n")[1]);
    assert.equal(dyn.fobRules.ready, false); assert.ok(dyn.fobRules.readyReason.length > 0); assert.equal(dyn.logisticsRules.lowStock, 120);
    assert.ok(st.catalog.cols.includes("cost") && st.catalog.cols.includes("kind") && st.catalog.cols.includes("desc"), "catalog cost / kind / desc");
    assert.ok(dyn.fobs.cols.includes("stock") && dyn.fobs.cols.includes("state") && dyn.convoys.cols.includes("cargo"));
  }
  if (name === "orders") {
    const dyn = JSON.parse(r.user.split("\n")[1]), st = JSON.parse(r.sections.static.split("\n")[1]);
    assert.ok(dyn.objectives.cols.includes("owner") && dyn.objectives.cols.includes("contested") && dyn.objectives.cols.includes("progress") && st.objectives.cols.includes("side"));
  }
}

console.log("test_leanPrompt: all passed");

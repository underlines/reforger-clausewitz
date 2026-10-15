// Offline unit test of agent/promptKit.mjs (no network, no game). node agent/test_promptKit.mjs
import assert from "node:assert/strict";
import * as K from "./promptKit.mjs";

// --- approxTokens / sectionStats
assert.equal(K.approxTokens("abcd"), 1);
assert.equal(K.approxTokens("abcde"), 2);
assert.equal(K.approxTokens(null), 0);
assert.deepEqual(K.sectionStats({ a: "x".repeat(40), b: "" }), { a: { chars: 40, tokens: 10 }, b: { chars: 0, tokens: 0 } });

// --- expandTerse: every channel
const full = K.expandTerse({
  o: [["US_Inf1", "ATTACK", "C", "push"], ["US_Inf2", "MOVE", [10, 20], "reposition"], ["US_Inf3", "FALL_BACK"]],
  b: [["rifle_squad", 2, "ATTACK", "C", "need force"], ["recon_team", 1, "RECON", [5, 6]]],
  f: [["ESTABLISH_FOB", "S1", "front"], ["UPGRADE_FOB", "F1"], ["RESUPPLY", "F1", "low"], ["CONVOY", "HQ", "F1", "move stock"]],
  m: [["US_Human1", [["CAPTURE", "D", "take it"], ["RECON", [1, 2], "look"]]]],
  k: 1, n: [120, "quiet"],
  sitrep: { headline: "h" },
});
assert.deepEqual(full.orders, [
  { squad: "US_Inf1", command: "ATTACK", target: "C", reason: "push" },
  { squad: "US_Inf2", command: "MOVE", pos: [10, 20], reason: "reposition" },
  { squad: "US_Inf3", command: "FALL_BACK", reason: "" },
]);
assert.deepEqual(full.buy, [
  { type: "rifle_squad", count: 2, command: "ATTACK", target: "C", reason: "need force" },
  { type: "recon_team", count: 1, command: "RECON", pos: [5, 6], reason: "" },
]);
assert.deepEqual(full.fobs, [
  { command: "ESTABLISH_FOB", site: "S1", reason: "front" },
  { command: "UPGRADE_FOB", fob: "F1", reason: "" },
  { command: "RESUPPLY", fob: "F1", reason: "low" },
  { command: "CONVOY", origin: "HQ", dest: "F1", reason: "move stock" },
]);
assert.deepEqual(full.missions, [{ squad: "US_Human1", offers: [{ kind: "CAPTURE", target: "D", brief: "take it" }, { kind: "RECON", pos: [1, 2], brief: "look" }] }]);
assert.equal(full.keep, true);
assert.equal(full.next_check_sec, 120);
assert.equal(full.next_check_reason, "quiet");
assert.deepEqual(full.sitrep, { headline: "h" });

// keep alone / empty
const keep = K.expandTerse({ k: 1 });
assert.deepEqual([keep.orders, keep.buy, keep.fobs, keep.missions, keep.keep], [[], [], [], [], true]);
assert.equal("next_check_sec" in keep, false);
const none = K.expandTerse({});
assert.equal(none.keep, false);
assert.deepEqual(none.orders, []);

// --- verbose pass-through (same object)
const verbose = { orders: [{ squad: "a", command: "MOVE", pos: [1, 2], reason: "x" }], keep: false, next_check_sec: 90 };
assert.equal(K.expandTerse(verbose), verbose);
const vkeep = { keep: true };
assert.equal(K.expandTerse(vkeep), vkeep);
// mixed: verbose + terse keys are merged
const mixed = K.expandTerse({ orders: [{ squad: "a", command: "MOVE", pos: [1, 2], reason: "" }], o: [["b", "DEFEND", "C"]] });
assert.equal(mixed.orders.length, 2);

// --- sloppy models
const sloppy = K.expandTerse({
  o: [{ s: "A", c: "attack", t: "C", r: "go" }, { squad: "B", command: "MOVE", p: [3, 4] }, "garbage", ["only"], null, 7],
  b: [{ type: "rifle_squad", count: "2", c: "DEFEND", t: "D" }, ["rifle_squad", "3", "MOVE", [1, 1], "r"]],
  f: [{ command: "ESTABLISH_FOB", site: "S1" }, ["WHAT", "x"], "junk"],
  m: [{ s: "H", o: [{ k: "RECON", p: [1, 2], b: "b" }] }, ["H2"]],
  k: "true",
  n: "150",
});
assert.deepEqual(sloppy.orders[0], { squad: "A", command: "ATTACK", target: "C", reason: "go" });
assert.deepEqual(sloppy.orders[1], { squad: "B", command: "MOVE", pos: [3, 4], reason: "" });
assert.deepEqual(sloppy.orders.slice(2), ["garbage", ["only"], null, 7], "malformed passed through as-is");
assert.deepEqual(sloppy.buy[0], { type: "rifle_squad", count: 2, command: "DEFEND", target: "D", reason: "" });
assert.equal(sloppy.buy[1].count, 3, "count string -> number");
assert.deepEqual(sloppy.fobs[0], { command: "ESTABLISH_FOB", site: "S1", reason: "" });
assert.deepEqual(sloppy.fobs.slice(1), [["WHAT", "x"], "junk"]);
assert.deepEqual(sloppy.missions[0], { squad: "H", offers: [{ kind: "RECON", pos: [1, 2], brief: "b" }] });
assert.deepEqual(sloppy.missions[1], ["H2"]);
assert.equal(sloppy.keep, true);
assert.equal(sloppy.next_check_sec, 150);
// non-objects never throw
for (const v of [null, undefined, 5, "x", []]) assert.doesNotThrow(() => K.expandTerse(v));
assert.doesNotThrow(() => K.expandTerse({ o: "x", b: 3, f: {}, m: null, n: { sec: 60, reason: "r" } }));
assert.equal(K.expandTerse({ n: { sec: 60, reason: "r" } }).next_check_reason, "r");

// --- terseSpec
const spec = { orders: ["MOVE", "ATTACK", "DEFEND", "RECON", "FALL_BACK"], buy: true, fobs: ["ESTABLISH_FOB", "UPGRADE_FOB", "RESUPPLY", "CONVOY"], missions: true, wake: { minSec: 60, maxSec: 600, defaultSec: 180 }, reasonWords: 12 };
const text = K.terseSpec(spec);
console.log("--- terseSpec (typical) ---\n" + text + "\n--- " + text.length + " chars ---");
assert.match(text, /12 words/);
assert.match(text, /"k":1/);
assert.match(text, /"n":\[120/);
assert.match(text, /60\.\.600/);
assert.match(text, /"<squad>"/);
assert.match(text, /CONVOY/);
const minimal = K.terseSpec({ orders: ["MOVE", "ATTACK"], buy: false, fobs: [], missions: false, wake: null, reasonWords: 8 });
assert.doesNotMatch(minimal, /"b":|"f":|"m":|"n":|next check|fobs|missions/);
assert.match(minimal, /8 words/);
assert.ok(minimal.length < text.length);

// --- terseSchema: structural check by hand + the spec example validates
function matchType(v, t) {
  const types = Array.isArray(t) ? t : [t];
  return types.some((x) => x === "string" ? typeof v === "string" : x === "number" ? typeof v === "number" : x === "integer" ? Number.isInteger(v)
    : x === "boolean" ? typeof v === "boolean" : x === "array" ? Array.isArray(v) : x === "null" ? v === null : x === "object" ? v && typeof v === "object" && !Array.isArray(v) : false);
}
function validate(v, s) {
  if (s.type && !matchType(v, s.type)) return false;
  if (s.items && Array.isArray(v)) { if (!v.every((x) => validate(x, s.items))) return false; }
  if (s.properties && v && typeof v === "object" && !Array.isArray(v)) {
    for (const [k, ps] of Object.entries(s.properties)) if (k in v && !validate(v[k], ps)) return false;
  }
  return true;
}
const schema = K.terseSchema(spec);
assert.equal(schema.type, "object");
assert.deepEqual(Object.keys(schema.properties).sort(), ["b", "f", "k", "m", "n", "o", "sitrep"]);
assert.doesNotThrow(() => JSON.stringify(schema));
// the example of the spec text, parsed out of the text
const exampleJson = text.slice(text.indexOf("{"), text.indexOf("}\n") + 1)
  .replace(/<[^>]+>/g, "x").replace(/\[x,z\]/g, "[1,2]");
const example = JSON.parse(exampleJson);
assert.ok(validate(example, schema), "spec example passes the schema");
assert.ok(validate(full && { o: [["a", "MOVE", [1, 2], "r"]], b: [["t", 1, "ATTACK", "C", "r"]], k: 1, n: [120, "r"] }, schema));
assert.ok(validate({}, schema));
assert.ok(!validate({ o: "x" }, schema), "schema rejects a non-array channel");
assert.ok(!validate({ o: [{ a: 1 }] }, schema), "schema rejects object items");
const expanded = K.expandTerse(example);
assert.ok(expanded.orders.length >= 2 && expanded.keep === true);
const minSchema = K.terseSchema({ orders: ["MOVE"], buy: false, fobs: [], missions: false, wake: null });
assert.deepEqual(Object.keys(minSchema.properties).sort(), ["k", "o", "sitrep"]);

// --- leanSituation on a realistic v2 situation
function makeSit(shift = 0) {
  const squads = [];
  for (let i = 0; i < 30; i++) {
    const veh = i % 3 === 0;
    squads.push({
      id: `US_Squad${i + 1}`, kind: veh ? "motorized" : "infantry", type: veh ? "motor_squad" : "rifle_squad",
      pos: [1000.123456 + i * 37.7 + shift, 12.3456, 2000.987654 - i * 21.1 + shift],
      agents: 6 + (i % 3), startAgents: 8, vehicle: veh ? { type: "truck_m998", health: 0.8734, mounted: true, state: "driving" } : null,
      humanLed: i === 5, wounded: 0, health: 0.91234, speed: 3.456789, heading: 123.456, state: i % 2 ? "moving" : "idle",
      objective: i % 4 === 0 ? null : { command: "ATTACK", target: "C", pos: [1500.5, 0, 1800.25], reason: "assault the hub with everything we have", ageSec: 100 + i, status: "active" } ,
      mission: i === 5 ? { state: "offered", kind: "CAPTURE", target: "D" } : null,
      idleSec: i % 4 === 0 ? 120 : 0, garrison: false, reserved: "", unknownExtra: { deep: { x: 1.23456 } },
    });
  }
  const contacts = [];
  for (let i = 0; i < 20; i++) contacts.push({ pos: [3000.5 + i * 11.11, 40.2, 3100.5 - i * 7.77], desc: i % 2 ? "2x infantry" : "BTR-70", kind: i % 2 ? "infantry" : "vehicle", ageSec: i < 12 ? i * 20 : 700 + i * 30, size: "squad", place: "near Morton", grid: "262-288" });
  const catalog = [];
  for (let i = 0; i < 12; i++) catalog.push({ type: `unit_type_${i}`, kind: i % 3 ? "infantry" : "armor", cost: 50 + i * 25, desc: `Description of unit type ${i}: a reasonably long catalog text that explains what this squad is good for in the field.` });
  return {
    v: 3, manifest: "abc123", faction: "US", time: 1234.5, timeLeftSec: 3200,
    war: { tickets: 350, enemyTickets: 310.5, supplies: 220, income: 60, liveAI: 38, aiCap: 50, vehicles: 3, vehicleCap: 6, phase: "running", mode: { id: "conflict", name: "Conflict", params: { timeLimit: 3600, startTickets: 400 } }, engineAI: { headroom: 12, extra: null } },
    objectives: [
      { id: "D", name: "Harbor", pos: [2000.4, 500.6], radius: 80, value: 2, owner: "USSR", contested: false, progress: -35.5, side: "left", garrisonHint: "" },
      { id: "C", name: "Timber Hub", pos: [1000.4, 1000.6], radius: 80, value: 3, owner: "none", contested: true, progress: 12.345, side: "centre" },
      { id: "A", name: "Alpha", pos: [100.4, 100.6], radius: 60, value: 1, owner: "US", contested: false, progress: 100, side: "right" },
    ],
    bases: [{ faction: "US", pos: [100.5, 100.5] }, { faction: "USSR", pos: [4000.5, 4000.5] }],
    catalog, squads, contacts,
    fobs: [{ id: "F1", state: "active", tier: 1, stock: 80, underAttack: false, pos: [900.5, 900.5] }],
    fobSites: [{ id: "S2", pos: [1200.5, 1300.5], grid: "12-13", place: "Pine Ridge", front: true, toFront: 120.5 }, { id: "S1", pos: [800.5, 700.5], grid: "08-07", place: "Oak Farm", front: false, toFront: 800 }],
    fobRules: { ready: true, cost: 150, max: 2, live: 1, inProgress: 0 },
    events: ["C Timber Hub contested", "US_Squad3 lost"],
    wake: { reason: "trigger", triggers: ["casualties"], sinceLastSec: 40 },
    structures: Array.from({ length: 40 }, (_, i) => ({ base: "US", kind: "arsenal", label: `Struct ${i}`, pos: [100 + i, 100], grid: "01-01" })),
    debug: { big: "x".repeat(500) },
    futureField: { alpha: 1.23456, beta: [1, 2, 3] },
    customList: [{ k: "a", v: 1.5 }, { k: "b", v: 2.5 }],
  };
}
const sit = makeSit();
const L = K.leanSituation(sit, { catalogDesc: false });
const st = L.stats;
console.log(`leanSituation: before=${st.beforeChars} after=${st.afterChars} (static=${st.staticChars}, dynamic=${st.dynamicChars}) -> ${(100 * st.afterChars / st.beforeChars).toFixed(1)}%`);
const L2 = K.leanSituation(sit, { catalogDesc: true });
console.log(`  with catalogDesc: after=${L2.stats.afterChars} (static=${L2.stats.staticChars})`);
assert.ok(st.afterChars < st.beforeChars * 0.45, "at least 55 % smaller");
assert.equal(st.staticChars, JSON.stringify(L.static).length);
assert.equal(st.dynamicChars, JSON.stringify(L.dynamic).length);
assert.equal(st.afterChars, st.staticChars + st.dynamicChars);
// dropped / filtered
const all = JSON.stringify(L);
assert.ok(!all.includes("Struct 0") && !all.includes('"debug"') && !all.includes("abc123"));
assert.ok(!JSON.stringify(L.static.catalog).includes("Description"), "catalog desc only on request");
assert.ok(JSON.stringify(L2.static.catalog).includes("Description"));
assert.deepEqual(L.static.catalog.cols.slice(0, 3), ["type", "kind", "cost"]);
// static content: objectives sorted by id with static fields only
assert.deepEqual(L.static.objectives.cols, ["id", "name", "pos", "radius", "value", "side"]);
assert.deepEqual(L.static.objectives.rows.map((r) => r[0]), ["A", "C", "D"]);
assert.deepEqual(L.static.objectives.rows[1][2], [1000, 1001], "pos rounded to ints");
assert.equal(L.static.faction, "US");
assert.equal(L.static.warMode.id, "conflict");
assert.equal(L.static.bases.rows.length, 2);
assert.ok(L.static.fobSites.cols.includes("place") && !L.static.fobSites.cols.includes("front"));
// dynamic content
assert.deepEqual(L.dynamic.objectives.cols.slice(0, 3), ["id", "owner", "contested"]);
assert.ok(L.dynamic.objectives.cols.includes("progress"));
assert.ok(!L.dynamic.objectives.cols.includes("garrisonHint"), "all-empty column dropped");
assert.ok(!L.dynamic.objectives.cols.includes("name"));
assert.equal(L.dynamic.squads.rows.length, 30);
const sc = L.dynamic.squads.cols;
assert.ok(sc.includes("vehicle.type") && sc.includes("objective.status") && sc.includes("objective.pos"), "one-level flatten");
assert.ok(!sc.includes("garrison") && !sc.includes("reserved"), "all-empty columns dropped");
assert.ok(sc.includes("unknownExtra.deep"), "unknown fields kept");
const row0 = L.dynamic.squads.rows[1];
assert.deepEqual(row0[sc.indexOf("pos")], [1038, 1980], "[x,y,z] -> [x,z] ints");
assert.equal(row0[sc.indexOf("health")], 0.91, "2 decimals");
assert.equal(row0[sc.indexOf("heading")], 123, ">=100 -> int");
// contacts: old ones dropped, freshest first, capped
assert.equal(L.dynamic.contacts.rows.length, 12);
const ai = L.dynamic.contacts.cols.indexOf("ageSec");
const ages = L.dynamic.contacts.rows.map((r) => r[ai]);
assert.deepEqual(ages, [...ages].sort((a, b) => a - b));
assert.equal(K.leanSituation(sit, { contactMax: 5 }).dynamic.contacts.rows.length, 5);
assert.equal(K.leanSituation(sit, { contactMaxAgeSec: 100 }).dynamic.contacts.rows.length, 6);
// war: mode moved to static, numbers rounded, rest kept
assert.equal(L.dynamic.war.enemyTickets, 311);
assert.ok(!("mode" in L.dynamic.war));
// unknown top-level fields and arrays of unknown objects survive
assert.deepEqual(L.dynamic.futureField, { alpha: 1.23, beta: [1, 2, 3] });
assert.deepEqual(L.dynamic.customList, { cols: ["k", "v"], rows: [["a", 1.5], ["b", 2.5]] });
assert.deepEqual(L.dynamic.events, sit.events);
assert.equal(L.dynamic.time, 1235);
assert.equal(L.dynamic.wake.reason, "trigger");
// static stable across calls with changed dynamic state; dynamic differs
const sitB = makeSit(55.5);
sitB.objectives.reverse();
sitB.war.supplies = 99;
sitB.contacts.reverse();
const LB = K.leanSituation(sitB, { catalogDesc: false });
assert.equal(JSON.stringify(LB.static), JSON.stringify(L.static), "static JSON byte-identical");
assert.notEqual(JSON.stringify(LB.dynamic), JSON.stringify(L.dynamic));
assert.equal(JSON.stringify(K.leanSituation(sit).static), JSON.stringify(K.leanSituation(sit).static));
// robustness
assert.doesNotThrow(() => K.leanSituation(null));
assert.doesNotThrow(() => K.leanSituation({}));
assert.doesNotThrow(() => K.leanSituation({ squads: [], contacts: "x", objectives: [1, 2], catalog: [{ type: "a" }, null] }));

console.log("promptKit: all tests passed");

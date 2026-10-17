// M4.6 FOB tool, pure parts (no sidecar, no network):   node dev/tools/agent/test_fobzoom.mjs
// zoom prompt + reply handling (fobZoomPrompt.mjs), the fobMap in the commander / planner prompts and the lean situation, terse ESTABLISH_FOB tuples.
import assert from "node:assert/strict";
import * as Z from "./fobZoomPrompt.mjs";
import * as P from "./commanderPrompt.mjs";
import * as K from "./promptKit.mjs";
import { macroFixture, zoomFixture } from "../fixtures/fobmap_fixture.mjs";

let n = 0;
const t = (name, fn) => { fn(); n++; console.log("  ok   " + name); };

// ---------- zoom digest ----------
t("zoomCells: 144 sub-cells with ids, glyphs and centres", () => {
  const cells = Z.zoomCells(zoomFixture());
  assert.equal(cells.length, 144);
  const a1 = cells[0], l12 = cells[143];
  assert.deepEqual([a1.id, a1.x, a1.z], ["a1", 5250, 7750]);
  assert.deepEqual([l12.id, l12.x, l12.z], ["l12", 6350, 6650]);
  assert.equal(cells.find((c) => c.id === "f6").terrain, "T");
  assert.equal(cells.find((c) => c.id === "c8").road, "=");
});

t("zoom prompt: system rules, intent, digest without the legend, no retry block on the first attempt", () => {
  const z = zoomFixture();
  const pr = Z.buildFobZoomPrompt({ req: { faction: "US", area: "E5", intent: "cover the Morton approach", attempt: 1, zoom: z } });
  assert.ok(pr.system.includes("keep-out") && pr.system.includes('"decline":true'));
  assert.ok(pr.user.includes("cover the Morton approach") && pr.user.includes('"rows"') && !pr.user.includes("sub-cell id = column letter"));
  assert.ok(!pr.user.includes("RETRY"));
  const tokens = K.approxTokens(pr.system + pr.user);
  assert.ok(tokens < 1500, "zoom call under 1500 tokens, got " + tokens);
  console.log(`       zoom prompt ~${tokens} tokens (system ${K.approxTokens(pr.system)}, user ${K.approxTokens(pr.user)})`);
});

t("zoom prompt: the retry carries the previous pick and the game's reason", () => {
  const pr = Z.buildFobZoomPrompt({ req: { faction: "US", area: "E5", intent: "x", attempt: 2, retry: { reason: "no road within 60 m", pos: [5750, 7050] }, zoom: zoomFixture() } });
  assert.ok(/RETRY \(attempt 2 of 2\)/.test(pr.user) && pr.user.includes("[5750,7050]") && pr.user.includes("no road within 60 m"));
});

t("normalizeZoomReply: cell, pos, decline, terse keys, garbage", () => {
  const z = zoomFixture();
  assert.deepEqual(Z.normalizeZoomReply({ cell: "F8", reason: "road" }, { zoom: z }), { ok: true, cell: "f8", reason: "road" });
  assert.deepEqual(Z.normalizeZoomReply({ c: "g9" }, { zoom: z }), { ok: true, cell: "g9", reason: "" });
  assert.deepEqual(Z.normalizeZoomReply({ pos: [5750.4, "7050"], reason: "r" }, { zoom: z }), { ok: true, pos: [5750, 7050], reason: "r" });
  assert.deepEqual(Z.normalizeZoomReply({ decline: true, reason: "nothing safe" }, { zoom: z }), { ok: true, decline: true, reason: "nothing safe" });
  assert.deepEqual(Z.normalizeZoomReply({ d: 1 }, { zoom: z }), { ok: true, decline: true, reason: "" });
  assert.equal(Z.normalizeZoomReply({ cell: "none" }, { zoom: z }).decline, true);
  assert.equal(Z.normalizeZoomReply({ hello: 1 }, { zoom: z }).ok, false);
  assert.equal(Z.normalizeZoomReply("x", {}).ok, false);
  assert.equal(Z.normalizeZoomReply({ cell: "zz99" }, { zoom: z }).ok, false, "not a sub-cell id and no pos");
});

t("parseZoomReply: JSON inside prose / fences, error without JSON", () => {
  assert.deepEqual(Z.parseZoomReply('Sure!\n```json\n{"cell":"a1"}\n```'), { cell: "a1" });
  assert.throws(() => Z.parseZoomReply("no json here"), /no JSON object/);
});

t("fakeZoomPick: usable ground beside a road nearest to `toward`; decline when none", () => {
  const pick = Z.fakeZoomPick({ zoom: zoomFixture() });
  assert.equal(pick.cell, "f8");
  const cells = Z.zoomCells(zoomFixture());
  const c = cells.find((x) => x.id === pick.cell);
  assert.ok(".b".includes(c.terrain) && c.road !== "_");
  const blocked = zoomFixture({ rows: zoomFixture().rows.map((r) => r.replace(/[=-]/g, "_")) });
  assert.equal(Z.fakeZoomPick({ zoom: blocked }).decline, true);
});

// ---------- prompts ----------
const manifest = (extra = []) => ({
  ...P.DEFAULT_MANIFEST, hash: "m", builtin: undefined,
  intents: [...P.DEFAULT_MANIFEST.intents, { name: "ESTABLISH_FOB", layer: "intent", channel: "fob", authority: "hq", cost: 0, tags: [], requires: [], desc: "Place a FOB.",
    params: [{ name: "area", type: "string" }, { name: "intent", type: "string" }, { name: "site", type: "string" }, { name: "reason", type: "string" }] }, ...extra],
});
const sit = (withMap) => ({
  v: 4, faction: "US", time: 900, war: { supplies: 500, tickets: 300, phase: "running", mode: { id: "c", name: "c", params: {} } },
  objectives: [{ id: "C", name: "Morton", pos: [5750, 7250], radius: 60, value: 1, owner: "none", grid: "057-072" }], bases: [{ faction: "US", pos: [1500, 0, 1500] }],
  squads: [], contacts: [], catalog: [], fobs: [], fobRules: { ready: true, cost: 300, max: 2, live: 0, inProgress: 0, earliestSec: 360, elapsedSec: 900 },
  ...(withMap ? { fobMap: macroFixture() } : { fobSites: [{ id: "S1", pos: [1, 2], name: "x" }] }),
});
const cfg = (o = {}) => ({ prompt: { lean: true, staticBlock: true }, output: { terse: true, reasonWords: 12 }, planner: { enabled: false }, ...o });

t("commander prompt with fobMap: two-step FOB paragraph, no fobSites wording, terse spec names the cell", () => {
  const o = P.buildCommanderPrompt({ config: cfg(), sit: sit(true), manifest: manifest() });
  assert.ok(o.sections.fobs.includes("STEP 1") && o.sections.fobs.includes("STEP 2"));
  assert.ok(o.sections.fobs.startsWith("FOBS:") && /STEP 2: the game then shows you only that cell/.test(o.sections.fobs));
  assert.ok(!/fobSites/.test(o.sections.fobs));
  assert.ok(o.sections.commands.includes('["ESTABLISH_FOB","<fobMap cell or none>","why"]'), "terse tuple example");
  assert.ok(o.sections.static.includes('"fobMap"') && o.sections.static.includes(macroFixture().rows[0]) && !o.user.includes(macroFixture().rows[0]), "terrain rows in the static block only");
  assert.ok(o.user.includes('"frontCell"') && o.user.includes('"objs"'), "front / objectives by cell in the dynamic part");
});

t("commander prompt without fobMap: the old fobSites paragraph is unchanged (old game)", () => {
  const o = P.buildCommanderPrompt({ config: cfg(), sit: sit(false), manifest: manifest() });
  assert.ok(o.sections.fobs.includes("fobSites") && !o.sections.fobs.includes("STEP 1"));
  assert.ok(o.sections.commands.includes('["ESTABLISH_FOB","<site>"]'));
});

t("verbose reply spec names area / intent when the situation has a fobMap", () => {
  const o = P.buildCommanderPrompt({ config: cfg({ output: { terse: false } }), sit: sit(true), manifest: manifest() });
  assert.ok(o.sections.commands.includes('"area":"<fobMap cell id or none>"'));
});

t("planner split: the orders call has no fobMap, the planner call has it", () => {
  const c = cfg({ planner: { enabled: true } });
  const orders = P.buildCommanderPrompt({ config: c, sit: sit(true), manifest: manifest() });
  assert.ok(!JSON.stringify(orders.sections).includes("fobMap"), "orders call: no fobMap anywhere");
  const plan = P.buildPlannerPrompt({ config: c, sit: sit(true), manifest: manifest() });
  assert.ok(plan.sections.fobs.includes("STEP 1") && plan.sections.static.includes('"fobMap"'));
});

t("fobMap size: macro digest of the 10 x 10 fixture stays small", () => {
  const m = macroFixture();
  const full = JSON.stringify(m).length, L = K.leanSituation({ fobMap: m });
  const tok = K.approxTokens(JSON.stringify(L.static)) + K.approxTokens(JSON.stringify(L.dynamic));
  console.log(`       fobMap raw ${full} chars (~${Math.ceil(full / 4)} tokens), lean static ${JSON.stringify(L.static).length} + dynamic ${JSON.stringify(L.dynamic).length} chars (~${tok} tokens)`);
  assert.ok(tok < 700, "macro digest under 700 tokens, got " + tok);
  assert.deepEqual(Object.keys(L.static.fobMap).sort(), ["cell", "cols", "legend", "n", "pois", "rows", "towns", "x0", "zTop"]);
  assert.deepEqual(Object.keys(L.dynamic.fobMap).sort(), ["contacts", "fobs", "frontCell", "front", "hq", "objs"].sort());
});

t("lean: the static fobMap part does not change when only the objectives / contacts move (cacheable prefix)", () => {
  const a = K.leanSituation({ fobMap: macroFixture() }), b = K.leanSituation({ fobMap: macroFixture({ contacts: [["A1", 9, "veh"]], front: "D", frontCell: "G3", objs: [] }) });
  assert.equal(JSON.stringify(a.static), JSON.stringify(b.static));
  assert.notEqual(JSON.stringify(a.dynamic), JSON.stringify(b.dynamic));
});

// ---------- terse tuples ----------
t("expandTerse: ESTABLISH_FOB tuple with a cell id stays {site} (validateV2 reads it as the area), [x,z] is a pos, objects keep area / intent", () => {
  const o = K.expandTerse({ f: [["ESTABLISH_FOB", "E5", "cover Morton"], ["ESTABLISH_FOB", [5750, "7050"], "why"], { c: "establish_fob", area: "none", reason: "too early" }] });
  assert.deepEqual(o.fobs[0], { command: "ESTABLISH_FOB", site: "E5", reason: "cover Morton" });
  assert.deepEqual(o.fobs[1], { command: "ESTABLISH_FOB", pos: [5750, 7050], reason: "why" });
  assert.deepEqual(o.fobs[2], { command: "ESTABLISH_FOB", area: "none", reason: "too early" });
});

console.log(`test_fobzoom: ${n} passed`);

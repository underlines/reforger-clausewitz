// node dev/tools/agent/test_personality.mjs
import assert from "node:assert/strict";
import { TRAITS, personaCommander, personaRadio, personaSitrep } from "./personality.mjs";

const KEYS = ["aggressive", "defensive", "methodical", "cautious", "opportunistic", "crazy"];
assert.deepEqual(Object.keys(TRAITS).sort(), [...KEYS].sort());
const sentences = (s) => (s.match(/[.!?](\s|$)/g) ?? []).length;
for (const k of KEYS) {
  const t = TRAITS[k];
  for (const f of ["label", "commander", "radio", "sitrep"]) assert.ok(typeof t[f] === "string" && t[f].length > 3, k + "." + f);
  for (const f of ["commander", "radio", "sitrep"]) assert.ok(sentences(t[f]) >= 1 && sentences(t[f]) <= 3, `${k}.${f} must be 1-3 sentences, got ${sentences(t[f])}`);
  const sit = { hq: { trait: k } };
  assert.ok(personaCommander(sit).includes(t.commander));
  assert.equal(personaRadio(sit), t.radio);
  assert.ok(personaSitrep(sit).includes(t.sitrep));
}
// crazy must stay useful, never refusing
assert.match(TRAITS.crazy.radio, /never refusing/);
for (const bad of [undefined, null, {}, { hq: null }, { hq: {} }, { hq: { trait: "" } }, { hq: { trait: "nope" } }, { hq: { trait: "toString" } }, { hq: { trait: 5 } }]) {
  assert.equal(personaCommander(bad), "");
  assert.equal(personaRadio(bad), "");
  assert.equal(personaSitrep(bad), "");
}
console.log("test_personality: ok");

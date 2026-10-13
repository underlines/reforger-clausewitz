// Pure-JS mirror of CLW_FriendlyWire.c (wire format F1): proves round trip, quantisation, sanitising and size per entry.
// Run: node dev/tools/test-friendly-wire.mjs
import assert from "node:assert/strict";

const VERSION = 1, MAX_LABEL = 24, MAX_KEY = 16, MAX_KIND = 16;
const clean = (t, max) => {
  let r = String(t ?? "").replace(/[|;,#\n\r\t]/g, " ").trim();
  return max > 0 && r.length > max ? r.slice(0, max) : r;
};
const toWire = (p) => {
  const head = `F1|${p.version ?? VERSION}|${clean(p.faction, MAX_KEY)}|${p.active ? 1 : 0}|${p.mask}|${p.identity}|${p.entries.length}#`;
  return head + p.entries.map((e) => [clean(e.t, 1), clean(e.key, MAX_KEY), e.x5, e.z5, e.h10, clean(e.kind, MAX_KIND), e.n, e.fl, clean(e.label, MAX_LABEL)].join(",")).join(";");
};
// Enforce Split(.., false) keeps empty tokens; Split(.., true) drops them. Entries use true on ";" and false on ",".
const fromWire = (w) => {
  const hash = w.indexOf("#");
  if (hash < 0) return null;
  const h = w.slice(0, hash).split("|");
  if (h.length < 7 || h[0] !== "F1" || parseInt(h[1]) !== VERSION) return null;
  const pic = { version: VERSION, faction: h[2], active: parseInt(h[3]) !== 0, mask: parseInt(h[4]), identity: parseInt(h[5]), entries: [] };
  for (const part of w.slice(hash + 1).split(";").filter((s) => s.length)) {
    const f = part.split(",");
    if (f.length < 8) continue;
    pic.entries.push({ t: f[0], key: f[1], x5: parseInt(f[2]), z5: parseInt(f[3]), h10: parseInt(f[4]), kind: f[5], n: parseInt(f[6]), fl: parseInt(f[7]), label: f[8] ?? "" });
  }
  return pic;
};
const quant = (x, z, yaw) => ({ x5: Math.floor(x / 5), z5: Math.floor(z / 5), h10: ((Math.round(yaw / 10) % 36) + 36) % 36 });

// quantisation: 12.8 km world => <= 2560, negatives floor, stationary jitter < 5 m inside a cell is identical
assert.deepEqual(quant(12799, 3, -179), { x5: 2559, z5: 0, h10: 18 });
assert.equal(quant(101, 0, 0).x5, quant(104.9, 0, 4.9).x5);
assert.equal(quant(-0.1, 0, 0).x5, -1);

const entries = [
  { t: "s", key: "US_G12", ...quant(6400.4, 3200.2, 90), kind: "mech", n: 6, fl: 1, label: "Alpha (Bob|;,#)\nX" },
  { t: "v", key: "US_V3", ...quant(5, -50, -90), kind: "air_rotary", n: 2, fl: 0, label: "UH-1H long name that exceeds the limit" },
  { t: "m", key: "US_G12.3", ...quant(10, 20, 359), kind: "infantry", n: 1, fl: 2, label: "" },
];
const pic = { faction: "US", active: true, mask: 15, identity: 0, entries };
const wire = toWire(pic);
assert.ok(!/\n/.test(wire));
const back = fromWire(wire);
assert.equal(back.entries.length, 3);
assert.equal(back.mask, 15);
assert.equal(back.entries[0].label, "Alpha (Bob    ) X");
assert.ok(back.entries[1].label.length <= MAX_LABEL);
assert.equal(back.entries[2].label, "");           // empty trailing label survives
assert.equal(back.entries[2].h10, 0);              // 359 deg => 36 steps => 0
for (const e of back.entries) assert.ok(!/[|;#]/.test(e.label + e.key));
assert.equal(toWire(back), wire);                   // round trip stable
assert.equal(fromWire(wire.replace("F1|1", "F1|2")), null);   // unknown version
assert.equal(fromWire("garbage"), null);
const inactive = fromWire(toWire({ faction: "US", active: false, mask: 0, identity: 1, entries: [] }));
assert.equal(inactive.active, false); assert.equal(inactive.entries.length, 0);

// size: typical entry and the worst case, 120 entries vs the 4000 char cap
const typical = toWire({ faction: "US", active: true, mask: 15, identity: 0, entries: [entries[0]] }).split("#")[1].length;
const worst = { t: "s", key: "K".repeat(16), x5: -2560, z5: -2560, h10: 35, kind: "k".repeat(16), n: 99, fl: 7, label: "L".repeat(24) };
const worstLen = toWire({ faction: "US", active: true, mask: 15, identity: 0, entries: [worst] }).split("#")[1].length;
console.log(`bytes/entry typical=${typical} worst=${worstLen}; 120 worst entries=${worstLen * 120 + 119} (cap 4000 trims the tail)`);
const typ120 = 120 * typical;
console.log(`120 typical entries = ${typ120} chars`);
assert.ok(typical < 60);
console.log("friendly wire: all assertions passed");

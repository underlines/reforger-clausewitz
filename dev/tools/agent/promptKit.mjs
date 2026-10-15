// Prompt kit for the Clausewitz commander: token estimates, the terse reply format (spec text, JSON schema, parser) and the lean
// situation (static/dynamic split + tabular encoding). Pure functions, no deps, content-agnostic (no faction keys, unit kinds or
// prefab names). Contract: dev/notes/llm-latency-design.md ("promptKit.mjs (A1)", "Lean situation").

export const approxTokens = (s) => Math.ceil(String(s ?? "").length / 4);

export function sectionStats(sections) {
  const out = {};
  for (const [name, text] of Object.entries(sections ?? {})) {
    const t = String(text ?? "");
    out[name] = { chars: t.length, tokens: approxTokens(t) };
  }
  return out;
}

// ---------------------------------------------------------------- terse reply spec

const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const isNum = (n) => typeof n === "number" && Number.isFinite(n);

// spec = {orders:[names], buy:bool, fobs:[names], missions:bool, wake:{minSec,maxSec,defaultSec}|null, reasonWords:12}
export function terseSpec(spec = {}) {
  const orders = Array.isArray(spec.orders) ? spec.orders.filter((x) => typeof x === "string" && x) : [];
  const fobs = Array.isArray(spec.fobs) ? spec.fobs.filter((x) => typeof x === "string" && x) : [];
  const buy = !!spec.buy, missions = !!spec.missions;
  const w = spec.wake && isNum(spec.wake.minSec) ? spec.wake : null;
  const words = isNum(spec.reasonWords) && spec.reasonWords > 0 ? Math.round(spec.reasonWords) : 12;

  const ex = [];
  const legend = [];
  if (orders.length) {
    const a = orders.includes("ATTACK") ? "ATTACK" : orders[0], b = orders.includes("MOVE") ? "MOVE" : (orders[1] ?? orders[0]);
    const items = [`["<squad>","${a}","C","why"]`, `["<squad>","${b}",[x,z],"why"]`];
    if (orders.includes("FALL_BACK")) items.push('["<squad>","FALL_BACK"]');
    ex.push(`"o":[${items.join(",")}]`);
    legend.push("o = orders [squad, command, target, reason]");
  }
  if (buy) {
    ex.push(`"b":[["<catalog type>",1,"${orders.find((n) => n === "ATTACK") ?? orders[0] ?? "ATTACK"}","C","why"]]`);
    legend.push("b = buy [type, count 1-3, command, target, reason]");
  }
  if (fobs.length) {
    const shape = (n) => (n === "CONVOY" ? `["CONVOY","<origin>","<dest>"]` : n === "ESTABLISH_FOB" ? `["ESTABLISH_FOB","<site>"]` : `["${n}","<fob>"]`);
    ex.push(`"f":[${fobs.map(shape).join(",")}]`);
    legend.push("f = fobs [command, site|fob|origin+dest]");
  }
  if (missions) {
    ex.push('"m":[["<human-led squad>",[["CAPTURE","D","brief"],["RECON",[x,z],"brief"]]]]');
    legend.push("m = missions [squad, [[kind, target, brief], ...]]");
  }
  const tail = ['"k":1'];
  if (w) tail.push('"n":[120,"why"]');
  ex.push(tail.join(","));

  let t = "REPLY with ONLY one JSON object, no prose, no code fences. Compact tuples, every key optional:\n{" + ex.join(",\n ") + "}\n";
  if (legend.length) t += legend.join("; ") + ".\n";
  t += 'Target slot: objective id string, or [x,z] position, or omit it. Reasons max ' + words + ' words. Omit empty keys. {"k":1} alone = keep all current orders.';
  if (w) t += ` n = next check [seconds ${w.minSec}..${w.maxSec}, reason]${isNum(w.defaultSec) ? ` (default ${w.defaultSec})` : ""}: long in quiet phases, short in fights.`;
  return t;
}

// JSON schema for response_format json_schema (strict:false). Permissive on purpose: tuples are arrays of string|number|array|null.
export function terseSchema(spec = {}) {
  const cell = { type: ["string", "number", "array", "null"] };
  const tuples = (desc) => ({ type: "array", description: desc, items: { type: "array", items: cell } });
  const props = {};
  if (Array.isArray(spec.orders) && spec.orders.length) props.o = tuples("orders: [squad, command, target|[x,z], reason]");
  if (spec.buy) props.b = tuples("buy: [type, count, command, target|[x,z], reason]");
  if (Array.isArray(spec.fobs) && spec.fobs.length) props.f = tuples("fobs: [command, site|fob|origin, dest?, reason?]");
  if (spec.missions) props.m = tuples("missions: [squad, [[kind, target|[x,z], brief], ...]]");
  props.k = { type: ["integer", "boolean"], description: "1 = keep all current orders" };
  if (spec.wake) props.n = { type: "array", description: "next check: [seconds, reason]", items: { type: ["string", "number"] } };
  props.sitrep = { type: "object" };
  return { type: "object", properties: props, additionalProperties: true };
}

// ---------------------------------------------------------------- terse -> canonical

const VERBOSE_KEYS = ["orders", "buy", "missions", "fobs", "keep"];
const TERSE_KEYS = ["o", "b", "f", "m", "k", "n"];
const up = (v) => (typeof v === "string" ? v.trim().toUpperCase() : v);
const num = (v) => (typeof v === "string" && v.trim() !== "" && Number.isFinite(Number(v)) ? Number(v) : v);
const has = (o, k) => o[k] !== undefined && o[k] !== null;
const pick = (o, ...keys) => { for (const k of keys) if (has(o, k)) return o[k]; return undefined; };

// slot value -> {target}|{pos}|{}
function slot(v) {
  if (typeof v === "string") return v === "" ? {} : { target: v };
  if (Array.isArray(v)) return { pos: v.map(num) };
  return {};
}
const withReason = (r) => ({ reason: r === undefined || r === null ? "" : r });

function aimOf(o) {
  const t = pick(o, "t", "target"), p = pick(o, "p", "pos");
  const a = slot(t);
  if (Array.isArray(p)) a.pos = p.map(num);
  return a;
}

function expandOrder(it) {
  if (Array.isArray(it)) {
    if (it.length < 2) return it;
    return { squad: it[0], command: up(it[1]), ...slot(it[2]), ...withReason(it[3]) };
  }
  if (isObj(it)) {
    return { squad: pick(it, "s", "squad"), command: up(pick(it, "c", "command")), ...aimOf(it), ...withReason(pick(it, "r", "reason")) };
  }
  return it;
}

function expandBuy(it) {
  if (Array.isArray(it)) {
    if (it.length < 2) return it;
    return { type: it[0], count: num(it[1]), command: up(it[2]), ...slot(it[3]), ...withReason(it[4]) };
  }
  if (isObj(it)) {
    return { type: pick(it, "y", "type", "ty"), count: num(pick(it, "n", "count")), command: up(pick(it, "c", "command")), ...aimOf(it), ...withReason(pick(it, "r", "reason")) };
  }
  return it;
}

function expandFob(it) {
  if (Array.isArray(it)) {
    const cmd = up(it[0]);
    if (cmd === "ESTABLISH_FOB") return { command: cmd, site: it[1], ...withReason(it[2]) };
    if (cmd === "UPGRADE_FOB" || cmd === "RESUPPLY") return { command: cmd, fob: it[1], ...withReason(it[2]) };
    if (cmd === "CONVOY") return { command: cmd, origin: it[1], dest: it[2], ...withReason(it[3]) };
    return it;
  }
  if (isObj(it)) {
    const o = { ...it };
    const c = pick(it, "c", "command");
    if (c !== undefined) { o.command = up(c); delete o.c; }
    const r = pick(it, "r", "reason");
    delete o.r;
    o.reason = r === undefined || r === null ? "" : r;
    return o;
  }
  return it;
}

function expandOffer(it) {
  if (Array.isArray(it)) {
    if (it.length < 2) return it;
    return { kind: up(it[0]), ...slot(it[1]), brief: it[2] === undefined || it[2] === null ? "" : it[2] };
  }
  if (isObj(it)) {
    const b = pick(it, "b", "brief");
    return { kind: up(pick(it, "k", "kind")), ...aimOf(it), brief: b === undefined || b === null ? "" : b };
  }
  return it;
}

function expandMission(it) {
  if (Array.isArray(it)) {
    if (it.length < 2 || !Array.isArray(it[1])) return it;
    return { squad: it[0], offers: it[1].map(expandOffer) };
  }
  if (isObj(it)) {
    const offers = pick(it, "o", "offers");
    return { squad: pick(it, "s", "squad"), offers: Array.isArray(offers) ? offers.map(expandOffer) : offers };
  }
  return it;
}

const truthy = (v) => v === true || v === 1 || v === "1" || (typeof v === "string" && v.toLowerCase() === "true");

export function expandTerse(obj) {
  if (!isObj(obj)) return obj;
  const hasVerbose = VERBOSE_KEYS.some((k) => k in obj);
  const hasTerse = TERSE_KEYS.some((k) => k in obj);
  if (hasVerbose && !hasTerse) return obj; // old verbose form: unchanged
  const list = (v, f) => (Array.isArray(v) ? v.map(f) : []);
  const cat = (verbose, terse) => [...(Array.isArray(verbose) ? verbose : []), ...terse];
  const out = {
    orders: cat(obj.orders, list(obj.o, expandOrder)),
    buy: cat(obj.buy, list(obj.b, expandBuy)),
    fobs: cat(obj.fobs, list(obj.f, expandFob)),
    missions: cat(obj.missions, list(obj.m, expandMission)),
    keep: truthy(obj.k) || truthy(obj.keep),
  };
  // next check: n:[sec,"reason"] | n:sec | n:{sec,reason}; verbose next_check_* wins when present
  const n = obj.n;
  let sec = obj.next_check_sec, why = obj.next_check_reason;
  if (Array.isArray(n)) { sec ??= n[0]; why ??= n[1]; }
  else if (isObj(n)) { sec ??= pick(n, "sec", "s", "seconds"); why ??= pick(n, "reason", "r"); }
  else if (n !== undefined && n !== null) sec ??= n;
  sec = num(sec);
  if (isNum(sec)) out.next_check_sec = sec;
  if (typeof why === "string" && why) out.next_check_reason = why;
  if (obj.sitrep !== undefined) out.sitrep = obj.sitrep;
  return out;
}

// ---------------------------------------------------------------- lean situation

const DROP_KEYS = new Set(["structures", "debug", "manifest", "v"]);
const STATIC_SCALARS = new Set(["faction", "map", "world", "mapInfo", "mode"]);
// array key -> {static: fields that go into the static block (in this order), sort: row sort key}
const STATIC_ARRAYS = {
  objectives: { fields: ["id", "name", "pos", "radius", "value", "side"], sort: "id" },
  fobSites: { fields: ["id", "name", "pos", "grid", "place"], sort: "id" },
  bases: { fields: null, sort: "faction" },
  catalog: { fields: null, sort: "type" },
};

const rnd = (n) => (Math.abs(n) >= 100 ? Math.round(n) : Math.round(n * 100) / 100);
const isPosKey = (k) => /(^|\.)pos$/i.test(k) || /Pos$/.test(k);

function round(v, key = "") {
  if (typeof v === "number") return Number.isFinite(v) ? rnd(v) : null;
  if (Array.isArray(v)) {
    if (isPosKey(key) && v.length === 3 && v.every(isNum)) return [Math.round(v[0]), Math.round(v[2])]; // [x,y,z] -> [x,z]
    if (isPosKey(key) && v.length === 2 && v.every(isNum)) return [Math.round(v[0]), Math.round(v[1])];
    return v.map((x) => round(x, key));
  }
  if (isObj(v)) { const o = {}; for (const [k, x] of Object.entries(v)) if (x !== undefined) o[k] = round(x, k); return o; }
  return v;
}

const emptyVal = (v) => v === null || v === undefined || v === "" || v === false || (Array.isArray(v) && v.length === 0) || (isObj(v) && Object.keys(v).length === 0);

function flatten(row) {
  const o = {};
  for (const [k, v] of Object.entries(row)) {
    if (isObj(v) && Object.keys(v).length) for (const [k2, v2] of Object.entries(v)) o[`${k}.${k2}`] = round(v2, k2 === "pos" ? "pos" : `${k}.${k2}`);
    else o[k] = round(v, k);
  }
  return o;
}

// array of objects -> {cols, rows}; `order` = preferred leading columns
function tabulate(rows, order = null) {
  const flat = rows.map((r) => flatten(isObj(r) ? r : {}));
  const cols = [];
  const seen = new Set();
  for (const k of order ?? []) if (!seen.has(k) && flat.some((r) => k in r)) { seen.add(k); cols.push(k); }
  for (const r of flat) for (const k of Object.keys(r)) if (!seen.has(k)) { seen.add(k); cols.push(k); }
  const keep = cols.filter((c) => c === "id" || flat.some((r) => !emptyVal(r[c])));
  return { cols: keep, rows: flat.map((r) => keep.map((c) => (r[c] === undefined ? null : r[c]))) };
}

const isRowArray = (v) => Array.isArray(v) && v.length > 0 && v.every(isObj);

const cmp = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
// recursively sorted keys so equal input gives byte-identical JSON
function stable(v) {
  if (Array.isArray(v)) return v.map(stable);
  if (isObj(v)) { const o = {}; for (const k of Object.keys(v).sort()) o[k] = stable(v[k]); return o; }
  return v;
}

export function leanSituation(sit, opts = {}) {
  const o = { catalogDesc: false, contactMaxAgeSec: 600, contactMax: 25, ...opts };
  const beforeChars = JSON.stringify(sit ?? null).length;
  const stat = {}, dyn = {};
  if (!isObj(sit)) return { static: stat, dynamic: dyn, stats: { beforeChars, afterChars: 0, staticChars: 0, dynamicChars: 0 } };

  for (const [key, val] of Object.entries(sit)) {
    if (DROP_KEYS.has(key) || val === undefined) continue;
    if (STATIC_SCALARS.has(key)) { stat[key] = round(val, key); continue; }

    const sa = STATIC_ARRAYS[key];
    if (sa && isRowArray(val)) {
      let rows = val;
      if (key === "catalog" && !o.catalogDesc) rows = rows.map((r) => { const { desc, ...rest } = r; return rest; });
      let sRows = rows, dRows = null;
      if (sa.fields) {
        sRows = rows.map((r) => Object.fromEntries(Object.entries(r).filter(([k]) => sa.fields.includes(k))));
        dRows = rows.map((r) => Object.fromEntries(Object.entries(r).filter(([k]) => k === "id" || !sa.fields.includes(k))));
      }
      const order = sa.sort ? [...rows.keys()].sort((a, b) => cmp(String(rows[a][sa.sort] ?? ""), String(rows[b][sa.sort] ?? "")) || a - b) : rows.map((_, i) => i);
      stat[key] = tabulate(order.map((i) => sRows[i]), sa.fields);
      if (dRows) {
        const dyn1 = tabulate(dRows, ["id"]);
        if (dyn1.cols.some((c) => c !== "id")) dyn[key] = dyn1; // dynamic part of objectives / fobSites (owner, contested, progress, ...)
      }
      continue;
    }

    if (key === "war" && isObj(val)) {
      const { mode, ...rest } = val;
      if (mode !== undefined) stat.warMode = stable(round(mode, "mode"));
      dyn.war = round(rest, "war");
      continue;
    }

    if (key === "contacts" && Array.isArray(val)) {
      const age = (c) => (isObj(c) && isNum(c.ageSec) ? c.ageSec : 0);
      const kept = val.map((c, i) => [c, i]).filter(([c]) => age(c) <= o.contactMaxAgeSec).sort((a, b) => age(a[0]) - age(b[0]) || a[1] - b[1]).slice(0, o.contactMax).map(([c]) => c);
      if (kept.length) dyn.contacts = isRowArray(kept) ? tabulate(kept, ["pos", "kind", "desc", "ageSec"]) : kept.map((c) => round(c));
      continue;
    }

    if (isRowArray(val)) { dyn[key] = tabulate(val, key === "squads" ? ["id"] : ["id"]); continue; }
    if (Array.isArray(val) && val.length === 0) continue;
    if (isObj(val) && Object.keys(val).length === 0) continue;
    if (val === null || val === "") continue;
    dyn[key] = round(val, key);
  }

  const staticOut = stable(stat);
  const staticChars = JSON.stringify(staticOut).length, dynamicChars = JSON.stringify(dyn).length;
  return { static: staticOut, dynamic: dyn, stats: { beforeChars, afterChars: staticChars + dynamicChars, staticChars, dynamicChars } };
}

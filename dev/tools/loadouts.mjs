// Loadout library store + REST handlers for the Clausewitz sidecar (plain Node, no deps).
// Contract: dev/notes/loadouts-impl.md ("Sidecar REST contract"), design: dev/notes/loadouts-arsenal-plan.md (R3, R5, R6).
// Persisted as <dataDir>/loadouts-store.json:
//   players[]      custom loadout records pushed by the game (key identityId + faction), game-owned
//   library[]      server loadouts ("lib-<n>"), max MAX_ACTIVE active per faction, order 1..MAX_ACTIVE among the active ones (inactive = 0)
//   compositions   {<kind>: [ROLE_*]} (a missing/empty kind = the game's defaults)
//   pins           {<faction>: {<kind>: [libId]}}
//   restartId      int, bumped by the dashboard "Restart war"; the game polls GET /control
//   lastSync       {time, round, gameVersion, addons[{guid,name}], factions[{key,name,side}]} of the last POST /loadouts/sync
// Item details (optional, WP7): a record may carry itemInfo [{id, name, key?, path?, type, cost, count}] and totalCost (supplies, -1 = unknown),
// resolved by the game (CLW_ItemInfo.c). Old records without them work everywhere; the list endpoint ships only a small `summary`, the full
// list comes from GET /api/loadouts/(library|player)/<id>/items.
// Game endpoints (X-CLW-Token, see auth.mjs): POST /loadouts/player, POST /loadouts/sync, POST /loadouts/status, GET /control.
// Dashboard endpoints (session): see handleApi.
import fs from "node:fs";
import path from "node:path";

export const MAX_ACTIVE = 7;
export const ROLES = ["LEADER", "RIFLEMAN", "MACHINEGUNNER", "ANTITANK", "GRENADIER", "SHARPSHOOTER", "MEDIC", "RADIOOPERATOR", "SCOUT", "AMMOBEARER", "SAPPER"].map((r) => "ROLE_" + r);
// CLW_ForceKind (addon/Scripts/Game/CLW_FactionProfile.c). logistics never fights (no squad roles) but is accepted.
export const KINDS = ["infantry", "recon", "motorized", "light", "mech", "armor", "armored_recon", "logistics"];
export const STATUSES = ["ok", "degraded", "invalid", "inactive", "unknown"];
const MAX_PLAYERS = 5000, MAX_LIBRARY = 500, MAX_COMP_LEN = 24, MAX_PINS = 24;
export const SYNC_LIMIT = 4_000_000, PLAYER_LIMIT = 1_000_000;

const isObj = (v) => !!v && typeof v === "object" && !Array.isArray(v);
const isInt = (v) => typeof v === "number" && Number.isInteger(v);
const line = (v, max) => typeof v === "string" && v.length <= max && !/[\u0000-\u001f]/.test(v);
const strList = (a, max, len) => (Array.isArray(a) ? a.filter((x) => typeof x === "string" && x.length > 0 && x.length <= len).slice(0, max) : []);
const fail = (status, error) => ({ ok: false, status, error });

// ---- item details ----
const MAX_ITEM_INFO = 600;
const TYPE_RE = /^[a-z][a-z_]{0,31}$/;
const PRIMARY_ORDER = ["rifle", "sniper", "machinegun", "launcher", "weapon", "pistol"];
const nameOf = (i) => i.name || i.key || (i.path ? String(i.path).split("/").pop().replace(/\.[a-z]+$/i, "") : "") || i.id;

// tolerant: skips malformed entries, clamps numbers, never throws; non-arrays give []
export function cleanItemInfo(a) {
  if (!Array.isArray(a)) return [];
  const out = [];
  for (const x of a) {
    if (out.length >= MAX_ITEM_INFO) break;
    if (!isObj(x) || !line(x.id, 400) || !x.id) continue;
    out.push({
      id: x.id, name: line(x.name, 200) ? x.name : "", ...(line(x.key, 200) && x.key ? { key: x.key } : {}), ...(line(x.path, 400) && x.path ? { path: x.path } : {}),
      type: typeof x.type === "string" && TYPE_RE.test(x.type) ? x.type : "other",
      cost: isInt(x.cost) && x.cost >= 0 && x.cost <= 1e9 ? x.cost : -1, count: isInt(x.count) && x.count >= 1 && x.count <= 9999 ? x.count : 1,
    });
  }
  return out;
}
const cleanTotal = (v) => (isInt(v) && v >= 0 && v <= 1e9 ? v : -1);

// small list-view digest of a record's item details; null when it has none
export function summarize(rec) {
  const info = rec?.itemInfo;
  if (!Array.isArray(info) || !info.length) return null;
  const types = {};
  let items = 0;
  for (const i of info) { items += i.count; types[i.type] = (types[i.type] ?? 0) + i.count; }
  let primary = "";
  for (const t of PRIMARY_ORDER) { const p = info.find((i) => i.type === t); if (p) { primary = nameOf(p); break; } }
  return { items, distinct: info.length, totalCost: cleanTotal(rec.totalCost), types, primary };
}

export function normRole(r) {
  if (r === "" || r === null || r === undefined) return "";
  if (typeof r !== "string") return null;
  const u = r.trim().toUpperCase();
  if (u === "") return "";
  const full = u.startsWith("ROLE_") ? u : "ROLE_" + u;
  return ROLES.includes(full) ? full : null;
}

function cleanPlayer(b, now) {
  if (!isObj(b)) return { error: "record must be a JSON object" };
  if (!line(b.identityId, 128) || !b.identityId) return { error: "identityId must be a non-empty single-line string (max 128)" };
  if (!line(b.faction, 64) || !b.faction) return { error: "faction must be a non-empty single-line string (max 64)" };
  if (typeof b.loadout !== "string" || !b.loadout) return { error: "loadout must be a non-empty string" };
  const status = STATUSES.includes(b.status) ? b.status : "unknown";
  return {
    value: {
      identityId: b.identityId, playerName: line(b.playerName, 128) ? b.playerName : "", faction: b.faction,
      loadout: b.loadout, basePrefab: line(b.basePrefab, 400) ? b.basePrefab : "",
      items: strList(b.items, 600, 400), addons: strList(b.addons, 600, 64),
      gameVersion: line(b.gameVersion, 64) ? b.gameVersion : "", savedAt: isInt(b.savedAt) && b.savedAt > 0 ? b.savedAt : Math.floor(now / 1000),
      status, missing: strList(b.missing, 600, 400),
      itemInfo: cleanItemInfo(b.itemInfo), totalCost: cleanTotal(b.totalCost),
    },
  };
}

export function createStore(dataDir, { now = () => Date.now(), log = () => {} } = {}) {
  const file = path.join(dataDir, "loadouts-store.json");
  let S = { v: 1, players: [], library: [], nextId: 1, compositions: {}, pins: {}, restartId: 0, lastSync: null };

  function load() {
    try {
      const j = JSON.parse(fs.readFileSync(file, "utf8"));
      if (isObj(j)) {
        S = { ...S, ...j };
        for (const k of ["players", "library"]) if (!Array.isArray(S[k])) S[k] = [];
        for (const k of ["compositions", "pins"]) if (!isObj(S[k])) S[k] = {};
        if (!isInt(S.restartId) || S.restartId < 0) S.restartId = 0;
        const maxN = Math.max(0, ...S.library.map((e) => Number(/^lib-(\d+)$/.exec(e.id)?.[1] ?? 0)));
        if (!isInt(S.nextId) || S.nextId <= maxN) S.nextId = maxN + 1;
      }
    } catch (e) { if (e.code !== "ENOENT") log(`could not read ${file}: ${e.message}`); }
  }
  function save() {
    try {
      fs.mkdirSync(dataDir, { recursive: true });
      const tmp = file + ".tmp";
      fs.writeFileSync(tmp, JSON.stringify(S));
      fs.renameSync(tmp, file);
      return true;
    } catch (e) { log(`could not save loadouts store: ${e.message}`); return false; }
  }
  load();

  const findPlayer = (id, faction) => S.players.find((p) => p.identityId === id && p.faction === faction);
  const entry = (id) => S.library.find((e) => e.id === id);
  const activeOf = (faction) => S.library.filter((e) => e.faction === faction && e.active).sort((a, b) => a.order - b.order || a.id.localeCompare(b.id, "en", { numeric: true }));
  function renumber(faction) { activeOf(faction).forEach((e, i) => { e.order = i + 1; }); for (const e of S.library) if (e.faction === faction && !e.active) e.order = 0; }

  // ---- game side ----
  function savePlayer(body) {
    const r = cleanPlayer(body, now());
    if (r.error) return fail(400, r.error);
    const cur = findPlayer(r.value.identityId, r.value.faction);
    if (cur) Object.assign(cur, r.value);
    else { if (S.players.length >= MAX_PLAYERS) return fail(409, "too many player loadouts"); S.players.push(r.value); }
    save();
    return { ok: true };
  }

  function sync(body) {
    if (!isObj(body)) return fail(400, "body must be a JSON object");
    let n = 0, bad = 0;
    for (const p of Array.isArray(body.players) ? body.players : []) {
      const r = cleanPlayer(p, now());
      if (r.error) { bad++; continue; }
      const cur = findPlayer(r.value.identityId, r.value.faction);
      if (cur) Object.assign(cur, r.value);
      else if (S.players.length < MAX_PLAYERS) S.players.push(r.value);
      n++;
    }
    S.lastSync = {
      time: now(), round: isInt(body.round) ? body.round : 0, gameVersion: line(body.gameVersion, 64) ? body.gameVersion : "",
      addons: (Array.isArray(body.addons) ? body.addons : []).filter((a) => isObj(a) && line(a.guid, 64)).slice(0, 600).map((a) => ({ guid: a.guid, name: line(a.name, 128) ? a.name : "" })),
      factions: (Array.isArray(body.factions) ? body.factions : []).filter((f) => isObj(f) && line(f.key, 64) && f.key).slice(0, 100).map((f) => ({ key: f.key, name: line(f.name, 128) ? f.name : "", side: isInt(f.side) ? f.side : -1 })),
    };
    save();
    log(`loadouts sync: players ${n} (${bad} rejected), library ${S.library.length}, restartId ${S.restartId}`);
    return { ok: true, reply: { v: 1, library: S.library.map(({ itemInfo, totalCost, ...rest }) => rest), compositions: S.compositions, pins: S.pins } };
  }

  function setStatus(body) {
    if (!isObj(body)) return fail(400, "body must be a JSON object");
    const st = (x) => (STATUSES.includes(x) ? x : "unknown");
    let lib = 0, pl = 0;
    for (const u of Array.isArray(body.library) ? body.library : []) {
      const e = isObj(u) ? entry(u.id) : null;
      if (e) { e.status = st(u.status); e.missing = strList(u.missing, 600, 400); lib++; }
    }
    for (const u of Array.isArray(body.players) ? body.players : []) {
      const p = isObj(u) ? findPlayer(u.identityId, u.faction) : null;
      if (p) { p.status = st(u.status); p.missing = strList(u.missing, 600, 400); pl++; }
    }
    S.statusTime = now();
    save();
    return { ok: true, library: lib, players: pl };
  }

  // ---- dashboard side ----
  function copyToLibrary({ identityId, faction, name, role }) {
    const p = findPlayer(identityId, faction);
    if (!p) return fail(404, "no such custom loadout");
    if (S.library.length >= MAX_LIBRARY) return fail(409, "library is full");
    const r = normRole(role ?? "");
    if (r === null) return fail(400, "unknown role");
    const nm = name === undefined || name === null || name === "" ? (p.playerName || p.identityId).slice(0, 80) : name;
    if (!line(nm, 80) || !nm.trim()) return fail(400, "name must be 1..80 characters");
    const e = {
      id: "lib-" + S.nextId++, name: nm.trim(), role: r, faction: p.faction, active: false, order: 0,
      loadout: p.loadout, basePrefab: p.basePrefab, items: [...p.items], addons: [...p.addons], gameVersion: p.gameVersion,
      createdFrom: { identityId: p.identityId, playerName: p.playerName }, status: p.status, missing: [...p.missing],
      itemInfo: (p.itemInfo ?? []).map((i) => ({ ...i })), totalCost: cleanTotal(p.totalCost),
    };
    S.library.push(e);
    save();
    return { ok: true, entry: e };
  }

  function updateEntry(id, patch) {
    const e = entry(id);
    if (!e) return fail(404, "no such library entry");
    if (!isObj(patch)) return fail(400, "body must be a JSON object");
    for (const k of Object.keys(patch)) if (!["name", "role", "active", "order"].includes(k)) return fail(400, `unknown field ${k}`);
    let name = e.name, role = e.role;
    if (patch.name !== undefined) { if (!line(patch.name, 80) || !patch.name.trim()) return fail(400, "name must be 1..80 characters"); name = patch.name.trim(); }
    if (patch.role !== undefined) { const r = normRole(patch.role); if (r === null) return fail(400, "unknown role"); role = r; }
    if (patch.active !== undefined && typeof patch.active !== "boolean") return fail(400, "active must be boolean");
    if (patch.order !== undefined && !isInt(patch.order)) return fail(400, "order must be an integer");
    const willBeActive = patch.active ?? e.active;
    if (patch.active === true && !e.active && activeOf(e.faction).length >= MAX_ACTIVE) return fail(409, `faction ${e.faction} already has ${MAX_ACTIVE} active loadouts (the limit); deactivate one first`);
    if (patch.order !== undefined && !willBeActive) return fail(409, "only active loadouts have an order");
    e.name = name; e.role = role;
    if (patch.active === true && !e.active) { e.order = activeOf(e.faction).length + 1; e.active = true; }
    else if (patch.active === false && e.active) { e.active = false; e.order = 0; renumber(e.faction); }
    if (patch.order !== undefined) {
      const list = activeOf(e.faction).filter((x) => x !== e);
      const pos = Math.min(Math.max(patch.order, 1), list.length + 1) - 1;
      list.splice(pos, 0, e);
      list.forEach((x, i) => { x.order = i + 1; });
    }
    save();
    return { ok: true, entry: e };
  }

  // ids = ALL active entries of the faction in the wanted order
  function reorder(faction, ids) {
    if (!line(faction, 64) || !Array.isArray(ids)) return fail(400, "faction and ids[] required");
    const act = activeOf(faction);
    if (ids.length !== act.length || new Set(ids).size !== ids.length || !act.every((e) => ids.includes(e.id))) return fail(400, "ids must list exactly the active entries of the faction");
    ids.forEach((id, i) => { entry(id).order = i + 1; });
    save();
    return { ok: true };
  }

  function removeEntry(id) {
    const e = entry(id);
    if (!e) return fail(404, "no such library entry");
    S.library = S.library.filter((x) => x !== e);
    for (const byKind of Object.values(S.pins)) for (const [k, ids] of Object.entries(byKind)) { byKind[k] = ids.filter((x) => x !== id); if (!byKind[k].length) delete byKind[k]; }
    renumber(e.faction);
    save();
    return { ok: true };
  }

  function setComposition(kind, roles) {
    if (!KINDS.includes(kind)) return fail(400, `kind must be one of ${KINDS.join(", ")}`);
    if (!Array.isArray(roles) || roles.length > MAX_COMP_LEN) return fail(400, `roles must be an array of at most ${MAX_COMP_LEN}`);
    const out = [];
    for (const r of roles) { const n = normRole(r); if (!n) return fail(400, `unknown role ${JSON.stringify(r)}`); out.push(n); }
    if (out.length) S.compositions[kind] = out; else delete S.compositions[kind];
    save();
    return { ok: true, compositions: S.compositions };
  }

  function setPins(faction, kind, ids) {
    if (!line(faction, 64) || !faction) return fail(400, "faction required");
    if (!KINDS.includes(kind)) return fail(400, `kind must be one of ${KINDS.join(", ")}`);
    if (!Array.isArray(ids) || ids.length > MAX_PINS) return fail(400, `ids must be an array of at most ${MAX_PINS}`);
    for (const id of ids) { const e = typeof id === "string" ? entry(id) : null; if (!e) return fail(400, `unknown library entry ${JSON.stringify(id)}`); if (e.faction !== faction) return fail(400, `${id} belongs to faction ${e.faction}`); }
    const byKind = (S.pins[faction] ??= {});
    if (ids.length) byKind[kind] = [...ids]; else delete byKind[kind];
    if (!Object.keys(byKind).length) delete S.pins[faction];
    save();
    return { ok: true, pins: S.pins };
  }

  const badCount = () => S.library.filter((e) => e.status === "degraded" || e.status === "invalid").length + S.players.filter((p) => p.status === "degraded" || p.status === "invalid").length;
  function restart() { S.restartId++; save(); log(`loadouts: restart requested, restartId ${S.restartId}`); return { ok: true, restartId: S.restartId }; }

  const slim = (o) => { const { loadout, itemInfo, ...rest } = o; return { ...rest, loadoutLen: typeof loadout === "string" ? loadout.length : 0, summary: summarize(o) }; };
  function view() {
    const facs = new Set([...S.players.map((p) => p.faction), ...S.library.map((e) => e.faction), ...(S.lastSync?.factions ?? []).map((f) => f.key)]);
    return {
      players: S.players.map(slim), library: S.library.map(slim), compositions: S.compositions, pins: S.pins, restartId: S.restartId,
      lastSync: S.lastSync, statusTime: S.statusTime ?? 0, factions: [...facs], roles: ROLES, kinds: KINDS, maxActive: MAX_ACTIVE,
      bad: badCount(),
    };
  }

  return { savePlayer, sync, setStatus, copyToLibrary, updateEntry, reorder, removeEntry, setComposition, setPins, restart, view, badCount, restartId: () => S.restartId, raw: () => S };
}

// ---------- HTTP ----------
// ctx = {send(res, code, obj), readBody(req, limit), originOk(req)}
async function jsonBody(req, res, ctx, limit) {
  if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) { ctx.send(res, 400, { error: "Content-Type must be application/json" }); return undefined; }
  let raw;
  try { raw = await ctx.readBody(req, limit); } catch { ctx.send(res, 413, { error: "body too large" }); return undefined; }
  try { return JSON.parse(raw || "{}"); } catch { ctx.send(res, 400, { error: "invalid JSON" }); return undefined; }
}
const reply = (ctx, res, r, extra) => (r.ok ? ctx.send(res, 200, extra ? extra(r) : r) : ctx.send(res, r.status, { error: r.error }));

// Game endpoints; the caller has already checked X-CLW-Token. Returns false when the route is not ours.
export async function handleGame(store, req, res, url, ctx) {
  if (req.method === "GET" && url === "/control") { ctx.send(res, 200, { restartId: store.restartId() }); return true; }
  if (req.method !== "POST" || !["/loadouts/player", "/loadouts/sync", "/loadouts/status"].includes(url)) return false;
  const body = await jsonBody(req, res, ctx, url === "/loadouts/sync" ? SYNC_LIMIT : PLAYER_LIMIT);
  if (body === undefined) return true;
  if (url === "/loadouts/player") reply(ctx, res, store.savePlayer(body), () => ({ ok: true }));
  else if (url === "/loadouts/sync") reply(ctx, res, store.sync(body), (r) => r.reply);
  else reply(ctx, res, store.setStatus(body));
  return true;
}

// Dashboard endpoints (session already checked by the caller). Returns false when the route is not ours.
export async function handleApi(store, req, res, url, ctx) {
  if (!url.startsWith("/api/loadouts")) return false;
  if (req.method === "GET" && url === "/api/loadouts") { ctx.send(res, 200, store.view()); return true; }
  const items = /^\/api\/loadouts\/(library|player)\/([^/]+)\/items$/.exec(url);
  if (req.method === "GET" && items) { // item details of one record (the list view only carries a summary)
    const S = store.raw(), dec = decodeURIComponent(items[2]);
    const rec = items[1] === "library" ? S.library.find((e) => e.id === dec) : S.players.find((p) => p.identityId + "|" + p.faction === dec);
    ctx.send(res, rec ? 200 : 404, rec ? { items: rec.items ?? [], itemInfo: rec.itemInfo ?? [], totalCost: cleanTotal(rec.totalCost) } : { error: "not found" });
    return true;
  }
  if (req.method === "GET" && /^\/api\/loadouts\/(library|player)\/[^/]+$/.test(url)) { // one record with its loadout string (export / debugging)
    const [, kind, id] = /^\/api\/loadouts\/(library|player)\/([^/]+)$/.exec(url);
    const S = store.raw(), dec = decodeURIComponent(id);
    const rec = kind === "library" ? S.library.find((e) => e.id === dec) : S.players.find((p) => p.identityId + "|" + p.faction === dec);
    ctx.send(res, rec ? 200 : 404, rec ?? { error: "not found" });
    return true;
  }
  if (!ctx.originOk(req)) { ctx.send(res, 403, { error: "forbidden origin" }); return true; }
  const lib = /^\/api\/loadouts\/library\/(lib-\d+)$/.exec(url);
  if (req.method === "DELETE" && lib) { reply(ctx, res, store.removeEntry(lib[1])); return true; }
  if (req.method !== "POST") { ctx.send(res, 404, { error: "not found" }); return true; }
  if (url === "/api/loadouts/restart") { reply(ctx, res, store.restart()); return true; }
  const body = await jsonBody(req, res, ctx, 100_000);
  if (body === undefined) return true;
  if (lib) reply(ctx, res, store.updateEntry(lib[1], body), (r) => ({ ok: true, entry: { ...r.entry, loadout: undefined } }));
  else if (url === "/api/loadouts/copy") reply(ctx, res, store.copyToLibrary(isObj(body) ? body : {}), (r) => ({ ok: true, entry: { ...r.entry, loadout: undefined } }));
  else if (url === "/api/loadouts/reorder") reply(ctx, res, store.reorder(body?.faction, body?.ids));
  else if (url === "/api/loadouts/compositions") reply(ctx, res, store.setComposition(body?.kind, body?.roles));
  else if (url === "/api/loadouts/pins") reply(ctx, res, store.setPins(body?.faction, body?.kind, body?.ids));
  else ctx.send(res, 404, { error: "not found" });
  return true;
}

// Admin login + game token + loadout library tests, fake provider, no API key, no game, no network:   node dev/tools/test-loadouts.mjs
// Starts its own sidecar on a free port with a temp config and a temp data dir (CLW_CONFIG / CLW_DATA_DIR) and stops ONLY that child.
// Covers: 401 / redirect without a session, login (wrong password, cookie flags, rate limit), session persistence across a sidecar restart,
// change password + logout, X-CLW-Token on/off, player save -> list -> copy -> activate (8th fails) -> sync reply -> status -> control restartId.
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import * as AUTH from "./auth.mjs";
import * as LO from "./loadouts.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
let pass = 0, fail = 0;
const ok = (cond, name, extra = "") => { if (cond) { pass++; console.log("  ok   " + name); } else { fail++; console.log("  FAIL " + name + (extra ? "  -> " + extra : "")); } };
const eq = (a, b, name) => ok(JSON.stringify(a) === JSON.stringify(b), name, `got ${JSON.stringify(a)} want ${JSON.stringify(b)}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });

// ---------- 1. pure helpers ----------
console.log("auth helpers");
const h = AUTH.hashPassword("hunter2hunter2");
ok(AUTH.validAdmin(h) && h.salt.length === 32 && h.hash.length === 128, "scrypt hash + random salt shape");
ok(AUTH.verifyPassword("hunter2hunter2", h) && !AUTH.verifyPassword("hunter2hunter3", h) && !AUTH.verifyPassword("", h), "verifyPassword");
ok(AUTH.hashPassword("hunter2hunter2").salt !== h.salt, "salt differs per hash");
ok(AUTH.verifyPassword("clausewitz", null) && !AUTH.verifyPassword("x", null), "no admin stored = default password");
ok(AUTH.safeEqual("abc", "abc") && !AUTH.safeEqual("abc", "abd") && !AUTH.safeEqual("abc", "abcd"), "safeEqual");
eq(AUTH.parseCookies("a=1; clw_session=abc; b=2"), { a: "1", clw_session: "abc", b: "2" }, "parseCookies");
ok(AUTH.gameTokenOk({ headers: {} }, "") && !AUTH.gameTokenOk({ headers: {} }, "t") && AUTH.gameTokenOk({ headers: { "x-clw-token": "t" } }, "t"), "gameTokenOk");
console.log("store");
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clw-lo-unit-"));
  const s = LO.createStore(dir);
  ok(s.savePlayer({ identityId: "a", faction: "US", loadout: "L", items: ["x"] }).ok, "savePlayer");
  ok(!s.savePlayer({ identityId: "", faction: "US", loadout: "L" }).ok && !s.savePlayer({ identityId: "a", faction: "US" }).ok, "savePlayer validates");
  eq(LO.normRole("medic"), "ROLE_MEDIC", "normRole accepts bare names");
  eq(LO.normRole("ROLE_NOPE"), null, "normRole rejects unknown");
  // item details (optional field): tolerant cleaning + summary
  const info = [
    { id: "{A}", name: "M4A1", type: "rifle", cost: 60, count: 1 }, { id: "{B}", name: "", key: "#AR-Mag_STANAG", path: "Prefabs/Weapons/Magazines/Mag_STANAG_30rnd.et", type: "magazine", cost: 3, count: 9 },
    { id: "{C}", name: "Medkit", type: "medical", cost: 5, count: 2 }, { id: "{D}", name: "Pistol", type: "pistol", cost: 20, count: 1 },
    { id: "", name: "no id" }, "junk", null, { id: "{E}", type: "Bad Type!", cost: -7, count: 0, name: 5 },
  ];
  ok(s.savePlayer({ identityId: "b", faction: "US", loadout: "L", itemInfo: info, totalCost: 211 }).ok, "savePlayer with itemInfo");
  const stored = s.raw().players.find((p) => p.identityId === "b");
  eq([stored.itemInfo.length, stored.itemInfo[4], stored.totalCost], [5, { id: "{E}", name: "", type: "other", cost: -1, count: 1 }, 211], "itemInfo cleaned: malformed entries dropped, numbers clamped, unknown type -> other");
  eq(LO.summarize(stored), { items: 14, distinct: 5, totalCost: 211, types: { rifle: 1, magazine: 9, medical: 2, pistol: 1, other: 1 }, primary: "M4A1" }, "summarize: counts, types, primary weapon");
  eq(LO.summarize({ itemInfo: [{ id: "{Z}", name: "", key: "", path: "Prefabs/Weapons/Foo_Bar.et", type: "mortar", cost: -1, count: 1 }] }).primary, "", "summarize: no weapon type, no primary");
  eq([LO.summarize({}), LO.summarize({ itemInfo: [] }), LO.cleanItemInfo("x"), LO.cleanItemInfo(undefined)], [null, null, [], []], "no/invalid itemInfo is tolerated");
  eq(s.raw().players.find((p) => p.identityId === "a").totalCost, -1, "old record: totalCost -1, itemInfo []");
  ok(s.savePlayer({ identityId: "b", faction: "US", loadout: "L2" }).ok && s.raw().players.find((p) => p.identityId === "b").itemInfo.length === 0, "re-save without itemInfo clears stale details");
  fs.rmSync(dir, { recursive: true, force: true });
}

// ---------- sidecar instance ----------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-lo-"));
const cfgFile = path.join(tmp, "cfg.json"), dataDir = path.join(tmp, "data");
fs.writeFileSync(cfgFile, JSON.stringify({ provider: "fake", memory: { enabled: false }, sitrep: { enabled: false } }));
const PORT = await freePort(), BASE = `http://127.0.0.1:${PORT}`;
let proc = null, log = "";
function start() {
  proc = spawn(process.execPath, [path.join(DIR, "llm_sidecar.mjs")], { env: { ...process.env, CLW_PORT: String(PORT), CLW_LLM: "fake", CLW_CONFIG: cfgFile, CLW_DATA_DIR: dataDir }, stdio: ["ignore", "pipe", "pipe"] });
  proc.stdout.on("data", (d) => (log += d)); proc.stderr.on("data", (d) => (log += d));
}
async function stop() { if (!proc) return; const p = proc; proc = null; p.kill(); await new Promise((r) => { p.once("exit", r); setTimeout(r, 2000); }); }
async function up() { for (let i = 0; i < 60; i++) { try { if ((await fetch(BASE + "/health", { headers: TOKEN ? { "X-CLW-Token": TOKEN } : {} })).status > 0) return true; } catch { /* not yet */ } await sleep(100); } return false; }
let TOKEN = "";

let cookie = "";
const raw = (m, p, body, headers = {}, useCookie = true) => fetch(BASE + p, { method: m, redirect: "manual", headers: { ...(body !== undefined ? { "Content-Type": "application/json" } : {}), ...(useCookie && cookie ? { Cookie: cookie } : {}), ...headers }, body: body === undefined ? undefined : JSON.stringify(body) });
const call = async (m, p, body, headers, useCookie) => { const r = await raw(m, p, body, headers, useCookie); let j = null; try { j = await r.json(); } catch { /* not json */ } return { status: r.status, json: j, res: r }; };
const login = async (password) => { const r = await call("POST", "/api/login", { password }, {}, false); if (r.status === 200) cookie = (r.res.headers.getSetCookie()[0] ?? "").split(";")[0]; return r; };
const rec = (n, faction = "US", extra = {}) => ({ identityId: "id-" + n, playerName: "Player" + n, faction, loadout: "LOADOUT-" + n + "-" + "x".repeat(50_000), basePrefab: "{AAAA}Prefabs/Characters/Factions/BLUFOR/US_Army/Character_US_Rifleman.et", items: ["{1}Prefabs/Weapons/M16A2.et", "{2}Prefabs/Items/Medkit.et"], addons: ["AAAAAAAAAAAAAAAA"], gameVersion: "1.5.0", savedAt: 1760000000 + n, status: "ok", missing: [], ...extra });

start();
try {
  ok(await up(), "sidecar started");

  console.log("no session");
  let r = await raw("GET", "/", undefined, {}, false);
  ok(r.status === 302 && r.headers.get("location") === "/login", "GET / redirects to /login");
  for (const p of ["/app.js", "/journal.js", "/loadouts.js", "/index.html"]) { r = await raw("GET", p, undefined, {}, false); ok(r.status === 302, `GET ${p} redirects`); }
  for (const p of ["/login", "/login.js", "/style.css", "/i18n.js", "/i18n/en.json"]) { r = await raw("GET", p, undefined, {}, false); ok(r.status === 200, `GET ${p} is public`); }
  for (const [m, p, b] of [["GET", "/api/state"], ["GET", "/api/config"], ["GET", "/api/loadouts"], ["GET", "/api/world"], ["GET", "/api/session"], ["POST", "/api/config", {}], ["POST", "/api/pause", {}], ["POST", "/api/loadouts/restart", {}], ["POST", "/api/password", { oldPassword: "a", newPassword: "bbbbbbbb" }]]) {
    r = await call(m, p, b, {}, false);
    ok(r.status === 401, `${m} ${p} -> 401 without session`, String(r.status));
  }
  eq((await call("GET", "/health", undefined, {}, false)).status, 200, "game endpoint /health open without session (no gameToken)");

  console.log("login");
  r = await login("wrong-password");
  ok(r.status === 401 && !r.res.headers.get("set-cookie"), "wrong password -> 401, no cookie");
  r = await login("clausewitz");
  ok(r.status === 200 && r.json.defaultPassword === true, "default password logs in, flags defaultPassword");
  const sc = r.res.headers.getSetCookie()[0];
  ok(/^clw_session=[0-9a-f]{64};/.test(sc) && /HttpOnly/.test(sc) && /SameSite=Strict/.test(sc) && /Path=\//.test(sc) && /Max-Age=2592000/.test(sc), "cookie flags: HttpOnly, SameSite=Strict, Path=/, 30 days", sc);
  ok(!/Secure/.test(sc), "no Secure flag over plain http");
  const probe = await call("POST", "/api/login", { password: "clausewitz" }, { "X-Forwarded-Proto": "https" }, false);
  ok(/Secure/.test(probe.res.headers.getSetCookie()[0] ?? ""), "Secure flag behind an https proxy header");
  r = await call("GET", "/api/session");
  ok(r.status === 200 && r.json.defaultPassword === true, "session works, default-password banner flag");
  r = await call("GET", "/api/state");
  ok(r.status === 200 && r.json.auth.defaultPassword === true && r.json.loadouts.bad === 0, "/api/state carries auth + loadouts info");
  r = await raw("GET", "/");
  ok(r.status === 200, "GET / with session");
  r = await raw("GET", "/login");
  ok(r.status === 302 && r.headers.get("location") === "/", "logged in: /login redirects to /");
  r = await call("GET", "/api/config");
  ok(r.status === 200 && !("admin" in r.json) && !("gameToken" in r.json) && r.json.gameTokenSet === false, "publicConfig: no admin hash, gameTokenSet flag");
  r = await call("POST", "/api/config", { admin: { salt: "aa".repeat(16), hash: "bb".repeat(64) } });
  eq(r.status, 400, "admin cannot be set through /api/config");

  console.log("loadouts: player save -> list -> copy -> activate");
  for (let i = 1; i <= 9; i++) {
    r = await call("POST", "/loadouts/player", rec(i), {}, false);
    ok(r.status === 200 && r.json.ok === true, `POST /loadouts/player #${i}`);
  }
  r = await call("POST", "/loadouts/player", rec(10, "USSR"), {}, false);
  r = await call("POST", "/loadouts/player", { identityId: "x", faction: "US" }, {}, false);
  eq(r.status, 400, "player record without loadout -> 400");
  const INFO = [{ id: "{1}Prefabs/Weapons/M16A2.et", name: "M16A2", type: "rifle", cost: 50, count: 1 }, { id: "{2}Prefabs/Items/Medkit.et", name: "", key: "#AR-Medkit", type: "medical", cost: 5, count: 2 }];
  r = await call("POST", "/loadouts/player", rec(1, "US", { playerName: "Renamed1", status: "degraded", missing: ["{9}Prefabs/Gone.et"], itemInfo: INFO, totalCost: 60, extraUnknownField: { a: 1 } }), {}, false);
  ok(r.status === 200, "record with itemInfo + unknown extra field accepted");
  r = await call("GET", "/api/loadouts");
  ok(r.status === 200 && r.json.players.length === 10, "list: 10 players (re-save upserts)", String(r.json?.players?.length));
  ok(r.json.players.every((p) => p.loadout === undefined && p.loadoutLen > 50_000), "list does not ship the loadout strings");
  eq([r.json.players.find((p) => p.identityId === "id-1").playerName, r.json.bad], ["Renamed1", 1], "upsert replaced the record; degraded counted");
  const pl1 = r.json.players.find((p) => p.identityId === "id-1"), pl2 = r.json.players.find((p) => p.identityId === "id-2");
  eq([pl1.itemInfo, pl1.summary, pl1.totalCost], [undefined, { items: 3, distinct: 2, totalCost: 60, types: { rifle: 1, medical: 2 }, primary: "M16A2" }, 60], "list: summary instead of the item list");
  eq([pl2.summary, pl2.totalCost], [null, -1], "list: old record without itemInfo -> summary null");
  r = await call("GET", "/api/loadouts/player/" + encodeURIComponent("id-1|US") + "/items");
  eq([r.status, r.json.itemInfo, r.json.totalCost, r.json.items.length], [200, INFO, 60, 2], "GET player items endpoint");
  r = await call("GET", "/api/loadouts/player/" + encodeURIComponent("id-2|US") + "/items");
  eq([r.status, r.json.itemInfo, r.json.totalCost], [200, [], -1], "items endpoint of an old record: empty list");
  eq((await call("GET", "/api/loadouts/player/nobody%7CUS/items")).status, 404, "items endpoint: unknown record -> 404");
  r = await call("GET", "/api/loadouts");
  eq([r.json.maxActive, r.json.kinds.includes("infantry"), r.json.roles.includes("ROLE_MEDIC"), r.json.factions.sort()], [7, true, true, ["US", "USSR"]], "meta: cap, kinds, roles, factions");
  r = await call("GET", "/api/state");
  eq(r.json.loadouts.bad, 1, "/api/state loadouts.bad (tab badge)");

  const ids = [];
  for (let i = 1; i <= 9; i++) {
    r = await call("POST", "/api/loadouts/copy", { identityId: "id-" + i, faction: "US", role: i === 2 ? "medic" : "" });
    ok(r.status === 200 && /^lib-\d+$/.test(r.json.entry.id) && r.json.entry.active === false && r.json.entry.order === 0, `copy #${i} -> inactive library entry`);
    ids.push(r.json.entry.id);
  }
  r = await call("POST", "/api/loadouts/copy", { identityId: "nobody", faction: "US" });
  eq(r.status, 404, "copy of unknown player -> 404");
  r = await call("POST", "/api/loadouts/copy", { identityId: "id-3", faction: "US", role: "ROLE_BOGUS" });
  eq(r.status, 400, "copy with unknown role -> 400");
  r = await call("GET", "/api/loadouts");
  const e1 = r.json.library.find((e) => e.id === ids[0]), e2 = r.json.library.find((e) => e.id === ids[1]);
  eq([e1.name, e1.status, e1.missing, e1.createdFrom, e1.faction], ["Renamed1", "degraded", ["{9}Prefabs/Gone.et"], { identityId: "id-1", playerName: "Renamed1" }, "US"], "copy carries name, status, missing, createdFrom");
  eq(e2.role, "ROLE_MEDIC", "role normalised");
  eq([e1.itemInfo, e1.summary.items, e2.summary], [undefined, 3, null], "library list: summary only; old-record copy has none");
  r = await call("GET", "/api/loadouts/library/" + ids[0] + "/items");
  eq([r.status, r.json.itemInfo, r.json.totalCost], [200, INFO, 60], "GET library items endpoint (copied from the player)");

  for (let i = 0; i < 7; i++) {
    r = await call("POST", "/api/loadouts/library/" + ids[i], { active: true });
    ok(r.status === 200 && r.json.entry.active && r.json.entry.order === i + 1, `activate #${i + 1} gets order ${i + 1}`);
  }
  r = await call("POST", "/api/loadouts/library/" + ids[7], { active: true });
  ok(r.status === 409 && /7/.test(r.json.error), "8th active loadout of the faction -> 409", JSON.stringify(r.json));
  r = await call("POST", "/api/loadouts/copy", { identityId: "id-10", faction: "USSR" });
  const ussr = r.json.entry.id;
  r = await call("POST", "/api/loadouts/library/" + ussr, { active: true });
  eq(r.status, 200, "the cap is per faction");
  r = await call("POST", "/api/loadouts/library/" + ids[0], { order: 3 });
  r = await call("GET", "/api/loadouts");
  const order = r.json.library.filter((e) => e.faction === "US" && e.active).sort((a, b) => a.order - b.order);
  eq(order.map((e) => e.id), [ids[1], ids[2], ids[0], ...ids.slice(3, 7)], "moving to order 3 renumbers 1..7 uniquely");
  r = await call("POST", "/api/loadouts/reorder", { faction: "US", ids: order.map((e) => e.id).reverse() });
  eq(r.status, 200, "reorder with the full active set");
  r = await call("POST", "/api/loadouts/reorder", { faction: "US", ids: [ids[0]] });
  eq(r.status, 400, "reorder with a partial set -> 400");
  r = await call("POST", "/api/loadouts/library/" + ids[3], { active: false });
  r = await call("GET", "/api/loadouts");
  const act = r.json.library.filter((e) => e.faction === "US" && e.active).map((e) => e.order).sort();
  eq([act, r.json.library.find((e) => e.id === ids[3]).order], [[1, 2, 3, 4, 5, 6], 0], "deactivate compacts the order");
  r = await call("POST", "/api/loadouts/library/" + ids[7], { active: true });
  eq(r.status, 200, "a slot is free again after deactivating");
  r = await call("POST", "/api/loadouts/library/" + ids[0], { name: "  Rifleman AK  ", role: "ROLE_RIFLEMAN" });
  eq([r.json.entry.name, r.json.entry.role], ["Rifleman AK", "ROLE_RIFLEMAN"], "edit name + role");
  r = await call("POST", "/api/loadouts/library/" + ids[0], { name: "" });
  eq(r.status, 400, "empty name rejected");
  r = await call("POST", "/api/loadouts/library/" + ids[0], { bogus: 1 });
  eq(r.status, 400, "unknown field rejected");
  r = await call("POST", "/api/loadouts/library/lib-9999", { name: "x" });
  eq(r.status, 404, "unknown entry -> 404");

  console.log("compositions + pins");
  r = await call("POST", "/api/loadouts/compositions", { kind: "infantry", roles: ["LEADER", "ROLE_MACHINEGUNNER", "rifleman"] });
  eq([r.status, r.json.compositions.infantry], [200, ["ROLE_LEADER", "ROLE_MACHINEGUNNER", "ROLE_RIFLEMAN"]], "set composition (roles normalised)");
  eq((await call("POST", "/api/loadouts/compositions", { kind: "spaceship", roles: [] })).status, 400, "unknown kind -> 400");
  eq((await call("POST", "/api/loadouts/compositions", { kind: "recon", roles: ["ROLE_NOPE"] })).status, 400, "unknown role -> 400");
  r = await call("POST", "/api/loadouts/compositions", { kind: "recon", roles: ["scout", "sharpshooter"] });
  r = await call("POST", "/api/loadouts/compositions", { kind: "recon", roles: [] });
  ok(r.status === 200 && !("recon" in r.json.compositions), "empty roles = back to game defaults (key removed)");
  r = await call("POST", "/api/loadouts/pins", { faction: "US", kind: "infantry", ids: [ids[0], ids[1]] });
  eq([r.status, r.json.pins], [200, { US: { infantry: [ids[0], ids[1]] } }], "set pins");
  eq((await call("POST", "/api/loadouts/pins", { faction: "US", kind: "infantry", ids: [ussr] })).status, 400, "pinning another faction's loadout -> 400");
  eq((await call("POST", "/api/loadouts/pins", { faction: "US", kind: "infantry", ids: ["lib-777"] })).status, 400, "pinning an unknown loadout -> 400");

  console.log("game: sync reply, status, control");
  r = await call("GET", "/control", undefined, {}, false);
  eq([r.status, r.json], [200, { restartId: 0 }], "GET /control -> restartId 0");
  r = await call("POST", "/loadouts/sync", { v: 1, round: 3, gameVersion: "1.5.0.1", addons: [{ guid: "AAAAAAAAAAAAAAAA", name: "Base Mod" }], factions: [{ key: "US", name: "US Army", side: 0 }, { key: "USSR", name: "Soviet", side: 1 }], players: [rec(11)] }, {}, false);
  ok(r.status === 200 && r.json.v === 1 && Array.isArray(r.json.library) && r.json.compositions && r.json.pins, "sync reply shape {v, library, compositions, pins}");
  const syncActive = r.json.library.filter((e) => e.active && e.faction === "US");
  ok(r.json.library.every((e) => !("itemInfo" in e) && !("totalCost" in e)), "sync reply: no item details (the game DTO does not know them)");
  ok(syncActive.length === 7 && syncActive.every((e) => typeof e.loadout === "string" && e.loadout.length > 50_000), "sync reply: active entries incl. full loadout strings");
  ok(r.json.library.some((e) => e.id === ids[0] && e.name === "Rifleman AK" && e.role === "ROLE_RIFLEMAN"), "sync reply: edited entry");
  eq(r.json.compositions, { infantry: ["ROLE_LEADER", "ROLE_MACHINEGUNNER", "ROLE_RIFLEMAN"] }, "sync reply: compositions");
  eq(r.json.pins, { US: { infantry: [ids[0], ids[1]] } }, "sync reply: pins");
  r = await call("GET", "/api/loadouts");
  ok(r.json.players.some((p) => p.identityId === "id-11") && r.json.lastSync.round === 3 && r.json.lastSync.addons[0].name === "Base Mod", "sync stored the players and lastSync meta");
  r = await call("POST", "/loadouts/status", { v: 1, library: [{ id: ids[1], status: "invalid", missing: ["{3}Prefabs/Weapons/Missing.et"] }, { id: ids[2], status: "degraded", missing: ["a"] }, { id: "lib-404", status: "ok" }], players: [{ identityId: "id-2", faction: "US", status: "degraded", missing: ["b"] }] }, {}, false);
  ok(r.status === 200 && r.json.library === 2 && r.json.players === 1, "POST /loadouts/status applied (unknown ids ignored)");
  r = await call("GET", "/api/loadouts");
  eq([r.json.library.find((e) => e.id === ids[1]).status, r.json.library.find((e) => e.id === ids[1]).missing, r.json.bad], ["invalid", ["{3}Prefabs/Weapons/Missing.et"], 5], "status stored per entry; bad count = degraded + invalid (library + players)");
  r = await call("GET", "/api/state");
  eq(r.json.loadouts.bad, 5, "/api/state loadouts.bad follows");
  r = await call("POST", "/api/loadouts/restart", {});
  eq([r.status, r.json.restartId], [200, 1], "restart war bumps restartId");
  r = await call("GET", "/control", undefined, {}, false);
  eq(r.json, { restartId: 1 }, "GET /control reflects the restart");
  await call("POST", "/api/loadouts/restart", {});
  eq((await call("GET", "/control", undefined, {}, false)).json.restartId, 2, "restartId increments on every restart");
  r = await call("DELETE", "/api/loadouts/library/" + ids[1]);
  r = await call("GET", "/api/loadouts");
  ok(!r.json.library.some((e) => e.id === ids[1]) && r.json.pins.US.infantry.length === 1, "delete removes the entry and its pins");

  console.log("game token");
  r = await call("POST", "/api/config", { gameToken: "s3cret-token-1" });
  ok(r.status === 200 && r.json.config.gameTokenSet === true && !JSON.stringify(r.json).includes("s3cret-token-1"), "set gameToken; never echoed");
  TOKEN = "s3cret-token-1";
  ok(!(await call("GET", "/api/config")).res.ok === false && !JSON.stringify((await call("GET", "/api/state")).json).includes("s3cret-token-1"), "token not in /api/state");
  const gameCalls = [["GET", "/health"], ["GET", "/control"], ["POST", "/loadouts/player", rec(20)], ["POST", "/loadouts/sync", { v: 1 }], ["POST", "/loadouts/status", { v: 1 }], ["POST", "/orders", { faction: "US", time: 1, squads: [], contacts: [] }], ["POST", "/manifest", { v: 3 }], ["POST", "/translate", {}], ["POST", "/world", {}]];
  for (const [m, p, b] of gameCalls) {
    const none = await call(m, p, b, {}, false), wrong = await call(m, p, b, { "X-CLW-Token": "nope" }, false), cookieOnly = await call(m, p, b, {}, true), good = await call(m, p, b, { "X-CLW-Token": TOKEN }, false);
    ok(none.status === 401 && wrong.status === 401 && cookieOnly.status === 401 && good.status !== 401, `${m} ${p}: 401 without / with wrong token or just the cookie; passes with the token`, `${none.status}/${wrong.status}/${cookieOnly.status}/${good.status}`);
  }
  r = await call("POST", "/api/config", { gameToken: "has space" });
  eq(r.status, 400, "gameToken with whitespace rejected");
  r = await call("POST", "/api/config", { gameToken: "" });
  ok(r.status === 200 && r.json.config.gameTokenSet === false, "empty gameToken clears it");
  TOKEN = "";
  eq((await call("GET", "/health", undefined, {}, false)).status, 200, "token off: game endpoints open again");

  console.log("persistence across a sidecar restart");
  const before = cookie;
  await stop(); start();
  ok(await up(), "sidecar restarted");
  r = await call("GET", "/api/loadouts");
  ok(r.status === 200 && r.json.library.length === 9 && r.json.restartId === 2 && !!r.json.lastSync && Object.keys(r.json.compositions).length === 1, "session cookie still valid; store reloaded (library, restartId, lastSync, compositions)");
  eq((await call("GET", "/control", undefined, {}, false)).json.restartId, 2, "restartId survives the restart");
  const sessions = JSON.parse(fs.readFileSync(path.join(dataDir, "sessions.json"), "utf8"));
  ok(!JSON.stringify(sessions).includes(before.split("=")[1]), "sessions.json holds only token hashes");

  console.log("change password, logout");
  r = await call("POST", "/api/password", { oldPassword: "nope", newPassword: "a-new-password" });
  eq(r.status, 403, "wrong old password -> 403");
  r = await call("POST", "/api/password", { oldPassword: "clausewitz", newPassword: "short" });
  eq(r.status, 400, "new password < 8 chars -> 400");
  r = await call("POST", "/api/password", { oldPassword: "clausewitz", newPassword: "clausewitz" });
  eq(r.status, 400, "new password = default -> 400");
  const other = cookie;
  r = await login("clausewitz"); const second = cookie; cookie = other;
  r = await call("POST", "/api/password", { oldPassword: "clausewitz", newPassword: "a-new-password" });
  eq([r.status, r.json.defaultPassword], [200, false], "change password");
  ok((await call("GET", "/api/session")).status === 200, "the changing session stays valid");
  cookie = second;
  eq((await call("GET", "/api/session")).status, 401, "other sessions are logged out");
  const saved = JSON.parse(fs.readFileSync(cfgFile, "utf8"));
  ok(AUTH.validAdmin(saved.admin) && !JSON.stringify(saved).includes("a-new-password") && AUTH.verifyPassword("a-new-password", saved.admin), "config file holds {salt, hash}, no plaintext");
  eq((await login("clausewitz")).status, 401, "old (default) password no longer works");
  r = await login("a-new-password");
  ok(r.status === 200 && r.json.defaultPassword === false, "new password logs in");
  eq((await call("GET", "/api/state")).json.auth.defaultPassword, false, "banner flag cleared");
  r = await call("POST", "/api/logout", {});
  ok(r.status === 200 && /clw_session=;/.test(r.res.headers.getSetCookie()[0]) && /Max-Age=0/.test(r.res.headers.getSetCookie()[0]), "logout clears the cookie");
  eq((await call("GET", "/api/state")).status, 401, "session invalid after logout (server side)");
  await stop(); start();
  ok(await up(), "sidecar restarted again");
  eq((await login("a-new-password")).status, 200, "password hash survives the restart");

  console.log("rate limit");
  for (let i = 0; i < 5; i++) await login("bad-" + i);
  r = await login("a-new-password");
  ok(r.status === 429 && Number(r.res.headers.get("retry-after")) > 0, "6th attempt within the window -> 429 + Retry-After, even with the right password", String(r.status));
  eq((await call("GET", "/api/state", undefined, {}, true)).status, 200, "an existing session is unaffected");
} catch (e) {
  fail++; console.log("  FAIL exception: " + (e?.stack ?? e));
} finally {
  await stop();
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* ignore */ }
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) console.log(log.split("\n").slice(-30).join("\n"));
process.exit(fail ? 1 : 0);

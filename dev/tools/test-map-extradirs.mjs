// Extra addon dirs for the dashboard map (env CLW_ADDON_DIRS / config addonDirs), no game, no network:   node dev/tools/test-map-extradirs.mjs
// Builds a fake game install + fake Workshop mod cache with tiny hand-written .pak files in a temp dir and checks
//   - resolveAddonDirs: parent dir of addons, single addon dir, loose source addon, missing dir, file, empty parent, duplicates
//   - merge / split of the env + config lists
//   - createGameFs: lookup order (game first, then addons in order), lazy opening of the addons, find(), loose files, {GUID} prefix
//   - the sidecar: config validation of addonDirs and the effective dirs in GET /api/world (starts its own sidecar, stops only that child)
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { createGameFs, resolveAddonDirs, splitPathList, mergeAddonDirEntries } from "./map/gamefs.mjs";
import { loginFetch } from "./test-login.mjs";

const DIR = path.dirname(fileURLToPath(import.meta.url));
let pass = 0, fail = 0;
const ok = (cond, name, extra = "") => { if (cond) { pass++; console.log("  ok   " + name); } else { fail++; console.log("  FAIL " + name + (extra ? "  -> " + extra : "")); } };
const eq = (a, b, name) => ok(JSON.stringify(a) === JSON.stringify(b), name, `got ${JSON.stringify(a)} want ${JSON.stringify(b)}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });

// ---- minimal PAC1 writer (uncompressed entries, head version 0 = offsets relative to the DATA payload) ----
function writePak(file, files) { // files: {"Worlds/A.ent": "text"}
  const names = Object.keys(files);
  const chunks = [], tree = { dirs: new Map(), files: [] };
  let off = 0;
  for (const n of names) {
    const data = Buffer.from(files[n], "utf8");
    chunks.push(data);
    const parts = n.split("/");
    let node = tree;
    for (const d of parts.slice(0, -1)) { if (!node.dirs.has(d)) node.dirs.set(d, { dirs: new Map(), files: [] }); node = node.dirs.get(d); }
    node.files.push({ name: parts[parts.length - 1], offset: off, size: data.length });
    off += data.length;
  }
  const entry = (name, node) => {
    const nb = Buffer.from(name, "utf8"), kids = [];
    for (const [dn, dnode] of node.dirs) kids.push(entry(dn, dnode));
    for (const f of node.files) {
      const fn = Buffer.from(f.name, "utf8"), b = Buffer.alloc(2 + fn.length + 24);
      b[0] = 1; b[1] = fn.length; fn.copy(b, 2);
      b.writeUInt32LE(f.offset, 2 + fn.length); b.writeUInt32LE(f.size, 6 + fn.length); b.writeUInt32LE(f.size, 10 + fn.length); // b[18] (compressed) stays 0
      kids.push(b);
    }
    const head = Buffer.alloc(2 + nb.length + 4);
    head[0] = 0; head[1] = nb.length; nb.copy(head, 2); head.writeUInt32LE(kids.length, 2 + nb.length);
    return Buffer.concat([head, ...kids]);
  };
  const chunk = (magic, body) => { const h = Buffer.alloc(8); h.writeUInt32BE(magic, 0); h.writeUInt32BE(body.length, 4); return Buffer.concat([h, body]); };
  const form = Buffer.alloc(12); form.writeUInt32BE(0x464f524d, 0); form.writeUInt32BE(0, 4); form.writeUInt32BE(0x50414331, 8);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.concat([form, chunk(0x48454144, Buffer.alloc(2)), chunk(0x44415441, Buffer.concat(chunks)), chunk(0x46494c45, entry("", tree))]));
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clw-extradirs-"));
const game = path.join(tmp, "game"), mods = path.join(tmp, "mods"), lonely = path.join(tmp, "lonely-addon");
writePak(path.join(game, "addons", "base", "data.pak"), { "Worlds/Shared.txt": "game", "Prefabs/G.et": "game-prefab", "UI/Textures/Map/Worlds/Arland.edds": "x" });
writePak(path.join(mods, "A_1111", "data.pak"), { "Worlds/Mod.ent": "modA", "Worlds/Shared.txt": "modA", "UI/Textures/Map/Worlds/Takistan.edds": "tex" });
writePak(path.join(mods, "B_2222", "data.pak"), { "Worlds/OnlyB.ent": "modB", "Worlds/Mod.ent": "modB" });
fs.mkdirSync(path.join(mods, "C_3333"), { recursive: true }); fs.writeFileSync(path.join(mods, "C_3333", "readme.txt"), "not an addon");
fs.mkdirSync(path.join(mods, "L_4444", "Worlds"), { recursive: true });
fs.writeFileSync(path.join(mods, "L_4444", "addon.gproj"), "GameProject {}"); fs.writeFileSync(path.join(mods, "L_4444", "Worlds", "Loose.ent"), "loose");
fs.writeFileSync(path.join(mods, "stray-file.txt"), "x");
writePak(path.join(lonely, "data.pak"), { "Worlds/Lonely.ent": "lonely" });
const emptyParent = path.join(tmp, "empty"); fs.mkdirSync(emptyParent);
const missing = path.join(tmp, "does-not-exist");
const base = (d) => path.basename(d);

try {
  console.log("resolveAddonDirs");
  let r = resolveAddonDirs([mods]);
  eq(r.dirs.map(base), ["A_1111", "B_2222", "L_4444"], "parent dir -> its addon subdirs (pak or addon.gproj), sorted; C_3333 and files ignored");
  eq(r.skipped, [], "parent with addons: nothing skipped");
  r = resolveAddonDirs([lonely]);
  eq(r.dirs, [lonely], "single addon dir stays as it is");
  r = resolveAddonDirs([missing]);
  eq([r.dirs.length, r.skipped.map((s) => s.path)], [0, [missing]], "missing dir is skipped and reported");
  r = resolveAddonDirs([path.join(mods, "stray-file.txt")]);
  ok(r.dirs.length === 0 && r.skipped.length === 1 && /not a directory/.test(r.skipped[0].reason), "a file is skipped");
  r = resolveAddonDirs([emptyParent]);
  ok(r.dirs.length === 0 && r.skipped.length === 1, "parent without addon dirs is reported");
  r = resolveAddonDirs([mods, path.join(mods, "A_1111"), lonely, missing, ""]);
  eq(r.dirs.map(base), ["A_1111", "B_2222", "L_4444", "lonely-addon"], "duplicates dropped, order kept, empty entries ignored");
  eq(resolveAddonDirs(undefined).dirs, [], "undefined -> nothing");

  console.log("env + config lists");
  const d = path.delimiter;
  eq(splitPathList(`a${d}b,c${d}${d} d `), ["a", "b", "c", "d"], "split on the path delimiter and ','");
  eq(mergeAddonDirEntries(`/e1${d}/e2`, ["/c1", "/e2", " /c2 "]), ["/e1", "/e2", "/c1", "/c2"], "env first, then config, no duplicates");
  eq(mergeAddonDirEntries(undefined, undefined), [], "nothing set");

  console.log("createGameFs");
  let g = createGameFs({ gameDir: game, extraDirs: [mods, lonely, missing] });
  eq(g.addonDirs.map(base), ["A_1111", "B_2222", "L_4444", "lonely-addon"], "effective addon dirs");
  ok(g.exists("Prefabs/G.et") && g.addonsOpened === 0, "game file found without opening any addon");
  eq(g.readFile("worlds/shared.txt").toString(), "game", "game pak wins over an addon for the same path");
  ok(g.addonsOpened === 0, "still no addon opened");
  eq(g.readFile("{AABBCCDDEEFF0011}Worlds/Mod.ent").toString(), "modA", "mod world resolved (GUID prefix ignored), first addon wins over B");
  eq(g.addonsOpened, 1, "only the first addon was opened");
  eq(g.readFile("Worlds/OnlyB.ent").toString(), "modB", "file from the second addon");
  eq(g.addonsOpened, 2, "two addons opened");
  ok(!g.exists("Worlds/Nope.ent") && g.addonsOpened === 4, "a miss walks all addons");
  eq(g.readFile("Worlds/Loose.ent").toString(), "loose", "loose source addon file");
  eq(g.readFile("Worlds/Lonely.ent").toString(), "lonely", "single addon dir file");
  eq(g.size("Worlds/Mod.ent"), 4, "size() through an addon");
  eq(g.size("Worlds/Nope.ent"), -1, "size() of a missing file");
  let threw = false; try { g.readFile("Worlds/Nope.ent"); } catch { threw = true; }
  ok(threw, "readFile of a missing file throws");
  const hits = g.find(/^ui\/textures\/map\/worlds\/.+\.edds$/i).map((p) => p.split("/").pop()).sort();
  eq(hits, ["Arland.edds", "Takistan.edds"], "find() covers game and addons");
  g = createGameFs({ gameDir: game, extraDirs: [mods] });
  eq(g.find(/\.edds$/i).length, 2, "find() on a fresh fs opens every addon on demand");
  eq(g.addonsOpened, 3, "all addons opened by find()");
  g = createGameFs({ gameDir: game, extraDirs: [] });
  ok(g.exists("Worlds/Shared.txt") && !g.exists("Worlds/Mod.ent"), "no extra dirs = game only");
  g = createGameFs({ gameDir: game, extraDirs: [missing] });
  ok(g.exists("Worlds/Shared.txt") && g.addonDirs.length === 0, "only a missing dir = game only, no throw");
  g = createGameFs({ gameDir: path.join(tmp, "no-game"), extraDirs: [lonely] });
  ok(g.pakCount === 1 && g.exists("Worlds/Lonely.ent"), "addons work without a game dir (pakCount counts them)");

  console.log("sidecar");
  const port = await freePort(), BASEURL = `http://127.0.0.1:${port}`;
  const cfgFile = path.join(tmp, "sidecar-config.json");
  fs.writeFileSync(cfgFile, JSON.stringify({ addonDirs: [lonely, missing] }));
  const p = spawn(process.execPath, [path.join(DIR, "llm_sidecar.mjs")], {
    env: { ...process.env, CLW_PORT: String(port), CLW_LLM: "fake", CLW_CONFIG: cfgFile, CLW_DATA_DIR: path.join(tmp, "data"), ARMA_REFORGER_DIR: game, CLW_ADDON_DIRS: [mods, missing].join(path.delimiter) },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let log = ""; p.stdout.on("data", (b) => { log += b; }); p.stderr.on("data", (b) => { log += b; });
  try {
    for (let i = 0; i < 100; i++) { try { if ((await fetch(BASEURL + "/health")).ok) break; } catch { /* starting */ } await sleep(100); }
    await loginFetch(BASEURL);
    const post = async (url, body) => { const x = await fetch(BASEURL + url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }); return { status: x.status, j: await x.json().catch(() => ({})) }; };
    const cur = await (await fetch(BASEURL + "/api/config")).json();
    eq(cur.addonDirs, [lonely, missing], "addonDirs loaded from the config file");
    let x = await post("/api/config", { addonDirs: [" /a ", "/a", "", "/b"] });
    ok(x.status === 200 && JSON.stringify(x.j.config.addonDirs) === JSON.stringify(["/a", "/b"]), "addonDirs trimmed + deduped");
    ok((await post("/api/config", { addonDirs: "nope" })).status === 400, "addonDirs must be an array");
    ok((await post("/api/config", { addonDirs: [1] })).status === 400, "entries must be strings");
    ok((await post("/api/config", { addonDirs: ["a\nb"] })).status === 400, "no newlines");
    ok((await post("/api/config", { addonDirs: Array.from({ length: 21 }, (_, i) => "/d" + i) })).status === 400, "max 20 entries");
    ok((await post("/api/config", { addonDirs: ["x".repeat(401)] })).status === 400, "max 400 chars per entry");
    x = await post("/api/config", { addonDirs: [lonely, missing] });
    ok(x.status === 200, "restore the dirs");
    x = await post("/world", { worldFile: "{0123456789ABCDEF}Worlds/Takistan/Takistan.ent", mapPrefab: "", mapOffset: [0, 0], mapSize: [1000, 1000] });
    ok(x.status === 200, "POST /world accepted");
    let w = null;
    for (let i = 0; i < 100; i++) { w = await (await fetch(BASEURL + "/api/world")).json(); if (w.map.status !== "resolving" && w.map.status !== "none") break; await sleep(100); }
    ok(w.map.status === "notfound" || w.map.status === "error" || w.map.status === "ready", "map build finished", w.map.status);
    eq(w.addonDirs.paths.map(base), ["A_1111", "B_2222", "L_4444", "lonely-addon"], "effective dirs: env (parent expanded) first, then config");
    eq(w.addonDirs.count, 4, "count reported");
    eq(w.addonDirs.skipped.map((s) => s.path), [missing], "missing dir (given in env and config) reported once as skipped");
    eq(log.split(`map addon dir skipped: ${missing}`).length - 1, 1, "the missing dir is logged once");
  } finally {
    const closed = new Promise((r) => p.once("close", r));
    p.kill();
    await Promise.race([closed, sleep(3000)]);
  }
} finally {
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* temp dir */ }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exitCode = fail ? 1 : 0;

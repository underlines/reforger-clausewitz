#!/usr/bin/env node
// Clausewitz: generate a tiny compat addon ("wrapper") that lets Clausewitz run on ANY map mod.
//
// The wrapper holds: addon.gproj (depends on Clausewitz + the map addon), a sub-scene world whose parent is the map's
// bare world, one layer with the nine Clausewitz entities (same as Worlds/CLW_Arland.ent), an AIWorld prefab and .meta files.
// The game mode entity sets NO scenario config: Clausewitz picks a scenario by world name or builds the auto scenario.
//
// Usage: node new-map-wrapper.mjs <map addon folder | world .ent path> [options]
//   <map addon folder>  packed Workshop addon (data.pak + resourceDatabase.rdb + addon.gproj) or a loose addon
//                       (addon.gproj + .meta files next to the resources; also the parent folder of an `addon/` subfolder)
//   <world .ent path>   a file path on disk inside a loose addon, or (with --addon) a virtual path like worlds/Takistan/Takistan.ent
// Options:
//   --out <dir>          output root (default ./wrappers); writes <out>/<Name>/...
//   --name <Name>        addon folder / project id (default Clausewitz_<WorldName>)
//   --world <path>       virtual path of the world .ent when several candidates exist (see --list)
//   --addon <dir>        map addon folder when the positional argument is a virtual world path
//   --soldier-nav <path> / --vehicle-nav <path>   virtual paths of the .nmn files (override the heuristic)
//   --own-nav            keep the navmesh of the map's own AIWorld prefab (default when that prefab references .nmn files)
//   --force-nav          always write the navmesh components (needs soldier and/or vehicle navmesh)
//   --no-nav             write the world only (AI will not move)
//   --prefix <p>         file prefix of the wrapper resources (default CLW_)
//   --spawn "x,y,z;x,y,z"  spawn point coordinates (USSR;US) instead of the town search
//   --list               print the discovered worlds / navmeshes / AIWorld prefabs / places and exit
//   --yes                pick the best-scored candidate when ambiguous (otherwise: print the candidates, exit code 2)
// Pak reader: the Enfusion MCP's dist (default D:/GitHub/enfusion-workbench-mcp/dist, override with env ENFUSION_MCP_DIST).
// No npm dependencies.

import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync, statSync } from "node:fs";
import { join, dirname, resolve, basename, relative, sep } from "node:path";
import { randomBytes } from "node:crypto";
import { pathToFileURL, fileURLToPath } from "node:url";

// ---------------------------------------------------------------- constants
const CLAUSEWITZ_GUID = "C1A05E7B3D9F4A28";
const BASE_GAME_GUID = "58D0FB3206B6F859";
const BASE_AIWORLD = "{E0A05C76552E7F58}Prefabs/AI/SCR_AIWorld.et"; // vanilla SCR_AIWorld.et (seen as parent of SCR_AIWorld_Takistan.et)
// Entity prefabs of the Clausewitz layer (copied from addon/Worlds/CLW_Arland_Layers/default.layer; keep in sync).
const REF = {
	perception: "{028DAEAD63E056BE}Prefabs/World/Game/PerceptionManager.et",
	factions: "{9A4E27C1B83D5F06}Prefabs/Managers/CLW_FactionManager.et",
	gameMode: "{CB770FDC19B2CC62}Prefabs/GameModes/CLW_GameMode.et",
	spawnUssr: "{4FEA25583D0975F7}Prefabs/MP/Spawning/SpawnPoint_USSR.et",
	spawnUs: "{5A2220DD0C16958B}Prefabs/MP/Spawning/SpawnPoint_US.et",
	loadouts: "{5B1F83D6E02A7C49}Prefabs/Managers/CLW_LoadoutManager.et",
	chat: "{F69BC912AC8236F9}Prefabs/MP/ScriptedChatEntity.et",
};
const SKIP_WORLD = /gamemaster|[_/]gm[_.]|empty|editor|test|mission|conflict|coop|campaign|tutorial|menu|splash|preview/i;

// ---------------------------------------------------------------- args
const argv = process.argv.slice(2);
const opt = { _: [] };
const FLAGS = new Set(["list", "yes", "no-nav", "own-nav", "force-nav", "help"]);
for (let i = 0; i < argv.length; i++) {
	const a = argv[i];
	if (a.startsWith("--")) {
		const k = a.slice(2);
		if (FLAGS.has(k)) opt[k] = true; else opt[k] = argv[++i];
	} else opt._.push(a);
}
function die(msg, code = 1) { console.error("ERROR: " + msg); process.exit(code); }
function warn(msg) { console.warn("WARN: " + msg); }
function info(msg) { console.log(msg); }
if (opt.help || opt._.length === 0) {
	console.log(readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n").filter((l) => l.startsWith("//")).map((l) => l.slice(3)).join("\n"));
	process.exit(opt.help ? 0 : 1);
}

// ---------------------------------------------------------------- source abstraction (packed or loose)
const MCP_DIST = process.env.ENFUSION_MCP_DIST || "D:/GitHub/enfusion-workbench-mcp/dist";

function walkFs(dir, base, out) {
	for (const e of readdirSync(dir, { withFileTypes: true })) {
		if (e.name === ".git" || e.name === "node_modules") continue;
		const p = base ? base + "/" + e.name : e.name;
		if (e.isDirectory()) walkFs(join(dir, e.name), p, out); else out.push(p);
	}
}

async function openSource(root) {
	const pakPath = join(root, "data.pak");
	const src = { root, packed: false, files: [], read: null, rdb: null, gproj: null };
	if (existsSync(pakPath)) {
		let PakVirtualFS;
		try { ({ PakVirtualFS } = await import(pathToFileURL(join(MCP_DIST, "pak/vfs.js")).href)); }
		catch (e) { die(`cannot load the pak reader from ${MCP_DIST}/pak/vfs.js (${e.message}); set ENFUSION_MCP_DIST`); }
		const vfs = new PakVirtualFS([pakPath]); // constructor is TS-private, plain JS here
		src.packed = true;
		src.files = vfs.allFilePaths();
		src.read = (p) => vfs.readFile(p);
	} else {
		walkFs(root, "", src.files);
		src.read = (p) => readFileSync(join(root, p));
	}
	const rdbPath = join(root, "resourceDatabase.rdb");
	if (existsSync(rdbPath)) src.rdb = readFileSync(rdbPath);
	return src;
}

// GUID of a resource: loose .meta first, then resourceDatabase.rdb (record = u32 len+1, path, 0, 6 bytes, 8-byte GUID little-endian).
function guidOf(src, vpath) {
	const metaName = vpath + ".meta";
	if (src.files.includes(metaName)) {
		const m = /Name\s+"\{([0-9A-Fa-f]{16})\}/.exec(src.read(metaName).toString("utf8"));
		if (m) return m[1].toUpperCase();
	}
	if (src.rdb) {
		const pb = Buffer.from(vpath, "latin1");
		const key = Buffer.concat([Buffer.alloc(4), pb, Buffer.from([0])]);
		key.writeUInt32LE(pb.length + 1, 0);
		let i = src.rdb.indexOf(key);
		if (i < 0) i = src.rdb.indexOf(Buffer.concat([pb, Buffer.from([0])])) - 4; // fallback without length prefix check
		if (i >= 0) {
			const g = i + 4 + pb.length + 1 + 6;
			if (g + 8 <= src.rdb.length) {
				let h = "";
				for (let k = 7; k >= 0; k--) h += src.rdb[g + k].toString(16).padStart(2, "0");
				return h.toUpperCase();
			}
		}
	}
	return null;
}
function refOf(src, vpath) {
	const g = guidOf(src, vpath);
	if (!g) die(`no GUID for ${vpath} (no .meta, not in resourceDatabase.rdb of ${src.root})`);
	return `{${g}}${vpath}`;
}

// ---------------------------------------------------------------- locate the map addon
let addonDir, worldArg = null;
const first = resolve(opt._[0]);
if (/\.ent$/i.test(opt._[0])) {
	if (opt.addon) { addonDir = resolve(opt.addon); worldArg = opt._[0].replace(/\\/g, "/"); }
	else if (existsSync(first)) { // a file on disk: walk up to the addon.gproj
		let d = dirname(first);
		while (d && !existsSync(join(d, "addon.gproj")) && dirname(d) !== d) d = dirname(d);
		if (!existsSync(join(d, "addon.gproj"))) die("world file is not inside an addon folder (no addon.gproj above it); use --addon");
		addonDir = d; worldArg = relative(d, first).split(sep).join("/");
	} else die("a virtual world path needs --addon <map addon folder>");
} else addonDir = first;
if (!existsSync(addonDir) || !statSync(addonDir).isDirectory()) die(`not a folder: ${addonDir}`);
if (!existsSync(join(addonDir, "addon.gproj")) && existsSync(join(addonDir, "addon", "addon.gproj"))) addonDir = join(addonDir, "addon");
if (!existsSync(join(addonDir, "addon.gproj"))) die(`no addon.gproj in ${addonDir}`);

const gprojText = readFileSync(join(addonDir, "addon.gproj"), "utf8").replace(/^\uFEFF/, "");
const mapGuid = (/GUID\s+"([0-9A-Fa-f]{16})"/.exec(gprojText) || [])[1]?.toUpperCase();
const mapId = (/\bID\s+"([^"]*)"/.exec(gprojText) || [])[1] || basename(addonDir);
const mapDeps = ((/Dependencies\s*\{([^}]*)\}/.exec(gprojText) || [])[1] || "").match(/[0-9A-Fa-f]{16}/g) || [];
if (!mapGuid) die("addon.gproj has no GUID");
const src = await openSource(addonDir);
info(`map addon ${mapId} {${mapGuid}} ${src.packed ? "packed" : "loose"}, ${src.files.length} files, rdb ${src.rdb ? "yes" : "no"}`);
info(`map dependencies: ${mapDeps.join(" ") || "(none)"}`);

// ---------------------------------------------------------------- discover worlds
const lower = (s) => s.toLowerCase();
const fileSet = new Set(src.files.map(lower));
function layerText(entPath, name) {
	const lp = entPath.replace(/\.ent$/i, "") + "_Layers/" + name;
	const real = src.files.find((f) => lower(f) === lower(lp));
	if (!real) return null;
	try { return src.read(real).toString("latin1"); } catch { return null; }
}
const worldCands = src.files.filter((f) => /\.ent$/i.test(f)).map((f) => {
	let score = 0, why = [];
	const dl = layerText(f, "default.layer");
	if (dl && /GenericWorldEntity/.test(dl.slice(0, 400))) { score += 10; why.push("terrain layer"); }
	else if (!dl && !fileSet.has(lower(f.replace(/\.ent$/i, "") + "_layers/default.layer"))) { score += 0; why.push("no default.layer"); }
	if (!SKIP_WORLD.test(f)) { score += 5; why.push("name ok"); }
	if (/(^|\/)worlds\//i.test(f)) score += 1;
	score -= f.split("/").length * 0.1;
	return { path: f, score, why: why.join(", ") };
}).sort((a, b) => b.score - a.score);

const navFiles = src.files.filter((f) => /\.nmn$/i.test(f));
const aiPrefabs = src.files.filter((f) => /(^|\/)prefabs\/ai\/[^/]*aiworld[^/]*\.et$/i.test(f));

// ---------------------------------------------------------------- places (for spawn points on land)
function scanPlaces() {
	const layers = src.files.filter((f) => /\.layer$/i.test(f) && /location|town|village|place|poi/i.test(f));
	const places = [];
	for (const lf of layers) {
		let t; try { t = src.read(lf).toString("latin1"); } catch { continue; }
		const re = /coords\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)/g; let m, prevEnd = 0;
		while ((m = re.exec(t))) {
			const head = t.lastIndexOf("$grp", m.index);
			const proto = t.slice(Math.max(head, 0), m.index);
			const seg = t.slice(Math.max(prevEnd, head, 0), m.index); // this entity only (after the previous coords line)
			prevEnd = m.index + m[0].length;
			const mt = [...seg.matchAll(/MainType\s+"([^"]*)"/g)].pop();
			const pm = [...proto.matchAll(/Prefabs\/World\/Locations\/([^"\s]*)/g)].pop();
			if (!pm) continue;
			const nm = [...seg.matchAll(/DisplayName\s+"([^"]*)"/g)].pop();
			places.push({ x: +m[1], y: +m[2], z: +m[3], kind: pm[1], name: nm ? nm[1] : "", type: mt ? mt[1] : "", file: lf });
		}
	}
	return places;
}
const places = scanPlaces();
const urbanAll = places.filter((p) => /urban|town|village|city/i.test(p.kind) && !/airport|airfield|base/i.test(p.kind + p.name));
// real settlements first (MainType Town/Village/City), every urban location prefab otherwise
const urbanTowns = urbanAll.filter((p) => /town|village|city/i.test(p.type));
const urban = urbanTowns.length >= 2 ? urbanTowns : urbanAll;

function pickSpawns(list) {
	if (list.length === 0) return null;
	const cx = list.reduce((s, p) => s + p.x, 0) / list.length, cz = list.reduce((s, p) => s + p.z, 0) / list.length;
	const near = [...list].sort((a, b) => Math.hypot(a.x - cx, a.z - cz) - Math.hypot(b.x - cx, b.z - cz)).slice(0, 40);
	let best = null;
	for (const a of near) for (const b of near) {
		const d = Math.hypot(a.x - b.x, a.z - b.z);
		if (a !== b && (!best || d > best.d)) best = { a, b, d };
	}
	if (!best) return { ussr: list[0], us: list[0], note: "single place" };
	return { ussr: best.a, us: best.b, note: `${Math.round(best.d)} m apart` };
}

if (opt.list) {
	info("\nworld candidates (best first):");
	for (const c of worldCands) info(`  ${c.score.toFixed(1).padStart(5)}  ${c.path}  [${c.why}]`);
	info("\nnavmeshes (.nmn):"); navFiles.forEach((f) => info("  " + f));
	info("\nAIWorld prefabs:"); aiPrefabs.forEach((f) => info("  " + f));
	info(`\nplaces: ${places.length} located, ${urban.length} urban; spawn pick: ${JSON.stringify(pickSpawns(urban) && { ussr: pickSpawns(urban).ussr.name, us: pickSpawns(urban).us.name })}`);
	process.exit(0);
}

// ---------------------------------------------------------------- choose world
let worldPath;
if (opt.world || worldArg) {
	const w = (opt.world || worldArg).replace(/\\/g, "/");
	worldPath = src.files.find((f) => lower(f) === lower(w));
	if (!worldPath) die(`world not found in the addon: ${w} (use --list)`);
} else {
	const top = worldCands.filter((c) => c.score >= 4);
	if (top.length === 0) die("no world .ent found in the addon (use --list, then --world)");
	const tied = top.filter((c) => c.score === top[0].score);
	if (tied.length > 1 && !opt.yes) {
		console.error("ambiguous world, candidates:");
		top.slice(0, 8).forEach((c) => console.error(`  ${c.score.toFixed(1)}  ${c.path}  [${c.why}]`));
		die("pass --world <path> (or --yes to take the first)", 2);
	}
	worldPath = top[0].path;
}
const worldBase = basename(worldPath).replace(/\.ent$/i, "");
const worldRef = refOf(src, worldPath);
info(`world: ${worldRef}`);

// ---------------------------------------------------------------- AIWorld / navmesh
function classifyNav(f) {
	const n = lower(basename(f));
	if (/lowres|low_res/.test(n)) return "lowres";
	if (/veh|btr|car|wheel|tank|apc|truck|chimera/.test(n)) return "vehicle";
	return "soldier";
}
let soldierNav = opt["soldier-nav"], vehicleNav = opt["vehicle-nav"];
const pickNav = (list, kind) => {
	const hit = list.filter((f) => classifyNav(f) === kind);
	const named = hit.filter((f) => lower(f).includes(lower(worldBase)));
	return (named.length ? named : hit);
};
const resolveNav = (v, kind) => {
	if (!v) return null;
	const f = src.files.find((x) => lower(x) === lower(v.replace(/\\/g, "/")));
	if (!f) die(`--${kind}-nav not found in the addon: ${v}`);
	return f;
};
soldierNav = resolveNav(soldierNav, "soldier"); vehicleNav = resolveNav(vehicleNav, "vehicle");
const navWarnings = [];
let aiParent = BASE_AIWORLD, aiParentClass = "SCR_AIWorld", aiOwnNav = false, navMode = "none";
const aiPick = aiPrefabs.filter((f) => lower(f).includes(lower(worldBase)))[0] || (aiPrefabs.length === 1 ? aiPrefabs[0] : null);
if (aiPick) {
	aiParent = refOf(src, aiPick);
	const t = src.read(aiPick).toString("utf8");
	aiParentClass = (/^\s*([A-Za-z_][\w]*)\s*(?:\w+\s*)?:\s*"/m.exec(t) || [])[1] || "SCR_AIWorld";
	aiOwnNav = /NavmeshFile\s+"/.test(t);
	info(`AIWorld prefab of the map: ${aiPick} (${aiOwnNav ? "references navmeshes" : "no navmesh files"})`);
} else info("no AIWorld prefab in the map addon, inheriting the vanilla SCR_AIWorld.et");
if (opt["no-nav"]) navMode = "none";
else if (opt["force-nav"] || soldierNav || vehicleNav || !aiOwnNav) {
	if (!soldierNav) { const c = pickNav(navFiles, "soldier"); if (c.length > 1) navWarnings.push(`several soldier navmeshes, using ${c[0]} (candidates: ${c.join(", ")})`); soldierNav = c[0] || null; }
	if (!vehicleNav) { const c = pickNav(navFiles, "vehicle"); if (c.length > 1) navWarnings.push(`several vehicle navmeshes, using ${c[0]} (candidates: ${c.join(", ")})`); vehicleNav = c[0] || null; }
	navMode = soldierNav || vehicleNav ? "explicit" : "none";
	if (navMode === "none") navWarnings.push(aiOwnNav ? "no .nmn found" : "the map addon has no navmesh (.nmn) and no AIWorld prefab with navmeshes: AI will not move. Generate navmeshes in Workbench (world editor, Navmesh tool) or use a map that ships them");
	if (aiOwnNav && navMode === "explicit" && !opt["force-nav"] && !soldierNav && !vehicleNav) navMode = "none";
} else navMode = "own";
if (navMode === "none" && !aiOwnNav && !opt["no-nav"] && !navWarnings.length) navWarnings.push("no navmesh: AI will not move");
if (navMode === "own") info("navmesh: inherited from the map's AIWorld prefab (no override written)");
else if (navMode === "explicit") info(`navmesh: soldier=${soldierNav || "-"} vehicle=${vehicleNav || "-"}`);
navWarnings.forEach(warn);

// ---------------------------------------------------------------- spawn points
let spawns, spawnNote;
if (opt.spawn) {
	const [a, b] = opt.spawn.split(";").map((s) => s.split(",").map(Number));
	if (!a || !b || a.length !== 3 || b.length !== 3 || [...a, ...b].some(isNaN)) die('--spawn needs "x,y,z;x,y,z"');
	spawns = { ussr: { x: a[0], y: a[1], z: a[2], name: "cli" }, us: { x: b[0], y: b[1], z: b[2], name: "cli" } };
	spawnNote = "from --spawn";
} else {
	const sp = pickSpawns(urban);
	if (sp) { spawns = sp; spawnNote = `towns: USSR at "${sp.ussr.name}", US at "${sp.us.name}" (${sp.note})`; }
	else {
		spawns = { ussr: { x: 2048, y: 0, z: 2048, name: "?" }, us: { x: 2048, y: 0, z: 2148, name: "?" } };
		spawnNote = "no places found";
		warn("no map places found: spawn points are at 2048 0 2048, edit the layer if that is sea (the game re-places base spawn points at runtime)");
	}
}
info(`spawn points: ${spawnNote}`);

// ---------------------------------------------------------------- output
const prefix = opt.prefix || "CLW_";
const name = opt.name || `Clausewitz_${worldBase}`;
const outRoot = resolve(opt.out || "wrappers");
const outDir = join(outRoot, name);
const wrapperWorld = `${prefix}${worldBase}`;
const worldFile = `Worlds/${wrapperWorld}.ent`;
const layerFile = `Worlds/${wrapperWorld}_Layers/default.layer`;
const aiFile = `Prefabs/AI/${prefix}AIWorld_${worldBase}.et`;

// fresh GUIDs: not in the map rdb, not used in the Clausewitz addon next to this script (if present)
const used = new Set();
const clwAddon = resolve(dirname(fileURLToPath(import.meta.url)), "../../addon");
if (existsSync(clwAddon)) {
	const all = []; walkFs(clwAddon, "", all);
	for (const f of all) if (/\.(meta|et|layer|ent|conf|gproj)$/i.test(f)) for (const m of readFileSync(join(clwAddon, f), "utf8").matchAll(/[0-9A-Fa-f]{16}/g)) used.add(m[0].toUpperCase());
}
function freshGuid() {
	for (;;) {
		const g = randomBytes(8).toString("hex").toUpperCase();
		if (used.has(g) || g[0] === "0") continue;
		if (src.rdb) { const le = Buffer.from(g, "hex").reverse(); if (src.rdb.indexOf(le) >= 0) continue; }
		used.add(g); return g;
	}
}
const wrapperAddonGuid = freshGuid(), worldGuid = freshGuid(), aiGuid = freshGuid();

const NL = "\r\n"; // layers are CRLF in the repo (the Workbench writes them so), the rest LF
const fmt = (n) => (Math.round(n * 1000) / 1000).toString();
const cx = (spawns.ussr.x + spawns.us.x) / 2, cz = (spawns.ussr.z + spawns.us.z) / 2;
const ent = (cls, nm, ref, x, y, z, extra = "") => `${cls} ${nm}${ref ? ` : "${ref}"` : ""} {${NL} coords ${fmt(x)} ${fmt(y)} ${fmt(z)}${NL}${extra}}${NL}`;
const layer =
	ent("SCR_AIWorld", "CLW_AIWorld", `{${aiGuid}}${aiFile}`, cx, 0, cz + 30) +
	ent("PerceptionManager", "CLW_PerceptionManager", REF.perception, cx, 0, cz + 40) +
	ent("SCR_FactionManager", "CLW_FactionManager", REF.factions, cx, 0, cz + 10) +
	ent("CLW_GameMode", "CLW_GameMode", REF.gameMode, cx, 0, cz) +
	ent("SCR_EditorSettingsEntity", "CLW_EditorSettings", null, cx, 0, cz - 10, ` m_bIsUnlimitedEditorLegal 1${NL}`) +
	ent("SCR_SpawnPoint", "CLW_Spawn_USSR", REF.spawnUssr, spawns.ussr.x + 40, spawns.ussr.y, spawns.ussr.z) +
	ent("SCR_SpawnPoint", "CLW_Spawn_US", REF.spawnUs, spawns.us.x + 40, spawns.us.y, spawns.us.z) +
	ent("SCR_LoadoutManager", "CLW_LoadoutManager", REF.loadouts, cx, 0, cz + 20) +
	ent("ScriptedChatEntity", "CLW_Chat", REF.chat, cx, 0, cz - 20);

function metaFile(guid, vpath, cls) {
	const cfg = (n, parent) => `  ${cls} ${n}${parent ? " : " + parent : ""} {\n  }\n`;
	return `MetaFileClass {\n Name "{${guid}}${vpath}"\n Configurations {\n${cfg("PC")}${["XBOX_ONE", "XBOX_SERIES", "PS4", "PS5", "HEADLESS"].map((n) => cfg(n, "PC")).join("")} }\n}`;
}
function navComponent(compGuid, cls, cfgGuid, fileRef, extra = "") {
	return `  NavmeshWorldComponent "{${compGuid}}" {\n   NavmeshSettings ${cls} "{50FC63BEBE3973C5}" {\n    NavmeshFilesConfig BaseNavmeshFilesConfig "{${cfgGuid}}" {\n     NavmeshFile "${fileRef}"\n    }\n${extra}   }\n  }\n`;
}
let aiText = `${aiParentClass} : "${aiParent}" {\n`;
if (navMode === "explicit") {
	aiText += " components {\n";
	// component / config GUIDs are the ones of CLW_AIWorld_Arland.et (they address the parent's components)
	if (soldierNav) aiText += navComponent("5584F30E67F617AD", "NavmeshWorld", "60B8A2995A7E8BC2", refOf(src, soldierNav));
	if (vehicleNav) aiText += navComponent("5584F30EEFEE1223", "ChimeraNavmeshWorld", "60B8A299509CF377", refOf(src, vehicleNav));
	aiText += " }\n";
}
aiText += "}\n";
aiText = aiText.replace(/\n$/, "");

const depList = [CLAUSEWITZ_GUID, mapGuid].map((g) => `"${g}"`).join(" ");
const gproj = `GameProject {\n ID "${name}"\n GUID "${wrapperAddonGuid}"\n TITLE "Clausewitz on ${mapId}"\n Dependencies {\n  ${depList}\n }\n Configurations {\n  GameProjectConfig PC {\n  }\n  GameProjectConfig HEADLESS {\n  }\n }\n}`;

const out = {
	"addon.gproj": gproj,
	[worldFile]: `SubScene {\n Parent "${worldRef}"\n}`,
	[worldFile + ".meta"]: metaFile(worldGuid, worldFile, "ENTResourceClass"),
	[layerFile]: layer,
	[aiFile]: aiText,
	[aiFile + ".meta"]: metaFile(aiGuid, aiFile, "EntityTemplateResourceClass"),
};
for (const [p, text] of Object.entries(out)) {
	const full = join(outDir, p);
	mkdirSync(dirname(full), { recursive: true });
	writeFileSync(full, text);
}
info(`\nwrote ${Object.keys(out).length} files to ${outDir}`);
info(`  addon GUID     ${wrapperAddonGuid}  (project id ${name})`);
info(`  world          ${worldFile}  {${worldGuid}}  parent ${worldRef}`);
info(`  AIWorld prefab ${aiFile}  {${aiGuid}}  parent ${aiParent}`);
info(`  dependencies   Clausewitz ${CLAUSEWITZ_GUID}, map ${mapGuid} (map needs: ${mapDeps.join(", ") || "-"})`);
info(`\nDedicated server: see dev/notes/maps.md. Server -addons list: ${CLAUSEWITZ_GUID},${[...mapDeps.filter((d) => d !== BASE_GAME_GUID), mapGuid, wrapperAddonGuid].join(",")}`);
info(`Server world argument: -World "Worlds/${wrapperWorld}.ent"`);
if (!existsSync(join(outDir, "resourceDatabase.rdb"))) info("Note: the wrapper has no resourceDatabase.rdb; open it once in Workbench (or let the server build it) to register the new resources.");

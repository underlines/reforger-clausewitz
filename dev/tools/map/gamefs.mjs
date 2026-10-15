// Virtual file system over an Arma Reforger install (addons/**/*.pak) plus optional extra addon dirs
// (Workshop mods, loose addon sources). Paks are indexed lazily on first use and cached.
//   findGameDir()                      -> install path or null
//   resolveAddonDirs(entries)          -> {dirs, skipped}: expands extraDirs entries to addon dirs (readdir only)
//   createGameFs({gameDir, extraDirs}) -> {readFile(path), exists(path), find(regex), size(path), pakCount, addonDirs}
// Lookup is case-insensitive with forward slashes. Order: game paks (alphabetical) first, then the extra addons in the given order
// (mods supplement the game, they do not override it). Game paks are indexed on first use; each extra addon is opened only when a
// lookup missed everything before it, so a vanilla world never touches ~200 mod paks and a mod world stops at the addon that has it.
// An extraDirs entry is one addon dir (directly holds *.pak, or addon.gproj for a loose source addon) or a parent dir of such addon dirs.
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { join, resolve, delimiter } from 'node:path';
import { homedir } from 'node:os';
import { openPak, readEntry, normalizeEntryPath } from './pak.mjs';

/** Auto-detect the "Arma Reforger" Steam install. Env ARMA_REFORGER_DIR overrides. */
export function findGameDir() {
  const env = process.env.ARMA_REFORGER_DIR;
  if (env && existsSync(join(env, 'addons'))) return env;

  const libs = new Set();
  const steamRoots = [
    'C:\\Program Files (x86)\\Steam',
    'C:\\Program Files\\Steam',
    join(homedir(), '.steam', 'steam'),
    join(homedir(), '.local', 'share', 'Steam'),
  ];
  for (const root of steamRoots) {
    if (!existsSync(root)) continue;
    libs.add(root);
    const vdf = join(root, 'steamapps', 'libraryfolders.vdf');
    try {
      const text = readFileSync(vdf, 'utf8');
      for (const m of text.matchAll(/"path"\s+"([^"]+)"/gi)) libs.add(m[1].replace(/\\\\/g, '\\'));
    } catch { /* no vdf */ }
  }
  for (const lib of libs) {
    const dir = join(lib, 'steamapps', 'common', 'Arma Reforger');
    if (existsSync(join(dir, 'addons'))) return dir;
  }
  return null;
}

/** Default Workshop/addon dir: ~/Documents/My Games/ArmaReforger/addons */
export function defaultWorkshopDir() {
  const d = join(homedir(), 'Documents', 'My Games', 'ArmaReforger', 'addons');
  return existsSync(d) ? d : null;
}

function listPaks(dir, depth = 0, out = []) {
  let ents;
  try { ents = readdirSync(dir, { withFileTypes: true }); } catch { return out; }
  ents.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  for (const e of ents) {
    const p = join(dir, e.name);
    if (e.isFile() && e.name.toLowerCase().endsWith('.pak')) out.push(p);
    else if (e.isDirectory() && depth < 3) listPaks(p, depth + 1, out);
  }
  return out;
}

function walkLoose(dir, rel, out, cap) {
  let ents;
  try { ents = readdirSync(dir, { withFileTypes: true }); } catch { return; }
  for (const e of ents) {
    if (out.length >= cap) return;
    if (e.isDirectory()) {
      if (e.name === '.git' || e.name === 'node_modules') continue;
      walkLoose(join(dir, e.name), rel ? `${rel}/${e.name}` : e.name, out, cap);
    } else if (!e.name.toLowerCase().endsWith('.pak')) {
      out.push(rel ? `${rel}/${e.name}` : e.name);
    }
  }
}

function isDir(p) {
  try { return statSync(p).isDirectory(); } catch { return false; }
}

/** Addon dir = directly contains a *.pak (packed addon) or an addon.gproj (loose source addon). */
function isAddonDir(dir) {
  let names;
  try { names = readdirSync(dir); } catch { return false; }
  return names.some((n) => { const l = n.toLowerCase(); return l.endsWith('.pak') || l === 'addon.gproj'; });
}

/**
 * Expand extraDirs entries into addon dirs. An entry that is an addon dir stays as it is; any other dir contributes its immediate
 * subdirectories that are addon dirs (sorted by name). Duplicates are dropped. Returns {dirs, skipped:[{path, reason}]}.
 * Only readdir/stat calls, no pak is opened.
 */
export function resolveAddonDirs(entries) {
  const dirs = [], skipped = [], seen = new Set();
  const add = (d) => { const k = resolve(d).toLowerCase(); if (!seen.has(k)) { seen.add(k); dirs.push(d); } };
  for (const raw of entries || []) {
    const e = typeof raw === 'string' ? raw.trim() : '';
    if (!e) continue;
    let st;
    try { st = statSync(e); } catch { skipped.push({ path: e, reason: 'not found or unreadable' }); continue; }
    if (!st.isDirectory()) { skipped.push({ path: e, reason: 'not a directory' }); continue; }
    if (isAddonDir(e)) { add(e); continue; }
    let kids;
    try { kids = readdirSync(e, { withFileTypes: true }); } catch { skipped.push({ path: e, reason: 'not readable' }); continue; }
    kids.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    let n = 0;
    for (const k of kids) {
      const p = join(e, k.name);
      // symlinks / Windows junctions to addon dirs count too (e.g. a dev addons folder of junctions to the Workshop cache)
      if (!k.isDirectory() && !(k.isSymbolicLink() && isDir(p))) continue;
      if (isAddonDir(p)) { add(p); n++; }
    }
    if (!n) skipped.push({ path: e, reason: 'no addon dirs inside (no *.pak or addon.gproj)' });
  }
  return { dirs, skipped };
}

/** Split a path list (env CLW_ADDON_DIRS): separators are the OS path delimiter and ','. Empty items are dropped. */
export function splitPathList(s, delim = delimiter) {
  return String(s || '').split(new RegExp(`[${delim},]`)).map((x) => x.trim()).filter(Boolean);
}

/** Effective addon dir entries: env list first, then the config list, without duplicates. */
export function mergeAddonDirEntries(envValue, cfgList) {
  const out = [];
  for (const d of [...splitPathList(envValue), ...(Array.isArray(cfgList) ? cfgList : [])]) if (typeof d === 'string' && d.trim() && !out.includes(d.trim())) out.push(d.trim());
  return out;
}

/** Sources of one addon dir: its own *.pak files (sorted); loose = no pak, a source addon read from disk. */
function addonSources(dir) {
  let names;
  try { names = readdirSync(dir); } catch { return { paks: [], loose: false }; }
  const paks = names.filter((n) => n.toLowerCase().endsWith('.pak')).sort().map((n) => join(dir, n));
  return { paks, loose: !paks.length };
}

export function createGameFs({ gameDir, extraDirs = [] } = {}) {
  gameDir = gameDir || findGameDir();
  const addons = resolveAddonDirs(extraDirs).dirs.map((dir) => ({ dir, ...addonSources(dir) }));
  let built = false;
  const index = new Map();  // game paks: lowercase path -> {path, pak}
  const extra = new Map();  // opened extra addons: lowercase path -> {path, pak} | {path, file}
  let gamePakCount = 0;
  let opened = 0;           // number of addons (from the front of `addons`) already indexed into `extra`

  function addPak(map, pakPath) {
    let pak;
    try { pak = openPak(pakPath); } catch (e) { console.warn(`[gamefs] skipping ${pakPath}: ${e.message}`); return false; }
    for (const [key, ent] of pak.files) if (!map.has(key)) map.set(key, { path: ent.path, pak });
    return true;
  }

  function addLoose(root) {
    const rels = [];
    walkLoose(root, '', rels, 200000);
    for (const rel of rels) {
      const key = rel.toLowerCase();
      if (!extra.has(key)) extra.set(key, { path: rel, file: join(root, ...rel.split('/')) });
    }
  }

  function openNextAddon() {
    const a = addons[opened++];
    for (const p of a.paks) addPak(extra, p);
    if (a.loose) addLoose(a.dir);
  }

  function build() {
    if (built) return;
    built = true;
    if (gameDir) for (const p of listPaks(join(gameDir, 'addons'))) if (addPak(index, p)) gamePakCount++;
  }

  /** Entry for a normalized key: game paks, then the extra addons in order, opening them one by one until it is found. */
  function lookup(key) {
    build();
    let e = index.get(key);
    if (e) return e;
    for (;;) {
      e = extra.get(key);
      if (e) return e;
      if (opened >= addons.length) return null;
      openNextAddon();
    }
  }

  const norm = (p) => normalizeEntryPath(String(p).replace(/^\{[0-9A-Fa-f]{16}\}/, ''));

  return {
    gameDir,
    /** Resolved extra addon dirs, in lookup order. */
    addonDirs: addons.map((a) => a.dir),
    /** Extra addons indexed so far (the rest were never needed). */
    get addonsOpened() { return opened; },
    // known pak count without opening the extra addons
    get pakCount() { build(); return gamePakCount + addons.reduce((n, a) => n + a.paks.length, 0); },
    exists(p) { return !!lookup(norm(p)); },
    size(p) {
      const k = norm(p), e = lookup(k);
      if (!e) return -1;
      if (e.file) return statSync(e.file).size;
      return e.pak.files.get(k).size;
    },
    /** Returns a Buffer; throws if the path does not exist. */
    readFile(p) {
      const e = lookup(norm(p));
      if (!e) throw new Error(`Not found in game FS: ${p}`);
      return e.file ? readFileSync(e.file) : readEntry(e.pak, e.path);
    },
    /** All virtual paths (original case) matching the regex. Opens every extra addon. */
    find(re) {
      build();
      while (opened < addons.length) openNextAddon();
      const out = [];
      for (const e of index.values()) if (re.test(e.path)) out.push(e.path);
      for (const [k, e] of extra) if (!index.has(k) && re.test(e.path)) out.push(e.path);
      return out;
    },
  };
}


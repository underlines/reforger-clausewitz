// Virtual file system over an Arma Reforger install (addons/**/*.pak) plus optional extra addon dirs
// (Workshop folder, loose addon sources). Paks are indexed lazily on first use and cached.
//   findGameDir()                      -> install path or null
//   createGameFs({gameDir, extraDirs}) -> {readFile(path), exists(path), find(regex), size(path), pakCount}
// Lookup is case-insensitive with forward slashes. Earlier sources win on duplicates:
// extraDirs (mods) first, then game paks in alphabetical order.
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
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

export function createGameFs({ gameDir, extraDirs = [] } = {}) {
  gameDir = gameDir || findGameDir();
  const extras = (extraDirs || []).filter((d) => d && existsSync(d));
  let built = false;
  const index = new Map(); // lowercase path -> {path, pak} | {path, file}
  let pakCount = 0;

  function addPak(pakPath) {
    let pak;
    try { pak = openPak(pakPath); } catch (e) { console.warn(`[gamefs] skipping ${pakPath}: ${e.message}`); return; }
    pakCount++;
    for (const [key, ent] of pak.files) if (!index.has(key)) index.set(key, { path: ent.path, pak });
  }

  function addLoose(root) {
    // Extra dirs may be a flat folder of addons; only treat files we can walk cheaply.
    const rels = [];
    walkLoose(root, '', rels, 200000);
    for (const rel of rels) {
      const key = rel.toLowerCase();
      if (!index.has(key)) index.set(key, { path: rel, file: join(root, ...rel.split('/')) });
    }
  }

  function build() {
    if (built) return;
    built = true;
    for (const d of extras) { for (const p of listPaks(d)) addPak(p); addLoose(d); }
    if (gameDir) for (const p of listPaks(join(gameDir, 'addons'))) addPak(p);
  }

  const norm = (p) => normalizeEntryPath(String(p).replace(/^\{[0-9A-Fa-f]{16}\}/, ''));

  return {
    gameDir,
    get pakCount() { build(); return pakCount; },
    exists(p) { build(); return index.has(norm(p)); },
    size(p) {
      build();
      const e = index.get(norm(p));
      if (!e) return -1;
      if (e.file) return statSync(e.file).size;
      return e.pak.files.get(norm(p)).size;
    },
    /** Returns a Buffer; throws if the path does not exist. */
    readFile(p) {
      build();
      const e = index.get(norm(p));
      if (!e) throw new Error(`Not found in game FS: ${p}`);
      return e.file ? readFileSync(e.file) : readEntry(e.pak, e.path);
    },
    /** All virtual paths (original case) matching the regex. */
    find(re) {
      build();
      const out = [];
      for (const e of index.values()) if (re.test(e.path)) out.push(e.path);
      return out;
    },
  };
}


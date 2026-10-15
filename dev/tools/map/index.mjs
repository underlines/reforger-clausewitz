// Map image resolver: finds the background texture of a Reforger world in the user's own game install
// (never redistributed), decodes EDDS/BC7 -> PNG and caches it.
//   resolveMapImage({mapPrefab, worldFile, gameDir, extraDirs, maxSize=2048, cacheDir, overrideDir})
//     -> {pngPath, width, height, source, texturePath, calibration?} | null
//   source: 'override' | 'prefab' | 'heuristic'
// CLI: node index.mjs <texture/.edds path | world name> [out.png] [maxSize]
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync, copyFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createGameFs, findGameDir, defaultWorkshopDir, resolveAddonDirs, mergeAddonDirEntries } from './gamefs.mjs';
import { parseEdds } from './edds.mjs';
import { decodeToRgba } from './bc7.mjs';
import { encodePng } from './png.mjs';

export { findGameDir, createGameFs, resolveAddonDirs, mergeAddonDirEntries };

const HERE = dirname(fileURLToPath(import.meta.url));
const DEFAULT_CACHE = resolve(HERE, '..', '.cache', 'maps');
const ALIASES = { eden: 'Everon' };

const stripGuid = (p) => String(p || '').replace(/^\s*"?\{[0-9A-Fa-f]{16}\}/, '').replace(/"$/, '').trim();

/** `{GUID}worlds/Arland/Arland.ent` -> `Arland` */
export function worldNameOf(worldFile) {
  if (!worldFile) return null;
  const base = stripGuid(worldFile).replace(/\\/g, '/').split('/').pop() || '';
  return base.replace(/\.[^.]+$/, '') || null;
}

// One fs per (gameDir, extraDirs) combination so paks are indexed once.
const fsCache = new Map();
function getFs(gameDir, extraDirs) {
  gameDir = gameDir || findGameDir();
  if (!extraDirs) { const w = defaultWorkshopDir(); extraDirs = w ? [w] : []; }
  const key = JSON.stringify([gameDir, extraDirs]);
  let g = fsCache.get(key);
  if (!g) { g = createGameFs({ gameDir, extraDirs }); fsCache.set(key, g); }
  return g;
}

function findCaseInsensitive(dir, fileName) {
  try {
    const want = fileName.toLowerCase();
    const hit = readdirSync(dir).find((n) => n.toLowerCase() === want);
    return hit ? join(dir, hit) : null;
  } catch { return null; }
}

function pngSize(file) {
  const b = readFileSync(file);
  return { width: b.readUInt32BE(16), height: b.readUInt32BE(20) };
}

// overrideDir: one dir or a list (first hit wins)
function fromOverride(overrideDir, names) {
  if (!overrideDir) return null;
  if (Array.isArray(overrideDir)) {
    for (const d of overrideDir) { const r = fromOverride(d, names); if (r) return r; }
    return null;
  }
  for (const n of names) {
    const png = findCaseInsensitive(overrideDir, `${n}.png`);
    if (!png) continue;
    let calibration = null;
    const js = findCaseInsensitive(overrideDir, `${n}.json`);
    if (js) { try { calibration = JSON.parse(readFileSync(js, 'utf8')); } catch (e) { console.warn(`[map] bad calibration json ${js}: ${e.message}`); } }
    const { width, height } = pngSize(png);
    const res = { pngPath: png, width, height, source: 'override', texturePath: null };
    if (calibration) res.calibration = calibration;
    return res;
  }
  return null;
}

/** Read a prefab and return the background texture path, following `: "{GUID}parent.et"` inheritance. */
function textureFromPrefab(gfs, prefabPath) {
  let path = stripGuid(prefabPath);
  for (let depth = 0; depth < 8 && path; depth++) {
    let text;
    try { text = gfs.readFile(path).toString('utf8'); } catch { return null; }
    for (const line of text.split(/\r?\n/)) {
      if (!/satellite|background/i.test(line)) continue;
      const m = line.match(/"(?:\{[0-9A-Fa-f]{16}\})?([^"]+\.edds)"/i);
      if (m) return m[1];
    }
    const first = text.split(/\r?\n/, 1)[0] || '';
    const p = first.match(/:\s*"(?:\{[0-9A-Fa-f]{16}\})?([^"]+)"/);
    path = p ? p[1] : null;
  }
  return null;
}

function textureByName(gfs, names) {
  const all = gfs.find(/^ui\/textures\/map\/worlds\/.+\.edds$/i);
  if (!all.length) return null;
  const base = (p) => p.split('/').pop().toLowerCase();
  for (const n of names) {
    const ln = n.toLowerCase();
    const tries = [
      (p) => base(p) === `${ln}rasterized.edds`,
      (p) => base(p) === `${ln}.edds`,
      (p) => base(p).startsWith(ln) && base(p).includes('rasterized'),
      (p) => base(p).startsWith(ln),
    ];
    for (const t of tries) { const hit = all.find(t); if (hit) return hit; }
  }
  return null;
}

function cachePathsFor(cacheDir, texturePath, maxSize) {
  const stem = texturePath.replace(/\.edds$/i, '').replace(/[^A-Za-z0-9._-]+/g, '_');
  return { png: join(cacheDir, `${stem}_${maxSize}.png`), json: join(cacheDir, `${stem}_${maxSize}.json`) };
}

/** Decode texture `texturePath` from `gfs` to a cached PNG whose largest side is <= maxSize. */
export function renderTexture(gfs, texturePath, { maxSize = 2048, cacheDir = DEFAULT_CACHE } = {}) {
  const c = cachePathsFor(cacheDir, texturePath, maxSize);
  if (existsSync(c.png) && existsSync(c.json)) {
    try {
      const meta = JSON.parse(readFileSync(c.json, 'utf8'));
      return { pngPath: c.png, width: meta.width, height: meta.height, texturePath, cached: true };
    } catch { /* rebuild */ }
  }
  const edds = parseEdds(gfs.readFile(texturePath));
  // mips are largest first: pick the largest one that fits, else the smallest available
  const mip = edds.mips.find((m) => Math.max(m.width, m.height) <= maxSize) || edds.mips[edds.mips.length - 1];
  const rgba = decodeToRgba({ format: edds.format, width: mip.width, height: mip.height, data: mip.data });
  mkdirSync(cacheDir, { recursive: true });
  writeFileSync(c.png, encodePng(mip.width, mip.height, rgba));
  writeFileSync(c.json, JSON.stringify({
    texturePath, width: mip.width, height: mip.height, sourceWidth: edds.width, sourceHeight: edds.height,
    dxgiFormat: edds.format, createdAt: new Date().toISOString(),
  }, null, 2));
  return { pngPath: c.png, width: mip.width, height: mip.height, texturePath, cached: false };
}

export async function resolveMapImage({
  mapPrefab, worldFile, gameDir, extraDirs, maxSize = 2048, cacheDir = DEFAULT_CACHE, overrideDir,
} = {}) {
  const worldName = worldNameOf(worldFile);
  const names = [];
  if (worldName) { names.push(worldName); if (ALIASES[worldName.toLowerCase()]) names.push(ALIASES[worldName.toLowerCase()]); }

  // (a) user override
  const ov = fromOverride(overrideDir, names);
  if (ov) return ov;

  const gfs = getFs(gameDir, extraDirs);
  if (!gfs.gameDir && !(gfs.pakCount > 0)) return null;

  // (b) MapEntity prefab -> "Satellite background image"
  let source = null, texturePath = null;
  if (mapPrefab) {
    const t = textureFromPrefab(gfs, mapPrefab);
    if (t && gfs.exists(t)) { texturePath = t; source = 'prefab'; }
  }
  // (c) heuristic by world name
  if (!texturePath && names.length) {
    const t = textureByName(gfs, names);
    if (t) { texturePath = t; source = 'heuristic'; }
  }
  if (!texturePath) return null;

  const r = renderTexture(gfs, texturePath, { maxSize, cacheDir });
  return { pngPath: r.pngPath, width: r.width, height: r.height, source, texturePath };
}

// ---- CLI ----
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [arg, out, size] = process.argv.slice(2);
  if (!arg) {
    console.log('usage: node index.mjs <path/to/texture.edds | WorldName> [out.png] [maxSize]');
    process.exit(1);
  }
  const maxSize = size ? parseInt(size, 10) : 2048;
  const t0 = Date.now();
  let res;
  if (/\.edds$/i.test(arg)) {
    const gfs = getFs();
    if (!gfs.exists(arg)) { console.error(`not found in game FS: ${arg}`); process.exit(2); }
    res = renderTexture(gfs, stripGuid(arg), { maxSize });
  } else {
    res = await resolveMapImage({ worldFile: arg, maxSize });
  }
  if (!res) { console.error('no map image found'); process.exit(2); }
  if (out) { copyFileSync(res.pngPath, out); res.copiedTo = resolve(out); }
  res.ms = Date.now() - t0;
  console.log(JSON.stringify(res, null, 2));
}

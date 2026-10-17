// FOB zoom (step 2 of the ESTABLISH_FOB tool, dev/notes/fob-tool-design.md). Side-effect free: importable by tests, no I/O, no config access.
// The game posts POST /fobzoom {v, faction, area, intent, attempt, retry?:{reason, pos}, zoom: <digest>}; the digest is the macro cell the commander picked
// at 100 m resolution (game CLW_MapDigest.ZoomJson): rows of 3-glyph sub-cells (elevation digit, terrain, road), pois, objs, contacts, own, toward.
//   buildFobZoomPrompt({config, req}) -> {system, user, sections}      one small LLM call (role "fobzoom")
//   parseZoomReply(text)              -> raw model object (JSON out of the reply text)
//   normalizeZoomReply(obj, req)      -> {ok, cell?, pos?, decline?, reason, error?}   what the sidecar answers the game with
//   fakeZoomPick(req)                 -> model-shaped object for the fake provider (deterministic: valid cell with a road nearest to `toward`)
//   zoomCells(zoom)                   -> [{id, col, row, elev, terrain, road, x, z}]
// Content-agnostic: no faction keys, place names or coordinates in here.

const isNum = (n) => typeof n === "number" && Number.isFinite(n);
const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v);

export const ZOOM_SYSTEM = `You are the HQ commander of one faction in a tactical war simulation (Arma Reforger). You already decided to build a forward operating base (FOB) in one map area; now pick the exact spot.
The user message holds the area at 100 m resolution. Each sub-cell is 3 glyphs: elevation digit (0 lowest .. 9 highest of the area), terrain, road. Sub-cell id = column letter + row number (row 1 = north, column a = west).
terrain: ~ water, x keep-out (the game refuses it: too close to an HQ, an objective, another FOB or an enemy contact, or on the enemy half), ^ too steep, T many buildings, b a few buildings, f forest, . open. road: = main road, - minor road, _ none.
A FOB is built by a transport truck with an escort: it needs a road (= or -) within about 60 m, flat open ground ('.' or 'b' beside a road; not a town centre T, not forest f), and room. Good spots shorten the way to the intent of the base (covering a flank, supporting an objective), stay away from known enemy contacts, sit on slightly higher ground when it costs nothing, and are closer to our units than to the enemy. "toward" is the front objective position, "own" lists our HQ / FOBs in this area.
If no sub-cell is good, decline: a bad FOB costs supplies and gets lost.
Reply with ONLY one JSON object, no prose, no code fences: {"cell":"f7","reason":"max 12 words"} or {"decline":true,"reason":"max 12 words"}. The game validates the spot and moves it to the nearest valid ground; if it refuses, you get ONE retry with the reason.
`;

/** The zoom digest as a prompt block (compact JSON; the legend is in the system prompt, so it is dropped). */
export function digestBlock(zoom) {
  if (!isObj(zoom)) return "AREA: (no digest)";
  const { legend: _legend, ...rest } = zoom;
  return "AREA " + (zoom.area ?? "?") + " (origin x0,zTop = north-west corner; positions [x,z] in metres):\n" + JSON.stringify(rest);
}

export function buildFobZoomPrompt({ req = {} } = {}) {
  const zoom = req.zoom;
  const system = ZOOM_SYSTEM;
  const intent = String(req.intent ?? "").trim();
  const head = `FOB for faction ${req.faction ?? "?"} in area ${req.area ?? zoom?.area ?? "?"}. Intent of the base: ${intent || "(none given: choose the best forward position)"}`;
  const digest = digestBlock(zoom);
  let retry = "";
  if (isObj(req.retry) && (req.retry.reason || req.retry.pos)) {
    retry = `\nRETRY (attempt ${req.attempt ?? 2} of 2): your previous pick${Array.isArray(req.retry.pos) ? " at [" + req.retry.pos.join(",") + "]" : ""} was refused by the game: ${String(req.retry.reason ?? "").slice(0, 400)}\nPick a different sub-cell that avoids that problem, or decline.`;
  }
  const user = head + "\n" + digest + retry;
  return { system, user, sections: { rules: system, request: head, digest, ...(retry ? { retry } : {}) } };
}

/** JSON object out of the reply text; throws when there is none. */
export function parseZoomReply(text) {
  const t = String(text ?? "");
  const a = t.indexOf("{"), b = t.lastIndexOf("}");
  if (a < 0 || b < a) throw new Error("no JSON object in fobzoom reply");
  const obj = JSON.parse(t.slice(a, b + 1));
  if (!isObj(obj)) throw new Error("fobzoom reply is not a JSON object");
  return obj;
}

const truthy = (v) => v === true || v === 1 || v === "1" || (typeof v === "string" && v.toLowerCase() === "true");

/** Model object -> the answer for the game. Terse forms (c = cell, d = decline, r = reason) are accepted. A cell / pos is passed through; the game checks it again. */
export function normalizeZoomReply(obj, req = {}) {
  if (!isObj(obj)) return { ok: false, error: "reply is not an object" };
  const reason = String(obj.reason ?? obj.r ?? "").trim().slice(0, 120);
  if (truthy(obj.decline) || truthy(obj.d) || String(obj.cell ?? obj.c ?? "").toLowerCase() === "none") return { ok: true, decline: true, reason };
  const cellRaw = obj.cell ?? obj.c ?? obj.subcell;
  if (typeof cellRaw === "string" && /^[a-pA-P]\d{1,2}$/.test(cellRaw.trim())) {
    return { ok: true, cell: cellRaw.trim().toLowerCase(), reason }; // an id outside the digest is refused by the game (it asks for the retry with its own text)
  }
  const p = obj.pos ?? obj.p;
  if (Array.isArray(p) && p.length >= 2 && isNum(Number(p[0])) && isNum(Number(p[1]))) return { ok: true, pos: [Math.round(Number(p[0])), Math.round(Number(p[1]))], reason };
  return { ok: false, error: "reply names no cell or pos" };
}

/** All sub-cells of a zoom digest. Rows are north -> south, 3 glyphs per sub-cell. */
export function zoomCells(zoom) {
  if (!isObj(zoom) || !Array.isArray(zoom.rows) || typeof zoom.cols !== "string") return [];
  const sub = isNum(zoom.sub) ? zoom.sub : 100, out = [];
  zoom.rows.forEach((row, r) => {
    for (let c = 0; c < zoom.cols.length; c++) {
      const g = String(row).slice(c * 3, c * 3 + 3);
      if (g.length < 3) continue;
      out.push({ id: zoom.cols[c] + (r + 1), col: c, row: r, elev: Number(g[0]), terrain: g[1], road: g[2],
        x: (zoom.x0 ?? 0) + (c + 0.5) * sub, z: (zoom.zTop ?? 0) - (r + 0.5) * sub });
    }
  });
  return out;
}

/** Fake provider: the usable sub-cell (not water / keep-out / steep / town / forest) with a road that is nearest to `toward`, ties to higher ground; decline when none. */
export function fakeZoomPick(req = {}) {
  const zoom = req.zoom, cells = zoomCells(zoom);
  const toward = Array.isArray(zoom?.toward) && zoom.toward.length >= 2 ? zoom.toward : null;
  const usable = cells.filter((c) => ".b".includes(c.terrain) && c.road !== "_");
  if (!usable.length) return { decline: true, reason: "fake provider: no usable ground beside a road in this area" };
  const d = (c) => (toward ? Math.hypot(c.x - toward[0], c.z - toward[1]) : 0);
  usable.sort((a, b) => d(a) - d(b) || b.elev - a.elev || a.id.localeCompare(b.id));
  return { cell: usable[0].id, reason: "fake provider: open ground beside a road toward the front" };
}

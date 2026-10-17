// Fixtures of the M4.6 FOB tool: the macro picture (`fobMap` of the situation) and the zoom digest (POST /fobzoom), in the exact shape the game's
// CLW_MapDigest produces (addon/Scripts/Game/CLW_MapDigest.c). Synthetic map: 10 x 10 macro cells of 1300 m (a sea strip in the south-east, a forest block,
// hills in the north, a main road along row 5), 12 x 12 sub-cells per zoom. Used by agent/test_fobzoom.mjs and test-fobzoom.mjs.

export const MACRO_LEGEND = "cell id = column letter + row number, row 1 = north, column A = west. Each cell is 2 glyphs, terrain then road. terrain: ~ sea/lake, c coast, T town/city, v village, M mountains, h hills, f forest, . open. road: = main road, - minor road, _ none.";
export const ZOOM_LEGEND = "sub-cell id = column letter + row number, row 1 = north, column a = west, 100 m each. Each sub-cell is 3 glyphs: elevation digit (0 lowest .. 9 highest of this area), terrain, road. terrain: ~ water, x keep-out (game rule: too close to an HQ, an objective, another FOB or an enemy contact, or on the enemy half: cannot be used), ^ too steep, T many buildings, b a few buildings, f forest, . open. road: = main, - minor, _ none.";

/** The situation block `fobMap`. Own HQ in B9 (south-west), enemy HQ in I2 (north-east), front objective "C" in E5. */
export function macroFixture(over = {}) {
  const cols = "ABCDEFGHIJ", n = 10, rows = [];
  for (let r = 1; r <= n; r++) {
    let line = "";
    for (let c = 0; c < cols.length; c++) {
      let t = ".", road = "_";
      if (r <= 2 && c >= 3 && c <= 6) t = "h";
      if (r >= 3 && r <= 4 && c >= 6 && c <= 7) t = "f";
      if (r >= 8 && c >= 7) t = r === 8 && c === 7 ? "c" : "~";
      if (r === 5) road = "=";
      if (c === 4 && r >= 5) road = r === 5 ? "=" : "-";
      if (r === 5 && c === 4) t = "v";
      if (r === 9 && c === 1) t = "T";
      line += t + road;
    }
    rows.push(line);
  }
  return {
    cell: 1300, x0: 0, zTop: 13000, cols, n, legend: MACRO_LEGEND, rows,
    towns: ["Harbor B9", "Morton E5"], pois: ["Airfield C7 (base) C7"],
    objs: [["A", "Harbor", "B9", "US", 0], ["B", "Mill", "D7", "none", 0], ["C", "Morton", "E5", "none", 1], ["D", "Ridge", "G3", "USSR", 0], ["E", "Quarry", "I2", "USSR", 0]],
    hq: { own: "B9", enemy: "I2" }, front: "C", frontCell: "E5", fobs: [], contacts: [["F4", 3, "inf"]],
    ...over,
  };
}

/** The zoom digest of area E5: 12 x 12 sub-cells of 100 m, north-west corner (5200, 7800). */
export function zoomFixture(over = {}) {
  const cols = "abcdefghijkl", n = 12, rows = [];
  for (let r = 0; r < n; r++) {
    let line = "";
    for (let c = 0; c < n; c++) {
      const elev = Math.min(9, Math.max(0, Math.round((r + (11 - c)) / 22 * 9)));
      let t = ".", road = "_";
      if (r >= 8 && c >= 9) t = "~";
      if (r <= 1) t = "x"; // keep-out near the objective
      if (r === 2 && c <= 3) t = "x";
      if (r >= 4 && r <= 6 && c >= 4 && c <= 6) t = c === 5 && r === 5 ? "T" : "b";
      if (r === 3 && c >= 8) t = "f";
      if (c === 0 && r >= 5) t = "^";
      if (r === 7) road = "=";
      if (c === 10 && r >= 3) road = "-";
      line += elev + t + road;
    }
    rows.push(line);
  }
  return {
    area: "E5", x0: 5200, zTop: 7800, sub: 100, cols, n, elev: [31, 118], legend: ZOOM_LEGEND, rows,
    pois: [["Morton", "name_village", "f6"]],
    objs: [["C", "Morton", "f6", 5750, 7250, 60, "none"]],
    contacts: [["j4", "infantry", 3, 15, 6120, 7390]],
    own: [["HQ", "b9"]],
    toward: [5750, 7250], towardId: "C",
    ...over,
  };
}

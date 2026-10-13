// Generates the mod stringtable files from dev/i18n/CLW.keys.txt (single source of truth for EN + DE):
//   addon/Language/CLW.st             string table source (String Editor)
//   addon/Language/CLW.en_us.conf     runtime string table, English (what the game loads)
//   addon/Language/CLW.de_de.conf     runtime string table, German
// Usage:  node dev/i18n/gen-stringtable.mjs          (writes the three files; prints the key count)
//         node dev/i18n/gen-stringtable.mjs --check  (only validates: duplicates, empty texts, %n consistency EN vs DE)
// Keys file: one key per line, "KEY ::: English ::: German" (KEY without the leading '#'). '%1..%9' are params, '%%' a percent sign.
// More languages later: add a column here (LANGS) and one more runtime conf + LanguageDefinition in addon.gproj.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..");
const LANGS = [
  { code: "en_us", column: 1 },
  { code: "de_de", column: 2 },
];

const rows = [];
const seen = new Set();
for (const raw of readFileSync(join(here, "CLW.keys.txt"), "utf8").split(/\r?\n/)) {
  const line = raw.trim();
  if (!line || line.startsWith("//")) continue;
  const parts = line.split(" ::: ");
  if (parts.length !== 3) throw new Error("bad line (need KEY ::: EN ::: DE): " + line);
  const [key, en, de] = parts.map((p) => p.trim());
  if (!/^CLW-[A-Za-z0-9]+_[A-Za-z0-9_]+$/.test(key)) throw new Error("bad key: " + key);
  if (seen.has(key)) throw new Error("duplicate key: " + key);
  if (!en || !de) throw new Error("empty text: " + key);
  const params = (t) => [...new Set(t.match(/%\d/g) || [])].sort().join(",");
  if (params(en) !== params(de)) throw new Error("param mismatch EN/DE: " + key + " (" + params(en) + " vs " + params(de) + ")");
  seen.add(key);
  rows.push([key, en, de]);
}

// map marker labels: one key per size combination (a map marker text cannot carry params), see CLW_ContactMarkers.MarkerText
const markerVeh = { en: ["", "vehicle", "veh section", "veh platoon", "mech force"], de: ["", "Fahrzeug", "Fz-Gruppe", "Fz-Zug", "mech. Kräfte"] };
const markerInf = { en: ["", "few inf", "fire team", "squad", "platoon", "company"], de: ["", "wenige Inf", "Feuertrupp", "Trupp", "Zug", "Kompanie"] };
for (let v = 0; v < markerVeh.en.length; v++) {
  for (let i = 0; i < markerInf.en.length; i++) {
    if (v === 0 && i === 0) continue;
    const join2 = (a, b) => [a[v], b[i]].filter(Boolean).join(" + ");
    rows.push([`CLW-Marker_V${v}_I${i}`, join2(markerVeh.en, markerInf.en), join2(markerVeh.de, markerInf.de)]);
  }
}

// vanilla runtime tables are sorted case-insensitively (checked in localization.de_de.conf): keep that order
rows.sort((a, b) => {
  const x = a[0].toLowerCase();
  const y = b[0].toLowerCase();
  return x < y ? -1 : x > y ? 1 : a[0] < b[0] ? -1 : 1;
});
console.log(rows.length + " keys");
if (process.argv.includes("--check")) process.exit(0);

const q = (t) => '"' + t.replace(/\\/g, "\\\\").replace(/"/g, '\\"') + '"';
const outDir = join(root, "addon", "Language");
mkdirSync(outDir, { recursive: true });

// string table source. [verify] the item layout was written from the CustomStringTableItem script class (Target_<lang> members) and
// the wiki; open the file once in the String Editor (and "Build Runtime Table") to confirm, the runtime confs below are authoritative.
let st = "StringTable {\n Items {\n";
for (const [key, en, de] of rows) {
  st += `  CustomStringTableItem ${key} {\n   Target_en_us ${q(en)}\n   Target_de_de ${q(de)}\n  }\n`;
}
st += " }\n}\n";
writeFileSync(join(outDir, "CLW.st"), st);

// runtime string tables: format verified against the vanilla Language/localization.<lang>.conf (parallel Ids / Texts arrays)
for (const lang of LANGS) {
  let conf = "StringTableRuntime {\n Ids {\n";
  for (const r of rows) conf += `  ${q(r[0])}\n`;
  conf += " }\n Texts {\n";
  for (const r of rows) conf += `  ${q(r[lang.column])}\n`;
  conf += " }\n}\n";
  writeFileSync(join(outDir, `CLW.${lang.code}.conf`), conf);
}
console.log("written to " + outDir);

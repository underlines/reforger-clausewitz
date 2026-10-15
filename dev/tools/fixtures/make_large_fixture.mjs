// Generates the prompt budget fixtures (deterministic, synthetic but shaped like a real RHS_USAF vs RHS_AFRF run, protocol v4):
//   node dev/tools/fixtures/make_large_fixture.mjs   ->   large_situation.json + large_manifest.json (next to this file)
// Replace large_situation.json with a real recording (env CLW_RECORD_DIR on the sidecar writes <faction>.json) to refresh the budgets against a live run;
// the budget test (test-prompt-budget.mjs) only needs the shape, not these exact values.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const DIR = path.dirname(fileURLToPath(import.meta.url));
let seed = 20261015;
const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296; };
const ri = (a, b) => Math.floor(a + rnd() * (b - a + 1));
const pick = (a) => a[Math.floor(rnd() * a.length)];
const F = "RHS_USAF", E = "RHS_AFRF";
const gridOf = (x, z) => String(Math.floor(x / 100)).padStart(3, "0") + "-" + String(Math.floor(z / 100)).padStart(3, "0");

// ---- map: 8 objectives on a diagonal from our HQ (1200, 1500) to theirs (9600, 9300)
const HQ = [1200, 1500], EHQ = [9600, 9300];
const names = ["Morton", "Harbor Bridge", "Chapel Hill", "Saint Philippe", "Le Moule", "Meaux", "Montignac", "Gravette"];
const owners = [F, F, F, "none", "none", E, E, E];
const objectives = names.map((name, i) => {
  const t = (i + 1) / (names.length + 1), side = ["left", "centre", "right"][i % 3];
  const x = Math.round(HQ[0] + (EHQ[0] - HQ[0]) * t + ((i % 3) - 1) * 520), z = Math.round(HQ[1] + (EHQ[1] - HQ[1]) * t - ((i % 3) - 1) * 380);
  const own = owners[i];
  return { id: String.fromCharCode(65 + i), name, pos: [x, z], grid: gridOf(x, z), radius: 60 + (i % 3) * 20, value: 1 + (i % 3), owner: own, contested: i === 3 || i === 5,
    progress: own === F ? 100 : own === "none" ? (i === 3 ? 38 : 0) : i === 5 ? -62 : -100, side };
});

// ---- catalog: 70 entries with descriptions (RHS-like type names)
const kinds = [["infantry", ["Rifle squad", "Fire team", "AT team", "AA team", "MG team", "Sniper team", "Medic team", "Engineer squad", "Marksman team", "Grenadier squad"], [45, 120], 9],
  ["recon", ["Recon team", "Scout team", "Forward observer"], [60, 110], 4],
  ["light", ["M1025 HMG", "M1025 MK19", "M1025 TOW", "M1165 HMG", "M1151 HMG", "M1151 MK19", "MRAP RG-33", "MaxxPro", "Cougar HMG", "LSSV"], [140, 260], 3],
  ["motorized", ["M939 transport", "M35 transport", "M977 HEMTT", "M1083 FMTV", "Supply truck", "M998 troop carrier", "Ural-style truck", "UTV rifle team", "ATV scout", "Fuel truck"], [90, 200], 10],
  ["mech", ["M113A3", "M2A3 Bradley", "M2A2 ODS", "M1126 Stryker", "M1127 Stryker RV", "M1128 MGS", "M1134 ATGM", "M1130 CV", "M1131 FSV", "M1135 NBCRV"], [260, 520], 8],
  ["armor", ["M1A1 Abrams", "M1A2 SEP", "M1A2 TUSK", "M60A3", "M551 Sheridan", "M1117 ASV", "LAV-25", "LAV-AT", "AAVP-7", "M1128 Assault"], [520, 900], 4],
  ["infantry", ["Mortar team", "Javelin team", "Stinger team", "JTAC team", "EOD team", "Combat engineers", "Heavy weapons squad"], [80, 150], 6],
  ["armored_recon", ["M3A3 Bradley", "M1127 Stryker", "LAV-R", "AH-6 scout", "Cougar CLS", "HMMWV scout", "M706 Commando", "ASV recon", "MRZR scout", "Polaris DAGOR"], [200, 340], 3]];
const catalog = [];
for (const [kind, ts, [lo, hi], seats] of kinds) for (const t of ts) {
  catalog.push({ type: `RHS_USAF_${t.replace(/[^A-Za-z0-9]+/g, "_")}`, kind, cost: Math.round(ri(lo, hi) / 5) * 5,
    desc: `${t} (${kind === "infantry" || kind === "recon" ? seats : "crew " + ri(2, 4) + (seats > 4 ? " + cargo " + seats : "")}), ${pick(["woodland", "desert", "OCP", "MARPAT"])} variant, ${pick(["standard loadout", "AT loadout", "support loadout", "light loadout"])}` });
}

// ---- squads: 30 (incl. 3 garrison, 2 reserved, 2 human-led, vehicles)
const objById = Object.fromEntries(objectives.map((o) => [o.id, o]));
const cmdFor = (o) => pick(["ATTACK", "DEFEND", "MOVE", "RECON"]) + "|" + o;
const squads = [];
const vehTypes = { light: "M1025 HMG", motorized: "M939 transport", mech: "M2A3 Bradley", armor: "M1A2 SEP", armored_recon: "M3A3 Bradley" };
const kindList = ["infantry", "infantry", "infantry", "infantry", "recon", "light", "motorized", "mech", "armor", "armored_recon"];
for (let i = 0; i < 30; i++) {
  const kind = i < 14 ? "infantry" : kindList[i % kindList.length];
  const a = i < 14 ? 9 : kind === "recon" ? 4 : 3 + (i % 3), start = a + (i % 4 === 0 ? 2 : 0);
  const garrison = i >= 27, human = i === 1 || i === 2, reserved = i === 20 ? "FOB_2" : i === 21 ? "FOB_2" : undefined;
  const o = String.fromCharCode(65 + (i % 8)), obj = objById[o];
  const [cmd] = cmdFor(o).split("|");
  const base = garrison ? HQ : [obj.pos[0] + ri(-400, 200), obj.pos[1] + ri(-300, 300)];
  const veh = kind === "infantry" || kind === "recon" ? null : { type: vehTypes[kind] ?? "M1025 HMG", health: Math.round(rnd() * 40 + 60) / 100, mounted: rnd() > 0.2, state: pick(["driving", "stopped", "driving"]) };
  const stateText = garrison ? "idle" : i % 5 === 0 ? "engaged" : pick(["moving", "attacking", "defending", "moving"]);
  const sq = { id: `${F}_${["Inf", "Rec", "Lgt", "Mot", "Mch", "Arm"][i % 6]}${i + 1}`, name: `${["Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel"][i % 8]} ${Math.floor(i / 8) + 1}-${(i % 4) + 1}`,
    kind, type: veh ? veh.type.replace(/ /g, "_") : kind === "recon" ? "RHS_USAF_Recon_Team" : `RHS_USAF_${pick(["Rifle_Squad", "AT_Team", "MG_Team", "Fire_Team"])}`,
    pos: [base[0], Math.round(rnd() * 90 + 5), base[1]], agents: a, startAgents: start, vehicle: veh, humanLed: human, wounded: Math.max(0, start - a) + (i % 7 === 0 ? 1 : 0),
    health: Math.round((0.5 + rnd() * 0.5) * 100) / 100, speed: stateText === "moving" ? Math.round(rnd() * 70) / 10 : 0, heading: ri(0, 359), state: stateText, dormant: i % 6 === 3 };
  if (reserved) sq.reserved = reserved;
  if (garrison) sq.garrison = true;
  if (!garrison && i % 9 === 8) sq.idleSec = ri(60, 400);
  sq.objective = garrison || (i % 9 === 8) ? null : { command: cmd, target: o, pos: obj.pos, reason: pick(["Seize the crossing before dusk", "Hold the line, armor support inbound", "Probe the flank, report contacts", "Reinforce the contested point", "Cut the road north"]), ageSec: ri(20, 600), status: pick(["active", "active", "active", "pending"]) };
  sq.mission = human ? { state: i === 1 ? "accepted" : "offered", kind: "CAPTURE", target: "D", brief: "Squad leader, seize Saint Philippe from the east and report." } : null;
  squads.push(sq);
}

// ---- contacts: 25 (3 stale beyond 600 s), freshest first like the game
const places = ["near Morton", "north of Harbor Bridge", "at Chapel Hill", "between Saint Philippe and Le Moule", "west of Meaux", "near Montignac", "south of Gravette", "unlocated"];
const sizes = ["a fire team", "a squad-sized element", "a platoon-sized element", "a single vehicle", "a vehicle section", "a large mechanized force with dismounts"];
const contacts = [];
for (let i = 0; i < 25; i++) {
  const o = objectives[3 + (i % 5)], x = o.pos[0] + ri(-500, 500), z = o.pos[1] + ri(-500, 500), veh = i % 3 === 0 ? ri(1, 4) : 0, inf = i % 3 === 0 ? ri(0, 6) : ri(2, 14);
  contacts.push({ pos: [x, Math.round(rnd() * 80), z], size: sizes[(i + (veh ? 3 : 0)) % sizes.length], kind: veh ? "vehicle" : "infantry", vehicles: veh, infantry: inf, place: places[i % places.length], grid: gridOf(x, z), ageSec: i < 22 ? i * 17 + ri(0, 10) : 700 + i * 20 });
}

const fobs = [
  { id: "FOB_1", site: "S2", name: "Pine Ridge", pos: [3900, 4300], grid: gridOf(3900, 4300), state: "active", build: 100, tier: 1, services: ["spawn", "command", "cache", "arsenal"], underAttack: true, stock: 260 },
  { id: "FOB_2", site: "S3", name: "Quarry Road", pos: [5200, 5600], grid: gridOf(5200, 5600), state: "building", build: 45, tier: 1, services: ["spawn"], underAttack: false, stock: 0, builder: squads[20].id },
];
const fobSites = [
  { id: "S4", name: "Marais Crossing", pos: [6100, 6400], grid: gridOf(6100, 6400), place: "near Saint Philippe", distFromHq: 6900, front: "D", toFront: 420 },
  { id: "S5", name: "Rue du Moulin", pos: [5800, 6900], grid: gridOf(5800, 6900), place: "north of Saint Philippe", distFromHq: 7100, front: "D", toFront: 640 },
  { id: "S6", name: "Les Hauts", pos: [6500, 5800], grid: gridOf(6500, 5800), place: "east of Chapel Hill", distFromHq: 6700, front: "D", toFront: 710 },
  { id: "S7", name: "Pont Rouge", pos: [5400, 7300], grid: gridOf(5400, 7300), place: "near Le Moule", distFromHq: 7500, front: "D", toFront: 980 },
];
const helpCalls = [
  { id: 4, player: "Underlines", squad: squads[1].id, grid: "058-072", pos: [5800, 7200], note: "taking fire from the treeline", ageSec: 45, answeredBy: "" },
  { id: 3, player: "Corporal_K", squad: squads[2].id, grid: "049-061", pos: [4900, 6100], note: "need AT, armor ahead", ageSec: 210, answeredBy: squads[14].id },
];
const requests = [
  { id: 17, player: "Underlines", role: "leader", kind: "VEHICLE", args: "M1025 HMG", status: "granted", reason: "", ageSec: 120 },
  { id: 16, player: "Corporal_K", role: "soldier", kind: "SQUAD", args: "", status: "refused", reason: "soldiers cannot request squads", ageSec: 300 },
  { id: 15, player: "Underlines", role: "leader", kind: "RESUPPLY", args: "ammo", status: "granted", reason: "", ageSec: 520 },
  { id: 14, player: "Sgt_Moreau", role: "leader", kind: "REINFORCE", args: "2 squads", status: "refused", reason: "supplies below reserve", ageSec: 800 },
  { id: 13, player: "Sgt_Moreau", role: "leader", kind: "RECON", args: "Gravette", status: "granted", reason: "", ageSec: 1100 },
];

const sit = {
  v: 4, faction: F, side: 0, identity: "BLUFOR", time: 2710, timeLeftSec: 4180,
  war: { tickets: 412, enemyTickets: 365, supplies: 640, supplyTotal: 900, income: 58, liveAI: 148, aiCap: 220, vehicles: 14, vehicleCap: 24, engineAI: { active: 470, limit: 600, headroom: 84 },
    mode: { id: "conquest", name: "Conquest", params: { timeLimitMin: 90, startTickets: 600, startSupplies: 400, incomeMultiplier: 1, battleSize: "large", ticketBleed: true, fobMax: 2, idleDespawnMin: 6, garrisonSquads: 3, convoyMax: 2 } },
    phase: "running", sides: [{ faction: F, identity: "BLUFOR" }, { faction: E, identity: "OPFOR" }] },
  objectives,
  bases: [{ id: "HQ", faction: F, pos: HQ, stock: 640 }, { id: "HQ", faction: E, pos: EHQ }],
  catalog, squads, contacts,
  events: ["objective D contested", `${F}_Inf5 took casualties (3)`, "mission offered to RHS_USAF_Inf2: CAPTURE D", "FOB_2 construction 45%", `${F}_Arm9 destroyed a BMP-2 near Chapel Hill`, "convoy CV_3 loading at HQ", "enemy air activity reported north", `${F}_Rec5 lost`],
  fobs, fobSites,
  fobRules: { max: 2, live: 1, inProgress: 1, cost: 300, upgradeCost: 150, build: "convoy", buildSec: 240, logistics: true, earliestSec: 300, elapsedSec: 2710, ready: false, readyReason: "a FOB is already in progress" },
  baseRules: { garrisonSquads: 3, garrison: 3, idleDespawnMin: 6, recentlyDespawned: 2, hqReserve: 50, lastDespawn: { squad: `${F}_Inf11`, kind: "infantry", refund: 45, agoSec: 380 } },
  convoys: [{ id: "CV_3", from: "HQ", to: "FOB_2", cargo: 400, state: "loading", pos: HQ, grid: gridOf(...HQ), truck: squads[21].id }, { id: "CV_2", from: "HQ", to: "FOB_1", cargo: 400, state: "delivered", pos: [3900, 4300], grid: gridOf(3900, 4300) }],
  logisticsRules: { convoyMax: 2, inFlight: 1, minCargo: 40, hqReserve: 50, lowStock: 120, truckCargo: 400 },
  requests,
  structures: Array.from({ length: 24 }, (_, i) => ({ base: i < 12 ? "HQ" : i < 20 ? "FOB_1" : "FOB_2", kind: pick(["arsenal", "spawn", "cache", "command", "depot"]), label: `Structure ${i + 1}`, pos: [HQ[0] + i * 7, HQ[1] + (i % 5) * 9], grid: gridOf(HQ[0], HQ[1]) })),
  env: { time: "19:40", date: "2026-10-15", daySpeed: 6, daylight: "dusk", sunrise: "06:12", sunset: "19:55", weather: "Overcast", next: "Rain", nextInMin: 18, plan: ["Overcast", "Rain", "Rain", "Clear"] },
  hq: { trait: "aggressive" },
  helpCalls,
};

// ---- manifest: orders + buy + missions + fob/logistics + player intents (the last channel never reaches the prompts)
const P = (name, type, required, hint) => ({ name, type, required, hint });
const orderParams = [P("squad", "unit", true, "squad id from the situation, humanLed=false, one order per squad"), P("target", "string", false, "objective id from the situation"), P("pos", "position", false, "[x, z]; alternative to target"), P("reason", "string", false, "short text")];
const ent = (name, channel, desc, params, tags = []) => ({ name, layer: "intent", channel, authority: channel === "player" ? "player" : "hq", cost: 0, tags, requires: [], desc, params });
const manifest = {
  hash: "large-1", faction: F, mode: "conquest", capabilities: ["fob", "logistics", "requests"], builtin: false, primitives: [],
  intents: [
    ent("MOVE", "orders", "Redeploy the squad to the objective or position.", orderParams, ["movement"]),
    ent("ATTACK", "orders", "Assault the objective or position (on an objective = capture it).", orderParams, ["offence"]),
    ent("DEFEND", "orders", "Hold the objective or position.", orderParams, ["defence"]),
    ent("RECON", "orders", "Go there and observe, no assault.", orderParams, ["recon"]),
    ent("FALL_BACK", "orders", "Retreat. No target = own base.", orderParams, ["defence"]),
    ent("BUY", "buy", "Buy new units from the catalog for a concrete assignment (max 3 entries per reply, total cost within war.supplies). Every entry names the order its units get when they arrive (command + target or pos): units are not bought to idle at the base. Idle squads at the base are stood down after war.mode.params.idleDespawnMin minutes and refunded.",
      [P("type", "string", true, "catalog type from the situation"), P("count", "int", false, "1 to 3"), P("command", "string", true, "REQUIRED assignment of the new units: MOVE, ATTACK, DEFEND or RECON (they get this order the moment they are ready; nothing is bought to wait at the base)"), P("target", "string", false, "objective id from the situation for that order"), P("pos", "position", false, "[x, z]; alternative to target"), P("reason", "string", false, "short text")], ["economy"]),
    ent("OFFER_MISSION", "missions", "Offer up to 3 missions to a human-led squad (humanLed=true): each offer has kind CAPTURE|DEFEND|RECON|DESTROY, a target (objective id) or pos [x, z], and a brief (max 240 chars, in the voice of HQ). Skip squads whose mission.state is accepted.", [P("squad", "unit", true, "human-led squad id from the situation")], ["missions"]),
    ent("ESTABLISH_FOB", "fob", "Establish a forward operating base (FOB) at a candidate site: pays the FOB cost, a transport with an escort drives out and builds it. A FOB gives a spawn point, a command post, a supply cache and an arsenal near the front. Only one FOB may be in progress per side, max war.mode.params.fobMax per side. Choose a site from fobSites; check fobRules for the cost and your supplies.", [P("site", "string", true, "site id from fobSites in the situation"), P("reason", "string", false, "short text")], ["fob"]),
    ent("UPGRADE_FOB", "fob", "Add a vehicle depot to an active FOB (tier 2) for half the FOB cost (fobRules.upgradeCost).", [P("fob", "string", true, "id of an active FOB (state active, tier 1) from fobs in the situation"), P("reason", "string", false, "short text")], ["fob"]),
    ent("RESUPPLY", "fob", "Send a supply truck convoy from the HQ to a FOB: the cargo is added to the FOB stock on arrival.", [P("fob", "string", true, "id of an active FOB from fobs in the situation"), P("reason", "string", false, "short text")], ["logistics"]),
    ent("CONVOY", "fob", "Move supply stock between two bases with a truck convoy.", [P("origin", "base", true, "origin base id (HQ or a FOB id)"), P("dest", "base", true, "destination base id"), P("reason", "string", false, "short text")], ["logistics"]),
    ent("REQUEST_VEHICLE", "player", "A player asks for a vehicle at his position.", [P("vehicle", "string", true, "vehicle type")], ["request"]),
    ent("REQUEST_BACKUP", "player", "A player calls for urgent help.", [P("note", "string", false, "short text")], ["request"]),
    ent("REPORT_CONTACT", "player", "A player reports an enemy sighting.", [P("kind", "string", true, "inf|veh|armor|air"), P("distance", "int", true, "metres"), P("direction", "string", true, "N..NW")], ["report"]),
  ],
};

fs.writeFileSync(path.join(DIR, "large_situation.json"), JSON.stringify(sit, null, 1) + "\n");
fs.writeFileSync(path.join(DIR, "large_manifest.json"), JSON.stringify(manifest, null, 1) + "\n");
console.log(`large_situation.json ${JSON.stringify(sit).length} chars (${squads.length} squads, ${contacts.length} contacts, ${catalog.length} catalog, ${objectives.length} objectives); large_manifest.json ${manifest.intents.length} intents`);

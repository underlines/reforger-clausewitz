// Dashboard fixture: POSTs a synthetic protocol-v2 situation for two factions (US vs USSR) to the sidecar's /orders, as the game would.
// Includes active and dormant squads (dormant: true|false), vehicle units, a human-led squad, objectives, contacts, events.
// Usage: node dev/tools/fixtures/post_situation.mjs [--once] [--url http://127.0.0.1:8788] [--no-dormant]   (default: repeat every 5 s)
const arg = (k, d) => { const i = process.argv.indexOf(k); return i < 0 ? d : process.argv[i + 1] ?? true; };
const URL_ = arg("--url", "http://127.0.0.1:8788"), ONCE = process.argv.includes("--once"), NO_DORM = process.argv.includes("--no-dormant");
const t0 = Date.now();
const params = { timeLimitMin: 90, startTickets: 600, startSupplies: 400, incomeMultiplier: 1.0, battleSize: "medium", ticketBleed: true };
const objectives = (side) => [
  { id: "A", name: "Camp Rhein", pos: [820, 900], radius: 80, value: 1, owner: "US", contested: false, progress: 100 },
  { id: "B", name: "Highway Crossing", pos: [1500, 1400], radius: 60, value: 2, owner: "US", contested: true, progress: 35 },
  { id: "C", name: "Old Mill", pos: [2100, 1900], radius: 70, value: 2, owner: "none", contested: false, progress: 0 },
  { id: "D", name: "Radar Hill", pos: [2700, 2300], radius: 90, value: 3, owner: "USSR", contested: false, progress: -100 },
  { id: "E", name: "Depot Nord", pos: [3300, 2900], radius: 80, value: 1, owner: "USSR", contested: false, progress: -100 },
].map((o) => (side === 0 ? o : { ...o, progress: -o.progress }));
const catalog = (f) => (f === "US"
  ? [{ type: "Rifle squad", kind: "infantry", cost: 60, desc: "Rifle squad (9)" }, { type: "AT team", kind: "infantry", cost: 90, desc: "AT team (4)" }, { type: "M998 HMG", kind: "light", cost: 140, desc: "Humvee with M2 (3 seats)" }, { type: "M113 APC", kind: "mech", cost: 260, desc: "APC (2 + 8)" }, { type: "M1A2", kind: "armor", cost: 520, desc: "Main battle tank (4)" }]
  : [{ type: "Rifle squad", kind: "infantry", cost: 55, desc: "Rifle squad (8)" }, { type: "Ural truck", kind: "motorized", cost: 120, desc: "Truck with squad (1 + 8)" }, { type: "BTR-70", kind: "mech", cost: 240, desc: "APC (2 + 8)" }, { type: "T-72", kind: "armor", cost: 500, desc: "Main battle tank (3)" }]);
const sq = (id, kind, type, x, z, a, s, dorm, extra = {}) => ({ id, kind, type, pos: [x, 0, z], agents: a, startAgents: s, vehicle: null, humanLed: false, wounded: Math.max(0, s - a) , health: a >= s ? 1 : 0.7, speed: dorm ? 0 : 1.4, heading: 60, state: dorm ? "idle" : "moving", dormant: NO_DORM ? undefined : dorm, objective: null, mission: null, ...extra });
const obj = (command, target, x, z, reason, status = "active") => ({ command, target, pos: [x, z], reason, ageSec: 40, status });
const veh = (type, mounted = true) => ({ type, health: 0.9, mounted, state: "driving" });
const squads = (f, wob) => (f === "US" ? [
  sq("US_Squad1", "infantry", "Rifle squad", 900 + wob, 950, 9, 9, false, { humanLed: true, state: "moving", mission: { state: "offered", kind: "CAPTURE", target: "B" }, objective: obj("MOVE", "B", 1500, 1400, "Seize the highway crossing") }),
  sq("US_Squad2", "infantry", "Rifle squad", 1380 + wob, 1320, 7, 9, false, { state: "engaged", objective: obj("ATTACK", "B", 1500, 1400, "Support the capture") }),
  sq("US_AT1", "infantry", "AT team", 1200, 1100, 4, 4, true, { objective: obj("DEFEND", "A", 820, 900, "Hold camp") }),
  sq("US_Hmv1", "light", "M998 HMG", 1650 + wob, 1500, 3, 3, false, { vehicle: veh("M998 HMG"), speed: 7, objective: obj("RECON", null, 2000, 1800, "Recon the mill") }),
  sq("US_Apc1", "mech", "M113 APC", 760, 880, 10, 10, true, { vehicle: veh("M113 APC", false), objective: obj("DEFEND", "A", 820, 900, "Reserve at base") }),
  sq("US_Tank1", "armor", "M1A2", 1100, 1000, 4, 4, true, { vehicle: veh("M1A2"), objective: obj("MOVE", "C", 2100, 1900, "Advance on the mill", "pending") }),
] : [
  sq("USSR_Squad1", "infantry", "Rifle squad", 2650, 2250, 8, 8, true, { objective: obj("DEFEND", "D", 2700, 2300, "Hold the radar hill") }),
  sq("USSR_Squad2", "infantry", "Rifle squad", 1750 - wob, 1620, 5, 8, false, { state: "engaged", wounded: 3, health: 0.55, objective: obj("ATTACK", "B", 1500, 1400, "Retake the crossing") }),
  sq("USSR_Truck1", "motorized", "Ural truck", 2400, 2100, 9, 9, false, { vehicle: veh("Ural truck"), speed: 8, objective: obj("MOVE", "B", 1500, 1400, "Reinforce B") }),
  sq("USSR_Btr1", "mech", "BTR-70", 3250, 2850, 10, 10, true, { vehicle: veh("BTR-70"), objective: obj("DEFEND", "E", 3300, 2900, "Guard the depot") }),
  sq("USSR_Tank1", "armor", "T-72", 2800, 2380, 3, 3, true, { vehicle: veh("T-72"), objective: obj("DEFEND", "D", 2700, 2300, "Hull-down on the hill") }),
] );
const post = async (f, side, n) => {
  const wob = Math.round(60 * Math.sin(n / 3));
  const other = f === "US" ? "USSR" : "US";
  const body = {
    v: 2, faction: f, side, identity: side === 0 ? "BLUFOR" : "OPFOR", time: 1260 + Math.round((Date.now() - t0) / 1000), timeLeftSec: 4140 - Math.round((Date.now() - t0) / 1000),
    war: { tickets: side === 0 ? 512 : 438, enemyTickets: side === 0 ? 438 : 512, supplies: side === 0 ? 310 : 180, income: side === 0 ? 42 : 36, liveAI: side === 0 ? 37 : 35, aiCap: 120, vehicles: side === 0 ? 4 : 3, vehicleCap: 12,
      mode: { id: "conquest", name: "Conquest", params }, phase: "running", sides: [{ faction: "US", identity: "BLUFOR" }, { faction: "USSR", identity: "OPFOR" }] },
    objectives: objectives(side), bases: [{ faction: "US", pos: [600, 760] }, { faction: "USSR", pos: [3500, 3100] }], catalog: catalog(f), squads: squads(f, wob),
    contacts: [{ pos: side === 0 ? [1750, 1620, 1620].slice(0, 1).concat([0, 1620]) : [1500, 0, 1380], desc: "5x " + other + " infantry", kind: "infantry", ageSec: 12 }, { pos: side === 0 ? [2700, 0, 2330] : [1100, 0, 1000], desc: "1x " + other + " vehicle", kind: "vehicle", ageSec: 140 }],
    events: n === 0 ? ["objective B contested", "USSR_Squad2 took casualties (3)", "mission offered to US_Squad1: CAPTURE B"] : [],
  };
  const r = await fetch(URL_ + "/orders", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  return r.status;
};
let n = 0;
const tick = async () => {
  try { console.log("posted", await post("US", 0, n), await post("USSR", 1, n)); } catch (e) { console.log("post failed:", e.message); }
  n++;
  if (!ONCE) setTimeout(tick, 5000);
};
tick();

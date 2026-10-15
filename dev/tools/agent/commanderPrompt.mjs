// Commander / SITREP prompt builders (extracted from llm_sidecar.mjs). Side-effect free: importable by tests, no I/O, no config access.
//   buildCommanderPrompt({config, sit, manifest, journal}) -> {system, user, sections, spec}
//   buildSitrepPrompt({config, sit, orders})               -> {system, user, sections}
//   buildPlannerPrompt({config, sit, manifest, journal, orders, trigger}) -> {system, user, sections, spec}   (planner split: buy + FOB + logistics)
//   buildCommanderPrompt with config.planner.enabled = the ORDERS call (no buy / FOB text, PLAN block in the user message)
// `sections` = {name: text}, every part of system + user that the call sends, measurable with promptKit.sectionStats.
// Layout of the v2 commander call (stable first, so providers can cache the prefix):
//   system = rules | commands + reply spec | persona | conditional paragraphs (FOBS / LOGISTICS / HELP CALLS only when they apply)
//   user   = situation JSON | wake | environment | HQ journal    (everything that changes between calls)
// Content-agnostic: names, kinds, types and objective ids come from the situation / manifest (nothing vanilla is hard-wired).
import * as J from "./journal.mjs";
import { personaCommander, personaSitrep } from "./personality.mjs";
import { terseSpec } from "./promptKit.mjs";
// environment paragraph (WP3, sit.env): lazy so the prompts still build when ./environment.mjs is missing or broken
let envPrompt = () => "";
try { ({ envPrompt } = await import("./environment.mjs")); } catch { /* optional */ }

const isNum = (n) => typeof n === "number" && Number.isFinite(n);

export const SYSTEM = `You are the HQ commander of one faction in a tactical war simulation (Arma Reforger).
You see only what your units have reported (fog of war): the contact list may be old or incomplete.
You command squads by giving at most one order per squad. Allowed commands, and only these:
- MOVE: redeploy the squad to pos [x, z]
- ATTACK: assault pos [x, z]
- DEFEND: hold pos [x, z]
Squads with humanLed=true are led by players; avoid ordering them unless clearly useful.
Weak squads (agents well below startAgents) should not attack. Squad positions are [x, y, z]; order positions are [x, z].
Reply with ONLY a JSON object, no prose, no code fences:
{"orders":[{"squad":"<squad id from the situation>","command":"MOVE|ATTACK|DEFEND","pos":[x,z],"reason":"short text"}]}
Use an empty orders array if no action is needed.
The situation may contain a "wake" object telling you why you are being consulted now: reason first|scheduled|trigger|manual,
triggers (short descriptions of what happened since your last decision) and sinceLastSec. React to the triggers first.`;

// Protocol v2 prompt (situation field "v":2). Content-agnostic: names, kinds, types and objective ids come from the situation.
export const SYSTEM_V2 = `You are the HQ commander of one faction in a tactical war simulation (Arma Reforger). You see only what your units reported (fog of war): contacts may be old or incomplete.
GOAL: win the war. Objectives (map points) decide it: the side holding fewer/less valuable objectives loses tickets over time (bleed), and tickets are also lost with every dead soldier and destroyed vehicle. Tickets at 0 = defeat. Supplies buy new units. So: hold or take objectives, preserve your tickets, spend supplies.
DOCTRINE
- Combined arms: motorized/mech/light/armor units (fast, firepower) cover distance and support; infantry takes and holds objectives. Send recon first to unknown or fogged objectives.
- Concentrate force: 2-3 units per target, never split into penny packets. Prefer one clear plan over many small moves. Do NOT keep units idle at the base: the game keeps a small garrison there itself (squads with garrison=true, never order them), and a squad that stands at the base without an order for baseRules.idleDespawnMin minutes is stood down and refunded (baseRules.recentlyDespawned counts them). Every unit should have a job.
- AT teams (or armed vehicles) when enemy vehicles are in contacts. Weak squads (agents well below startAgents or vehicle.health below 0.4) FALL_BACK, do not attack. health and vehicle.health are fractions 0..1: 1 = undamaged / full health, 0 = destroyed.
- SPAWN ON ASSIGNMENT: buy only for a concrete need (an objective to take or defend that has too few units, a recon target) and put the assignment into the same buy entry: "command" (MOVE|ATTACK|DEFEND|RECON) plus "target" (objective id) or "pos"; the new units get that order the moment they are ready. A buy entry without an assignment is dropped. Never buy "to have reserves". Squads with idleSec (seconds without an order) need an order now; give every unordered non-garrison squad a task in your reply or accept that it will be stood down. Keep supplies ~40 in reserve and respect the caps. Respect the caps (liveAI<aiCap, vehicles<vehicleCap) and the catalog costs. Do not buy when war.engineAI.headroom is below the unit size (engine AI near its limit): the buy is refused.
- Do not re-order squads that already execute a sensible order (objective.status active, young ageSec).
SITUATION FIELDS
war: your tickets, enemyTickets, supplies, income per minute, liveAI/aiCap, vehicles/vehicleCap; timeLeftSec = time limit (absent or -1 = no limit); tickets -1 = not used by this mode. war.mode {id,name,params} holds the rules of the chosen war mode (time limit, start tickets, ...): follow them; war.phase is "running" while you are consulted. objectives: id, name, pos [x,z], radius m, value, owner ("none" or a faction key), contested, progress (-100..100 toward the capturer). bases: base positions per faction. catalog: what you may buy (type, kind, cost, desc). squads: kind (infantry|recon|motorized|light|mech|armor|armored_recon), type, pos [x,y,z], agents/startAgents, vehicle {type,health,mounted,state} or null, humanLed, objective (its current order and status), mission (for human-led squads). contacts: pos, kind, desc, ageSec. events: what happened since your last decision. wake: why you are consulted now.
`;
// The COMMANDS / REPLY part of the v2 prompt is appended by commandsSection(manifest): the game's action manifest decides what exists.

// Planner split (config planner.enabled): the commander call is the ORDERS call (orders + missions); economy, FOB and logistics text lives in PLANNER_SYSTEM.
// This variant has no buy / FOB / logistics wording at all (tests check it); the planner's last result reaches it as the PLAN block in the user message.
export const SYSTEM_ORDERS_V2 = `You are the HQ commander of one faction in a tactical war simulation (Arma Reforger). You see only what your units reported (fog of war): contacts may be old or incomplete.
GOAL: win the war. Objectives (map points) decide it: the side holding fewer/less valuable objectives loses tickets over time (bleed), and tickets are also lost with every dead soldier and destroyed vehicle. Tickets at 0 = defeat. So: hold or take objectives and preserve your tickets. A separate planner officer handles reinforcements, forward bases and supply runs; you give orders and missions to the squads that exist.
DOCTRINE
- Combined arms: motorized/mech/light/armor units (fast, firepower) cover distance and support; infantry takes and holds objectives. Send recon first to unknown or fogged objectives.
- Concentrate force: 2-3 units per target, never split into penny packets. Prefer one clear plan over many small moves. Do NOT keep units idle at the base: the game keeps a small garrison there itself (squads with garrison=true, never order them), and a squad that stands at the base without an order for baseRules.idleDespawnMin minutes is stood down and refunded (baseRules.recentlyDespawned counts them). Every unit should have a job.
- AT teams (or armed vehicles) when enemy vehicles are in contacts. Weak squads (agents well below startAgents or vehicle.health below 0.4) FALL_BACK, do not attack. health and vehicle.health are fractions 0..1: 1 = undamaged / full health, 0 = destroyed.
- Squads with idleSec (seconds without an order) need an order now; give every unordered non-garrison squad a task in your reply or accept that it will be stood down. Squads with a reserved field are busy with a task of the planner: never order them.
- The PLAN block in the user message shows what the planner has arranged lately (reinforcements with the order their units receive on arrival, and when it runs next): plan your orders around it, do not duplicate it.
- Do not re-order squads that already execute a sensible order (objective.status active, young ageSec).
SITUATION FIELDS
war: your tickets, enemyTickets, income per minute, liveAI/aiCap, vehicles/vehicleCap; timeLeftSec = time limit (absent or -1 = no limit); tickets -1 = not used by this mode. war.mode {id,name,params} holds the rules of the chosen war mode (time limit, start tickets, ...): follow them; war.phase is "running" while you are consulted. objectives: id, name, pos [x,z], radius m, value, owner ("none" or a faction key), contested, progress (-100..100 toward the capturer). bases: base positions per faction. squads: kind (infantry|recon|motorized|light|mech|armor|armored_recon), type, pos [x,y,z], agents/startAgents, vehicle {type,health,mounted,state} or null, humanLed, objective (its current order and status), mission (for human-led squads). contacts: pos, kind, desc, ageSec. events: what happened since your last decision. wake: why you are consulted now.
`;
const ORDERS_FLANK = "Consider flanks: a route off the direct axis can beat a frontal push. Each objective has side left|centre|right of the line from your HQ to the enemy HQ (seen from your HQ, facing the enemy).";

// The PLANNER call (role planner): owns buy + FOB / logistics channels. Only economy doctrine, no squad-order text.
export const PLANNER_SYSTEM = `You are the economy and logistics planner on the HQ staff of one faction in a tactical war simulation (Arma Reforger). You see only what your units reported (fog of war).
Another officer gives the squad orders (ORDERS IN FORCE are in the user message); you decide what to buy, where to build forward bases and which supply convoys to send. You do NOT give squad orders.
GOAL: win the war. Objectives (map points) decide it: the side holding fewer/less valuable objectives loses tickets over time (bleed), and tickets are also lost with every dead soldier and destroyed vehicle. Tickets at 0 = defeat. Supplies buy new units: spend them where they win or hold objectives, keep a reserve.
DOCTRINE
- SPAWN ON ASSIGNMENT: buy only for a concrete need (an objective to take or defend that has too few units in the orders in force, a recon target) and put the assignment into the same buy entry: "command" (MOVE|ATTACK|DEFEND|RECON) plus "target" (objective id) or "pos"; the new units get that order the moment they are ready. A buy entry without an assignment is dropped. Never buy "to have reserves". Squads that stand idle at the base (idleSec) mean enough units already exist: do not buy more for the same objective.
- Keep supplies ~40 in reserve and respect the caps (liveAI<aiCap, vehicles<vehicleCap) and the catalog costs. Do not buy when war.engineAI.headroom is below the unit size (engine AI near its limit): the buy is refused.
- Combined arms: infantry takes and holds objectives; motorized/mech/light/armor units cover distance and support; AT teams (or armed vehicles) when enemy vehicles are in contacts; recon for unknown objectives.
- Reply with nothing when there is nothing worth doing now: you are consulted again soon.
SITUATION FIELDS
war: your tickets, enemyTickets, supplies, income per minute, liveAI/aiCap, vehicles/vehicleCap, engineAI.headroom; timeLeftSec = time limit (absent or -1 = no limit); war.mode {id,name,params} holds the rules of the chosen war mode: follow them. objectives: id, name, pos [x,z], radius m, value, owner ("none" or a faction key), contested, progress. bases: base positions per faction. catalog: what you may buy (type, kind, cost, desc). squads: kind, type, pos [x,y,z], agents/startAgents, vehicle, humanLed, objective (its current order and status). contacts: pos, kind, desc, ageSec. events: what happened lately.
`;

// Built-in manifest: used for v2 posts and when a v3 post names a manifest hash the sidecar does not know (it then asks the game to
// resend with needManifest). Mirrors the game's Configs/CLW/Actions/Actions.conf at the time of protocol v3.
const ORDER_PARAMS = [
  { name: "squad", type: "unit", required: true, hint: "squad id from the situation, humanLed=false, one order per squad" },
  { name: "target", type: "string", required: false, hint: "objective id from the situation" },
  { name: "pos", type: "position", required: false, hint: "[x, z]; alternative to target" },
  { name: "reason", type: "string", required: false, hint: "short text" },
];
const orderEntry = (name, desc, tags) => ({ name, layer: "intent", channel: "orders", authority: "hq", cost: 0, tags, requires: [], desc, params: ORDER_PARAMS });
export const DEFAULT_MANIFEST = {
  hash: "builtin", faction: "", mode: "", capabilities: [], builtin: true,
  intents: [
    orderEntry("MOVE", "Redeploy the squad to the objective or position.", ["movement"]),
    orderEntry("ATTACK", "Assault the objective or position (on an objective = capture it).", ["offence"]),
    orderEntry("DEFEND", "Hold the objective or position.", ["defence"]),
    orderEntry("RECON", "Go there and observe, no assault.", ["recon"]),
    orderEntry("FALL_BACK", "Retreat. No target = own base.", ["defence"]),
    { name: "BUY", layer: "intent", channel: "buy", authority: "hq", cost: 0, tags: ["economy"], requires: [], desc: "Buy new units from the catalog (max 3 entries per reply, total cost within war.supplies).",
      params: [{ name: "type", type: "string", required: true, hint: "catalog type from the situation" }, { name: "count", type: "int", required: false, hint: "1 to 3" }, { name: "reason", type: "string", required: false, hint: "short text" }] },
    { name: "OFFER_MISSION", layer: "intent", channel: "missions", authority: "hq", cost: 0, tags: ["missions"], requires: [], desc: "Offer up to 3 missions to a human-led squad (humanLed=true): each offer has kind CAPTURE|DEFEND|RECON|DESTROY, a target (objective id) or pos [x, z], and a brief (max 240 chars, in the voice of HQ). Skip squads whose mission.state is accepted.",
      params: [{ name: "squad", type: "unit", required: true, hint: "human-led squad id from the situation" }] },
  ],
  primitives: [],
};

// Reply channels of the command JSON (orders / buy / missions) with their entries. Later phases add channels here and in the game.
export const channelOf = (m, ch) => (Array.isArray(m?.intents) ? m.intents : []).filter((e) => (e.channel ?? "orders") === ch);
export const orderNamesOf = (m) => new Set(channelOf(m, "orders").map((e) => e.name));
export const fieldsText = (e) => (Array.isArray(e.params) ? e.params : []).map((p) => `${p.name}${p.required ? "*" : ""}${Array.isArray(p.enum) && p.enum.length ? "=" + p.enum.join("|") : ""}${p.hint ? " (" + p.hint + ")" : ""}`).join(", ");
export const sentence = (t) => String(t ?? "").trim().replace(/\.$/, "");

// FOB paragraph (M3 phase F): added to the prompt only when the manifest contains FOB intents (capability fob on, fobMax > 0).
export const FOB_PROMPT = "FOBS: a FOB is a forward base (spawn point, command post, supply cache, arsenal) that a transport builds at a site you pick from fobSites (pos, grid, place, front, toFront); it costs fobRules.cost supplies up front (refunded if it fails), takes time to build and is lost if the enemy holds the zone. TIMING: establish a FOB only after the opening phase, toward the front you are fighting for, when it shortens reinforcement paths - never at war start. fobRules.ready says whether the game accepts one now (fobRules.earliestSec of war time must have passed AND you must know the front: an enemy contact, a contested or lost objective); while ready is false read fobRules.readyReason and do not send ESTABLISH_FOB (it is dropped). Prefer the first fobSites entry (best toward the front) once a front objective is clear and the FOB is worth fobRules.cost; keep ~100 supplies for units; fobRules.live < fobRules.max and fobRules.inProgress = 0; fobs[] lists yours (state planned|moving|building|active|lost, build %, tier, underAttack). Upgrade an active tier-1 FOB (adds the vehicle depot) when supplies are plentiful. Squads with a reserved field are building a FOB: do not order them.\n";

// Logistics paragraph (M3 phase L): added only when the manifest contains RESUPPLY (capability logistics on, FOBs allowed, a supply truck in the catalog).
export const LOGISTICS_PROMPT = "LOGISTICS: stock is per base. war.supplies is the HQ stock (income lands there, units and FOBs are paid from it); each FOB has its own fobs[].stock, which its arsenal, requests and upgrade draw from, and a new FOB starts empty. A supply truck convoy (convoys[]: id, from, to, cargo, state loading|en_route|delivered|lost, pos, grid) loads min(truck capacity logisticsRules.truckCargo, source stock) at departure, drives to the destination and adds it to that stock on arrival; a destroyed truck loses its cargo. Keep every active FOB above logisticsRules.lowStock with RESUPPLY (HQ -> FOB) while the HQ can spare a load (keep ~50), one convoy at a time per FOB and at most logisticsRules.convoyMax in flight; CONVOY moves stock between two bases. Do not order squads that have a reserved field (convoy trucks).\n";

// The wakeup paragraph is appended only when the model may choose its next wakeup.
export function wakeupText(w) {
  return `
You are not polled continuously: you also choose when you are consulted next. Add to the JSON object the optional fields
"next_check_sec" (integer seconds, allowed range ${w.minSec}..${w.maxSec}, default ${w.defaultSec}) and "next_check_reason" (short text), e.g.
{"orders":[...],"next_check_sec":120,"next_check_reason":"quiet, no contacts"}.
You are woken early anyway when something important happens (new contact, casualties, lost squad, objective reached or changed owner, tickets dropping, new units, mission events), so choose
long intervals in quiet phases and short ones during fights or while an assault is developing.`;
}

// Faction SITREP template (real formats: US/NATO SITREP, SMEAC five-paragraph order, SALUTE enemy report; short, player-useful).
export const SITREP_KEYS = ["situation", "enemy", "mission", "execution", "logistics"];
export const SITREP_TITLES = { situation: "Situation", enemy: "Intel", mission: "Mission", execution: "Execution", logistics: "Logistics" };
export const SITREP_CAP = 420, SITREP_HEADLINE_CAP = 140, SITREP_FRESH_SEC = 360;
// The SITREP is its own call (role sitrep): this is the body of its system prompt (persona tone and the help-call line are added by buildSitrepPrompt).
export const SITREP_PROMPT = `You are the HQ staff officer of one faction in a tactical war simulation (Arma Reforger). You see only what your units reported (fog of war).
TASK: write the SITREP of OUR faction: a natural NATO/US-style situation report, read aloud to our soldiers. Only what we know. Reply with ONLY one JSON object, no prose, no code fences. String keys: headline, situation, enemy, mission, execution, logistics. Plain text, \\n for line breaks, no markup.
The user message holds the situation and the orders HQ has given (with reasons): the mission and execution paragraphs describe those orders in words.
Style:
- headline: BLUF, max 110 chars: who is ahead, what matters now.
- Words, not data: no raw numbers, coordinates, "age 12s", unit ids (US_Inf2) or faction keys. Say "we/our forces/the enemy", "the mechanized platoon", "our AT team", "two elements". At most one number per section, only if it helps (minutes left, a grid).
- Enemy size from contact "size" (else "vehicles"/"infantry"/"desc"): fire team, squad-, platoon-, company-sized element; single vehicle, vehicle section/platoon, large mechanized force, "with dismounts". Time: just now / last few minutes / earlier. Place: contact "place" (nearest named place or objective) plus the 100 m "grid" if given ("north of Morton, grid 262-288"); objectives as letter + name. Never raw coordinates; no place: "unlocated".
- Trends in words: tickets bleeding / steady / ahead; supplies healthy / tight / dry; strength good / thin / exhausted.
- situation: objectives held/contested/lost, ticket trend, tempo. enemy: SALUTE as plain prose, freshest first, 1-3 sentences. mission: SMEAC who/what/where/why, 1-2 sentences. execution: "Main effort: ... Supporting effort: ... Reserve: ..." then one line on what helps players. logistics: supplies, replacements, vehicles, weak elements, in words.
Example (invented): {"headline":"Holding two of five points; enemy armor massing north, push on Delta now.","situation":"We hold Alpha and Bravo; Charlie is contested. Tickets are bleeding slowly but we remain ahead.","enemy":"Platoon-sized mechanized force with dismounts moving south through Harbor Bridge just now. Squad-sized element at Delta, last few minutes.","mission":"Defeat the enemy at Charlie Mill and secure Delta to deny him the crossing.","execution":"Main effort: mechanized platoon to Charlie. Supporting effort: two rifle elements fix Delta. Reserve: AT team at Bravo. Players: join a squad moving on Charlie.","logistics":"Supplies healthy, income steady, replacements ordered. One element is thin and falling back."}`;

// Help calls (/backup): the game puts the open ones in sit.helpCalls[].
export const HELP_CALLS_PROMPT = "HELP CALLS: the situation may carry helpCalls[] ({id, player, squad, grid, pos:[x,z], note, ageSec, answeredBy}) from players asking for backup. An open call (answeredBy empty) is urgent: send or keep a unit there (DEFEND or MOVE to pos) unless impossible. A call already answered by a squad needs no second unit unless that squad is gone or weak.\n";
const SITREP_HELP_PROMPT = "Open help calls (helpCalls[] with an empty answeredBy) must be mentioned in the execution paragraph.\n";

// ---------------------------------------------------------------- manifest-driven COMMANDS + REPLY part

/** Does the FOB paragraph apply? Manifest has FOB intents AND (own FOBs exist OR the game says one is possible OR there is no fobRules block). */
export function fobRelevant(manifest, sit) {
  if (!channelOf(manifest, "fob").length) return false;
  const fr = sit && typeof sit === "object" ? sit.fobRules : null;
  return (Array.isArray(sit?.fobs) && sit.fobs.length > 0) || !fr || typeof fr !== "object" || fr.ready !== false;
}
/** LOGISTICS paragraph: RESUPPLY in the manifest AND at least one active FOB or a convoy in the situation. */
export function logisticsRelevant(manifest, sit) {
  if (!channelOf(manifest, "fob").some((e) => e.name === "RESUPPLY")) return false;
  return (Array.isArray(sit?.fobs) && sit.fobs.some((f) => f?.state === "active")) || (Array.isArray(sit?.convoys) && sit.convoys.length > 0);
}
export const helpCallsRelevant = (sit) => Array.isArray(sit?.helpCalls) && sit.helpCalls.length > 0;

/** Spec for promptKit.terseSpec / terseSchema from the manifest + config (channels come from the manifest, wake from the config when dynamic). */
export function terseSpecOf(manifest, c) {
  const m = manifest ?? DEFAULT_MANIFEST, w = c?.wakeup;
  return {
    orders: channelOf(m, "orders").map((e) => e.name),
    buy: channelOf(m, "buy").length > 0,
    fobs: channelOf(m, "fob").map((e) => e.name),
    missions: channelOf(m, "missions").length > 0,
    wake: w?.dynamic ? { minSec: w.minSec, maxSec: w.maxSec, defaultSec: w.defaultSec } : null,
    reasonWords: isNum(c?.output?.reasonWords) ? c.output.reasonWords : 12,
  };
}

/** Constraints on the reply channels (shared by the verbose and the terse reply spec). */
function replyLimits(buys, fobs) {
  return (buys.length ? "buy: max 3 entries, count 1-3, total cost <= war.supplies, every entry with command + target/pos (the order of the new units). " : "")
    + (fobs.length ? "fobs: at most 3 entries per reply, at most 1 of them ESTABLISH_FOB or UPGRADE_FOB. " : "");
}

/**
 * COMMANDS + REPLY paragraphs of the v2/v3 prompt, generated from the manifest (entries with unmet capabilities are not in it).
 * -> {commands, reply}. The FOB / LOGISTICS paragraphs are NOT in here (see buildCommanderPrompt: conditional, at the end of the system prompt).
 * opts = {terse, spec}: terse reply spec (promptKit.terseSpec) instead of the verbose REPLY block.
 */
export function commandsSection(m, opts = {}) {
  const orders = channelOf(m, "orders"), buys = channelOf(m, "buy"), offers = channelOf(m, "missions"), fobs = channelOf(m, "fob");
  let t = "";
  if (orders.length) {
    t += "COMMANDS (orders, one per squad, only for squads with humanLed=false)\n";
    t += orders.map((e) => `${e.name} (${sentence(e.desc).replace(/^[A-Z](?![A-Z])/, (c) => c.toLowerCase()).replace(/. /g, "; ")})`).join(", ") + ".\n";
    t += `Order fields ("command" is the name above; * = required): ${fieldsText(orders[0])}.\n`;
  }
  if (buys.length) t += buys.map((e) => `${e.name}: ${sentence(e.desc)}. Fields: ${fieldsText(e)}.\n`).join("");
  if (fobs.length) t += fobs.map((e) => `${e.name}: ${sentence(e.desc)}. Fields: ${fieldsText(e)}.\n`).join("");
  if (!opts.omitHuman) t += offers.length
    ? "HUMAN-LED SQUADS (humanLed=true) are never ordered. " + offers.map((e) => sentence(e.desc)).join(". ") + ".\n"
    : "HUMAN-LED SQUADS (humanLed=true) are never ordered and get no missions.\n";
  let reply;
  if (opts.terse) {
    reply = terseSpec(opts.spec) + "\n" + replyLimits(buys, fobs).trimEnd();
  } else {
    const parts = [], keys = [];
    if (orders.length) {
      const a = orders[0].name, b = (orders[1] ?? orders[0]).name;
      parts.push(`"orders":[{"squad":"<id>","command":"${a}","target":"C","reason":"short"},{"squad":"<id>","command":"${b}","pos":[x,z],"reason":"short"}]`);
      keys.push("orders");
    }
    if (buys.length) { parts.push('"buy":[{"type":"<catalog type>","count":1,"command":"ATTACK","target":"C","reason":"short"}]'); keys.push("buy"); }
    if (fobs.length) { parts.push('"fobs":[{"command":"ESTABLISH_FOB","site":"<fobSites id>","reason":"short"}]'); keys.push("fobs"); }
    if (offers.length) { parts.push('"missions":[{"squad":"<human-led id>","offers":[{"kind":"CAPTURE","target":"D","brief":"..."}]}]'); keys.push("missions"); }
    reply = "REPLY with ONLY one JSON object, no prose, no code fences:\n{" + parts.join(",\n ") + "}\n"
      + (keys.length ? `${keys.join("/")} may be empty or omitted. ` : "") + replyLimits(buys, fobs) + '{"keep":true} alone = keep all current orders.';
  }
  return { commands: t, reply };
}

// ---------------------------------------------------------------- planner split helpers
const PLAN_CHANNELS = ["buy", "fob"];
/** A manifest view without / with only the given channels (the orders call drops buy + fob, the planner call keeps only those). */
const manifestWithout = (m, chs) => ({ ...m, intents: (m.intents ?? []).filter((e) => !chs.includes(e.channel ?? "orders")) });
const manifestOnly = (m, chs) => ({ ...m, intents: (m.intents ?? []).filter((e) => chs.includes(e.channel ?? "orders")) });
/** Does the planner own anything in this manifest (buy or fob intents)? */
export const plannerChannels = (m) => PLAN_CHANNELS.some((ch) => channelOf(m ?? DEFAULT_MANIFEST, ch).length > 0);
/** Is the planner split active for this config + manifest? (planner.enabled and something for it to do) */
export const plannerSplit = (config, manifest) => !!config?.planner?.enabled && plannerChannels(manifest ?? DEFAULT_MANIFEST);

const compact = (sections) => Object.fromEntries(Object.entries(sections).filter(([, v]) => v));
const safe = (fn, sit) => { try { return fn(sit) || ""; } catch { return ""; } };
const ORDER = ["rules", "commands", "persona", "fobs", "logistics", "helpCalls", "situation", "plan", "wake", "env", "journal"];
const reorder = (s) => Object.fromEntries(ORDER.filter((k) => k in s).map((k) => [k, s[k]]));

/** The PLAN block of the orders call (split mode): the planner's last result + when it runs next. plan = {buy, fobs, ageSec, nextInSec, running} | null. */
export function planBlock(plan) {
  const head = "PLAN (the planner handles reinforcements, forward bases and supply runs on its own schedule; you only give orders):\n";
  if (!plan || (!plan.buy?.length && !plan.fobs?.length && plan.ageSec === undefined)) {
    return head + "none yet" + (plan?.running ? " (the planner is working on it now)" : plan && isNum(plan.nextInSec) ? ` (the planner runs in about ${Math.round(plan.nextInSec)} s)` : "");
  }
  const buy = (plan.buy ?? []).map((b) => ({ type: b.type, count: b.count, command: b.command, ...(b.target ? { target: b.target } : b.pos ? { pos: b.pos } : {}), reason: b.reason ?? "" }));
  const fobs = (plan.fobs ?? []).map((f) => ({ command: f.command, ...(f.site ? { site: f.site } : {}), ...(f.fob ? { fob: f.fob } : {}), ...(f.origin ? { origin: f.origin, dest: f.dest } : {}), reason: f.reason ?? "" }));
  const o = { lastRunAgoSec: isNum(plan.ageSec) ? Math.round(plan.ageSec) : null, ...(plan.running ? { runningNow: true } : isNum(plan.nextInSec) ? { nextRunInSec: Math.round(plan.nextInSec) } : {}), buy, fobs };
  return head + JSON.stringify(o);
}

/**
 * The commander call. config = the sidecar config (wakeup, memory, output, planner groups are read), sit = the situation as sent to the model
 * (v2 may carry "wake"; "structures" is stripped), manifest = the action manifest, journal = HQ JOURNAL block ("" = none).
 * split (default: config.planner.enabled and the manifest has buy/fob intents) = this is the ORDERS call: buy / FOB / logistics leave the prompt and the reply spec,
 * plan (planBlock input) is added to the user message.
 * -> {system, user, sections:{name:text}, spec: terse spec | null}
 */
export function buildCommanderPrompt({ config = {}, sit = null, manifest = DEFAULT_MANIFEST, journal = "", split, plan = null } = {}) {
  const v2 = !!sit && (sit.v === 2 || sit.v === 3 || sit.v === 4);
  const { structures: _structures, ...llmSit } = sit ?? {};
  const wakeText = config.wakeup?.dynamic ? wakeupText(config.wakeup) : "";
  if (!v2) { // protocol v1: unchanged single system prompt, wake stays inside the situation object
    const system = SYSTEM + wakeText;
    const situation = "Current situation:\n" + JSON.stringify(llmSit);
    return { system, user: situation + (journal ? "\n\n" + journal : ""), sections: compact({ rules: system, situation, journal }), spec: null };
  }
  const full = manifest ?? DEFAULT_MANIFEST;
  const isSplit = split ?? plannerSplit(config, full);
  const m = isSplit ? manifestWithout(full, PLAN_CHANNELS) : full;
  const terse = !!config.output?.terse;
  const spec = terse ? terseSpecOf(m, config) : null;
  const cs = commandsSection(m, { terse, spec });
  const rules = isSplit
    ? SYSTEM_ORDERS_V2 + "\n" + ORDERS_FLANK + " " + J.SQUAD_NAME_PROMPT + (config.memory?.enabled ? " " + J.JOURNAL_RULE.replace(", FOB", "") : "") + "\n"
    : SYSTEM_V2 + "\n" + J.FLANK_PROMPT + " " + J.SQUAD_NAME_PROMPT + (config.memory?.enabled ? " " + J.JOURNAL_RULE : "") + "\n";
  const commands = cs.commands + cs.reply + (terse ? "" : wakeText) + "\n";
  const persona = safe(personaCommander, sit);
  const fobs = !isSplit && fobRelevant(m, sit) ? FOB_PROMPT : "", logistics = !isSplit && logisticsRelevant(m, sit) ? LOGISTICS_PROMPT : "", helpCalls = helpCallsRelevant(sit) ? HELP_CALLS_PROMPT : "";
  const system = rules + commands + persona + fobs + logistics + helpCalls;
  const { wake, ...rest } = llmSit;
  const situation = "Current situation:\n" + JSON.stringify(rest);
  const planText = isSplit ? planBlock(plan) : "";
  const wakeBlock = wake ? "WAKE (why you are consulted now):\n" + JSON.stringify(wake) : "";
  const env = safe(envPrompt, sit);
  const user = situation + (planText ? "\n\n" + planText : "") + (wakeBlock ? "\n\n" + wakeBlock : "") + (env ? "\n\n" + env : "") + (journal ? "\n\n" + journal : "");
  return { system, user, sections: reorder(compact({ rules, commands, persona, fobs, logistics, helpCalls, situation, plan: planText, wake: wakeBlock, env, journal })), spec };
}

/**
 * The PLANNER call (role planner): buy + FOB / logistics only. orders = the last validated orders [{squad, command, target|pos, reason}] (orders in force),
 * journal = HQ JOURNAL block (intent + goals, "" = none), trigger = why the planner runs now (string, "" = scheduled).
 * No persona, environment or help-call paragraph and no wake text beyond `trigger`.
 * -> {system, user, sections:{rules, commands, fobs, logistics, situation, orders, journal, trigger}, spec: terse spec | null}
 * The reply is b / f / k only (terse) or buy / fobs / keep (verbose); responseFormat for the call: promptKit.terseSchema(spec).
 */
export function buildPlannerPrompt({ config = {}, sit = null, manifest = DEFAULT_MANIFEST, journal = "", orders = [], trigger = "" } = {}) {
  const { structures: _structures, wake: _wake, ...llmSit } = sit ?? {};
  const m = manifestOnly(manifest ?? DEFAULT_MANIFEST, PLAN_CHANNELS);
  const terse = !!config.output?.terse;
  const spec = terse ? { ...terseSpecOf(m, config), wake: null, missions: false } : null;
  const cs = commandsSection(m, { terse, spec, omitHuman: true });
  const reply = cs.reply.replace('{"k":1} alone = keep all current orders.', '{"k":1} alone = nothing to buy or build now.');
  const rules = PLANNER_SYSTEM;
  const commands = cs.commands + reply + "\n";
  const fobs = fobRelevant(m, sit) ? FOB_PROMPT : "", logistics = logisticsRelevant(m, sit) ? LOGISTICS_PROMPT : "";
  const system = rules + commands + fobs + logistics;
  const situation = "Current situation:\n" + JSON.stringify(llmSit);
  const list = Array.isArray(orders) ? orders : [];
  const ord = "ORDERS IN FORCE (last decision of the commander, with reasons):\n" + (list.length
    ? JSON.stringify(list.map((o) => ({ squad: o.squad, command: o.command, ...(o.target ? { target: o.target } : o.pos ? { pos: o.pos } : {}), reason: o.reason ?? "" })))
    : "none yet");
  const trig = "PLANNER WAKE: " + (trigger || "scheduled run");
  const user = situation + "\n\n" + ord + (journal ? "\n\n" + journal : "") + "\n\n" + trig;
  return { system, user, sections: compact({ rules, commands, fobs, logistics, situation, orders: ord, journal, trigger: trig }), spec };
}

/**
 * The SITREP call (role sitrep). orders = the commander's last validated orders [{squad, command, target|pos, reason}].
 * -> {system, user, sections:{rules, persona, help, situation, orders}}
 */
export function buildSitrepPrompt({ sit = null, orders = [] } = {}) {
  const { structures: _structures, wake: _wake, ...llmSit } = sit ?? {};
  const persona = safe(personaSitrep, sit);
  const help = helpCallsRelevant(sit) ? SITREP_HELP_PROMPT : "";
  const system = SITREP_PROMPT + persona + (help ? "\n" + help : "");
  const situation = "Current situation:\n" + JSON.stringify(llmSit);
  const list = Array.isArray(orders) ? orders : [];
  const ord = "HQ orders in force (last commander decision, with reasons):\n" + (list.length
    ? JSON.stringify(list.map((o) => ({ squad: o.squad, command: o.command, ...(o.target ? { target: o.target } : o.pos ? { pos: o.pos } : {}), reason: o.reason ?? "" })))
    : "none yet");
  return { system, user: situation + "\n\n" + ord, sections: compact({ rules: SITREP_PROMPT, persona, help, situation, orders: ord }) };
}

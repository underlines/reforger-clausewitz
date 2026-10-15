// Clausewitz LLM bridge sidecar + read-only web dashboard. Plain Node 18+, no npm deps.
//   node dev/tools/llm_sidecar.mjs
// Env (initial defaults only; saved settings in dev/tools/sidecar-config.json win):
//   CLW_PORT (default 8788), CLW_LLM=fake|anthropic|openai-compatible|openrouter (default fake),
//   ANTHROPIC_API_KEY, CLW_MODEL (default claude-sonnet-5-5)
// Listens on 127.0.0.1 only (CLW_HOST to change; CLW_ALLOWED_HOSTS = extra host names for a remote sidecar). Dashboard + /api/* need the admin login (auth.mjs, default password in README);
// game endpoints use header X-CLW-Token when config gameToken is set. Loadout library: loadouts.mjs (/loadouts/*, /control, /api/loadouts*).
// CLW_DATA_DIR = data dir (default dev/tools/.cache: sessions.json, loadouts-store.json, map caches).
//
// Game API:
//   GET  /health -> {"ok":true,"mode":"<provider>"}
//   POST /orders: one faction's situation. Two protocol versions, selected by the field "v":
//
//   Protocol v1 (no "v" field; legacy, still supported):
//     {"faction":"US","time":123,
//      "squads":[{"id":"US_Squad1","pos":[x,y,z],"agents":8,"startAgents":8,
//                 "mode":"NONE|SEARCH|ATTACK|DEFEND","humanLed":false}],
//      "contacts":[{"pos":[x,y,z],"desc":"2x infantry","ageSec":12}]}
//     -> 200 {"orders":[{"squad":"US_Squad1","command":"MOVE|ATTACK|DEFEND","pos":[x,z],"reason":"..."}]}
//
//   Protocol v2 ("v":2; M2 "real war"; design: dev/notes/m2-design.md section 8). Nothing is hard-wired: faction keys, unit kinds,
//   catalog types and objective ids all come from the situation.
//     {"v":2,"faction":"US","time":1234,"timeLeftSec":3200,
//      "war":{"tickets":350,"enemyTickets":310,"supplies":220,"income":60,"liveAI":38,"aiCap":50,"vehicles":3,"vehicleCap":6},
//      "objectives":[{"id":"C","name":"Timber Hub","pos":[x,z],"radius":80,"value":2,"owner":"none|<faction>","contested":false,"progress":-35}],
//      "bases":[{"faction":"US","pos":[x,z]}],
//      "catalog":[{"type":"rifle_squad","kind":"infantry","cost":100,"desc":"..."}],
//      "squads":[{"id","kind","type","pos":[x,y,z],"agents","startAgents","vehicle":{"type","health","mounted","state"}|null,"humanLed",
//                 "wounded","health","speed","heading","state","objective":{"command","target","pos","reason","ageSec","status"}|null,
//                 "mission":{"state":"offered|accepted|none","kind","target"}|null}],
//      "contacts":[{"pos":[x,y,z],"desc","kind":"infantry|vehicle|unknown","ageSec"}],
//      "events":["C captured by USSR", ...]}   (events since the last POST; the sidecar accumulates them per faction and hands the
//                                               model the ones since its last call)
//     -> 200 {"orders":[{"squad","command":"MOVE|ATTACK|DEFEND|RECON|FALL_BACK","target":"<objective id>" | "pos":[x,z],"reason"}],
//             "buy":[{"type":"<catalog type>","count":1..3,"reason"}],
//             "missions":[{"squad":"<human-led squad>","offers":[{"kind":"CAPTURE|DEFEND|RECON|DESTROY","target":"<objective id>" | "pos":[x,z],"brief":"<=240 chars"}]}]}
//     Validation (everything invalid is dropped with a reason, see "Dropped" in the exchange log): orders need a known squad that is NOT
//     humanLed, a known objective id as target or a finite pos (FALL_BACK may have neither = own base), one order per squad; buy: known
//     catalog type, integer count 1..3, max 3 entries, running cost <= war.supplies; missions: only humanLed squads without an accepted
//     mission, max 3 offers per squad, target = objective id or pos. A model reply with "keep":true and nothing else = keep orders.
//
//   Protocol v3 ("v":3; M3 phase R, design: dev/notes/decision-command-tools.md) = v2 + the action manifest. POST /manifest:
//     {"v":3,"faction","hash","mode","capabilities":["services",..],
//      "intents":[{"name":"ATTACK","layer":"intent","channel":"orders|buy|missions","authority":"hq","cost":0,"tags":[],"requires":[<capability>],
//                  "desc","params":[{"name","type":"string|int|enum|position|unit|base","required","enum":[],"hint"}]}],
//      "primitives":[...same shape, layer "primitive"...]}  -> 200 {"ok":true,"hash"}. The game sends it at war start and on every change
//     (mode / capability / faction); entries with unmet capabilities are not in it. Every v3 situation carries "manifest":"<hash>".
//     The prompt's command list, the order/buy/mission validation and the fake provider come from the manifest of that hash (the built-in
//     one mirrors Actions.conf). Unknown hash (sidecar restarted) -> built-in manifest + the reply carries "needManifest":true and the
//     game resends. v2 posts (no hash) keep working with the built-in manifest. GET /api/state has "manifests" (dashboard Actions tab).
//     M3 phase Q: the situation carries "requests":[{"id","player","role":"leader|soldier","kind":"vehicle|squad|resupply|reinforce|recon","args",
//     "status":"granted|refused","reason","ageSec"}] (recent player requests the game's rules decided; fast path, no LLM). Passed through
//     unchanged: GET /api/state factions[<name>].situation.requests (dashboard Requests tab). Protocol stays v3.
//     Base structures: the situation carries "structures":[{"base","kind","label","pos":[x,z],"grid"}] (own side, one per placed service, same set as the in-game map markers). Dashboard map layer only (GET /api/state factions[<name>].situation.structures); stripped from the LLM prompt.
//     Reply channels: orders / buy / missions (same command JSON as v2) + "fobs" (M3 phase F: [{"command":"ESTABLISH_FOB","site":"<fobSites id>"} or {"command":"UPGRADE_FOB","fob":"<fobs id>"}], max 1; phase L adds {"command":"RESUPPLY","fob":"<fobs id>"} and {"command":"CONVOY","origin":"<base id>","dest":"<base id>"}, max 3 per reply in total, only with the logistics capability + blocks convoys[] / logisticsRules; situation blocks fobs[] / fobSites[] / fobRules). New channels need game + sidecar support.
//
//   POST /hq {faction, msg:{id,player,role,squad,grid,pos,lang,text,ageSec,history}} (WP11 fast path, X-CLW-Token): runs the /hq agent turn at once against the
//     faction's cached situation (capped at 25 s) and answers ON THAT REQUEST {handled:true, hqReplies?, hqActions?}; {handled:false, reason} = nothing started (no cached
//     situation / message already started by the other path), the hq[] of POST /orders stays the fallback. Dedupe by faction|id|player|text; if the game hung up before the
//     turn ended, the result rides on the next /orders reply as before. See "FAST PATH" in the HQ chat section.
//   Protocol v4 ("v":4; i18n, design: dev/notes/i18n-design.md) = v3 + languages. v3 stays accepted; no language field = English / old behaviour.
//     /hq (POST /orders "hq"[] messages): each message may carry "lang" (ISO 639-1, the asker's language). The strong LLM then writes the reply in that
//       language in the SAME agent call (final text = JSON {"reply","setLanguage"?}). hqReplies[] entries: {msgId,text,setLanguage?}; setLanguage = "<xx>"
//       or "auto" (validated ^[a-z]{2}$|auto, else dropped), only when the asker asked to be answered in some language; it concerns that player only.
//     Broadcast texts: the /orders POST may carry "langs":["de","fr"] (recipient languages). When the reply has a "sitrep", it also has
//       sitrep.text_by_lang = {en:{headline,situation,enemy,mission,execution,logistics}, de:{...}} (canonical English + one weak-LLM translation per
//       language, parallel, cached by sha1(text)+lang, timeout config i18n.translateTimeoutSec = 10 s -> that language is omitted, flagged in the
//       exchange "i18n" field). POST /translate {texts:{name:"english"},langs:[..]} -> {ok,text_by_lang,failed,cached} does the same for any broadcast text.
//     Providers: two slots, strong (the top-level provider/model/baseUrl/apiKey/timeoutSec = the old single provider) and weak (config "weak", provider
//       "none" = unset). Every LLM call names a role (commander|journal|hq|translate); only translate uses the weak slot, no weak slot = strong translates.
//       Config API: GET /api/config also returns strong{}, weak{} (apiKey as apiKeySet/apiKeyLast4), roles, translateSlot; POST /api/config accepts
//       "weak":{provider,model,baseUrl,apiKey,timeoutSec}|null, "strong":{same} and "i18n":{enabled,translateTimeoutSec}; POST /api/test {"slot":"translate"}.
//     Backup (fallback) model per slot: strong.fallback / top-level "fallback" (strong) and weak.fallback = null | {model, provider?, baseUrl?, apiKey?, timeoutSec?}
//       (omitted fields inherit from the slot; the slot's key is only inherited while provider + baseUrl stay the same). A call goes to the primary and, after a
//       RETRYABLE failure (HTTP 408/429/5xx, timeout, network error, "cooling down"), ONCE to the backup (agent turns: the backup takes over the remaining steps).
//       400/401/403/404 and invalid model output never fall back. Cooldown is per (slot, model) (20 s after 429/5xx, also after a timeout when a backup exists).
//       Exchange log + /api/state hq[]: modelUsed, fellBack, primaryError; /api/state counters.modelFallbacks {strong,weak}, modelCooldowns {"<slot>|<model>": sec}.
//       POST /api/config "fallback":null clears; POST /api/test {"slot":"strong|weak|translate","which":"primary|fallback"} tests one model alone.
//     Fake provider: translation = "[<lang>] <text>", /hq reply = "[<lang>] <reply>" (also after setLanguage), so e2e can check routing without a key.
//
//   Faction SITREP (design: dev/notes/sitrep-design.md). The game POST may carry "sitrep": the rules SITREP the game built from the
//     same fog-limited picture {scope,unit,source:"rules",warTimeSec,headline,situation,enemy,mission,execution,logistics}; the sidecar
//     removes it before the model call. When the config "sitrep" group is enabled the model is asked, inside the SAME commander call
//     (no extra round trip), for a "sitrep" object with the same keys when none was produced for sitrep.intervalSec, on the first call,
//     or when an objective/ticket/loss trigger woke it. The reply then carries "sitrep" (also on keep replies); the game shows it to
//     that faction's players only. GET /api/state: factions[<name>].sitrep =
//     {text, headline, sections:[{id,title,body}], scope:"faction", unit, source:"llm"|"rules", generatedAt (epoch ms), warTimeSec}
//     (the LLM one while it is at most 360 s of war time old, else the game's rules one, else a sidecar-built rules one; null before any).
//
//   HQ war journal (M4 round 3 WP5; agent/journal.mjs, section "HQ war journal" below): per faction intent (1 x 120 chars), 3 goals (open|done|failed),
//     3 lessons, written ONLY by a small review call every memory.reviewMin minutes (JSON, validated against the caps; invalid -> old journal kept), plus
//     the attack record "tries" counted by CODE from the ATTACK orders sent and the game's outcome events ("outcome <squad> <ATTACK|DEFEND> <target|-> <done|failed>: <why>",
//     "<squad> lost", "<id> <name> captured by <faction>"). One HQ JOURNAL block follows the situation in the commander prompt and in the /hq agent prompt.
//     Reset when a new war starts (phase back to setup, game time or war id changes). Config group memory {enabled:true, reviewMin:5}. GET /api/state "journals" {faction: view}.
//     v2 objectives may carry side left|centre|right (of the HQ-HQ axis, from the faction's own view): the flank nudge in the prompt.
//   Any LLM/validation failure, or the panel's "enabled" switch off -> 200 {"orders":[]} (v2: plus empty buy/missions)
//   (game falls back to its rule commander). Unparsable request body -> 400.
//   Cost control (real providers only, not "fake"): settings minIntervalSec (at most one provider call per
//   faction per N s, 0 = off) and skipUnchanged (skip the call when the situation is materially unchanged: same squad
//   ids/modes/agents/kinds/vehicle state, every squad and contact moved < 75 m, same contact count; v2 also same objective
//   owners/contested, tickets within 5 %, supplies within 100, same AI/vehicle counts, no new events). A skipped request
//   answers 200 {"orders":[],"keep":true}: the game keeps its current orders (no rule fallback).
//
// HQ scheduler (all providers, per faction): the game polls every 10 s but the LLM is only called on the first request, at the
//   scheduled time (model-chosen next_check_sec, clamped to wakeup.minSec..maxSec, else wakeup.defaultSec; x budget stretch factor),
//   when a trigger fires (config "triggers": new contact, contact near squad, casualties, squad lost, strength drop, objective
//   reached, squad change; v2 also objectiveChanged (any owner change), ticketsDropPct (own tickets fell >= N % since the last call),
//   unitSpawned (new squad / spawn event), missionEvent (mission offered/accepted/done); min gap triggers.cooldownSec) or after POST /api/wake {faction?}. minIntervalSec is the hard floor
//   between any two calls. Other polls answer {"orders":[],"keep":true} without an exchange entry (counted as counters.gated).
//   The model sees a "wake" object {reason,triggers,sinceLastSec}; /api/state has "scheduler" per faction.
//
// OpenRouter provider ("openrouter", baseUrl defaults to https://openrouter.ai/api/v1; a saved "openai-compatible" config
//   pointing at openrouter.ai is migrated automatically): each request adds usage:{include:true} and records usage.cost (USD).
//   Local metrics (session cost/requests/tokens, cost by model and by faction; reset on restart) are in /api/state "usage".
//   Key budget: GET <baseUrl>/key is polled at startup, after a config save, every 10 min (60 s while the limit is reached)
//   and on POST /api/usage/refresh. Warnings at 80 % / 95 % of the key limit (polled value minus local cost since the poll);
//   at 100 % and setting stopAtLimit (default true) real provider calls stop: /orders answers {"orders":[]} (reason
//   "budget limit reached", counted as fallback) until a refresh shows budget again.
//
// World / map (see "world info" below): game POSTs /world {worldFile,mapPrefab,mapOffset,mapSize,boundsMin,boundsMax} -> {ok,worldKey,needRoads,needLocations},
//   /world/roads (chunks) and /world/locations. Dashboard: GET /api/world, /api/map.png, /api/vector; POST /api/map/reload, /api/vector/reset.
//   Cache in dev/tools/.cache/ (world.json, vector/<worldKey>.json, maps/). Overrides: dev/tools/maps/<World>.png + .json.
//
// Dashboard (http://127.0.0.1:8788/, static files from dev/tools/dashboard/):
//   GET  /api/state   -> factions, latest situations/orders, recent exchanges (ring of 100), counters, config (no secrets);
//                        activeFactions = factions of the current war (same war epoch, last post <= staleAfterSec of the newest post)
//   GET  /api/config  -> current settings; apiKey is reported only as {apiKeySet, apiKeyLast4}
//   POST /api/config  -> change settings (JSON, localhost Origin/Host only), persisted to sidecar-config.json
//   POST /api/usage/refresh -> re-poll the OpenRouter key budget now -> {ok,usage}
//   POST /api/test    -> send a tiny canned situation through the current provider -> {ok,orders,error,latencyMs}
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { cleanHistory } from "./agent/hqHistory.mjs";
import * as I18N from "./agent/i18n.mjs"; // protocol v4: languages, structured /hq reply, translator (section "i18n" below)
import * as J from "./agent/journal.mjs"; // HQ war journal (pure functions), see "HQ war journal" below
import * as FB from "./agent/fallback.mjs"; // backup (fallback) model per slot, see "model fallback" below
import * as AUTH from "./auth.mjs"; // admin login (session cookie) + game token (X-CLW-Token)
import * as LO from "./loadouts.mjs"; // loadout library store + REST handlers (/loadouts/*, /control, /api/loadouts*)
import { personaCommander, personaSitrep } from "./agent/personality.mjs"; // HQ character traits (sit.hq.trait): commander / SITREP prompt text
// environment paragraph (WP3, sit.env): lazy so the sidecar still starts when ./agent/environment.mjs is missing or broken
let envPrompt = () => "";
try { ({ envPrompt } = await import("./agent/environment.mjs")); } catch { /* optional */ }

const DIR = path.dirname(fileURLToPath(import.meta.url));
const DASH_DIR = path.join(DIR, "dashboard");
const CONFIG_FILE = process.env.CLW_CONFIG ? path.resolve(process.env.CLW_CONFIG) : path.join(DIR, "sidecar-config.json"); // CLW_CONFIG: a second instance (tests) keeps its own settings file
const DATA_DIR = process.env.CLW_DATA_DIR ? path.resolve(process.env.CLW_DATA_DIR) : path.join(DIR, ".cache"); // map/world caches, sessions.json, loadouts-store.json (CLW_DATA_DIR: tests)
const PORT = Number(process.env.CLW_PORT) || 8788;
const HOST = process.env.CLW_HOST || "127.0.0.1";	// 0.0.0.0 = reachable in the LAN (dashboard for other devices)
const COMMANDS = new Set(["MOVE", "ATTACK", "DEFEND"]);
const MISSION_KINDS = new Set(["CAPTURE", "DEFEND", "RECON", "DESTROY"]);
const BUY_COMMANDS = new Set(["MOVE", "ATTACK", "DEFEND", "RECON"]); // assignment of bought units (spawn on assignment)
const isV2 = (sit) => sit?.v === 2 || sit?.v === 3 || sit?.v === 4; // v3 = v2 + manifest hash; v4 = v3 + languages (lang / langs[] / setLanguage / text_by_lang)
const PROVIDERS = ["fake", "anthropic", "openai-compatible", "openrouter"];
// HQ engine: llm = LLM provider decides; rules = sidecar answers {"orders":[]} so the game's rule commander drives (no LLM calls);
// model = future trained decision-model tier (not implemented: treated like rules, logged once).
const ENGINES = ["llm", "rules", "model"];
let paused = false; // runtime only (not persisted): no LLM calls, every poll answers {"orders":[],"keep":true}
let modelEngineLogged = false;
const OR_BASE = "https://openrouter.ai/api/v1";
const RING_MAX = 100;
const isNum = (n) => typeof n === "number" && Number.isFinite(n);

const SYSTEM = `You are the HQ commander of one faction in a tactical war simulation (Arma Reforger).
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
const SYSTEM_V2 = `You are the HQ commander of one faction in a tactical war simulation (Arma Reforger). You see only what your units reported (fog of war): contacts may be old or incomplete.
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

// Built-in manifest: used for v2 posts and when a v3 post names a manifest hash the sidecar does not know (it then asks the game to
// resend with needManifest). Mirrors the game's Configs/CLW/Actions/Actions.conf at the time of protocol v3.
const ORDER_PARAMS = [
  { name: "squad", type: "unit", required: true, hint: "squad id from the situation, humanLed=false, one order per squad" },
  { name: "target", type: "string", required: false, hint: "objective id from the situation" },
  { name: "pos", type: "position", required: false, hint: "[x, z]; alternative to target" },
  { name: "reason", type: "string", required: false, hint: "short text" },
];
const orderEntry = (name, desc, tags) => ({ name, layer: "intent", channel: "orders", authority: "hq", cost: 0, tags, requires: [], desc, params: ORDER_PARAMS });
const DEFAULT_MANIFEST = {
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
const channelOf = (m, ch) => (Array.isArray(m?.intents) ? m.intents : []).filter((e) => (e.channel ?? "orders") === ch);
const orderNamesOf = (m) => new Set(channelOf(m, "orders").map((e) => e.name));
const fieldsText = (e) => (Array.isArray(e.params) ? e.params : []).map((p) => `${p.name}${p.required ? "*" : ""}${Array.isArray(p.enum) && p.enum.length ? "=" + p.enum.join("|") : ""}${p.hint ? " (" + p.hint + ")" : ""}`).join(", ");
const sentence = (t) => String(t ?? "").trim().replace(/\.$/, "");

// FOB paragraph (M3 phase F): added to the prompt only when the manifest contains FOB intents (capability fob on, fobMax > 0).
const FOB_PROMPT = "FOBS: a FOB is a forward base (spawn point, command post, supply cache, arsenal) that a transport builds at a site you pick from fobSites (pos, grid, place, front, toFront); it costs fobRules.cost supplies up front (refunded if it fails), takes time to build and is lost if the enemy holds the zone. TIMING: establish a FOB only after the opening phase, toward the front you are fighting for, when it shortens reinforcement paths - never at war start. fobRules.ready says whether the game accepts one now (fobRules.earliestSec of war time must have passed AND you must know the front: an enemy contact, a contested or lost objective); while ready is false read fobRules.readyReason and do not send ESTABLISH_FOB (it is dropped). Prefer the first fobSites entry (best toward the front) once a front objective is clear and the FOB is worth fobRules.cost; keep ~100 supplies for units; fobRules.live < fobRules.max and fobRules.inProgress = 0; fobs[] lists yours (state planned|moving|building|active|lost, build %, tier, underAttack). Upgrade an active tier-1 FOB (adds the vehicle depot) when supplies are plentiful. Squads with a reserved field are building a FOB: do not order them.\n";

// Logistics paragraph (M3 phase L): added only when the manifest contains RESUPPLY (capability logistics on, FOBs allowed, a supply truck in the catalog).
const LOGISTICS_PROMPT = "LOGISTICS: stock is per base. war.supplies is the HQ stock (income lands there, units and FOBs are paid from it); each FOB has its own fobs[].stock, which its arsenal, requests and upgrade draw from, and a new FOB starts empty. A supply truck convoy (convoys[]: id, from, to, cargo, state loading|en_route|delivered|lost, pos, grid) loads min(truck capacity logisticsRules.truckCargo, source stock) at departure, drives to the destination and adds it to that stock on arrival; a destroyed truck loses its cargo. Keep every active FOB above logisticsRules.lowStock with RESUPPLY (HQ -> FOB) while the HQ can spare a load (keep ~50), one convoy at a time per FOB and at most logisticsRules.convoyMax in flight; CONVOY moves stock between two bases. Do not order squads that have a reserved field (convoy trucks).\n";

// COMMANDS + REPLY paragraphs of the v2/v3 prompt, generated from the manifest (entries with unmet capabilities are not in it).
function commandsSection(m) {
  const orders = channelOf(m, "orders"), buys = channelOf(m, "buy"), offers = channelOf(m, "missions"), fobs = channelOf(m, "fob");
  let t = "";
  if (orders.length) {
    t += "COMMANDS (orders, one per squad, only for squads with humanLed=false)\n";
    t += orders.map((e) => `${e.name} (${sentence(e.desc).replace(/^[A-Z](?![A-Z])/, (c) => c.toLowerCase()).replace(/. /g, "; ")})`).join(", ") + ".\n";
    t += `Order fields ("command" is the name above; * = required): ${fieldsText(orders[0])}.\n`;
  }
  if (buys.length) t += buys.map((e) => `${e.name}: ${sentence(e.desc)}. Fields: ${fieldsText(e)}.\n`).join("");
  if (fobs.length) t += FOB_PROMPT + fobs.map((e) => `${e.name}: ${sentence(e.desc)}. Fields: ${fieldsText(e)}.\n`).join("");
  if (fobs.some((e) => e.name === "RESUPPLY")) t += LOGISTICS_PROMPT;
  t += offers.length
    ? "HUMAN-LED SQUADS (humanLed=true) are never ordered. " + offers.map((e) => sentence(e.desc)).join(". ") + ".\n"
    : "HUMAN-LED SQUADS (humanLed=true) are never ordered and get no missions.\n";
  const parts = [], keys = [];
  if (orders.length) {
    const a = orders[0].name, b = (orders[1] ?? orders[0]).name;
    parts.push(`"orders":[{"squad":"<id>","command":"${a}","target":"C","reason":"short"},{"squad":"<id>","command":"${b}","pos":[x,z],"reason":"short"}]`);
    keys.push("orders");
  }
  if (buys.length) { parts.push('"buy":[{"type":"<catalog type>","count":1,"command":"ATTACK","target":"C","reason":"short"}]'); keys.push("buy"); }
  if (fobs.length) { parts.push('"fobs":[{"command":"ESTABLISH_FOB","site":"<fobSites id>","reason":"short"}]'); keys.push("fobs"); }
  if (offers.length) { parts.push('"missions":[{"squad":"<human-led id>","offers":[{"kind":"CAPTURE","target":"D","brief":"..."}]}]'); keys.push("missions"); }
  t += "REPLY with ONLY one JSON object, no prose, no code fences:\n{" + parts.join(",\n ") + "}\n";
  t += (keys.length ? `${keys.join("/")} may be empty or omitted. ` : "") + (buys.length ? "buy: max 3 entries, count 1-3, total cost <= war.supplies, every entry with command + target/pos (the order of the new units). " : "") + (fobs.length ? "fobs: at most 3 entries per reply, at most 1 of them ESTABLISH_FOB or UPGRADE_FOB. " : "") + '{"keep":true} alone = keep all current orders.';
  return t;
}

// The wakeup paragraph is appended only when the model may choose its next wakeup.
function wakeupText(w) {
  return `
You are not polled continuously: you also choose when you are consulted next. Add to the JSON object the optional fields
"next_check_sec" (integer seconds, allowed range ${w.minSec}..${w.maxSec}, default ${w.defaultSec}) and "next_check_reason" (short text), e.g.
{"orders":[...],"next_check_sec":120,"next_check_reason":"quiet, no contacts"}.
You are woken early anyway when something important happens (new contact, casualties, lost squad, objective reached or changed owner, tickets dropping, new units, mission events), so choose
long intervals in quiet phases and short ones during fights or while an assault is developing.`;
}

// Faction SITREP template (real formats: US/NATO SITREP, SMEAC five-paragraph order, SALUTE enemy report; short, player-useful).
const SITREP_KEYS = ["situation", "enemy", "mission", "execution", "logistics"];
const SITREP_TITLES = { situation: "Situation", enemy: "Intel", mission: "Mission", execution: "Execution", logistics: "Logistics" };
const SITREP_CAP = 420, SITREP_HEADLINE_CAP = 140, SITREP_FRESH_SEC = 360;
const SITREP_PROMPT = `
SITREP: the reply MUST also contain a "sitrep" object (last key): a natural NATO/US-style situation report of OUR faction, read aloud to our soldiers. Only what we know. Plain text, \\n for line breaks, no markup. String keys: headline, situation, enemy, mission, execution, logistics.
Style:
- headline: BLUF, max 110 chars: who is ahead, what matters now.
- Words, not data: no raw numbers, coordinates, "age 12s", unit ids (US_Inf2) or faction keys. Say "we/our forces/the enemy", "the mechanized platoon", "our AT team", "two elements". At most one number per section, only if it helps (minutes left, a grid).
- Enemy size from contact "size" (else "vehicles"/"infantry"/"desc"): fire team, squad-, platoon-, company-sized element; single vehicle, vehicle section/platoon, large mechanized force, "with dismounts". Time: just now / last few minutes / earlier. Place: contact "place" (nearest named place or objective) plus the 100 m "grid" if given ("north of Morton, grid 262-288"); objectives as letter + name. Never raw coordinates; no place: "unlocated".
- Trends in words: tickets bleeding / steady / ahead; supplies healthy / tight / dry; strength good / thin / exhausted.
- situation: objectives held/contested/lost, ticket trend, tempo. enemy: SALUTE as plain prose, freshest first, 1-3 sentences. mission: SMEAC who/what/where/why, 1-2 sentences. execution: "Main effort: ... Supporting effort: ... Reserve: ..." then one line on what helps players. logistics: supplies, replacements, vehicles, weak elements, in words.
Example (invented): {"headline":"Holding two of five points; enemy armor massing north, push on Delta now.","situation":"We hold Alpha and Bravo; Charlie is contested. Tickets are bleeding slowly but we remain ahead.","enemy":"Platoon-sized mechanized force with dismounts moving south through Harbor Bridge just now. Squad-sized element at Delta, last few minutes.","mission":"Defeat the enemy at Charlie Mill and secure Delta to deny him the crossing.","execution":"Main effort: mechanized platoon to Charlie. Supporting effort: two rifle elements fix Delta. Reserve: AT team at Bravo. Players: join a squad moving on Charlie.","logistics":"Supplies healthy, income steady, replacements ordered. One element is thin and falling back."}`;

// Help calls (/backup): the game puts the open ones in sit.helpCalls[].
const HELP_CALLS_PROMPT = "HELP CALLS: the situation may carry helpCalls[] ({id, player, squad, grid, pos:[x,z], note, ageSec, answeredBy}) from players asking for backup. An open call (answeredBy empty) is urgent: send or keep a unit there (DEFEND or MOVE to pos) unless impossible, and mention it in the SITREP execution. A call already answered by a squad needs no second unit unless that squad is gone or weak.\n";

// System prompt incl. the wakeup paragraph. v2 = protocol v2 situation. sitrep = ask for the SITREP object too.
// sit (optional) = the situation: adds the HQ personality (sit.hq.trait), the environment paragraph (sit.env) and, with a SITREP, its tone.
function systemPrompt(c, v2 = false, sitrep = false, manifest = DEFAULT_MANIFEST, sit = null) {
  let base = v2 ? SYSTEM_V2 + commandsSection(manifest ?? DEFAULT_MANIFEST) : SYSTEM;
  if (v2) base += "\n" + J.FLANK_PROMPT + " " + J.SQUAD_NAME_PROMPT + (c.memory?.enabled ? " " + J.JOURNAL_RULE : "") + "\n";
  if (v2) base += HELP_CALLS_PROMPT;
  if (v2 && sit) {
    try { base += personaCommander(sit); } catch { /* optional */ }
    try { const env = envPrompt(sit); if (env) base += "\n" + env + "\n"; } catch { /* optional */ }
  }
  if (c.wakeup?.dynamic) base += wakeupText(c.wakeup);
  if (v2 && sitrep) base += SITREP_PROMPT + (sit ? personaSitrep(sit) : "");
  return base;
}

// ---------- config ----------
const cfg = {
  provider: PROVIDERS.includes(process.env.CLW_LLM) ? process.env.CLW_LLM : "fake",
  model: process.env.CLW_MODEL || "claude-sonnet-5-5",
  baseUrl: "",
  apiKey: process.env.ANTHROPIC_API_KEY || "",
  timeoutSec: 20,
  fallback: null,    // backup model of the strong slot: null | {model, provider?, baseUrl?, apiKey?, timeoutSec?} (omitted fields inherit from the slot)
  maxTokens: 1024,
  // weak LLM slot (protocol v4, i18n): provider "none" = no weak slot, the strong slot (the top-level provider/model/baseUrl/apiKey/timeoutSec above) translates
  weak: { provider: "none", model: "", baseUrl: "", apiKey: "", timeoutSec: 10, fallback: null },
  enabled: true,
  engine: "llm",
  minIntervalSec: 0,
  skipUnchanged: true,
  stopAtLimit: true,
  gameDir: "",       // Arma Reforger install dir for the map image; empty = auto-detect
  mapMaxSize: 2048,  // longest side of the served map PNG
  // HQ scheduler: how often the LLM is called (see "HQ scheduler" below)
  wakeup: { dynamic: true, defaultSec: 60, minSec: 20, maxSec: 300, budgetStretch: true },
  // events that wake the HQ before its scheduled time (each tunable; 0 / false = off)
  triggers: { enabled: true, newContact: true, contactNearSquadM: 300, casualties: 2, squadLost: true, strengthDropPct: 25, objectiveReached: true, squadChange: true, cooldownSec: 15,
    objectiveChanged: true, ticketsDropPct: 10, unitSpawned: true, missionEvent: true, idleUnits: true, idleSec: 90 },
  // faction SITREP written by the model inside the commander reply: asked on the first call, every intervalSec at most, and on big events
  sitrep: { enabled: true, intervalSec: 120 },
  // HQ chat (/hq <text>): AI SDK agent turn per player message, see agent/agentTurn.mjs and "HQ chat" below
  agent: { enabled: true, maxSteps: 4, timeoutSec: 40, maxConcurrent: 3 },
  // HQ war journal (intent / goals / lessons written by a small review call every reviewMin minutes + the game-counted attack record), see "HQ war journal" below
  memory: { enabled: true, reviewMin: 5 },
  // broadcast translation (SITREP text_by_lang, POST /translate): translateTimeoutSec per language, then that language is omitted
  i18n: { enabled: true, translateTimeoutSec: 10 },
  // dashboard login: {salt, hash} (scrypt) of the admin password; null = the default password from the README (see auth.mjs)
  admin: null,
  // shared secret of the game channel (header X-CLW-Token on /orders, /manifest, /translate, /world*, /health, /loadouts/*, /control); empty = open
  gameToken: "",
};

// Group settings: "bool" or [min, max] number range per key. Partial objects are merged onto the current group.
const GROUP_SPECS = {
  sitrep: { enabled: "bool", intervalSec: [30, 3600] },
  agent: { enabled: "bool", maxSteps: [1, 8], timeoutSec: [5, 120], maxConcurrent: [1, 10] },
  memory: { enabled: "bool", reviewMin: [1, 60] },
  i18n: { enabled: "bool", translateTimeoutSec: [1, 60] },
  wakeup: { dynamic: "bool", defaultSec: [5, 3600], minSec: [5, 3600], maxSec: [5, 7200], budgetStretch: "bool" },
  triggers: { enabled: "bool", newContact: "bool", contactNearSquadM: [0, 5000], casualties: [0, 1000], squadLost: "bool", strengthDropPct: [0, 100], objectiveReached: "bool", squadChange: "bool", cooldownSec: [0, 600],
    objectiveChanged: "bool", ticketsDropPct: [0, 100], unitSpawned: "bool", missionEvent: "bool", idleUnits: "bool", idleSec: [0, 3600] },
};
function validateGroup(name, v) {
  if (!v || typeof v !== "object" || Array.isArray(v)) return { error: `${name} must be an object` };
  const spec = GROUP_SPECS[name], out = { ...cfg[name] };
  for (const [k, x] of Object.entries(v)) {
    const s = spec[k];
    if (!s) return { error: `unknown setting ${name}.${k}` };
    if (s === "bool") { if (typeof x !== "boolean") return { error: `${name}.${k} must be boolean` }; }
    else if (!isNum(x) || x < s[0] || x > s[1]) return { error: `${name}.${k} must be a number ${s[0]}..${s[1]}` };
    out[k] = x;
  }
  if (name === "wakeup" && out.minSec > out.maxSec) return { error: "wakeup.minSec must be <= wakeup.maxSec" };
  return { value: out };
}

// Slot fields shared by the top-level (strong) config and the weak slot.
function validateSlotField(k, v) {
  switch (k) {
    case "provider":
      if (!PROVIDERS.includes(v)) return { error: `provider must be one of ${PROVIDERS.join(", ")}` };
      return { value: v };
    case "model":
      if (typeof v !== "string" || v.length > 200) return { error: "model must be a string (max 200)" };
      return { value: v.trim() };
    case "baseUrl": {
      if (typeof v !== "string" || v.length > 300) return { error: "baseUrl must be a string" };
      const t = v.trim();
      if (t) {
        let u; try { u = new URL(t); } catch { return { error: "baseUrl is not a valid URL" }; }
        if (u.protocol !== "http:" && u.protocol !== "https:") return { error: "baseUrl must be http(s)" };
      }
      return { value: t.replace(/\/+$/, "") };
    }
    case "apiKey":
      if (typeof v !== "string" || v.length > 500 || /[\r\n]/.test(v)) return { error: "apiKey must be a single-line string" };
      return { value: v.trim() };
    case "timeoutSec":
      if (!isNum(v) || v < 1 || v > 300) return { error: "timeoutSec must be a number 1..300" };
      return { value: v };
  }
  return { error: `unknown setting ${k}` };
}
// Backup model of a slot: null clears; an object is merged onto the current one. model is required; provider/baseUrl/timeoutSec may be ""/null
// (= inherit from the slot); apiKey "" keeps the stored key. Only the fields that are set are stored.
function validateFallback(v, cur, label) {
  if (v === null) return { value: null };
  if (!v || typeof v !== "object" || Array.isArray(v)) return { error: `${label} must be an object or null` };
  const out = { ...(cur ?? {}) };
  for (const [k, x] of Object.entries(v)) {
    if (!["model", "provider", "baseUrl", "apiKey", "timeoutSec"].includes(k)) return { error: `unknown setting ${label}.${k}` };
    if ((k === "provider" || k === "baseUrl") && x === "") { delete out[k]; continue; }
    if (k === "timeoutSec" && x === null) { delete out[k]; continue; }
    const r = validateSlotField(k, x);
    if (r.error) return { error: `${label}.` + r.error };
    if (k === "apiKey" && r.value === "") continue;
    if (k === "baseUrl" && !r.value) { delete out[k]; continue; }
    out[k] = r.value;
  }
  if (!out.model) return { error: `${label}.model is required` };
  return { value: out };
}
// weak slot: partial object merged onto the current slot; null / {provider:"none"} = no weak slot; apiKey "" keeps the stored key.
function validateWeak(v) {
  if (v === null) return { value: { ...cfg.weak, provider: "none" } };
  if (!v || typeof v !== "object" || Array.isArray(v)) return { error: "weak must be an object or null" };
  const out = { ...cfg.weak };
  for (const [k, x] of Object.entries(v)) {
    if (k === "provider" && x === "none") { out.provider = "none"; continue; }
    if (k === "fallback") {
      const f = validateFallback(x, cfg.weak.fallback, "weak.fallback");
      if (f.error) return f;
      out.fallback = f.value; continue;
    }
    if (!["provider", "model", "baseUrl", "apiKey", "timeoutSec"].includes(k)) return { error: `unknown setting weak.${k}` };
    const r = validateSlotField(k, x);
    if (r.error) return { error: "weak." + r.error };
    if (k === "apiKey" && r.value === "") continue;
    out[k] = r.value;
  }
  return { value: out };
}
function validateConfig(inp) {
  if (!inp || typeof inp !== "object" || Array.isArray(inp)) return { error: "body must be a JSON object" };
  const out = {};
  for (const [k, v] of Object.entries(inp)) {
    switch (k) {
      case "provider": case "model": case "baseUrl": case "apiKey": case "timeoutSec": {
        const r = validateSlotField(k, v);
        if (r.error) return r;
        out[k] = r.value; break;
      }
      case "strong": { // the strong slot IS the top-level provider/model/baseUrl/apiKey/timeoutSec (backward compatible); this is a grouped way to set it
        if (!v || typeof v !== "object" || Array.isArray(v)) return { error: "strong must be an object" };
        for (const [sk, sv] of Object.entries(v)) {
          if (sk === "fallback") {
            const f = validateFallback(sv, cfg.fallback, "strong.fallback");
            if (f.error) return f;
            out.fallback = f.value; continue;
          }
          const r = validateSlotField(sk, sv);
          if (r.error) return { error: "strong." + r.error };
          out[sk] = r.value;
        }
        break;
      }
      case "fallback": { // top-level way to set the strong slot's backup model (same as strong.fallback)
        const f = validateFallback(v, cfg.fallback, "fallback");
        if (f.error) return f;
        out.fallback = f.value; break;
      }
      case "weak": {
        const r = validateWeak(v);
        if (r.error) return r;
        out.weak = r.value; break;
      }
      case "admin": // only loaded from the saved file; the password is changed through POST /api/password
        if (v !== null && !AUTH.validAdmin(v)) return { error: "admin must be null or {salt, hash}" };
        out.admin = v === null ? null : { salt: v.salt, hash: v.hash }; break;
      case "gameToken":
        if (typeof v !== "string" || v.length > 200 || /[\s\u0000-\u001f]/.test(v)) return { error: "gameToken must be a string without whitespace (max 200), empty = off" };
        out.gameToken = v; break;
      case "maxTokens":
        if (!Number.isInteger(v) || v < 16 || v > 32000) return { error: "maxTokens must be an integer 16..32000" };
        out.maxTokens = v; break;
      case "enabled":
        if (typeof v !== "boolean") return { error: "enabled must be boolean" };
        out.enabled = v; break;
      case "engine":
        if (!ENGINES.includes(v)) return { error: `engine must be one of ${ENGINES.join(", ")}` };
        out.engine = v; break;
      case "minIntervalSec":
        if (!isNum(v) || v < 0 || v > 600) return { error: "minIntervalSec must be a number 0..600" };
        out.minIntervalSec = v; break;
      case "skipUnchanged":
        if (typeof v !== "boolean") return { error: "skipUnchanged must be boolean" };
        out.skipUnchanged = v; break;
      case "stopAtLimit":
        if (typeof v !== "boolean") return { error: "stopAtLimit must be boolean" };
        out.stopAtLimit = v; break;
      case "gameDir":
        if (typeof v !== "string" || v.length > 400 || /[\r\n]/.test(v)) return { error: "gameDir must be a single-line string" };
        out.gameDir = v.trim(); break;
      case "mapMaxSize":
        if (!Number.isInteger(v) || v < 256 || v > 8192) return { error: "mapMaxSize must be an integer 256..8192" };
        out.mapMaxSize = v; break;
      case "wakeup":
      case "sitrep":
      case "agent":
      case "memory":
      case "i18n":
      case "triggers": {
        const g = validateGroup(k, v);
        if (g.error) return g;
        out[k] = g.value; break;
      }
      default:
        return { error: `unknown setting ${k}` };
    }
  }
  return { value: out };
}

// A saved openai-compatible config that points at openrouter.ai is the openrouter provider (key is kept).
function migrateOpenRouter(c) {
  if (c.provider === "openai-compatible" && /openrouter\.ai/i.test(c.baseUrl)) { c.provider = "openrouter"; c.baseUrl = ""; console.log("migrated provider openai-compatible (openrouter.ai) -> openrouter"); }
}

try {
  const saved = validateConfig(JSON.parse(fs.readFileSync(CONFIG_FILE, "utf8")));
  if (saved.value) { Object.assign(cfg, saved.value); migrateOpenRouter(cfg); }
  else console.log(`ignoring ${CONFIG_FILE}: ${saved.error}`);
} catch (e) {
  if (e.code !== "ENOENT") console.log(`could not read ${CONFIG_FILE}: ${e.message}`);
}

function saveConfig() {
  try { fs.writeFileSync(CONFIG_FILE, JSON.stringify(cfg, null, 2)); return true; }
  catch (e) { console.log(`could not save config: ${e.message}`); return false; }
}

function publicConfig() {
  const { apiKey, weak, fallback, admin, gameToken, ...rest } = cfg;
  const slot = (o) => ({ provider: o.provider, model: o.model, baseUrl: o.baseUrl, timeoutSec: o.timeoutSec, apiKeySet: !!o.apiKey, apiKeyLast4: o.apiKey ? o.apiKey.slice(-4) : "", fallback: FB.publicFallback(o.fallback) });
  // strong = the top-level provider/model/baseUrl/apiKey/timeoutSec (kept for old clients); weak.provider "none" = no weak slot (strong translates)
  return { ...rest, gameTokenSet: !!gameToken, apiKeySet: !!apiKey, apiKeyLast4: apiKey ? apiKey.slice(-4) : "", fallback: FB.publicFallback(fallback), strong: slot(cfg), weak: slot(weak), roles: ROLE_SLOT, translateSlot: weak.provider !== "none" ? "weak" : "strong" };
}

// ---------- state ----------
const state = {
  startedAt: Date.now(),
  factions: {},   // name -> {ts, situation, orders}
  exchanges: [],  // newest first, max RING_MAX
  counters: { requests: 0, gated: 0, skipped: 0, errors: 0, fallbacks: 0, inputTokens: 0, outputTokens: 0, modelFallbacks: { strong: 0, weak: 0 } }, // fallbacks = rule-commander fallbacks; modelFallbacks = calls answered by the backup model
  lastRequestTs: 0,
  debugLog: [],   // game debug lines (CLW_Debug), newest first, max DEBUG_MAX, deduped across the factions' posts
};
const DEBUG_MAX = 300;
const debugSeen = new Set();
function addDebugLines(lines) {
  if (!Array.isArray(lines)) return;
  for (const l of lines.slice(0, 50)) {
    if (!l || typeof l.msg !== "string") continue;
    const msg = l.msg.slice(0, 200), t = isNum(l.t) ? l.t : 0, key = `${l.id}|${t}|${msg}`;
    if (debugSeen.has(key)) continue;
    debugSeen.add(key);
    state.debugLog.unshift({ ts: Date.now(), t, msg });
  }
  if (state.debugLog.length > DEBUG_MAX) state.debugLog.length = DEBUG_MAX;
  if (debugSeen.size > DEBUG_MAX * 4) { debugSeen.clear(); for (const d of state.debugLog) debugSeen.add(`|${d.t}|${d.msg}`); }
}

// ---------- OpenRouter usage / budget ----------
const POLL_MS = 10 * 60 * 1000, POLL_REACHED_MS = 60 * 1000;
const session = { cost: 0, requests: 0, tokensIn: 0, tokensOut: 0, byModel: {}, byFaction: {} };
const keyState = { data: null, ts: 0, error: null, polling: false, costSincePoll: 0 };
let budgetLevel = "ok"; // last logged level: ok | warn | crit | reached

function recordUsage(model, faction, u) {
  const cost = isNum(u?.cost) ? u.cost : 0;
  session.cost += cost; session.tokensIn += u?.input ?? 0; session.tokensOut += u?.output ?? 0;
  keyState.costSincePoll += cost;
  const add = (m, k) => { const e = (m[k] ??= { cost: 0, requests: 0 }); e.cost += cost; e.requests++; };
  add(session.byModel, model || "?"); add(session.byFaction, faction || "?");
  checkBudget();
}

// Budget of the active key limit: polled limit_remaining minus local cost since that poll. null = no limit known.
function budget() {
  const d = keyState.data;
  if (cfg.provider !== "openrouter" || !d || !isNum(d.limit) || d.limit <= 0 || !isNum(d.limit_remaining)) return null;
  const remaining = d.limit_remaining - keyState.costSincePoll;
  const pct = Math.max(0, Math.min(100, 100 * (d.limit - remaining) / d.limit));
  const level = remaining <= 0 ? "reached" : pct >= 95 ? "crit" : pct >= 80 ? "warn" : "ok";
  return { limit: d.limit, remaining, pct, level, resetting: d.limit_reset ?? null };
}
// Spending pace. Assumption: OpenRouter resets on UTC boundaries (daily 00:00, weekly Monday 00:00, monthly 1st 00:00).
// Period limit: projected = u / elapsedFraction (needs f >= 0.05 and u > 0). No reset: session burn rate (needs >= 5 min). No limit: $/h only.
function periodBounds(reset, now) {
  const d = new Date(now), y = d.getUTCFullYear(), m = d.getUTCMonth(), day = d.getUTCDate();
  if (reset === "daily") { const s = Date.UTC(y, m, day); return [s, s + 86400000]; }
  if (reset === "weekly") { const s = Date.UTC(y, m, day - ((d.getUTCDay() + 6) % 7)); return [s, s + 7 * 86400000]; }
  if (reset === "monthly") return [Date.UTC(y, m, 1), Date.UTC(y, m + 1, 1)];
  return null;
}
function pace() {
  const d = keyState.data;
  if (cfg.provider !== "openrouter" || !d) return null;
  const now = Date.now(), hours = (now - state.startedAt) / 3600000;
  const rate = hours * 60 >= 5 && session.cost > 0 ? session.cost / hours : null;
  const hasLimit = isNum(d.limit) && d.limit > 0;
  if (!hasLimit) return { kind: "rate", ratePerHour: rate };
  const bounds = periodBounds(d.limit_reset, now);
  if (!bounds) {
    const remaining = (d.limit_remaining ?? (d.limit - (d.usage ?? 0))) - keyState.costSincePoll;
    return { kind: "rate", ratePerHour: rate, hoursToLimit: rate && remaining > 0 ? remaining / rate : null };
  }
  const [start, end] = bounds, f = (now - start) / (end - start);
  const u = (d[{ daily: "usage_daily", weekly: "usage_weekly", monthly: "usage_monthly" }[d.limit_reset]] ?? 0) + keyState.costSincePoll;
  const out = { kind: "period", reset: d.limit_reset, elapsed: f, expected: d.limit * f, used: u, resetsInMs: end - now, projected: null, delta: null };
  if (f >= 0.05 && u > 0) { out.projected = u / f; out.delta = out.projected - d.limit; }
  return out;
}
const LEVELS = ["ok", "warn", "crit", "reached"];
function checkBudget() {
  const b = budget(), level = b ? b.level : "ok";
  if (level !== budgetLevel) {
    if (LEVELS.indexOf(level) > LEVELS.indexOf(budgetLevel)) {
      console.log(`BUDGET ${level === "reached" ? "LIMIT REACHED" : level === "crit" ? "95% used" : "80% used"}: $${(b.limit - b.remaining).toFixed(4)} of $${b.limit} (${b.pct.toFixed(1)}%)${level === "reached" && cfg.stopAtLimit ? " - provider calls stopped" : ""}`);
    } else console.log(`budget back to ${level}`);
    budgetLevel = level;
  }
}

const KEY_FIELDS = ["usage", "usage_daily", "usage_weekly", "usage_monthly", "limit", "limit_remaining"];
async function pollKey() {
  if (cfg.provider !== "openrouter" || !cfg.apiKey || keyState.polling) return;
  keyState.polling = true;
  const ac = new AbortController(), timer = setTimeout(() => ac.abort(), 10000);
  try {
    const r = await fetch((cfg.baseUrl || OR_BASE) + "/key", { signal: ac.signal, headers: { authorization: "Bearer " + cfg.apiKey } });
    if (!r.ok) throw new Error(`HTTP ${r.status}: ${(await r.text()).slice(0, 150)}`);
    const d = (await r.json())?.data ?? {};
    const k = { label: typeof d.label === "string" ? d.label : "", limit_reset: typeof d.limit_reset === "string" ? d.limit_reset : null, is_free_tier: !!d.is_free_tier };
    for (const f of KEY_FIELDS) k[f] = isNum(d[f]) ? d[f] : null;
    if (k.limit !== null && k.limit_remaining === null) k.limit_remaining = k.limit - (k.usage ?? 0); // tolerate a missing field
    keyState.data = k; keyState.error = null; keyState.costSincePoll = 0;
  } catch (e) {
    keyState.error = e?.name === "AbortError" ? "timeout" : String(e?.message ?? e);
    console.log(`  key poll failed: ${keyState.error}`);
  } finally { clearTimeout(timer); keyState.polling = false; keyState.ts = Date.now(); checkBudget(); }
}
setInterval(() => {
  const age = Date.now() - keyState.ts;
  if (age >= POLL_MS || (budgetLevel === "reached" && age >= POLL_REACHED_MS)) void pollKey();
}, 15000).unref();

function usageView() {
  const on = cfg.provider === "openrouter";
  return {
    enabled: on, session, stopAtLimit: cfg.stopAtLimit,
    key: on ? { data: keyState.data, ts: keyState.ts, error: keyState.error, polling: keyState.polling, costSincePoll: keyState.costSincePoll, keySet: !!cfg.apiKey } : null,
    budget: on ? (budget() ? { ...budget(), pace: pace() } : null) : null,
    pace: on ? pace() : null, // also present without a limit (rate only)
  };
}
const budgetStopped = (c) => c.provider === "openrouter" && c.stopAtLimit && budget()?.level === "reached";

// ---------- faction SITREP ----------
const clipText = (v, cap) => (typeof v === "string" ? scrubSitrep(v).replace(/\r/g, "").replace(/\t/g, " ").replace(/\\n/g, "\n").trim().slice(0, cap) : "");
// Normalises a SITREP object from the model or the game: {headline, situation, enemy, mission, execution, logistics} or null.
function normSitrep(x) {
  if (!x || typeof x !== "object" || Array.isArray(x)) return null;
  const out = { headline: clipText(x.headline, SITREP_HEADLINE_CAP) };
  let any = !!out.headline;
  for (const k of SITREP_KEYS) { out[k] = clipText(x[k], SITREP_CAP); if (out[k]) any = true; }
  return any ? out : null;
}
// The dashboard / API shape: {text, headline, sections:[{id,title,body}], scope, unit, source, generatedAt, warTimeSec}.
function sitrepContract(n, unit, source, generatedAt, warTimeSec) {
  const sections = SITREP_KEYS.filter((k) => n[k]).map((k) => ({ id: k, title: SITREP_TITLES[k], body: n[k] }));
  const text = [n.headline, ...sections.map((x) => x.title + ": " + x.body)].filter(Boolean).join("\n");
  return { text, headline: n.headline, sections, scope: "faction", unit, source, generatedAt, warTimeSec: isNum(warTimeSec) ? warTimeSec : null };
}
const clock = (sec) => { const t = Math.max(0, Math.round(sec)); return Math.floor(t / 60) + ":" + String(t % 60).padStart(2, "0"); };
// Template-based SITREP from a protocol v2 situation (fog-limited by construction). Used by the fake provider and as the rules
// fallback when the game sent none. Mirrors CLW_SitrepBuilder.c in the game, shorter.
// Plain-words helpers for the SITREP (no raw numbers / ids). Squad "kind"/"type" come from the game; unknown kinds read as "element".
const unitWord = (q) => {
  const t = String(q?.type ?? "").toLowerCase(), k = String(q?.kind ?? "").toLowerCase();
  if (/\bat\b|anti/.test(t)) return "AT team";
  return ({ infantry: "rifle element", light: "light vehicle section", mech: "mechanized element", armor: "armor element", motorized: "motorized element", air: "air element" })[k] ?? "element";
};
// Strips leftover raw "(1234,5678)" coordinate pairs and "age 12s" fragments the model may still emit.
const scrubSitrep = (s) => s.replace(/\(\s*-?\d{2,6}(?:\.\d+)?\s*,\s*-?\d{2,6}(?:\.\d+)?(?:\s*,\s*-?\d{1,6}(?:\.\d+)?)?\s*\)/g, "").replace(/,?\s*\bage:?\s*\d+\s*s\b/gi, "").replace(/[ \t]{2,}/g, " ").replace(/\s+([,.;])/g, "$1");
// Nearest named place (world locations the game posted) for contacts that lack "place"; the game's "place"/"grid" always win.
function annotateContacts(sit) {
  if (!sit || !Array.isArray(sit.contacts) || !sit.contacts.some((c) => c && typeof c.place !== "string" && okPos(c.pos))) return sit;
  let locs = null;
  try { locs = worldState.key ? loadVectors(worldState.key).locations : null; } catch { locs = null; }
  if (!Array.isArray(locs) || !locs.length) return sit;
  const contacts = sit.contacts.map((c) => {
    if (!c || typeof c.place === "string" || !okPos(c.pos)) return c;
    const p = xz(c.pos);
    let best = null, bd = 2500;
    for (const l of locs) { const d = dist2(p, l.pos); if (d < bd) { bd = d; best = l; } }
    return best ? { ...c, place: best.name } : c;
  });
  return { ...sit, contacts };
}
const infWord = (n) => (n <= 4 ? "fire team" : n <= 12 ? "squad-sized element" : n <= 40 ? "platoon-sized element" : "company-sized element");
const vehWord = (n) => (n <= 1 ? "single vehicle" : n <= 3 ? "vehicle section" : n <= 8 ? "vehicle platoon" : "large mechanized force");
function contactSize(c) {
  if (typeof c.size === "string" && c.size) return c.size;
  let v = isNum(c.vehicles) ? c.vehicles : 0, i = isNum(c.infantry) ? c.infantry : 0;
  if (!v && !i && typeof c.desc === "string") { const m = /(\d+)x\s.*?(vehicle|infantry)/i.exec(c.desc); if (m) { if (m[2].toLowerCase() === "vehicle") v = +m[1]; else i = +m[1]; } }
  if (v && i) return vehWord(v) + " with dismounts";
  return v ? vehWord(v) : i ? infWord(i) : "unidentified element";
}
const cap1 = (s) => s[0].toUpperCase() + s.slice(1);
function fakeSitrep(sit, plan = null) {
  sit = annotateContacts(sit);
  const me = sit.faction, war = sit.war ?? {};
  const objs = (Array.isArray(sit.objectives) ? sit.objectives : []).filter((o) => o && typeof o.id === "string");
  const squads = (Array.isArray(sit.squads) ? sit.squads : []).filter((q) => q && typeof q.id === "string");
  const contacts = (Array.isArray(sit.contacts) ? sit.contacts : []).filter((c) => c && okPos(c.pos));
  const label = (o) => (o.name ? o.id + " (" + o.name + ")" : o.id);
  const list = (a) => (a.length < 2 ? a.join("") : a.slice(0, -1).join(", ") + " and " + a[a.length - 1]);
  const held = objs.filter((o) => o.owner === me), enemyHeld = objs.filter((o) => o.owner && o.owner !== "none" && o.owner !== me), open = objs.filter((o) => !o.owner || o.owner === "none");
  const contested = objs.filter((o) => o.contested);
  const tk = war.tickets, et = war.enemyTickets;
  const lead = isNum(tk) && isNum(et) && tk >= 0 && et >= 0 ? (tk > et * 1.15 ? "ahead" : tk * 1.15 < et ? "behind" : "level") : null;
  const nearObj = (pos) => { // objective label within 400 m of the contact, else ""
    let best = null, bd = 400;
    for (const o of objs) if (okPos(o.pos)) { const d = dist2(xz(pos), xz(o.pos)); if (d < bd) { bd = d; best = o; } }
    return best ? label(best) : "";
  };
  const n = {};
  const bluf = !objs.length ? "no objectives in play" : held.length * 2 > objs.length ? "we hold most of the map" : held.length ? "we hold part of the map" : "we hold no objective";
  n.headline = `${cap1(bluf)}${lead === "ahead" ? ", tickets in our favor" : lead === "behind" ? ", tickets running against us" : ""}${contested.length ? "; " + contested.map((o) => o.id).join("/") + " contested" : ""}.`;
  n.situation = (held.length ? `We hold ${list(held.map(label))}.` : "We hold no objective.") + (enemyHeld.length ? ` The enemy holds ${list(enemyHeld.map(label))}.` : "") + (open.length ? ` ${cap1(list(open.map(label)))} still open.` : "")
    + (contested.length ? ` ${cap1(list(contested.map(label)))} contested.` : "") + (lead ? ` Tickets: we are ${lead === "level" ? "level with the enemy" : lead === "ahead" ? "ahead of the enemy" : "behind the enemy"}.` : "")
    + (isNum(sit.timeLeftSec) && sit.timeLeftSec > 0 ? ` About ${Math.round(sit.timeLeftSec / 60)} minutes remain.` : "");
  const when = (a) => (a < 20 ? "just now" : a < 300 ? "in the last few minutes" : "earlier");
  const cs = contacts.slice().sort((a, b) => (a.ageSec ?? 0) - (b.ageSec ?? 0)).slice(0, 3);
  n.enemy = cs.length ? cs.map((c) => {
    const where = (typeof c.place === "string" && c.place) || nearObj(c.pos);
    return `${cap1(contactSize(c))} ${where ? "near " + where : "unlocated"}${typeof c.grid === "string" && c.grid ? ", grid " + c.grid : ""}, ${when(c.ageSec ?? 0)}.`;
  }).join(" ") : "No enemy contact at this time.";
  const tasked = (plan?.orders?.length ? plan.orders.map((o) => ({ q: squads.find((s) => s.id === o.squad), objective: { command: o.command, target: o.target } })) : squads.filter((q) => q.objective && q.objective.status === "active").map((q) => ({ q, objective: q.objective })));
  const groups = new Map();
  for (const t of tasked) { const k = t.objective.target ?? "the next position"; (groups.get(k) ?? groups.set(k, []).get(k)).push(t); }
  const gs = [...groups.entries()].sort((a, b) => b[1].length - a[1].length);
  const tname = (k) => { const o = objs.find((x) => x.id === k); return o ? label(o) : k; };
  const crew = (arr) => { const u = [...new Set(arr.map((t) => unitWord(t.q)))]; return u.length === 1 ? (arr.length > 1 ? (arr.length === 2 ? "two " : "several ") + u[0] + "s" : "the " + u[0]) : "a mixed force"; };
  const verb = (arr) => (arr.some((t) => /ATTACK/i.test(t.objective.command)) ? "attacks" : arr.some((t) => /DEFEND/i.test(t.objective.command)) ? "holds" : "moves on");
  const idle = squads.filter((q) => !tasked.some((t) => t.q === q));
  n.mission = gs.length ? `Seize and hold ${tname(gs[0][0])}, deny the enemy the initiative and preserve our tickets.` : "Consolidate, scout and take or hold the objectives to preserve our tickets.";
  n.execution = (gs[0] ? `Main effort: ${crew(gs[0][1])} ${verb(gs[0][1])} ${tname(gs[0][0])}.` : "No main effort assigned yet.")
    + (gs[1] ? ` Supporting effort: ${crew(gs[1][1])} ${verb(gs[1][1])} ${tname(gs[1][0])}.` : "") + (idle.length ? ` Reserve: ${idle.length > 1 ? "several elements" : "one element"} in position.` : "")
    + (gs[0] ? ` Players: join a squad moving on ${tname(gs[0][0])}.` : "");
  const minCost = Math.min(...(Array.isArray(sit.catalog) ? sit.catalog.map((e) => e?.cost).filter(isNum) : []), Infinity);
  const sup = !isNum(war.supplies) ? "" : war.supplies < minCost ? "Supplies are dry" : war.supplies < minCost * 3 ? "Supplies are tight" : "Supplies are healthy";
  const str = isNum(war.liveAI) && isNum(war.aiCap) && war.aiCap > 0 ? (war.liveAI / war.aiCap > 0.7 ? "force strength good" : war.liveAI / war.aiCap > 0.35 ? "force strength moderate" : "force strength thin") : "";
  const weak = squads.filter((q) => isNum(q.agents) && isNum(q.startAgents) && q.startAgents > 0 && q.agents < q.startAgents * 0.5);
  n.logistics = [sup && isNum(war.income) && war.income > 0 ? sup + ", income steady" : sup, str].filter(Boolean).join("; ") + (sup || str ? "." : "") + (weak.length ? ` ${weak.length > 1 ? "Several elements are" : "One element is"} badly worn and needs relief.` : " No element needs relief.");
  return n;
}
// Per faction: the last model SITREP, the rules cache (generatedAt only moves when the content changes), and when the model last wrote one.
const sitrepState = {}; // faction -> {lastAt, llm:{value,generatedAt,warTimeSec}|null, rules:{sig,value,generatedAt}|null}
const srFor = (f) => (sitrepState[f] ??= { lastAt: 0, llm: null, rules: null });
function composeSitrep(faction, sit, gameSitrep) {
  if (!isV2(sit)) return null;
  const st = srFor(faction), nowMs = Date.now();
  const g = normSitrep(gameSitrep) ?? normSitrep(fakeSitrep(sit));
  if (g) {
    const sig = JSON.stringify(SITREP_KEYS.map((k) => g[k]));
    if (!st.rules || st.rules.sig !== sig) st.rules = { sig, value: g, generatedAt: nowMs, warTimeSec: sit.time };
    else { st.rules.value = g; st.rules.warTimeSec = sit.time; }
  }
  const age = st.llm && isNum(sit.time) ? sit.time - st.llm.warTimeSec : Infinity;
  if (st.llm && age >= -5 && age <= SITREP_FRESH_SEC) return { ...sitrepContract(st.llm.value, faction, "llm", st.llm.generatedAt, st.llm.warTimeSec), ...(st.llm.textByLang ? { text_by_lang: st.llm.textByLang } : {}) };
  if (st.rules) return sitrepContract(st.rules.value, faction, "rules", st.rules.generatedAt, st.rules.warTimeSec);
  return null;
}

// ---------- i18n (protocol v4) ----------
// Roles -> slots. Every LLM call names a role; only "translate" maps to the weak slot (no weak slot configured -> strong translates).
const ROLE_SLOT = { commander: "strong", journal: "strong", hq: "strong", translate: "weak" };
function slotCfg(c, role = "strong") {
  const w = c.weak;
  if (ROLE_SLOT[role] === "weak" && w && w.provider && w.provider !== "none") return { ...c, provider: w.provider, model: w.model || c.model, baseUrl: w.baseUrl, apiKey: w.apiKey, timeoutSec: w.timeoutSec || c.timeoutSec };
  return c;
}
// ---------- model fallback ----------
// Every slot (strong, weak) may carry a backup model (config "fallback", see agent/fallback.mjs). callWithFallback() is the one place that
// routes a call: primary first, ONE retry on the backup after a retryable failure (429, 5xx, timeout, network error, cooling down).
// Cooldown state is per (slot, model): a rate-limited primary does not block the backup, and while it cools down calls go straight to the
// backup until the cooldown expires, then the primary is tried again. No backup configured = exactly the old single-model behaviour.
const RATE_COOLDOWN_MS = Number(process.env.CLW_RATE_COOLDOWN_MS) || 20000; // after a 429/5xx the model is skipped for this long (or its Retry-After); the env override is for tests
const cooldowns = new Map();   // "<slot>|<model>" -> until (epoch ms)
const coolKey = (slot, c) => slot + "|" + c.model;
const coolLeftMs = (slot, c) => Math.max(0, (cooldowns.get(coolKey(slot, c)) ?? 0) - Date.now());
/** { slot: "strong"|"weak" (the slot that really answers), primary: full provider config, fallback: full provider config | null } */
function slotChain(c, role = "strong") {
  const weak = ROLE_SLOT[role] === "weak" && c.weak?.provider && c.weak.provider !== "none";
  const primary = weak ? slotCfg(c, role) : c;
  return { slot: weak ? "weak" : "strong", primary, fallback: primary.provider === "fake" ? null : FB.resolveFallback(primary, weak ? c.weak.fallback : c.fallback) };
}
function noteCooldown(slot, c, err, hasFallback) {
  const ms = FB.cooldownFor(err, RATE_COOLDOWN_MS, hasFallback);
  if (ms > 0) cooldowns.set(coolKey(slot, c), Date.now() + ms);
}
function noteFellBack(slot, primaryModel, err) {
  state.counters.modelFallbacks[slot]++;
  console.log(`  ${slot} model ${primaryModel} failed (${String(err?.message ?? err).slice(0, 120)}): using the backup model`);
}
/** True when no model of the slot can be called right now (all cooling down). */
function slotBlocked(c, role = "strong") {
  const ch = slotChain(c, role);
  return ch.primary.provider !== "fake" && coolLeftMs(ch.slot, ch.primary) > 0 && (!ch.fallback || coolLeftMs(ch.slot, ch.fallback) > 0);
}
/** callProvider through the slot chain. Result: callProvider's {raw, usage} + {modelUsed, provider, fellBack, primaryError}. On total failure the thrown error carries primaryError / fellBack. opts.test = primary only, no cooldown bookkeeping. */
async function callWithFallback(c, sit, opts = {}) {
  const ch = slotChain(c, opts.role ?? "strong");
  const cools = !opts.test && (!!ch.fallback || opts.role === "commander" || opts.role === "journal"); // without a backup only the commander/journal calls use the cooldown (as before)
  const attempt = async (cx) => {
    if (cools && cx.provider !== "fake") { const left = coolLeftMs(ch.slot, cx); if (left > 0) throw Object.assign(new Error(`provider cooling down after rate limit/server error (${Math.ceil(left / 1000)}s left)`), { cooling: true }); }
    try { const p = await callProvider(c, sit, { ...opts, slotCfg: cx }); return { ...p, modelUsed: cx.model, provider: cx.provider }; }
    catch (e) { if (cools) noteCooldown(ch.slot, cx, e, !!ch.fallback); throw e; }
  };
  try { return { ...(await attempt(ch.primary)), fellBack: false, primaryError: null }; }
  catch (e) {
    if (opts.test || !ch.fallback || !FB.isRetryable(e)) throw e;
    const primaryError = String(e?.message ?? e).slice(0, 200);
    noteFellBack(ch.slot, ch.primary.model, e);
    try { return { ...(await attempt(ch.fallback)), fellBack: true, primaryError }; }
    catch (e2) { e2.fellBack = true; e2.primaryError = primaryError; e2.modelUsed = null; throw e2; }
  }
}
function cooldownsView() { // "<slot>|<model>" -> seconds left (only the ones still cooling down)
  const out = {};
  for (const [k, until] of cooldowns) { const left = Math.ceil((until - Date.now()) / 1000); if (left > 0) out[k] = left; else cooldowns.delete(k); }
  return out;
}
const translator = I18N.createTranslator({
  callModel: async ({ system, userMsg, maxTokens, timeoutSec }) => {
    const sc = slotCfg(cfg, "translate");
    if (budgetStopped(sc)) throw new Error("budget limit reached");
    session.requests++;
    const p = await callWithFallback(cfg, { faction: "i18n" }, { system, userMsg, maxTokens, timeoutSec, role: "translate" });
    if (p.provider === "openrouter") recordUsage(p.modelUsed, "i18n", p.usage);
    return p;
  },
  isFake: () => slotCfg(cfg, "translate").provider === "fake",
  timeoutMs: () => cfg.i18n.translateTimeoutSec * 1000,
  attempts: () => (slotChain(cfg, "translate").fallback ? 2 : 1), // the budget is per attempt: a hanging primary must leave time for the backup
  log: (m) => console.log("  " + m),
});
const sitrepFields = (sr) => ({ headline: sr.headline, ...Object.fromEntries(SITREP_KEYS.map((k) => [k, sr[k]])) });
// POST /translate {texts:{name:"english text"}, langs:["de",..]} -> {ok, text_by_lang:{en:{..}, de:{..}}, failed:[{lang,reason}], cached:[lang]}
// Generic entry for broadcast texts the game produces itself (announcements); same translator, cache and timeout as the SITREP.
async function handleTranslate(req, res) {
  let b;
  try { b = JSON.parse(await readBody(req)); } catch { return send(res, 400, { error: "invalid JSON" }); }
  const langs = I18N.normLangs(b?.langs);
  if (!b || typeof b.texts !== "object" || !b.texts || Array.isArray(b.texts)) return send(res, 400, { error: "texts must be an object {name: text}" });
  if (!cfg.i18n.enabled || !langs.length) return send(res, 200, { ok: true, text_by_lang: { en: b.texts }, failed: [], cached: [] });
  const tr = await translator.translateFields(b.texts, langs);
  state.counters.errors += tr.failed.length;
  return send(res, 200, { ok: true, text_by_lang: tr.by_lang, failed: tr.failed, cached: tr.cached });
}

// ---------- providers ----------
function fakeOrders(sit) {
  const orders = [];
  const contacts = Array.isArray(sit.contacts) ? sit.contacts.filter((c) => Array.isArray(c?.pos) && isNum(c.pos[0]) && isNum(c.pos[2])) : [];
  if (!contacts.length) return orders;
  for (const s of sit.squads) {
    if (!Array.isArray(s?.pos) || !isNum(s.pos[0]) || !isNum(s.pos[2])) continue;
    if (s.agents < s.startAgents * 0.5) {
      orders.push({ squad: s.id, command: "DEFEND", pos: [s.pos[0], s.pos[2]], reason: "weak squad, holding position" });
      continue;
    }
    let best = null, bd = Infinity;
    for (const c of contacts) {
      const d = Math.hypot(c.pos[0] - s.pos[0], c.pos[2] - s.pos[2]);
      if (d < bd) { bd = d; best = c; }
    }
    orders.push({ squad: s.id, command: "ATTACK", pos: [best.pos[0], best.pos[2]], reason: `nearest contact: ${best.desc ?? "unknown"}` });
  }
  return orders;
}

// Fake provider, protocol v2: simple doctrine, content-agnostic (everything from the situation).
const xz = (p) => (Array.isArray(p) ? (p.length > 2 ? [p[0], p[2]] : [p[0], p[1]]) : [NaN, NaN]);
const dist2 = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);
function fakeV2(sit, manifest = DEFAULT_MANIFEST) {
  const me = sit.faction, out = { orders: [], buy: [], missions: [], fobs: [] };
  const names = orderNamesOf(manifest), canBuy = channelOf(manifest, "buy").length > 0, canOffer = channelOf(manifest, "missions").length > 0;
  const objs = (Array.isArray(sit.objectives) ? sit.objectives : []).filter((o) => o && typeof o.id === "string" && okPos(o.pos));
  const squads = (Array.isArray(sit.squads) ? sit.squads : []).filter((s) => s && typeof s.id === "string" && okPos(s.pos));
  const contacts = (Array.isArray(sit.contacts) ? sit.contacts : []).filter((c) => c && okPos(c.pos));
  const war = sit.war ?? {}, supplies = isNum(war.supplies) ? war.supplies : 0;
  const myBase = (Array.isArray(sit.bases) ? sit.bases : []).find((b) => b?.faction === me && okPos(b.pos));
  const base = myBase ? xz(myBase.pos) : squads.length ? xz(squads[0].pos) : [0, 0];

  // buy: the cheapest entry of every kind, rotated by game time; keep a 100 reserve
  const catalog = (Array.isArray(sit.catalog) ? sit.catalog : []).filter((e) => e && typeof e.type === "string" && isNum(e.cost) && e.cost > 0);
  if (canBuy && supplies > 200 && catalog.length && !(isNum(war.aiCap) && isNum(war.liveAI) && war.liveAI >= war.aiCap) && !(war.engineAI && isNum(war.engineAI.headroom) && war.engineAI.headroom < 3)) {
    const cheapest = new Map();
    for (const e of catalog) if (!cheapest.has(e.kind) || e.cost < cheapest.get(e.kind).cost) cheapest.set(e.kind, e);
    const list = [...cheapest.values()].sort((a, b) => a.cost - b.cost);
    const vehFull = isNum(war.vehicles) && isNum(war.vehicleCap) && war.vehicles >= war.vehicleCap;
    let budget = supplies - 100;
    const rot = list.length ? Math.floor((isNum(sit.time) ? sit.time : 0) / 60) % list.length : 0;
    // spawn on assignment: buy only for an objective that is not ours, and only when no unordered non-garrison squad could take it
    const idleOwn = (Array.isArray(sit.squads) ? sit.squads : []).filter((s) => s && !s.humanLed && !s.garrison && !s.reserved && !s.objective);
    const buyTarget = (Array.isArray(sit.objectives) ? sit.objectives : []).filter((o) => o && typeof o.id === "string" && o.owner !== me)[0];
    for (let i = 0; i < list.length && out.buy.length < 3 && buyTarget && !idleOwn.length; i++) {
      const e = list[(i + rot) % list.length];
      if (e.cost > budget || (vehFull && e.kind !== "infantry" && e.kind !== "recon")) continue;
      budget -= e.cost;
      out.buy.push({ type: e.type, count: 1, command: "ATTACK", target: buyTarget.id, reason: "cheapest " + e.kind + " entry for " + buyTarget.id + ", supplies " + supplies });
    }
  }

  // FOBs (M3 phase F): when affordable, on a rotating minute so it does not happen on every poll; then upgrade an active tier-1 FOB
  const fobNames = new Set(channelOf(manifest, "fob").map((e) => e.name)), fobRules = sit.fobRules ?? {};
  if (fobNames.size && isNum(fobRules.cost) && isNum(fobRules.max)) {
    const minute = Math.floor((isNum(sit.time) ? sit.time : 0) / 60);
    const mine = Array.isArray(sit.fobs) ? sit.fobs : [], sites = Array.isArray(sit.fobSites) ? sit.fobSites : [];
    if (fobNames.has("ESTABLISH_FOB") && fobRules.ready !== false && sites[0] && fobRules.live < fobRules.max && !fobRules.inProgress && supplies >= fobRules.cost + 100 && minute % 3 === 1)
      out.fobs.push({ command: "ESTABLISH_FOB", site: sites[0].id, reason: `fake provider: forward base at ${sites[0].name ?? sites[0].id}` });
    else if (fobNames.has("UPGRADE_FOB") && isNum(fobRules.upgradeCost) && supplies >= fobRules.upgradeCost + 150 && minute % 3 === 2) {
      const up = mine.find((f) => f?.state === "active" && f.tier < 2);
      if (up) out.fobs.push({ command: "UPGRADE_FOB", fob: up.id, reason: "fake provider: vehicle depot" });
    }
  }
  // Logistics (M3 phase L): RESUPPLY an active FOB whose stock is low, when the HQ can spare a load and no convoy is on its way there
  const logiRules = sit.logisticsRules ?? {};
  if (fobNames.has("RESUPPLY") && isNum(logiRules.lowStock) && (logiRules.inFlight ?? 0) < (logiRules.convoyMax ?? 2) && supplies >= (logiRules.minCargo ?? 40) + 40) {
    const convoys = Array.isArray(sit.convoys) ? sit.convoys : [];
    const low = (Array.isArray(sit.fobs) ? sit.fobs : []).find((f) => f?.state === "active" && isNum(f.stock) && f.stock < logiRules.lowStock && !convoys.some((c) => c?.to === f.id && ["planned", "loading", "en_route"].includes(c.state)));
    if (low) out.fobs.push({ command: "RESUPPLY", fob: low.id, reason: `fake provider: ${low.id} stock ${low.stock} is low` });
  }

  const ai = squads.filter((s) => !s.humanLed && !s.reserved && !s.garrison);
  const assigned = new Set();
  const give = (s, command, tgt, reason) => { assigned.add(s.id); out.orders.push(tgt.id ? { squad: s.id, command, target: tgt.id, reason } : { squad: s.id, command, pos: tgt.pos, reason }); };
  const weak = (s) => isNum(s.agents) && isNum(s.startAgents) && s.startAgents > 0 && s.agents < s.startAgents * 0.5;
  if (names.has("FALL_BACK")) for (const s of ai) if (weak(s)) give(s, "FALL_BACK", { pos: base }, "weak unit, falling back to base");
  // defend own objectives with a contact within 400 m
  if (names.has("DEFEND")) for (const o of objs.filter((o) => o.owner === me)) {
    const near = contacts.filter((c) => dist2(xz(c.pos), xz(o.pos)) <= 400);
    if (!near.length) continue;
    const free = ai.filter((s) => !assigned.has(s.id)).sort((a, b) => dist2(xz(a.pos), xz(o.pos)) - dist2(xz(b.pos), xz(o.pos)));
    if (free[0]) give(free[0], "DEFEND", { id: o.id }, `contact ${Math.round(dist2(xz(near[0].pos), xz(o.pos)))} m from ${o.id}`);
  }
  // attack the objective not owned by us that is nearest to our base, with up to 2 units
  const targets = objs.filter((o) => o.owner !== me).sort((a, b) => dist2(xz(a.pos), base) - dist2(xz(b.pos), base));
  if (names.has("ATTACK") && targets[0]) {
    const t = targets[0];
    const free = ai.filter((s) => !assigned.has(s.id)).sort((a, b) => dist2(xz(a.pos), xz(t.pos)) - dist2(xz(b.pos), xz(t.pos)));
    for (const s of free.slice(0, 2)) give(s, "ATTACK", { id: t.id }, `nearest objective not ours: ${t.name ?? t.id}`);
  }
  // offers for human-led squads that have no accepted mission
  if (canOffer) for (const s of squads.filter((s) => s.humanLed && s.mission?.state !== "accepted")) {
    const p = xz(s.pos), offers = [];
    const byDist = (a, b) => dist2(xz(a.pos), p) - dist2(xz(b.pos), p);
    const nonOwned = objs.filter((o) => o.owner !== me).sort(byDist), own = objs.filter((o) => o.owner === me).sort(byDist);
    if (nonOwned[0]) offers.push({ kind: "CAPTURE", target: nonOwned[0].id, brief: `Seize ${nonOwned[0].name ?? nonOwned[0].id}. It is held by ${nonOwned[0].owner === "none" ? "nobody" : nonOwned[0].owner}; holding it bleeds the enemy's tickets.` });
    const hot = contacts.slice().sort(byDist)[0];
    if (hot && offers.length < 2 && dist2(xz(hot.pos), p) <= 1500) offers.push({ kind: "DESTROY", pos: xz(hot.pos), brief: `Destroy the reported enemy force (${hot.desc ?? "contact"}) before it reaches our lines.` });
    if (offers.length < 2 && own[0]) offers.push({ kind: "DEFEND", target: own[0].id, brief: `Hold ${own[0].name ?? own[0].id}; expect enemy pressure.` });
    if (offers.length < 2 && nonOwned[1]) offers.push({ kind: "RECON", target: nonOwned[1].id, brief: `Scout ${nonOwned[1].name ?? nonOwned[1].id} and report enemy strength.` });
    if (offers.length) out.missions.push({ squad: s.id, offers: offers.slice(0, 2) });
  }
  return out;
}

// Shared by both providers: pull the JSON object out of the reply text.
// v1: needs an orders array. v2: orders/buy/missions may be omitted, but the reply must contain at least one of them or keep:true.
function extractOrders(text, v2 = false) {
  const a = text.indexOf("{"), b = text.lastIndexOf("}");
  if (a < 0 || b < a) throw new Error("no JSON object in reply");
  const obj = JSON.parse(text.slice(a, b + 1));
  if (!obj || typeof obj !== "object" || Array.isArray(obj)) throw new Error("reply is not a JSON object");
  if (v2) {
    if (!["orders", "buy", "missions", "fobs"].some((k) => Array.isArray(obj[k])) && obj.keep !== true && !(obj.sitrep && typeof obj.sitrep === "object")) throw new Error("reply has no orders/buy/missions/fobs array");
  } else if (!Array.isArray(obj.orders)) throw new Error("reply has no orders array");
  return obj;
}

const SITREP_EXTRA_TOKENS = 450; // room for the SITREP object on top of the configured maxTokens
// Returns {raw, usage:{input,output}}. Throws Error on transport/HTTP failure.
async function callProvider(c, sit, opts = {}) {
  c = opts.slotCfg ?? slotCfg(c, opts.role ?? "strong"); // role -> slot (only "translate" uses the weak slot); opts.slotCfg = already resolved (primary or backup)
  const { structures: _structures, ...llmSit } = sit ?? {}; // situation.structures[] (base markers, dashboard map only) is not sent to the model
  const userMsg = opts.userMsg ?? ("Current situation:\n" + JSON.stringify(llmSit) + (opts.journal ? "\n\n" + opts.journal : "")); // opts.userMsg / system / maxTokens: the journal review call
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), (opts.timeoutSec ?? c.timeoutSec) * 1000);
  try {
    let url, headers, body;
    if (c.provider === "anthropic") {
      if (!c.apiKey) throw new Error("API key not set");
      url = (c.baseUrl || "https://api.anthropic.com") + "/v1/messages";
      headers = { "x-api-key": c.apiKey, "anthropic-version": "2023-06-01", "content-type": "application/json" };
      body = { model: c.model, max_tokens: opts.maxTokens ?? c.maxTokens + (opts.sitrep ? SITREP_EXTRA_TOKENS : 0), system: opts.system ?? systemPrompt(c, isV2(sit), opts.sitrep, opts.manifest, sit), messages: [{ role: "user", content: userMsg }] };
    } else {
      const base = c.provider === "openrouter" ? (c.baseUrl || OR_BASE) : c.baseUrl;
      if (!base) throw new Error("baseUrl not set");
      url = base + "/chat/completions";
      headers = { "content-type": "application/json" };
      if (c.apiKey) headers.authorization = "Bearer " + c.apiKey;
      body = { model: c.model, max_tokens: opts.maxTokens ?? c.maxTokens + (opts.sitrep ? SITREP_EXTRA_TOKENS : 0), messages: [{ role: "system", content: opts.system ?? systemPrompt(c, isV2(sit), opts.sitrep, opts.manifest, sit) },{ role: "user", content: userMsg }] };
      if (c.provider === "openrouter") body.usage = { include: true };
    }
    const r = await fetch(url, { method: "POST", signal: ac.signal, headers, body: JSON.stringify(body) });
    if (!r.ok) {
      const err = new Error(`HTTP ${r.status}: ${(await r.text()).slice(0, 200)}`);
      err.status = r.status;
      if (r.status === 429 || r.status >= 500) err.cooldownMs = Math.max(RATE_COOLDOWN_MS, (Number(r.headers.get("retry-after")) || 0) * 1000);
      throw err;
    }
    const j = await r.json();
    if (j?.error && !j.choices && !j.content) { // some gateways answer 200 with {error:{code,message}}
      const err = new Error(`HTTP ${j.error.code ?? "?"}: ${String(j.error.message ?? JSON.stringify(j.error)).slice(0, 200)}`);
      if (typeof j.error.code === "number") { err.status = j.error.code; if (j.error.code === 429 || j.error.code >= 500) err.cooldownMs = RATE_COOLDOWN_MS; }
      throw err;
    }
    let text, usage;
    if (c.provider === "anthropic") {
      text = (j.content ?? []).filter((b) => b.type === "text").map((b) => b.text).join("");
      usage = { input: j.usage?.input_tokens ?? 0, output: j.usage?.output_tokens ?? 0 };
    } else {
      text = j.choices?.[0]?.message?.content ?? "";
      usage = { input: j.usage?.prompt_tokens ?? 0, output: j.usage?.completion_tokens ?? 0 };
      if (isNum(j.usage?.cost)) usage.cost = j.usage.cost;
    }
    return { raw: text, usage };
  } finally {
    clearTimeout(timer);
  }
}

function validate(raw, sit, dropped = []) {
  const ids = new Set(sit.squads.map((s) => s?.id));
  const seen = new Set(), out = [];
  for (const o of raw) {
    const why = !o || typeof o !== "object" ? "not an object"
      : !ids.has(o.squad) ? `unknown squad ${o.squad}`
      : seen.has(o.squad) ? `duplicate order for ${o.squad}`
      : !COMMANDS.has(o.command) ? `bad command ${o.command}`
      : !Array.isArray(o.pos) || o.pos.length !== 2 || !isNum(o.pos[0]) || !isNum(o.pos[1]) ? "bad pos"
      : typeof o.reason !== "string" ? "bad reason" : null;
    if (why) { console.log(`  dropped order: ${why}`); dropped.push({ order: o, reason: why }); continue; }
    seen.add(o.squad);
    out.push({ squad: o.squad, command: o.command, pos: [o.pos[0], o.pos[1]], reason: o.reason.slice(0, 120) });
  }
  return out;
}

// Protocol v2 validation. Returns {orders, buy, missions}; every dropped item goes to `dropped` with a reason.
function validateV2(obj, sit, dropped = [], manifest = DEFAULT_MANIFEST) {
  const orderNames = orderNamesOf(manifest), buyOk = channelOf(manifest, "buy").length > 0, missionsOk = channelOf(manifest, "missions").length > 0;
  const drop = (key, item, reason) => { console.log(`  dropped ${key}: ${reason}`); dropped.push({ [key]: item, reason }); };
  const squads = new Map((Array.isArray(sit.squads) ? sit.squads : []).filter((s) => s && typeof s.id === "string").map((s) => [s.id, s]));
  const objIds = new Set((Array.isArray(sit.objectives) ? sit.objectives : []).filter((o) => o && typeof o.id === "string").map((o) => o.id));
  const xzOk = (p) => Array.isArray(p) && p.length === 2 && isNum(p[0]) && isNum(p[1]);
  // target/pos resolution shared by orders and offers: {target} | {pos} | {} (none given) | {error}
  const aim = (o) => {
    if (typeof o.target === "string" && o.target) {
      if (objIds.has(o.target)) return { target: o.target };
      if (!xzOk(o.pos)) return { error: `unknown target ${o.target}` };
    } else if (o.target !== undefined && o.target !== null) return { error: "bad target" };
    if (o.pos !== undefined && o.pos !== null) return xzOk(o.pos) ? { pos: [o.pos[0], o.pos[1]] } : { error: "bad pos" };
    return {};
  };
  const res = { orders: [], buy: [], missions: [], fobs: [] };

  const seen = new Set();
  for (const o of Array.isArray(obj.orders) ? obj.orders : []) {
    const s = o && typeof o === "object" ? squads.get(o.squad) : null;
    const a = s ? aim(o) : {};
    const why = !o || typeof o !== "object" ? "not an object"
      : !s ? `unknown squad ${o.squad}`
      : s.humanLed ? `${o.squad} is human-led: no orders, only missions`
      : s.garrison ? "squad " + o.squad + " is the HQ garrison (defends the HQ, not available for orders)"
      : s.reserved ? `${o.squad} is reserved (${s.reserved})`
      : seen.has(o.squad) ? `duplicate order for ${o.squad}`
      : !orderNames.has(o.command) ? `bad command ${o.command}`
      : a.error ? a.error
      : !a.target && !a.pos && o.command !== "FALL_BACK" ? "order needs a target or pos"
      : o.reason !== undefined && typeof o.reason !== "string" ? "bad reason" : null;
    if (why) { drop("order", o, why); continue; }
    seen.add(o.squad);
    res.orders.push({ squad: o.squad, command: o.command, ...(a.target ? { target: a.target } : a.pos ? { pos: a.pos } : {}), reason: (o.reason ?? "").slice(0, 120) });
  }

  const catalog = new Map((Array.isArray(sit.catalog) ? sit.catalog : []).filter((e) => e && typeof e.type === "string" && isNum(e.cost)).map((e) => [e.type, e]));
  let left = isNum(sit.war?.supplies) ? sit.war.supplies : 0;

  // FOB intents (channel "fob"): at most 1 per reply; the game validates again (cap, one in progress, site, supplies)
  const fobNames = new Set(channelOf(manifest, "fob").map((e) => e.name));
  const fobSites = new Set((Array.isArray(sit.fobSites) ? sit.fobSites : []).filter((x) => x && typeof x.id === "string").map((x) => x.id));
  const myFobs = new Map((Array.isArray(sit.fobs) ? sit.fobs : []).filter((x) => x && typeof x.id === "string").map((x) => [x.id, x]));
  const fr = sit.fobRules ?? {}, lr = sit.logisticsRules ?? {};
  const baseIds = new Set(["HQ", ...[...myFobs.values()].filter((x) => x.state === "active").map((x) => x.id)]);
  for (const f of Array.isArray(obj.fobs) ? obj.fobs : []) {
    const cmd = f && typeof f === "object" ? (typeof f.command === "string" && f.command ? f.command.toUpperCase() : f.site ? "ESTABLISH_FOB" : f.fob ? "UPGRADE_FOB" : "") : "";
    const mine = cmd === "UPGRADE_FOB" && f ? myFobs.get(f.fob) : null;
    const why = !f || typeof f !== "object" ? "not an object"
      : !fobNames.size ? "FOBs are not available in this war mode"
      : !fobNames.has(cmd) ? `bad fob command ${f.command}`
      : res.fobs.length >= 3 ? "more than 3 fob entries per reply"
      : (cmd === "ESTABLISH_FOB" || cmd === "UPGRADE_FOB") && res.fobs.some((x) => x.command === "ESTABLISH_FOB" || x.command === "UPGRADE_FOB") ? "more than 1 ESTABLISH_FOB/UPGRADE_FOB per reply"
      : cmd === "RESUPPLY" ? (!myFobs.get(f.fob) ? `unknown fob ${f.fob}` : myFobs.get(f.fob).state !== "active" ? `${f.fob} is not active`
        : isNum(lr.convoyMax) && isNum(lr.inFlight) && lr.inFlight + res.fobs.filter((x) => x.command === "RESUPPLY" || x.command === "CONVOY").length >= lr.convoyMax ? "convoy limit reached"
        : res.fobs.some((x) => x.command === "RESUPPLY" && x.fob === f.fob) ? `${f.fob} already gets a convoy in this reply`
        : (Array.isArray(sit.convoys) ? sit.convoys : []).some((c) => c?.to === f.fob && ["loading", "planned", "en_route"].includes(c.state)) ? `a convoy is already on its way to ${f.fob}`
        : isNum(lr.minCargo) && left < lr.minCargo ? `HQ stock ${left} is below the minimum load ${lr.minCargo}` : null)
      : cmd === "CONVOY" ? (!baseIds.has(f.origin) || !baseIds.has(f.dest) ? `unknown or inactive base ${f.origin} -> ${f.dest}` : f.origin === f.dest ? "origin and destination are the same"
        : isNum(lr.convoyMax) && isNum(lr.inFlight) && lr.inFlight + res.fobs.filter((x) => x.command === "RESUPPLY" || x.command === "CONVOY").length >= lr.convoyMax ? "convoy limit reached" : null)
      : cmd === "ESTABLISH_FOB" ? (fr.ready === false ? "too early for a FOB: " + (fr.readyReason ?? "not ready") + (isNum(fr.earliestSec) ? " (earliest " + fr.earliestSec + " s, elapsed " + (fr.elapsedSec ?? "?") + " s)" : "")
        : isNum(fr.earliestSec) && isNum(fr.elapsedSec) && fr.elapsedSec < fr.earliestSec ? "too early for a FOB: earliestSec " + fr.earliestSec + " not reached (elapsed " + fr.elapsedSec + ")"
        : !fobSites.has(f.site) ? `unknown fob site ${f.site}`
        : isNum(fr.live) && isNum(fr.max) && fr.live >= fr.max ? "FOB limit reached"
        : fr.inProgress > 0 ? "another FOB is still being established"
        : isNum(fr.cost) && fr.cost > left ? `FOB cost ${fr.cost} exceeds remaining supplies ${left}` : null)
      : (!mine ? `unknown fob ${f.fob}` : mine.state !== "active" ? `${f.fob} is not active` : mine.tier >= 2 ? `${f.fob} is already tier 2`
        : isNum(fr.upgradeCost) && isNum(mine.stock) && fr.upgradeCost > mine.stock ? `upgrade cost ${fr.upgradeCost} exceeds the stock of ${f.fob} (${mine.stock}); RESUPPLY it first`
        : !isNum(mine.stock) && isNum(fr.upgradeCost) && fr.upgradeCost > left ? `upgrade cost ${fr.upgradeCost} exceeds remaining supplies ${left}` : null);
    if (why) { drop("fob", f, why); continue; }
    if (cmd === "ESTABLISH_FOB") left -= fr.cost ?? 0;
    else if (cmd === "UPGRADE_FOB" && !isNum(mine?.stock)) left -= fr.upgradeCost ?? 0;
    else if (cmd === "RESUPPLY" || (cmd === "CONVOY" && f.origin === "HQ")) left -= Math.min(isNum(lr.truckCargo) ? lr.truckCargo : 0, Math.max(0, left)); // the load leaves the HQ stock (approximation for CONVOY from a FOB)
    const out = cmd === "ESTABLISH_FOB" ? { site: f.site } : cmd === "CONVOY" ? { origin: f.origin, dest: f.dest } : { fob: f.fob };
    res.fobs.push({ command: cmd, ...out, reason: typeof f.reason === "string" ? f.reason.slice(0, 120) : "" });
  }

  // buys after the FOB/logistics entries: the game applies them in that order too (a FOB is not starved by buys in the same reply)
  for (const b of Array.isArray(obj.buy) ? obj.buy : []) {
    const e = b && typeof b === "object" ? catalog.get(b.type) : null;
    const cost = e ? e.cost * b.count : 0;
    const why = !b || typeof b !== "object" ? "not an object"
      : !buyOk ? "buy is not available in this war mode"
      : res.buy.length >= 3 ? "more than 3 buy entries"
      : !e ? `unknown catalog type ${b.type}`
      : !Number.isInteger(b.count) || b.count < 1 || b.count > 3 ? "count must be an integer 1..3"
      : cost > left ? `cost ${cost} exceeds remaining supplies ${left}` : null;
    // spawn on assignment: the units must come with their first order (command + objective target or position)
    const ba = why || !b ? {} : aim(b);
    const bwhy = why ? why
      : !BUY_COMMANDS.has(b.command) ? "a buy needs an assignment: command MOVE|ATTACK|DEFEND|RECON (got " + b.command + ")"
      : ba.error ? "bad assignment: " + ba.error
      : !ba.target && !ba.pos ? "a buy needs an assignment: target (objective id) or pos [x, z]" : null;
    if (bwhy) { drop("buy", b, bwhy); continue; }
    left -= cost;
    res.buy.push({ type: b.type, count: b.count, command: b.command, ...(ba.target ? { target: ba.target } : { pos: ba.pos }), reason: typeof b.reason === "string" ? b.reason.slice(0, 120) : "" });
  }

  const offered = new Set();
  for (const m of Array.isArray(obj.missions) ? obj.missions : []) {
    const s = m && typeof m === "object" ? squads.get(m.squad) : null;
    const why = !m || typeof m !== "object" ? "not an object"
      : !s ? `unknown squad ${m.squad}`
      : !missionsOk ? "missions are not available in this war mode"
      : !s.humanLed ? `${m.squad} is not human-led: missions are for human-led squads only`
      : s.mission?.state === "accepted" ? `${m.squad} already has an accepted mission`
      : offered.has(m.squad) ? `duplicate missions entry for ${m.squad}`
      : !Array.isArray(m.offers) ? "offers must be an array" : null;
    if (why) { drop("mission", m, why); continue; }
    const offers = [];
    for (const f of m.offers) {
      const a = f && typeof f === "object" ? aim(f) : {};
      const w2 = !f || typeof f !== "object" ? "not an object"
        : offers.length >= 3 ? "more than 3 offers per squad"
        : !MISSION_KINDS.has(f.kind) ? `bad mission kind ${f.kind}`
        : a.error ? a.error
        : !a.target && !a.pos ? "offer needs a target or pos"
        : f.brief !== undefined && typeof f.brief !== "string" ? "bad brief" : null;
      if (w2) { drop("offer", { squad: m.squad, offer: f }, w2); continue; }
      offers.push({ kind: f.kind, ...(a.target ? { target: a.target } : { pos: a.pos }), brief: (f.brief ?? "").slice(0, 240) });
    }
    if (offers.length) { offered.add(m.squad); res.missions.push({ squad: m.squad, offers }); }
  }
  return res;
}

// After a 429/5xx the provider is skipped for a while; the game falls back to the rule commander meanwhile.

// Runs one situation through the given config. Never throws.
async function computeOrders(c, sit, isTest = false, opts = {}) {
  const r = { orders: [], buy: [], missions: [], fobs: [], keep: false, dropped: [], rawReply: null, usage: null, ok: true, reason: null };
  try {
    if (!sit || typeof sit !== "object" || !Array.isArray(sit.squads)) throw new Error("no squads array");
    const v2 = isV2(sit);
    let raw;
    if (v2) {
      let obj;
      if (c.provider === "fake") { obj = fakeV2(sit, opts.manifest); if (opts.sitrep) obj.sitrep = fakeSitrep(sit, obj); }
      else {
        session.requests++;
        const p = await callWithFallback(c, sit, { ...opts, role: "commander", test: isTest });
        if (p.provider === "openrouter") recordUsage(p.modelUsed, typeof sit.faction === "string" ? sit.faction : "?", p.usage);
        r.rawReply = p.raw; r.usage = p.usage; r.modelUsed = p.modelUsed; r.fellBack = p.fellBack; r.primaryError = p.primaryError;
        obj = extractOrders(p.raw, true);
        if (isNum(obj.next_check_sec)) r.nextCheck = { sec: Math.round(obj.next_check_sec), reason: typeof obj.next_check_reason === "string" ? obj.next_check_reason.slice(0, 120) : "" };
      }
      Object.assign(r, validateV2(obj, sit, r.dropped, opts.manifest));
      r.keep = (obj.keep === true || (!r.orders.length && !r.buy.length && !r.missions.length && !r.fobs.length && !!obj.sitrep && !Array.isArray(obj.orders) && !Array.isArray(obj.buy) && !Array.isArray(obj.missions) && !Array.isArray(obj.fobs))) && !r.orders.length && !r.buy.length && !r.missions.length && !r.fobs.length;
      if (obj.sitrep !== undefined) {
        r.sitrep = normSitrep(obj.sitrep);
        if (!r.sitrep) { console.log("  dropped sitrep: not a usable object"); r.dropped.push({ sitrep: obj.sitrep, reason: "not a usable sitrep object" }); }
      }
      return r;
    }
    if (c.provider === "fake") raw = fakeOrders(sit);
    else {
      session.requests++;
      const p = await callWithFallback(c, sit, { role: "commander", test: isTest });
      if (p.provider === "openrouter") recordUsage(p.modelUsed, typeof sit.faction === "string" ? sit.faction : "?", p.usage);
      r.rawReply = p.raw; r.usage = p.usage; r.modelUsed = p.modelUsed; r.fellBack = p.fellBack; r.primaryError = p.primaryError;
      const obj = extractOrders(p.raw);
      raw = obj.orders;
      if (isNum(obj.next_check_sec)) r.nextCheck = { sec: Math.round(obj.next_check_sec), reason: typeof obj.next_check_reason === "string" ? obj.next_check_reason.slice(0, 120) : "" };
    }
    r.orders = validate(raw, sit, r.dropped);
  } catch (e) {
    if (e?.fellBack) { r.fellBack = true; r.primaryError = e.primaryError; }
    r.ok = false;
    r.reason = e?.name === "AbortError" ? "timeout" : String(e?.message ?? e);
    r.orders = []; r.buy = []; r.missions = []; r.fobs = []; r.keep = false;
    console.log(`  error: ${r.reason}`);
  }
  return r;
}

// Materially unchanged = same squad ids/modes/agent counts, same contact count, nothing moved >= 75 m.
const MOVE_EPS = 75;
function moved(a, b) { return !(Array.isArray(a) && Array.isArray(b)) || Math.hypot((a[0] ?? 0) - (b[0] ?? 0), (a[2] ?? 0) - (b[2] ?? 0)) >= MOVE_EPS; }
const TICKET_EPS_PCT = 5, SUPPLY_EPS = 100;
const objList = (s) => (Array.isArray(s?.objectives) ? s.objectives.filter((o) => o && typeof o.id === "string") : []);
// v2 part of unchanged(): an objective owner change is never "unchanged"; war numbers must be materially the same too.
function warUnchanged(a, b, pendingEvents) {
  if (pendingEvents > 0) return false;
  const oa = new Map(objList(a).map((o) => [o.id, o]));
  const ob = objList(b);
  if (oa.size !== ob.length) return false;
  for (const o of ob) {
    const p = oa.get(o.id);
    if (!p || String(p.owner) !== String(o.owner) || !!p.contested !== !!o.contested) return false;
  }
  const wa = a.war ?? {}, wb = b.war ?? {};
  if (isNum(wa.tickets) !== isNum(wb.tickets) || isNum(wa.supplies) !== isNum(wb.supplies)) return false;
  if (isNum(wa.tickets) && Math.abs(wa.tickets - wb.tickets) > Math.max(5, wa.tickets * TICKET_EPS_PCT / 100)) return false;
  if (isNum(wa.enemyTickets) && isNum(wb.enemyTickets) && Math.abs(wa.enemyTickets - wb.enemyTickets) > Math.max(5, wa.enemyTickets * TICKET_EPS_PCT / 100)) return false;
  if (isNum(wa.supplies) && Math.abs(wa.supplies - wb.supplies) >= SUPPLY_EPS) return false;
  if (wa.liveAI !== wb.liveAI || wa.vehicles !== wb.vehicles) return false;
  return true;
}
function unchanged(a, b, pendingEvents = 0) {
  if (!a || !b || !Array.isArray(a.squads) || !Array.isArray(b.squads) || a.squads.length !== b.squads.length) return false;
  if ((isV2(a) || isV2(b)) && !(isV2(a) && isV2(b) && warUnchanged(a, b, pendingEvents))) return false;
  const old = new Map(a.squads.map((s) => [s?.id, s]));
  for (const s of b.squads) {
    const o = old.get(s?.id);
    if (!o || o.mode !== s.mode || o.agents !== s.agents || !!o.humanLed !== !!s.humanLed || moved(o.pos, s.pos)) return false;
    if (isV2(b) && (o.kind !== s.kind || o.vehicle?.mounted !== s.vehicle?.mounted || o.vehicle?.state !== s.vehicle?.state
      || !!o.vehicle !== !!s.vehicle || o.mission?.state !== s.mission?.state || o.objective?.status !== s.objective?.status)) return false;
  }
  const ca = Array.isArray(a.contacts) ? a.contacts : [], cb = Array.isArray(b.contacts) ? b.contacts : [];
  if (ca.length !== cb.length) return false;
  const used = new Set();
  for (const c of cb) { // each new contact must match an unused old one within 75 m
    const i = ca.findIndex((o, k) => !used.has(k) && !moved(o?.pos, c?.pos));
    if (i < 0) return false;
    used.add(i);
  }
  return true;
}
const lastCall = {}; // faction -> {ts of last provider attempt}

// ---------- action manifest (protocol v3) ----------
// The game POSTs /manifest {v:3,faction,hash,mode,capabilities,intents,primitives} at war start and whenever it changes (mode /
// capability / faction change); every v3 situation carries the hash. Unknown hash (e.g. sidecar restarted) -> the built-in manifest
// is used and the reply says needManifest:true, the game then resends. Kept per faction in memory only.
const manifests = {}; // faction -> {hash, faction, mode, capabilities, intents, primitives, receivedAt}
const strList = (a, max = 40, len = 80) => (Array.isArray(a) ? a.filter((x) => typeof x === "string").slice(0, max).map((x) => x.slice(0, len)) : []);
function cleanEntry(e) {
  if (!e || typeof e !== "object" || typeof e.name !== "string" || !e.name || e.name.length > 60) return null;
  const params = (Array.isArray(e.params) ? e.params : []).slice(0, 20).filter((p) => p && typeof p.name === "string" && p.name).map((p) => ({
    name: p.name.slice(0, 40), type: typeof p.type === "string" ? p.type.slice(0, 20) : "string", required: p.required === true,
    ...(Array.isArray(p.enum) && p.enum.length ? { enum: strList(p.enum, 40, 60) } : {}), hint: typeof p.hint === "string" ? p.hint.slice(0, 200) : "" }));
  return { name: e.name, layer: e.layer === "primitive" ? "primitive" : "intent", channel: typeof e.channel === "string" && e.channel ? e.channel.slice(0, 20) : "orders",
    authority: typeof e.authority === "string" ? e.authority.slice(0, 12) : "hq", cost: isNum(e.cost) ? e.cost : 0, tags: strList(e.tags), requires: strList(e.requires),
    ...(Array.isArray(e.steps) ? { steps: strList(e.steps) } : {}), desc: typeof e.desc === "string" ? e.desc.slice(0, 700) : "", params };
}
function cleanManifest(m) {
  if (!m || typeof m !== "object" || typeof m.faction !== "string" || !m.faction || typeof m.hash !== "string" || !m.hash || m.hash.length > 64) return null;
  const list = (a) => (Array.isArray(a) ? a.slice(0, 200).map(cleanEntry).filter(Boolean) : []);
  return { hash: m.hash, faction: m.faction, mode: typeof m.mode === "string" ? m.mode.slice(0, 40) : "", capabilities: strList(m.capabilities), intents: list(m.intents), primitives: list(m.primitives) };
}
async function handleManifest(req, res) {
  let raw;
  try { raw = JSON.parse(await readBody(req, 300_000)); } catch { return send(res, 400, { error: "invalid JSON" }); }
  const m = cleanManifest(raw);
  if (!m) return send(res, 400, { error: "bad manifest (needs faction, hash, intents[])" });
  manifests[m.faction] = { ...m, receivedAt: Date.now() };
  console.log(`manifest faction=${m.faction} hash=${m.hash} mode=${m.mode || "?"} intents=${m.intents.length} primitives=${m.primitives.length} capabilities=[${m.capabilities.join(",")}]`);
  send(res, 200, { ok: true, hash: m.hash });
}

// ---------- HQ chat (M3 phase A: /hq <free text>) ----------
// The situation carries hq:[{id,player,role,squad,grid,pos:[x,z],text,ageSec}] = the faction's pending player messages (re-sent every
// poll until the game has an answer). Each new message starts ONE agent turn (agent/agentTurn.mjs, AI SDK ToolLoopAgent, <= agent.maxSteps
// steps) in the background; at most agent.maxConcurrent run at once. The result waits in hqOut[faction] and rides on the NEXT reply for that
// faction: hqActions [{msgId,id,action,args:[{name,value}]}] (the game runs them through its action registry as the player) and
// hqReplies [{msgId,text,fallback?}] (fallback:true = no answer, the game shows its /request menu hint). The SDK is loaded lazily, so the
// strategic tick keeps working without node_modules.
// FAST PATH (WP11): the game also POSTs each message on its own to POST /hq {faction,msg} (right when the player types it) and gets the
// answer on THAT request ({handled,hqReplies,hqActions}), so the reply no longer waits for the commander LLM call that carries the
// situation POST. The agent turn runs against the situation cached by the faction's last /orders POST (hqSitCache). Dedupe by the same
// hqSeen key: a message started by one path is skipped by the other. If the game hung up before the turn ended, the result goes to
// hqOut and rides on the next /orders reply as before. {handled:false,reason} = nothing started, the situation path stays in charge.
const HQ_MAX = 60, HQ_SEEN_MAX = 400, HQ_MAX_AGE_SEC = 90, HQ_FAST_TURN_SEC = 25; // 25 s < the game's /hq REST timeout (30 s)
const hqSitCache = {};       // faction -> {sit, ts} (the last valid situation, cached on arrival, before the commander call)
const hqLog = [];            // newest first: dashboard "HQ chat" view
const hqSeen = new Set();    // faction|id|player|text
const hqOut = {};            // faction -> {actions:[], replies:[]}
const hqWaiting = [];        // jobs waiting for a free slot
let hqRunning = 0, agentMod = null;
const hqOutFor = (f) => (hqOut[f] ??= { actions: [], replies: [] });
async function loadAgent() { return (agentMod ??= await import("./agent/agentTurn.mjs")); }

function cleanHqMessage(m) {
  if (!m || typeof m !== "object" || !isNum(m.id) || typeof m.text !== "string" || !m.text.trim()) return null;
  const str = (v, n) => (typeof v === "string" ? v.slice(0, n) : "");
  return { id: m.id, player: str(m.player, 60) || "player", role: m.role === "leader" ? "leader" : "soldier", squad: str(m.squad, 60), grid: str(m.grid, 20),
    pos: Array.isArray(m.pos) && m.pos.length >= 2 && m.pos.every(isNum) ? m.pos.slice(0, 3) : null, text: m.text.trim().slice(0, 400), ageSec: isNum(m.ageSec) ? m.ageSec : 0,
    lang: I18N.normLang(m.lang) ?? "", history: cleanHistory(m.history) }; // protocol v4: the asker's language (ISO 639-1); "" = v3 message, no language handling
}
function hqFallbackReason(c) {
  if (!c.enabled) return "sidecar disabled";
  if (c.engine !== "llm") return "engine " + c.engine;
  if (!c.agent?.enabled) return "agent turns disabled";
  if (paused) return "HQ paused";
  if (budgetStopped(c)) return "budget limit reached";
  if (!["fake", "openrouter", "openai-compatible"].includes(c.provider)) return `provider ${c.provider} has no agent turn`;
  return null;
}
function hqFinish(entry, faction, r) {
  const out = { actions: [], replies: [] }; // this turn's delivery: to the fast request if it is still open, else to the next /orders reply
  entry.state = "done"; entry.ok = r.ok; entry.error = r.error ?? null; entry.latencyMs = r.latencyMs; entry.usage = r.usage; entry.steps = r.steps; entry.tools = r.tools;
  entry.modelUsed = r.ok ? (r.modelUsed ?? entry.model) : null; entry.fellBack = !!r.fellBack; entry.primaryError = r.primaryError ?? null;
  entry.actions = r.actions; entry.reply = r.ok ? r.replyText : ""; entry.lang = entry.msg.lang || null; entry.setLanguage = r.ok ? (r.setLanguage ?? null) : null;
  if (r.ok) {
    for (const a of r.actions) out.actions.push({ msgId: entry.msg.id, id: a.id, action: a.action, args: a.args });
    out.replies.push({ msgId: entry.msg.id, text: r.replyText, ...(r.setLanguage ? { setLanguage: r.setLanguage } : {}) }); // setLanguage: v4 only, validated ^[a-z]{2}$|auto
  } else out.replies.push({ msgId: entry.msg.id, text: "", fallback: true });
  const viaFast = !!entry.fast?.deliver(out);
  if (!viaFast) { const q = hqOutFor(faction); q.actions.push(...out.actions); q.replies.push(...out.replies); }
  console.log(`hq ${viaFast ? "fast " : ""}faction=${faction} msg=${entry.msg.id} player=${entry.msg.player} ${r.ok ? `actions=${r.actions.length} steps=${r.steps.length}` : "FAILED " + r.error} ${r.latencyMs}ms${r.usage?.cost ? ` cost=$${r.usage.cost.toFixed(5)}` : ""}`);
}
async function hqRun(job) {
  hqRunning++;
  const { faction, entry, sit, manifest } = job, c = job.cfg;
  try {
    const mod = await loadAgent();
    if (c.provider !== "fake") { session.requests++; }
    const ch = slotChain(c, "hq");
    const fallback = ch.fallback ? { config: ch.fallback, primaryCooling: () => coolLeftMs(ch.slot, ch.primary) > 0,
      onFailure: (which, err) => noteCooldown(ch.slot, which === "primary" ? ch.primary : ch.fallback, err, true), onFallback: (err) => noteFellBack(ch.slot, ch.primary.model, err) } : null;
    const r = await mod.runAgentTurn({ manifest, situation: sit, message: entry.msg, config: c, agentConfig: { maxSteps: c.agent.maxSteps, timeoutSec: entry.fast ? Math.min(c.agent.timeoutSec, HQ_FAST_TURN_SEC) : c.agent.timeoutSec }, journal: journalBlock(faction, c, sit), fallback });
    if ((r.fellBack ? ch.fallback?.provider : c.provider) === "openrouter") recordUsage(r.modelUsed ?? c.model, faction, r.usage);
    hqFinish(entry, faction, r);
  } catch (e) {
    hqFinish(entry, faction, { ok: false, error: String(e?.message ?? e).slice(0, 300), replyText: "", actions: [], steps: [], tools: [], usage: null, latencyMs: Date.now() - entry.ts });
  } finally {
    hqRunning--;
    while (hqRunning < Math.max(1, job.cfg.agent.maxConcurrent) && hqWaiting.length) void hqRun(hqWaiting.shift());
  }
}
// Called by handleOrders with the faction's situation (hq already cut out of it).
// fast (optional) = {deliver(out) -> bool, skip(reason)}: the POST /hq request that carries the (single) message.
function hqIngest(faction, sit, list, manifest, c, fast = null) {
  if (!Array.isArray(list)) return;
  for (const raw of list.slice(0, 10)) {
    const msg = cleanHqMessage(raw);
    if (!msg) continue;
    const key = `${faction}|${msg.id}|${msg.player}|${msg.text}`;
    if (hqSeen.has(key)) { fast?.skip("already started"); continue; }
    hqSeen.add(key);
    if (hqSeen.size > HQ_SEEN_MAX) { const first = hqSeen.values().next().value; hqSeen.delete(first); }
    const entry = { ts: Date.now(), faction, msg, provider: c.provider, model: c.provider === "fake" ? null : c.model, modelUsed: null, fellBack: false, primaryError: null, state: "running", ok: null, error: null, steps: [], tools: [], actions: [], reply: "", usage: null, latencyMs: null, lang: msg.lang || null, setLanguage: null, historyCount: msg.history.length, path: fast ? "fast" : "situation", fast };
    hqLog.unshift(entry); if (hqLog.length > HQ_MAX) hqLog.length = HQ_MAX;
    const why = hqFallbackReason(c) ?? (msg.ageSec > HQ_MAX_AGE_SEC ? "message too old" : null);
    if (why) {
      entry.state = "done"; entry.ok = false; entry.error = "fallback: " + why;
      const fb = { actions: [], replies: [{ msgId: msg.id, text: "", fallback: true }] };
      if (!fast?.deliver(fb)) hqOutFor(faction).replies.push(...fb.replies);
      console.log(`hq ${fast ? "fast " : ""}faction=${faction} msg=${msg.id} player=${msg.player}: fallback (${why})`);
      continue;
    }
    const job = { faction, entry, sit, manifest, cfg: { ...c, agent: { ...c.agent } } };
    if (hqRunning < Math.max(1, c.agent.maxConcurrent)) void hqRun(job); else hqWaiting.push(job);
  }
}
// Results of finished turns for the next reply (each delivered once).
function hqDrain(faction) {
  const o = hqOut[faction];
  if (!o || (!o.actions.length && !o.replies.length)) return {};
  const res = { ...(o.actions.length ? { hqActions: o.actions } : {}), ...(o.replies.length ? { hqReplies: o.replies } : {}) };
  hqOut[faction] = { actions: [], replies: [] };
  return res;
}

// ---------- HQ war journal ----------
// Simple first version of the commander's long-term memory (dev/VISION.md 1c, dev/notes/m4-round3-plan.md WP5). Per faction: intent (1 x 120 chars),
// 3 goals, 3 lessons, written ONLY by a small review call (no tools, no inline edits) every memory.reviewMin minutes, plus the attack record ("tries")
// that the CODE counts from the ATTACK orders it sent and the game's outcome events. Reset at war start. Pure logic + prompt text: agent/journal.mjs.
// Injection: one "HQ JOURNAL" block after the situation in the commander prompt (callProvider opts.journal) and in the /hq agent prompt.
// GET /api/state "journals" {faction: view} (dashboard Journal tab); settings group "memory" {enabled, reviewMin}.
const journals = {};        // faction -> journal (agent/journal.mjs emptyJournal)
const journalBlockKey = {}; // faction -> last logged block (log the size once per change)
const journalFor = (f) => (journals[f] ??= J.emptyJournal());
const jlog = (f) => (m) => console.log(`journal faction=${f} ${m}`);

// Every v2 situation: war boundary (reset), bookkeeping, outcome events (only while the war runs).
function journalTick(faction, sit, events, prevSit) {
  let j = journalFor(faction);
  const why = J.newWarReason(j, sit);
  if (why) { journals[faction] = j = J.emptyJournal(); j.resetReason = why; journalBlockKey[faction] = ""; console.log(`journal faction=${faction} reset: ${why}`); }
  J.noteSituation(j, sit);
  if (typeof sit?.war?.phase === "string" && sit.war.phase !== "running") return j;
  J.ingestEvents(j, events, sit, prevSit, jlog(faction));
  return j;
}
// The orders just validated for this faction: ATTACKs on objectives are tries.
function journalOrders(faction, sit, orders) {
  if (Array.isArray(orders) && orders.length) J.noteOrders(journalFor(faction), orders, sit, jlog(faction));
}
// The prompt block ("" = memory off or nothing to say yet). The size is logged once per change.
function journalBlock(faction, c, sit) {
  if (!c.memory?.enabled) return "";
  const block = J.renderBlock(journalFor(faction), sit), key = block.replace(/, \d+ min ago/g, "");
  if (key !== (journalBlockKey[faction] ?? "")) {
    journalBlockKey[faction] = key;
    console.log(`journal faction=${faction} prompt block ${block.length} chars (~${Math.ceil(block.length / 4)} tokens) changed`);
  }
  return block;
}
async function journalReview(faction, c, sit) {
  const j = journalFor(faction), t0 = Date.now(), periodMs = c.memory.reviewMin * 60000;
  j.reviewing = true; j.nextReviewAt = t0 + periodMs;
  let usage = null;
  try {
    let reply;
    if (c.provider === "fake") reply = J.fakeReview();
    else {
      session.requests++;
      const p = await callWithFallback(c, { faction }, { system: J.reviewSystem(), userMsg: J.reviewUser(sit, j), maxTokens: 400, role: "journal" });
      usage = p.usage;
      if (p.provider === "openrouter") recordUsage(p.modelUsed, faction, p.usage);
      reply = p.raw;
    }
    const v = J.validateReview(reply);
    if (journals[faction] !== j) { console.log(`journal faction=${faction} review discarded: the journal was reset meanwhile`); return; }
    if (v.error) {
      j.rejected++; j.lastError = v.error; j.nextReviewAt = Math.min(j.nextReviewAt, Date.now() + 60000);
      console.log(`journal faction=${faction} review REJECTED (${v.error}), keeping the old journal; reply: ${typeof reply === "string" ? reply.slice(0, 200).replace(/\s+/g, " ") : "-"}`);
      return;
    }
    j.intent = v.value.intent; j.goals = v.value.goals; j.lessons = v.value.lessons;
    j.reviews++; j.reviewedAt = Date.now(); j.reviewWarTime = isNum(sit?.time) ? sit.time : null; j.lastError = "";
    console.log(`journal faction=${faction} review ok (${c.provider === "fake" ? "fake" : c.model}) intent="${j.intent}" goals=${j.goals.length} lessons=${j.lessons.length}${v.notes.length ? " notes=[" + v.notes.join("; ") + "]" : ""} ${Date.now() - t0}ms${usage ? ` tokens=${usage.input}+${usage.output}` : ""}${isNum(usage?.cost) ? ` cost=$${usage.cost.toFixed(5)}` : ""}`);
  } catch (e) {
    j.rejected++; j.lastError = e?.name === "AbortError" ? "timeout" : String(e?.message ?? e).slice(0, 200);
    j.nextReviewAt = Math.min(j.nextReviewAt, Date.now() + 60000);
    console.log(`journal faction=${faction} review FAILED (${j.lastError}), keeping the old journal`);
  } finally {
    j.reviewing = false;
  }
}
// Started from the poll of a running war when due; runs in the background (the orders reply never waits for it).
function journalMaybeReview(faction, c, sit) {
  if (!c.memory?.enabled || !c.enabled || c.engine !== "llm" || paused || sit?.war?.phase !== "running") return;
  const j = journalFor(faction);
  if (j.reviewing || Date.now() < j.nextReviewAt) return;
  if (c.provider !== "fake" && (budgetStopped(c) || slotBlocked(c, "journal"))) return;
  console.log(`journal faction=${faction} review due (every ${c.memory.reviewMin} min, ${j.reviews} done)`);
  void journalReview(faction, { ...c, memory: { ...c.memory } }, sit).catch((e) => console.log(`journal faction=${faction} review error: ${e?.message ?? e}`));
}
function journalsView() {
  const out = {};
  for (const [f, j] of Object.entries(journals)) out[f] = J.view(j, state.factions[f]?.situation ?? null, !!cfg.memory?.enabled);
  return out;
}

// ---------- HQ scheduler ----------
// Per faction the LLM is called on: first request, scheduled due time (default/model-chosen interval), a trigger
// (something happened since the baseline = situation at the last call) or a manual wake. Otherwise the poll is gated
// (answered {"orders":[],"keep":true}, counted only). minIntervalSec is the hard floor between two calls.
const sched = {}; // faction -> scheduler state
function schedFor(f) {
  return (sched[f] ??= { baseline: null, lastLlmAt: 0, nextDueAt: 0, chosenSec: null, chosenReason: "", intervalSec: null, stretch: 1,
    lastWakeReason: null, lastWakeTriggers: [], gated: 0, manual: false, failed: false, pendingEvents: [] });
}
const d2 = (a, b) => Math.hypot((a[0] ?? 0) - (b[0] ?? 0), (a[a.length > 2 ? 2 : 1] ?? 0) - (b[b.length > 2 ? 2 : 1] ?? 0));
const okPos = (p) => Array.isArray(p) && p.length >= 2 && p.every(isNum);
const IDLE_MODES = new Set(["none", "idle"]);
const NEW_CONTACT_M = 150, OBJECTIVE_M = 50;

// Descriptions of what changed between the baseline situation and now, per the enabled triggers.
const SPAWN_EVENT = /spawn|purchas|bought|reinforc|arriv/i, MISSION_EVENT = /mission|offer/i;
function evalTriggers(t, base, sit, events = []) {
  const out = [];
  if (isV2(sit) && isV2(base)) { // protocol v2: war picture first, so the 8-entry cap never hides it
    if (t.objectiveChanged) {
      const bo = new Map(objList(base).map((o) => [o.id, o]));
      for (const o of objList(sit)) {
        const p = bo.get(o.id);
        if (p && String(p.owner) !== String(o.owner)) out.push(`objective ${o.id}${o.name ? " " + o.name : ""}: ${p.owner} -> ${o.owner}`);
      }
    }
    const tb = base.war?.tickets, tn = sit.war?.tickets;
    if (t.ticketsDropPct > 0 && isNum(tb) && isNum(tn) && tb > 0) {
      const pct = 100 * (tb - tn) / tb;
      if (pct >= t.ticketsDropPct) out.push(`tickets ${tb} -> ${tn} (-${Math.round(pct)}%)`);
    }
    if (t.unitSpawned) {
      const known = new Set((base.squads ?? []).map((s) => s?.id));
      for (const s of sit.squads ?? []) if (s?.id && !known.has(s.id)) out.push(`unit spawned: ${s.id}${s.kind ? " (" + s.kind + ")" : ""}`);
      for (const e of events) if (SPAWN_EVENT.test(String(e))) out.push(`event: ${String(e).slice(0, 80)}`);
    }
    if (t.missionEvent) {
      const bm = new Map((base.squads ?? []).map((s) => [s?.id, s]));
      for (const s of sit.squads ?? []) {
        const p = bm.get(s?.id);
        if (p && s.humanLed && (p.mission?.state ?? "none") !== (s.mission?.state ?? "none")) out.push(`${s.id} mission ${p.mission?.state ?? "none"} -> ${s.mission?.state ?? "none"}`);
      }
      for (const e of events) if (MISSION_EVENT.test(String(e))) out.push(`event: ${String(e).slice(0, 80)}`);
    }
  }
  // squads the game stood down as idle (event "<id> stood down ...") are not losses
  const down = new Set();
  for (const e of events) { const m = /^(\S+) stood down/.exec(String(e)); if (m) down.add(m[1]); }
  if (isV2(sit) && isV2(base) && t.idleUnits && t.idleSec > 0) {
    const bi = new Map((base.squads ?? []).map((s) => [s?.id, s]));
    for (const s of sit.squads ?? []) {
      if (!s?.id || s.garrison || s.humanLed || s.reserved || !isNum(s.idleSec) || s.idleSec < t.idleSec) continue;
      const was = bi.get(s.id);
      if (isNum(was?.idleSec) && was.idleSec >= t.idleSec) continue; // already known idle at the last call
      out.push(s.id + " idle " + s.idleSec + "s without an order");
    }
  }
  const bs = new Map((base.squads ?? []).filter((s) => !down.has(s?.id)).map((s) => [s?.id, s]));
  const cs = new Map((sit.squads ?? []).map((s) => [s?.id, s]));
  const bc = (Array.isArray(base.contacts) ? base.contacts : []).filter((c) => okPos(c?.pos));
  const cc = (Array.isArray(sit.contacts) ? sit.contacts : []).filter((c) => okPos(c?.pos));
  if (t.newContact) {
    for (const c of cc) if (!bc.some((o) => d2(o.pos, c.pos) <= NEW_CONTACT_M)) out.push(`new contact: ${c.desc ?? "unknown"}`);
  }
  if (t.contactNearSquadM > 0) {
    for (const [id, s] of cs) {
      if (!okPos(s?.pos)) continue;
      let best = null;
      for (const c of cc) {
        const d = d2(s.pos, c.pos);
        if (d > t.contactNearSquadM || (best && d >= best.d)) continue;
        const b = bs.get(id);
        const closeBefore = okPos(b?.pos) && bc.some((o) => d2(o.pos, c.pos) <= NEW_CONTACT_M && d2(b.pos, o.pos) <= t.contactNearSquadM);
        if (!closeBefore) best = { d, c };
      }
      if (best) out.push(`contact ${Math.round(best.d)} m from ${id}`);
    }
  }
  let lost = 0;
  for (const [id, b] of bs) {
    const s = cs.get(id), now = s && isNum(s.agents) ? s.agents : 0;
    if (isNum(b?.agents)) lost += Math.max(0, b.agents - now);
    if (t.squadLost && isNum(b?.agents) && b.agents > 0 && now <= 0) out.push(`${id} lost`);
    if (!s) continue;
    if (t.strengthDropPct > 0 && isNum(b.agents) && isNum(s.agents)) {
      const start = s.startAgents > 0 ? s.startAgents : b.startAgents;
      const pct = start > 0 ? 100 * (b.agents - s.agents) / start : 0;
      if (pct >= t.strengthDropPct && s.agents > 0) out.push(`${id} lost ${Math.round(pct)}% strength`);
    }
    if (t.objectiveReached) {
      const reached = (x) => okPos(x?.objective?.pos) && okPos(x?.pos) && d2(x.pos, x.objective.pos) <= OBJECTIVE_M;
      const idle = (x) => IDLE_MODES.has(String(x?.mode ?? "").toLowerCase());
      if ((reached(s) && !reached(b)) || (idle(s) && !idle(b))) out.push(`${id} reached objective`);
    }
    if (t.squadChange && !!b.humanLed !== !!s.humanLed) out.push(`${id} ${s.humanLed ? "now player-led" : "no longer player-led"}`);
  }
  if (t.casualties > 0 && lost >= t.casualties) out.push(`${lost} casualties`);
  if (t.squadChange && !(t.unitSpawned && isV2(sit))) for (const id of cs.keys()) if (!bs.has(id)) out.push(`${id} appeared`);
  return [...new Set(out)].slice(0, 8);
}

// Interval multiplier from the OpenRouter pace estimate: projected spend over the key limit stretches the interval (max x3).
function stretchFactor(c) {
  if (!c.wakeup.budgetStretch) return 1;
  const p = pace(), limit = keyState.data?.limit;
  if (p?.kind !== "period" || !isNum(p.projected) || !isNum(limit) || limit <= 0 || p.projected <= limit) return 1;
  return Math.min(p.projected / limit, 3);
}

// Next wakeup after a call (or a skipped scheduled check). chosen = model's next_check_sec or null.
function scheduleNext(c, sc, chosen, reason, now) {
  const w = c.wakeup;
  let sec = w.defaultSec;
  if (w.dynamic && isNum(chosen)) sec = Math.max(w.minSec, Math.min(w.maxSec, chosen));
  sc.chosenSec = sec; sc.chosenReason = w.dynamic && isNum(chosen) ? reason : "";
  sc.stretch = stretchFactor(c);
  sc.intervalSec = Math.round(sec * sc.stretch);
  sc.nextDueAt = now + sc.intervalSec * 1000;
}

// Decide for one poll: {call, reason, triggers} or {gate:true} or {skip:"..."}.
function decideWake(c, sc, lc, sit, now) {
  if (c.minIntervalSec > 0 && now - lc.ts < c.minIntervalSec * 1000) return { gate: true };
  if (sc.manual) return { call: true, reason: "manual", triggers: [] };
  if (!sc.baseline) return { call: true, reason: "first", triggers: [] };
  if (c.triggers.enabled && now - lc.ts >= c.triggers.cooldownSec * 1000) {
    const fired = evalTriggers(c.triggers, sc.baseline, sit, sc.pendingEvents);
    if (fired.length) return { call: true, reason: "trigger", triggers: fired };
  }
  if (now >= sc.nextDueAt) {
    if (c.skipUnchanged && unchanged(sc.baseline, sit, sc.pendingEvents.length)) return { skip: "skipped: unchanged" };
    return { call: true, reason: "scheduled", triggers: [] };
  }
  return { gate: true };
}

function schedulerView() {
  const now = Date.now(), out = {};
  for (const [f, sc] of Object.entries(sched)) {
    out[f] = {
      nextDueInSec: sc.baseline || sc.nextDueAt ? Math.max(0, Math.round((sc.nextDueAt - now) / 1000)) : 0,
      chosenSec: sc.chosenSec, intervalSec: sc.intervalSec, chosenReason: sc.chosenReason, stretch: sc.stretch,
      lastWakeReason: sc.lastWakeReason, lastWakeTriggers: sc.lastWakeTriggers, lastLlmAgoSec: sc.lastLlmAt ? Math.round((now - sc.lastLlmAt) / 1000) : null,
      gated: sc.gated, manual: sc.manual, failed: sc.failed,
    };
  }
  return out;
}

// ---------- http helpers ----------
function send(res, code, obj) {
  res.writeHead(code, { "Content-Type": "application/json", "Cache-Control": "no-store" });
  res.end(JSON.stringify(obj));
}

// ---------- auth + loadouts wiring (modules auth.mjs / loadouts.mjs) ----------
const auth = AUTH.createAuth({ dataDir: DATA_DIR, getAdmin: () => cfg.admin, setAdmin: (a) => { cfg.admin = a; saveConfig(); } });
const loadouts = LO.createStore(DATA_DIR, { log: (m) => console.log(m) });
const GAME_ROUTES = new Set(["/health", "/orders", "/hq", "/manifest", "/translate", "/world", "/world/roads", "/world/locations", "/world/buildings", "/loadouts/player", "/loadouts/sync", "/loadouts/status", "/control"]);
const redirect = (res, to) => { res.writeHead(302, { Location: to, "Cache-Control": "no-store" }); res.end(); };

const MIME = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".mjs": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".woff2": "font/woff2", ".png": "image/png", ".json": "application/json",
};
function serveStatic(res, name) {
  fs.readFile(path.join(DASH_DIR, name), (err, data) => {
    if (err) return send(res, 404, { error: "not found" });
    res.writeHead(200, { "Content-Type": MIME[path.extname(name)] ?? "application/octet-stream", "Cache-Control": "no-store" });
    res.end(data);
  });
}

// CLW_HOST=0.0.0.0 opens the dashboard to the LAN: then private-network addresses pass the host/origin checks too
// (anyone in the LAN can then pause the sidecar or change its config; the API key is never sent to the browser).
const LAN = HOST !== "127.0.0.1" && HOST !== "localhost";
// CLW_ALLOWED_HOSTS: extra host names (comma-separated, e.g. a public domain or IP behind a reverse proxy) for a sidecar on another machine
const EXTRA_HOSTS = (process.env.CLW_ALLOWED_HOSTS ?? "").split(",").map((h) => h.trim()).filter(Boolean)
  .map((h) => h.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"));
const HOST_NAMES = (LAN
  ? "localhost|127\\.0\\.0\\.1|10\\.\\d+\\.\\d+\\.\\d+|192\\.168\\.\\d+\\.\\d+|172\\.(1[6-9]|2\\d|3[01])\\.\\d+\\.\\d+|[\\w-]+(\\.local|\\.lan)?"
  : "localhost|127\\.0\\.0\\.1") + EXTRA_HOSTS.map((h) => "|" + h).join("");
const LOCAL_HOST = new RegExp(`^(${HOST_NAMES})(:\\d+)?$`, "i");
const LOCAL_ORIGIN = new RegExp(`^https?:\\/\\/(${HOST_NAMES})(:\\d+)?$`, "i");
const hostOk = (req) => LOCAL_HOST.test(req.headers.host ?? "");
const originOk = (req) => !req.headers.origin || LOCAL_ORIGIN.test(req.headers.origin);

function readBody(req, limit = 2_000_000) {
  return new Promise((resolve, reject) => {
    let body = "";
    req.on("data", (c) => { body += c; if (body.length > limit) { reject(new Error("body too large")); req.destroy(); } });
    req.on("end", () => resolve(body));
    req.on("error", reject);
  });
}

// ---------- handlers ----------
async function handleOrders(req, res) {
  let sit;
  let rawBody = "";
  try { rawBody = await readBody(req); sit = JSON.parse(rawBody); } catch (e) {
    const at = Number(/position (\d+)/.exec(e.message)?.[1]) || 0; // show where the game's JSON broke
    console.log("  orders: invalid JSON (" + e.message + "): " + rawBody.slice(Math.max(0, at - 200), at + 200));
    return send(res, 400, { error: "invalid JSON" });
  }
  let gameSitrep = null; // the game's rules SITREP rides along; it never goes to the model
  if (sit && typeof sit === "object" && "sitrep" in sit) { gameSitrep = sit.sitrep; delete sit.sitrep; }
  if (sit && typeof sit === "object" && "debug" in sit) { addDebugLines(sit.debug); delete sit.debug; } // game debug lines: dashboard only, never the model
  let manifestHash = null; // protocol v3: hash of the action manifest the situation was built for; never goes to the model
  if (sit && typeof sit === "object" && "manifest" in sit) { manifestHash = typeof sit.manifest === "string" && sit.manifest ? sit.manifest.slice(0, 64) : null; delete sit.manifest; }
  let langs = []; // protocol v4: recipient languages of broadcast texts (SITREP); never goes to the model
  if (sit && typeof sit === "object" && "langs" in sit) { langs = I18N.normLangs(sit.langs); delete sit.langs; }
  let hqList = null; // pending HQ chat messages (M3 phase A): they start agent turns, they never go to the strategic model
  if (sit && typeof sit === "object" && "hq" in sit) { hqList = sit.hq; delete sit.hq; }
  const t0 = Date.now();
  const snapshot = { ...cfg };
  const faction = typeof sit?.faction === "string" ? sit.faction : "?";
  const known = manifestHash ? manifests[faction] : null;
  const manifest = known && known.hash === manifestHash ? known : DEFAULT_MANIFEST;
  if (sit && typeof sit === "object" && Array.isArray(sit.squads)) hqSitCache[faction] = { sit, ts: Date.now() }; // for POST /hq, available while the commander call is still running
  if (hqList) hqIngest(faction, sit, hqList, manifests[faction] ?? DEFAULT_MANIFEST, snapshot);
  const needManifest = !!manifestHash && manifest === DEFAULT_MANIFEST; // v3 post with an unknown hash: ask the game to resend
  const lc = (lastCall[faction] ??= { ts: 0, sit: null });
  const valid = !!sit && Array.isArray(sit.squads);
  const sc = schedFor(faction);
  const v2 = valid && isV2(sit);
  let r, wake = null, interval = null;
  const newEvents = v2 && Array.isArray(sit.events) ? sit.events.filter((e) => typeof e === "string" && e).map((e) => e.slice(0, 200)) : [];
  if (newEvents.length) { // events are "since the last POST": keep them for the model until its next call, and for the dashboard log
    sc.pendingEvents.push(...newEvents);
    if (sc.pendingEvents.length > 30) sc.pendingEvents.splice(0, sc.pendingEvents.length - 30);
  }
  if (v2) journalTick(faction, sit, newEvents, state.factions[faction]?.situation ?? null); // HQ journal: war boundary + outcome events -> tries
  // a v2 faction without squads can still spend supplies
  const canBuy = v2 && isNum(sit.war?.supplies) && Array.isArray(sit.catalog) && sit.catalog.some((e) => isNum(e?.cost) && e.cost <= sit.war.supplies);
  const phase = v2 && typeof sit.war?.phase === "string" ? sit.war.phase : "running";
  if (phase !== "running") {
    // setup / ended: the game is not fighting yet (or anymore): no LLM call, keep, not an exchange
    sc.gated++;
    r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: "phase " + phase, skipped: true, gated: true, phaseGate: true };
  } else if (valid && sit.squads.length === 0 && !canBuy) {
    // faction has no squads left: just record the (empty) situation so the dashboard stops showing the last survivors; no LLM call
    r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: "no squads left", gated: true, rules: true };
  } else if (snapshot.enabled && snapshot.engine !== "llm") {
    // rules/model engine: game's rule commander drives; not an exchange, only counted as gated
    if (snapshot.engine === "model" && !modelEngineLogged) { modelEngineLogged = true; console.log("engine 'model' is not implemented yet: behaving like 'rules'"); }
    r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: "engine: " + snapshot.engine, gated: true, rules: true };
  } else if (snapshot.enabled && paused) {
    sc.gated++;
    r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: "paused", skipped: true, gated: true, paused: true };
  } else if (snapshot.enabled && valid && budgetStopped(snapshot)) r = { orders: [], dropped: [], rawReply: null, usage: null, ok: false, reason: "budget limit reached" };
  else if (!snapshot.enabled) r = { orders: [], dropped: [], rawReply: null, usage: null, ok: false, reason: "disabled" };
  else {
    const now = Date.now();
    const dec = valid ? decideWake(snapshot, sc, lc, sit, now) : { call: true, reason: "first", triggers: [] }; // invalid body: computeOrders reports the error
    if (dec.gate) {
      sc.gated++; // not an exchange: only counted; after a failed call the game keeps its rule fallback instead of "keep"
      r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: "gated", skipped: true, gated: true };
    } else if (dec.skip) {
      scheduleNext(snapshot, sc, sc.chosenSec, sc.chosenReason, now);
      r = { orders: [], dropped: [], rawReply: null, usage: null, ok: true, reason: dec.skip, skipped: true };
    } else {
      wake = { reason: dec.reason, triggers: dec.triggers, sinceLastSec: lc.ts ? Math.round((now - lc.ts) / 1000) : 0 };
      lc.ts = now;
      if (valid) { sc.manual = false; sc.baseline = sit; sc.lastLlmAt = now; sc.lastWakeReason = wake.reason; sc.lastWakeTriggers = wake.triggers; }
      const callSit = !valid ? sit : v2 ? { ...annotateContacts(sit), events: sc.pendingEvents.slice(-15), wake } : { ...sit, wake };
      if (valid) sc.pendingEvents = [];
      const srCfg = snapshot.sitrep, srSt = srFor(faction);
      const wantSitrep = v2 && !!srCfg?.enabled && (!srSt.lastAt || Date.now() - srSt.lastAt >= srCfg.intervalSec * 1000 || dec.reason === "manual" || dec.triggers.some((t) => /objective|tickets|lost/i.test(t)));
      r = await computeOrders(snapshot, callSit, false, { sitrep: wantSitrep, manifest, journal: v2 ? journalBlock(faction, snapshot, sit) : "" });
      if (v2 && r.ok && !r.keep) journalOrders(faction, sit, r.orders);
      if (r.sitrep) {
        srSt.lastAt = Date.now();
        srSt.llm = { value: r.sitrep, generatedAt: srSt.lastAt, warTimeSec: isNum(sit.time) ? sit.time : 0 };
        console.log(`sitrep faction=${faction} source=${snapshot.provider} headline="${r.sitrep.headline}"`);
        if (langs.length && snapshot.i18n.enabled) { // one weak-LLM translation call per non-English language, parallel, cached; failures omit that language
          const tr = await translator.translateFields(sitrepFields(r.sitrep), langs);
          r.sitrep = { ...r.sitrep, text_by_lang: tr.by_lang };
          srSt.llm.textByLang = tr.by_lang;
          r.i18n = { langs, ok: Object.keys(tr.by_lang).filter((l) => l !== "en"), cached: tr.cached, failed: tr.failed, slot: slotCfg(snapshot, "translate") === snapshot ? "strong" : "weak" };
          state.counters.errors += tr.failed.length;
          console.log(`sitrep faction=${faction} translated langs=[${r.i18n.ok}] cached=[${tr.cached}] failed=[${tr.failed.map((x) => x.lang + ":" + x.reason)}]`);
        }
      } else if (wantSitrep && r.ok) console.log(`sitrep faction=${faction}: requested but the reply had none`);
      if (valid) {
        sc.failed = !r.ok;
        if (r.ok) scheduleNext(snapshot, sc, r.nextCheck?.sec, r.nextCheck?.reason ?? "", Date.now());
        else { sc.chosenSec = snapshot.wakeup.minSec; sc.chosenReason = "retry after error"; sc.intervalSec = sc.chosenSec; sc.nextDueAt = Date.now() + sc.intervalSec * 1000; }
        interval = sc.intervalSec;
      }
    }
  }
  if (v2) journalMaybeReview(faction, snapshot, sit);
  const latencyMs = Date.now() - t0;

  if (r.gated) state.counters.gated++;
  else {
    state.counters.requests++;
    if (r.skipped) state.counters.skipped++;
    if (!r.ok && r.reason !== "disabled" && r.reason !== "budget limit reached") state.counters.errors++;
    if (!r.skipped && !r.keep && (!r.ok || (r.orders.length === 0 && !r.buy?.length && !r.missions?.length && !r.fobs?.length))) state.counters.fallbacks++;
    if (r.usage) { state.counters.inputTokens += r.usage.input; state.counters.outputTokens += r.usage.output; }
  }
  state.lastRequestTs = Date.now();
  if (sit && typeof sit === "object") {
    // Unknown squad fields are passed through untouched (situation is stored as received).
    // prevPos / orderTs are derived helpers for the Squads view (movement delta, order age).
    const old = state.factions[faction];
    const prevPos = {}, orderTs = { ...(old?.orderTs ?? {}) };
    for (const s of old?.situation?.squads ?? []) {
      if (s && Array.isArray(s.pos)) prevPos[s.id] = { pos: s.pos, ts: old.ts, time: old.situation.time };
    }
    for (const o of r.orders) orderTs[o.squad] = state.lastRequestTs;
    const keepOld = r.skipped || r.keep;
    const eventLog = [...(old?.eventLog ?? [])];
    for (const text of newEvents.slice().reverse()) eventLog.unshift({ ts: state.lastRequestTs, time: isNum(sit.time) ? sit.time : null, text });
    if (eventLog.length > 40) eventLog.length = 40;
    // war epoch (read-only bookkeeping for activeFactions): a faction whose game time went backwards starts a new epoch, unless it
    // is catching up with an epoch another faction already started.
    if (old && (old.epoch ?? 0) === (state.epoch ?? 0) && isNum(old.situation?.time) && isNum(sit.time) && sit.time < old.situation.time - 5) state.epoch = (state.epoch ?? 0) + 1;
    state.factions[faction] = { ts: state.lastRequestTs, epoch: state.epoch ?? 0, situation: sit, side: isNum(sit.side) ? sit.side : null, identity: typeof sit.identity === "string" ? sit.identity : null, hqTrait: typeof sit.hq?.trait === "string" ? sit.hq.trait : null, manifest: manifestHash ? { hash: manifestHash, known: !needManifest } : null, sitrep: composeSitrep(faction, sit, gameSitrep), orders: keepOld ? (old?.orders ?? []) : r.orders,
      buy: keepOld ? (old?.buy ?? []) : (r.buy ?? []), missions: keepOld ? (old?.missions ?? []) : (r.missions ?? []), fobs: keepOld ? (old?.fobs ?? []) : (r.fobs ?? []), eventLog, prevPos, orderTs };
  }
  if (!r.gated) state.exchanges.unshift({
    ts: state.lastRequestTs, faction, provider: snapshot.enabled ? snapshot.provider : "disabled",
    model: snapshot.provider === "fake" ? null : snapshot.model, modelUsed: r.modelUsed ?? (snapshot.provider === "fake" ? null : snapshot.model), fellBack: !!r.fellBack, primaryError: r.primaryError ?? null, i18n: r.i18n ?? null,
    request: sit, rawReply: r.rawReply, orders: r.orders, buy: r.buy ?? [], missions: r.missions ?? [], fobs: r.fobs ?? [], keep: !!r.keep, sitrep: r.sitrep ?? null, dropped: r.dropped,
    latencyMs, ok: r.ok, reason: r.reason, usage: r.usage,
    wake, nextCheck: r.nextCheck ?? null, intervalSec: interval,
  });
  if (state.exchanges.length > RING_MAX) state.exchanges.length = RING_MAX;

  const nContacts = Array.isArray(sit?.contacts) ? sit.contacts.length : 0;
  if (!r.gated) console.log(`orders faction=${faction} squads=${sit?.squads?.length ?? 0} contacts=${nContacts} orders=${r.orders.length}${v2 ? ` buy=${r.buy?.length ?? 0} missions=${r.missions?.length ?? 0}${r.fobs?.length ? ` fobs=${r.fobs.length}` : ""}` : ""} ${latencyMs}ms${r.skipped ? " (" + r.reason + ")" : ""}${wake ? " wake=" + wake.reason + (wake.triggers.length ? " [" + wake.triggers.join("; ") + "]" : "") : ""}${r.nextCheck ? " next=" + r.nextCheck.sec + "s" : ""}`);
  const reply = r.rules ? { orders: [] } : r.paused || r.phaseGate ? { orders: [], keep: true } : r.gated && sc.failed ? { orders: [] } : r.skipped || r.keep ? { orders: [], keep: true, ...(r.sitrep ? { sitrep: r.sitrep } : {}) }
    : v2 ? { orders: r.orders, buy: r.buy ?? [], missions: r.missions ?? [], ...(r.fobs?.length ? { fobs: r.fobs } : {}), ...(r.sitrep ? { sitrep: r.sitrep } : {}) } : { orders: r.orders };
  if (needManifest) reply.needManifest = true;
  Object.assign(reply, hqDrain(faction));
  send(res, 200, reply);
}

// POST /hq {faction, msg:{id,player,role,squad,grid,pos,lang,text,ageSec,history}} -> {handled, reason?, hqReplies?, hqActions?} (see FAST PATH above).
// The response comes when the agent turn ends (<= HQ_FAST_TURN_SEC); handled:false = nothing was started (no cached situation, duplicate, bad message).
async function handleHq(req, res) {
  let body;
  try { body = JSON.parse(await readBody(req)); } catch { return send(res, 400, { error: "invalid JSON" }); }
  const faction = typeof body?.faction === "string" ? body.faction : "";
  const msg = cleanHqMessage(body?.msg);
  if (!faction || !msg) return send(res, 400, { error: "faction and msg {id,text,...} required" });
  const cached = hqSitCache[faction];
  if (!cached) return send(res, 200, { handled: false, reason: "no situation cached for " + faction });
  let closed = false, done = false;
  res.on("close", () => { closed = true; });
  const fast = {
    skip: (reason) => { done = true; send(res, 200, { handled: false, reason }); },
    deliver: (out) => {
      if (done || closed || res.destroyed || res.writableEnded) return false;
      done = true;
      send(res, 200, { handled: true, ...(out.actions.length ? { hqActions: out.actions } : {}), ...(out.replies.length ? { hqReplies: out.replies } : {}) });
      return true;
    },
  };
  hqIngest(faction, cached.sit, [body.msg], manifests[faction] ?? DEFAULT_MANIFEST, { ...cfg }, fast);
}

// ---------- world info, map image, vector overlay (roads, locations) ----------
// Game -> POST /world (every 60 s), /world/roads (chunks), /world/locations. The map image is resolved in the background by
// ./map/index.mjs from the user's own game install (never committed); vector data is cached in .cache/vector/<worldKey>.json.
const CACHE_DIR = DATA_DIR;
const WORLD_FILE = path.join(CACHE_DIR, "world.json");
const VECTOR_DIR = path.join(CACHE_DIR, "vector");
const worldState = { info: null, key: "", ts: 0 };
const mapState = { status: "none", key: "", pngPath: null, width: 0, height: 0, source: "", texturePath: "", calibration: null, error: null, ts: 0, detectedGameDir: "" };
const vectors = new Map();   // worldKey -> {roads: [...]|null, locations: [...]|null}
const roadAsm = new Map();   // worldKey -> {chunks: Map(idx -> roads), total: -1}
const bldAsm = new Map();    // same for buildings: chunks of [cx,cz,w,d,yaw,cat]
const numArr = (a, n) => Array.isArray(a) && a.length >= n && a.slice(0, n).every(isNum);

function worldKeyOf(info) {
  const base = (s) => String(s ?? "").replace(/\\/g, "/").split("/").pop().replace(/\.[^.]*$/, "");
  const k = (base(info.mapPrefab).replace(/^MapEntity_/i, "") || base(info.worldFile) || "world").replace(/[^A-Za-z0-9_-]/g, "_").slice(0, 60);
  return k || "world";
}

function cleanWorld(b) {
  if (!b || typeof b !== "object") return null;
  const str = (v) => (typeof v === "string" ? v.slice(0, 300) : "");
  return {
    worldFile: str(b.worldFile), mapPrefab: str(b.mapPrefab),
    mapOffset: numArr(b.mapOffset, 2) ? b.mapOffset.slice(0, 2) : [0, 0], mapSize: numArr(b.mapSize, 2) ? b.mapSize.slice(0, 2) : [0, 0],
    boundsMin: numArr(b.boundsMin, 3) ? b.boundsMin.slice(0, 3) : [0, 0, 0], boundsMax: numArr(b.boundsMax, 3) ? b.boundsMax.slice(0, 3) : [0, 0, 0],
  };
}

function loadVectors(key) {
  if (!vectors.has(key)) {
    let v = { roads: null, locations: null, buildings: null };
    try { const j = JSON.parse(fs.readFileSync(path.join(VECTOR_DIR, key + ".json"), "utf8")); v = { roads: Array.isArray(j.roads) ? j.roads : null, locations: Array.isArray(j.locations) ? j.locations : null, buildings: Array.isArray(j.buildings) ? j.buildings : null }; } catch { /* none cached */ }
    vectors.set(key, v);
  }
  return vectors.get(key);
}
function saveVectors(key) {
  try { fs.mkdirSync(VECTOR_DIR, { recursive: true }); fs.writeFileSync(path.join(VECTOR_DIR, key + ".json"), JSON.stringify(vectors.get(key))); }
  catch (e) { console.log(`could not save vector cache: ${e.message}`); }
}

// Image resolver: lazy import so the sidecar still starts if ./map/index.mjs is missing or broken.
async function resolveMap(force = false) {
  const info = worldState.info;
  if (!info) return;
  const key = worldState.key;
  if (!force && mapState.key === key && mapState.status !== "none" && mapState.status !== "error") return;
  Object.assign(mapState, { status: "resolving", key, pngPath: null, width: 0, height: 0, source: "", texturePath: "", calibration: null, error: null, ts: Date.now() });
  try {
    const mod = await import("./map/index.mjs");
    let gameDir = cfg.gameDir;
    try { mapState.detectedGameDir = (await mod.findGameDir?.()) || ""; } catch { mapState.detectedGameDir = ""; }
    if (!gameDir) gameDir = mapState.detectedGameDir;
    const r = await mod.resolveMapImage({
      mapPrefab: info.mapPrefab, worldFile: info.worldFile, gameDir, extraDirs: [], maxSize: cfg.mapMaxSize,
      cacheDir: path.join(CACHE_DIR, "maps"), overrideDir: path.join(DIR, "maps"),
    });
    if (mapState.key !== key) return; // world changed meanwhile
    if (!r || !r.pngPath) mapState.status = "notfound";
    else Object.assign(mapState, { status: "ready", pngPath: r.pngPath, width: r.width, height: r.height, source: r.source ?? "", texturePath: r.texturePath ?? "", calibration: r.calibration ?? null });
    console.log(`map ${key}: ${mapState.status}${r ? ` (${r.source}, ${r.width}x${r.height})` : ""}`);
  } catch (e) {
    mapState.status = "error"; mapState.error = String(e?.message ?? e);
    console.log(`map ${key}: resolve failed: ${mapState.error}`);
  }
  mapState.ts = Date.now();
}

function mapExtent() {
  const c = mapState.calibration, i = worldState.info;
  if (c && isNum(c.x0) && isNum(c.z0) && isNum(c.x1) && isNum(c.z1)) return { x0: c.x0, z0: c.z0, x1: c.x1, z1: c.z1 };
  if (i && i.mapSize[0] > 0 && i.mapSize[1] > 0) return { x0: i.mapOffset[0], z0: i.mapOffset[1], x1: i.mapOffset[0] + i.mapSize[0], z1: i.mapOffset[1] + i.mapSize[1] };
  if (i && i.boundsMax[0] > i.boundsMin[0] && i.boundsMax[2] > i.boundsMin[2]) return { x0: i.boundsMin[0], z0: i.boundsMin[2], x1: i.boundsMax[0], z1: i.boundsMax[2] };
  return null;
}

function worldView() {
  const v = worldState.key ? loadVectors(worldState.key) : { roads: null, locations: null, buildings: null };
  return {
    worldKey: worldState.key, info: worldState.info, ts: worldState.ts, extent: mapExtent(),
    map: { status: mapState.status, width: mapState.width, height: mapState.height, source: mapState.source, texturePath: mapState.texturePath, flipY: !!mapState.calibration?.flipY, error: mapState.error, ts: mapState.ts, extent: mapExtent() },
    vector: { roads: v.roads ? v.roads.length : null, locations: v.locations ? v.locations.length : null, buildings: v.buildings ? v.buildings.length : null },
    gameDir: cfg.gameDir, detectedGameDir: mapState.detectedGameDir, mapMaxSize: cfg.mapMaxSize,
  };
}

function setWorld(info) {
  const key = worldKeyOf(info), changed = key !== worldState.key || JSON.stringify(info) !== JSON.stringify(worldState.info);
  worldState.info = info; worldState.key = key; worldState.ts = Date.now();
  if (changed) {
    try { fs.mkdirSync(CACHE_DIR, { recursive: true }); fs.writeFileSync(WORLD_FILE, JSON.stringify(info, null, 2)); } catch (e) { console.log(`could not save world.json: ${e.message}`); }
    console.log(`world ${key}: mapPrefab=${info.mapPrefab || "-"} worldFile=${info.worldFile || "-"} mapSize=${info.mapSize.join("x")}`);
  }
  void resolveMap(false);
}
try { const j = cleanWorld(JSON.parse(fs.readFileSync(WORLD_FILE, "utf8"))); if (j) setWorld(j); } catch { /* no cached world yet */ }

function validRoads(arr) {
  const out = [];
  for (const r of Array.isArray(arr) ? arr : []) {
    if (r && isNum(r.w) && Array.isArray(r.p) && r.p.length >= 4 && r.p.length % 2 === 0 && r.p.every(isNum)) out.push({ w: r.w, p: r.p });
  }
  return out;
}

// Game-facing POST /world, /world/roads, /world/locations (no Origin check: the game sends none; localhost host only).
async function handleWorldPost(req, res, url) {
  if (!hostOk(req)) return send(res, 403, { error: "forbidden host" });
  let b, raw = "";
  try { raw = await readBody(req); b = JSON.parse(raw); } catch (e) {
    console.log(`world post ${url}: invalid JSON (${e.message}) len=${raw.length}`);
    try { fs.mkdirSync(path.join(path.dirname(fileURLToPath(import.meta.url)), ".cache"), { recursive: true }); fs.writeFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), ".cache", "bad-world-body.txt"), raw); } catch {}
    return send(res, 400, { error: "invalid JSON" });
  }
  if (url === "/world") {
    const info = cleanWorld(b);
    if (!info) return send(res, 400, { error: "bad world info" });
    setWorld(info);
    const v = loadVectors(worldState.key);
    return send(res, 200, { ok: true, worldKey: worldState.key, needRoads: !v.roads, needLocations: !v.locations, needBuildings: !v.buildings });
  }
  const key = typeof b?.worldKey === "string" ? b.worldKey.replace(/[^A-Za-z0-9_-]/g, "_").slice(0, 60) : "";
  if (!key) return send(res, 400, { error: "worldKey missing" });
  const v = loadVectors(key);
  if (url === "/world/locations") {
    const items = (Array.isArray(b.items) ? b.items : []).filter((i) => i && typeof i.name === "string" && numArr(i.pos, 2))
      .map((i) => ({ name: i.name.slice(0, 80), type: typeof i.type === "string" ? i.type.slice(0, 30) : "", kind: typeof i.kind === "string" && /^[a-z]{1,20}$/.test(i.kind) ? i.kind : "", faction: typeof i.faction === "string" ? i.faction.replace(/[^A-Za-z0-9_-]/g, "").slice(0, 20) : "", pos: [i.pos[0], i.pos[1]] })).slice(0, 2000);
    v.locations = items; saveVectors(key);
    console.log(`world ${key}: locations n=${items.length}`);
    return send(res, 200, { ok: true, n: items.length });
  }
  if (url === "/world/buildings") {
    if (!Number.isInteger(b.chunk) || b.chunk < 0) return send(res, 400, { error: "bad chunk" });
    if (b.chunk === 0 && bldAsm.get(key)?.chunks.has(0)) bldAsm.set(key, { chunks: new Map(), total: -1 }); // real restart only (parallel POSTs may arrive out of order)
    const basm = bldAsm.get(key) ?? (bldAsm.set(key, { chunks: new Map(), total: -1 }), bldAsm.get(key));
    basm.chunks.set(b.chunk, (Array.isArray(b.b) ? b.b : []).filter((r) => Array.isArray(r) && r.length >= 5 && r.slice(0, 5).every(isNum)).map((r) => [r[0], r[1], r[2], r[3], r[4], isNum(r[5]) ? r[5] : 0]));
    if (b.last === true && Number.isInteger(b.total)) basm.total = b.total;
    if (basm.total >= 0 && basm.chunks.size >= basm.total) {
      const list = [];
      for (let i = 0; i < basm.total; i++) for (const r of basm.chunks.get(i) ?? []) list.push(r);
      v.buildings = list; bldAsm.delete(key); saveVectors(key);
      console.log(`world ${key}: buildings n=${list.length} chunks=${basm.total}`);
    }
    return send(res, 200, { ok: true });
  }
  // /world/roads
  if (!Number.isInteger(b.chunk) || b.chunk < 0) return send(res, 400, { error: "bad chunk" });
  if (b.chunk === 0) roadAsm.set(key, { chunks: new Map(), total: -1 }); // a restart of the upload discards a partial one
  const asm = roadAsm.get(key) ?? (roadAsm.set(key, { chunks: new Map(), total: -1 }), roadAsm.get(key));
  asm.chunks.set(b.chunk, validRoads(b.roads));
  if (b.last === true && Number.isInteger(b.total)) asm.total = b.total;
  if (asm.total >= 0 && asm.chunks.size >= asm.total) {
    const roads = [];
    for (let i = 0; i < asm.total; i++) roads.push(...(asm.chunks.get(i) ?? []));
    v.roads = roads; roadAsm.delete(key); saveVectors(key);
    console.log(`world ${key}: roads n=${roads.length} chunks=${asm.total}`);
  }
  return send(res, 200, { ok: true });
}

function handleWorldApi(req, res, url) {
  if (req.method === "GET" && url === "/api/world") return send(res, 200, worldView());
  if (req.method === "GET" && url === "/api/vector") {
    const v = worldState.key ? loadVectors(worldState.key) : { roads: null, locations: null, buildings: null };
    return send(res, 200, { roads: v.roads ?? [], locations: v.locations ?? [], buildings: v.buildings ?? [] });
  }
  if (req.method === "GET" && url === "/api/map.png") {
    if (mapState.status !== "ready" || !mapState.pngPath) return send(res, 404, { error: "no map image" });
    return fs.readFile(mapState.pngPath, (err, data) => {
      if (err) return send(res, 404, { error: "map image missing" });
      res.writeHead(200, { "Content-Type": "image/png", "Cache-Control": "public, max-age=3600" });
      res.end(data);
    });
  }
  if (req.method === "POST" && (url === "/api/map/reload" || url === "/api/vector/reset")) {
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    if (url === "/api/map/reload") { void resolveMap(true); return send(res, 200, { ok: true }); }
    const key = worldState.key;
    if (key) { vectors.set(key, { roads: null, locations: null, buildings: null }); roadAsm.delete(key); bldAsm.delete(key); try { fs.rmSync(path.join(VECTOR_DIR, key + ".json"), { force: true }); } catch { /* ignore */ } }
    return send(res, 200, { ok: true });
  }
  return false;
}

// Factions of the CURRENT war (generic, no names): same epoch as the newest post and last post within STALE_FACTION_MS of the newest one.
// Setup-phase factions that stopped posting after the host picked other factions, or factions of a previous war, drop out.
const STALE_FACTION_MS = 60_000;
function activeFactions() {
  const es = Object.entries(state.factions);
  if (!es.length) return [];
  const ep = state.epoch ?? 0, newest = Math.max(...es.map(([, e]) => e.ts ?? 0));
  let live = es.filter(([, e]) => (e.epoch ?? 0) === ep && (e.ts ?? 0) >= newest - STALE_FACTION_MS);
  // once a war runs, factions still reporting the setup phase are the setup defaults the host did not pick
  const phase = (e) => e.situation?.war?.phase;
  if (live.some(([, e]) => phase(e) === "running")) live = live.filter(([, e]) => phase(e) !== "setup");
  // side A first (situation.side), unknown side last, then alphabetical
  const sideOf = (e) => (isNum(e.situation?.side) && e.situation.side >= 0 ? e.situation.side : 99);
  return live.sort((x, y) => sideOf(x[1]) - sideOf(y[1]) || x[0].localeCompare(y[0])).map(([n]) => n);
}

// Manifests for the dashboard (what each faction's game last sent) plus the built-in fallback.
function manifestsView() {
  return { factions: manifests, builtin: DEFAULT_MANIFEST };
}

async function handleApi(req, res, url) {
  if (!hostOk(req)) return send(res, 403, { error: "forbidden host" });
  const loCtx = { send, readBody, originOk };
  if (await AUTH.handleHttp(auth, req, res, url, loCtx) !== false) return; // /api/login, /api/logout, /api/session, /api/password
  if (!auth.isLoggedIn(req)) return send(res, 401, { error: "login required" }); // every other /api/* endpoint needs the session
  if (await LO.handleApi(loadouts, req, res, url, loCtx)) return; // /api/loadouts*
  if (handleWorldApi(req, res, url) !== false) return;
  if (req.method === "GET" && url === "/api/state") {
    return send(res, 200, {
      now: Date.now(), startedAt: state.startedAt, lastRequestTs: state.lastRequestTs,
      factions: state.factions, activeFactions: activeFactions(), staleAfterSec: STALE_FACTION_MS / 1000,
      exchanges: state.exchanges, hq: hqLog.map(({ fast, ...e }) => e), debugLog: state.debugLog, manifests: manifestsView(), counters: state.counters, modelCooldowns: cooldownsView(), config: publicConfig(), auth: { defaultPassword: auth.isDefaultPassword() }, loadouts: { bad: loadouts.badCount(), restartId: loadouts.restartId() }, usage: usageView(), scheduler: schedulerView(), journals: journalsView(), paused,
    });
  }
  if (req.method === "POST" && url === "/api/pause") {
    if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) return send(res, 400, { error: "Content-Type must be application/json" });
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    let inp;
    try { inp = JSON.parse((await readBody(req, 10_000)) || "{}"); } catch { return send(res, 400, { error: "invalid JSON" }); }
    if (typeof inp?.paused !== "boolean") return send(res, 400, { error: "paused must be boolean" });
    if (paused && !inp.paused) for (const sc of Object.values(sched)) sc.manual = true; // resume: consult the HQ on the next poll
    paused = inp.paused;
    console.log(paused ? "HQ paused: no LLM calls" : "HQ resumed");
    return send(res, 200, { ok: true, paused });
  }
  if (req.method === "POST" && url === "/api/wake") {
    if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) return send(res, 400, { error: "Content-Type must be application/json" });
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    let inp;
    try { inp = JSON.parse((await readBody(req, 10_000)) || "{}"); } catch { return send(res, 400, { error: "invalid JSON" }); }
    const f = inp?.faction;
    if (f !== undefined && typeof f !== "string") return send(res, 400, { error: "faction must be a string" });
    const names = f ? [f] : Object.keys(state.factions);
    for (const n of names) schedFor(n).manual = true;
    return send(res, 200, { ok: true, woken: names });
  }
  if (req.method === "POST" && url === "/api/usage/refresh") {
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    await pollKey();
    return send(res, 200, { ok: !keyState.error, usage: usageView() });
  }
  if (req.method === "GET" && url === "/api/config") return send(res, 200, publicConfig());
  if (req.method === "POST" && (url === "/api/config" || url === "/api/test")) {
    if (!/^application\/json\b/i.test(req.headers["content-type"] ?? "")) return send(res, 400, { error: "Content-Type must be application/json" });
    if (!originOk(req)) return send(res, 403, { error: "forbidden origin" });
    let inp;
    try { inp = JSON.parse((await readBody(req, 100_000)) || "{}"); } catch { return send(res, 400, { error: "invalid JSON" }); }
    if (inp && typeof inp === "object" && "admin" in inp) return send(res, 400, { error: "the admin password is changed through POST /api/password" });
    // /api/test {slot:"strong"|"weak"|"translate", which:"primary"|"fallback"}: slot "strong" (default) = a commander call; "weak" / "translate" = a translation call
    // (translate = the weak slot, else strong); which "fallback" tests the slot's backup model on its own. Both are tested without any fallback routing.
    let testSlot = "strong", testWhich = "primary";
    if (url === "/api/test" && inp && typeof inp === "object") {
      if (inp.slot !== undefined) { if (!["strong", "weak", "translate"].includes(inp.slot)) return send(res, 400, { error: "slot must be strong, weak or translate" }); testSlot = inp.slot; delete inp.slot; }
      if (inp.which !== undefined) { if (!["primary", "fallback"].includes(inp.which)) return send(res, 400, { error: "which must be primary or fallback" }); testWhich = inp.which; delete inp.which; }
    }
    const testTranslate = testSlot !== "strong";
    const v = validateConfig(inp);
    if (v.error) return send(res, 400, { error: v.error });
    if (v.value.apiKey === "") delete v.value.apiKey; // empty = keep stored key
    if (url === "/api/config") {
      const wasEngine = cfg.engine;
      Object.assign(cfg, v.value);
      if (wasEngine !== "llm" && cfg.engine === "llm") for (const sc of Object.values(sched)) sc.manual = true; // back to LLM: call on the next poll
      migrateOpenRouter(cfg);
      keyState.data = null; keyState.error = null; keyState.costSincePoll = 0; checkBudget();
      void pollKey();
      const saved = saveConfig();
      return send(res, 200, { ok: true, saved, config: publicConfig() });
    }
    // /api/test: optional overrides in the body are used for this test only (not saved)
    const tcfg = { ...cfg, ...v.value };
    if (testTranslate) {
      if (testSlot === "weak" && (!tcfg.weak || tcfg.weak.provider === "none")) return send(res, 400, { error: "no weak slot configured" });
      const ch = slotChain(tcfg, "translate");
      const sc = testWhich === "fallback" ? ch.fallback : ch.primary;
      if (!sc) return send(res, 400, { error: "no backup model configured for this slot" });
      const t0 = Date.now();
      const one = I18N.createTranslator({ // one-off translator on exactly this model: no cache, no fallback routing, the body overrides apply
        callModel: async ({ system, userMsg, maxTokens, timeoutSec }) => callProvider(tcfg, { faction: "i18n" }, { system, userMsg, maxTokens, timeoutSec, role: "translate", slotCfg: sc }),
        isFake: () => sc.provider === "fake", timeoutMs: () => tcfg.i18n.translateTimeoutSec * 1000,
      });
      const tr = await one.translateFields({ test: "Enemy infantry sighted near the bridge." }, ["de"], { fresh: true });
      return send(res, 200, { ok: !tr.failed.length, text: tr.by_lang.de?.test ?? null, error: tr.failed[0]?.reason ?? null, latencyMs: Date.now() - t0, provider: sc.provider, model: sc.provider === "fake" ? null : sc.model, slot: ch.slot, which: testWhich });
    }
    let ccfg = tcfg;
    if (testWhich === "fallback") {
      const ch = slotChain(tcfg, "commander");
      if (!ch.fallback) return send(res, 400, { error: "no backup model configured for this slot" });
      ccfg = ch.fallback;
    }
    const sit = {
      faction: "TEST", time: 0,
      squads: [{ id: "TEST_Squad1", pos: [1000, 0, 1000], agents: 8, startAgents: 8, mode: "NONE", humanLed: false }],
      contacts: [{ pos: [1500, 0, 1200], desc: "2x infantry", ageSec: 10 }],
    };
    const t0 = Date.now();
    const r = await computeOrders(ccfg, sit, true);
    return send(res, 200, { ok: r.ok, orders: r.orders, dropped: r.dropped, rawReply: r.rawReply, error: r.reason, latencyMs: Date.now() - t0, provider: ccfg.provider, model: ccfg.provider === "fake" ? null : ccfg.model, slot: "strong", which: testWhich });
  }
  return send(res, 404, { error: "not found" });
}

http.createServer((req, res) => {
  // "/ch/<channel>/<route>" -> "/<route>": the game opens one REST context per channel (hq, cmd-<faction>) because one context runs its
  // requests one after another (CLW_SidecarLink.Context); the prefix only selects the game-side context.
  const url = (req.url ?? "/").split("?")[0].replace(/^\/ch\/[A-Za-z0-9_.-]+(?=\/)/, "");
  // game channel: no cookie, header X-CLW-Token when config gameToken is set
  if (GAME_ROUTES.has(url) && !AUTH.gameTokenOk(req, cfg.gameToken)) return send(res, 401, { error: "bad or missing X-CLW-Token" });
  if (url === "/control" || url.startsWith("/loadouts/")) {
    return void LO.handleGame(loadouts, req, res, url, { send, readBody, originOk }).then((h) => { if (!h && !res.headersSent) send(res, 404, { error: "not found" }); })
      .catch((e) => { console.log(`  loadouts handler error: ${e?.message ?? e}`); if (!res.headersSent) send(res, 500, { error: "internal" }); });
  }
  if (req.method === "GET" && url === "/health") return send(res, 200, { ok: true, mode: cfg.provider });
  if (req.method === "POST" && url === "/manifest") {
    return void handleManifest(req, res).catch((e) => { if (!res.headersSent) send(res, 500, { error: String(e?.message ?? e) }); });
  }
  if (req.method === "POST" && url === "/translate") {
    return void handleTranslate(req, res).catch((e) => { if (!res.headersSent) send(res, 500, { error: String(e?.message ?? e) }); });
  }
  if (req.method === "POST" && url === "/hq") {
    return void handleHq(req, res).catch((e) => { console.log(`  hq handler error: ${e?.message ?? e}`); if (!res.headersSent) send(res, 500, { error: "internal" }); });
  }
  if (req.method === "POST" && url === "/orders") {
    return void handleOrders(req, res).catch((e) => { console.log(`  handler error: ${e?.message ?? e}`); if (!res.headersSent) send(res, 500, { error: "internal" }); });
  }
  if (req.method === "POST" && (url === "/world" || url === "/world/roads" || url === "/world/locations" || url === "/world/buildings")) {
    return void handleWorldPost(req, res, url).catch((e) => { if (!res.headersSent) send(res, 500, { error: String(e?.message ?? e) }); });
  }
  if (url.startsWith("/api/")) {
    return void handleApi(req, res, url).catch((e) => { if (!res.headersSent) send(res, 500, { error: String(e?.message ?? e) }); });
  }
  if (req.method === "GET") {
    if (!hostOk(req)) return send(res, 403, { error: "forbidden host" });
    // login page + its assets are public; every other dashboard page / asset needs the session
    if (url === "/login") return auth.isLoggedIn(req) ? redirect(res, "/") : serveStatic(res, "login.html");
    if (url === "/login.js") return serveStatic(res, "login.js");
    const publicAsset = url === "/style.css" || url === "/i18n.js" || /^\/i18n\/[a-z]{2,3}\.json$/.test(url);
    if (!publicAsset && !auth.isLoggedIn(req)) return redirect(res, "/login");
    if (url === "/" || url === "/index.html") return serveStatic(res, "index.html");
    if (url === "/app.js") return serveStatic(res, "app.js");
    if (url === "/journal.js") return serveStatic(res, "journal.js");
    if (url === "/loadouts.js") return serveStatic(res, "loadouts.js");
    if (url === "/i18n.js") return serveStatic(res, "i18n.js");
    const li = /^\/i18n\/([a-z]{2,3})\.json$/.exec(url); // dashboard dictionaries (dashboard/i18n/<lang>.json)
    if (li) return serveStatic(res, "i18n/" + li[1] + ".json");
    if (url === "/style.css") return serveStatic(res, "style.css");
    // vendored libraries: flat names only (no slashes, no dot-dot), known extensions
    const m = /^\/vendor\/([A-Za-z0-9][A-Za-z0-9._-]*)$/.exec(url);
    if (m && !m[1].includes("..") && MIME[path.extname(m[1]).toLowerCase()]) return serveStatic(res, "vendor/" + m[1]);
  }
  send(res, 404, { error: "not found" });
}).listen(PORT, HOST, () => {
  console.log(`clausewitz llm sidecar (${cfg.provider}${cfg.enabled ? "" : ", DISABLED"}) on http://${HOST}:${PORT}  (dashboard at /${LAN ? ", open to the LAN" : ""})`);
  void pollKey();
});

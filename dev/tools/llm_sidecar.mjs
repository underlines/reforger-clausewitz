// Clausewitz LLM bridge sidecar + read-only web dashboard. Plain Node 18+, no npm deps.
//   node dev/tools/llm_sidecar.mjs
// Env (initial defaults only; saved settings in dev/tools/sidecar-config.json win):
//   CLW_PORT (default 8788), CLW_LLM=fake|anthropic|openai-compatible|openrouter (default fake),
//   ANTHROPIC_API_KEY, CLW_MODEL (default claude-sonnet-5-5)
// Listens on 127.0.0.1 only (CLW_HOST to change; CLW_ALLOWED_HOSTS = extra host names for a remote sidecar). Dashboard + /api/* need the admin login (auth.mjs, default password in README);
// game endpoints use header X-CLW-Token when config gameToken is set. Loadout library: loadouts.mjs (/loadouts/*, /control, /api/loadouts*).
// CLW_DATA_DIR = data dir (default dev/tools/.cache: sessions.json, loadouts-store.json, map caches).
// CLW_RECORD_DIR = optional: the last RAW v2 situation of each faction that went to a commander call is written to <dir>/<faction>.json (overwrite, at most once per
//   60 s per faction; the body as the game posted it, with structures). Use it on a real DS run to refresh dev/tools/fixtures/large_situation.json (prompt budget test).
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
//       "none" = unset). Every LLM call names a role (commander|planner|sitrep|journal|hq|translate); config "roles" maps each to "strong"|"weak" (default commander/planner/hq strong, sitrep/journal/translate weak; see "Planner split, tiering" below), a role on weak without a weak slot = the strong slot answers.
//       Config API: GET /api/config also returns strong{}, weak{} (apiKey as apiKeySet/apiKeyLast4), roles, translateSlot; POST /api/config accepts
//       "weak":{provider,model,baseUrl,apiKey,timeoutSec}|null, "strong":{same} and "i18n":{enabled,translateTimeoutSec}; POST /api/test {"slot":"translate"}.
//     Backup (fallback) model per slot: strong.fallback / top-level "fallback" (strong) and weak.fallback = null | {model, provider?, baseUrl?, apiKey?, timeoutSec?}
//       (omitted fields inherit from the slot; the slot's key is only inherited while provider + baseUrl stay the same). A call goes to the primary and, after a
//       RETRYABLE failure (HTTP 408/429/5xx, timeout, network error, "cooling down"), ONCE to the backup (agent turns: the backup takes over the remaining steps).
//       400/401/403/404 and invalid model output never fall back. Cooldown is per (slot, model) (20 s after 429/5xx, also after a timeout when a backup exists); the HQ chat role has its OWN keys "hq:<slot>|<model>", so commander / planner failures never cool down the model /hq uses and vice versa.
//       Exchange log + /api/state hq[]: modelUsed, fellBack, primaryError; /api/state counters.modelFallbacks {strong,weak}, modelCooldowns {"<slot>|<model>": sec}.
//       POST /api/config "fallback":null clears; POST /api/test {"slot":"strong|weak|translate","which":"primary|fallback"} tests one model alone.
//     Fake provider: translation = "[<lang>] <text>", /hq reply = "[<lang>] <reply>" (also after setLanguage), so e2e can check routing without a key.
//
//   Faction SITREP (design: dev/notes/sitrep-design.md, dev/notes/llm-latency-design.md). The game POST may carry "sitrep": the rules SITREP the game built
//     from the same fog-limited picture {scope,unit,source:"rules",warTimeSec,headline,situation,enemy,mission,execution,logistics}; the sidecar removes it
//     before any model call. The model SITREP is NOT part of the commander call any more: when the config "sitrep" group is enabled and one is due (first
//     commander call, every sitrep.intervalSec, a manual wake, or an objective/tickets/lost trigger) the sidecar starts a SEPARATE background call (role
//     "sitrep" = weak slot, strong when none is configured) next to the commander call, at most one in flight per faction, with the same situation plus the
//     commander's last validated orders (with reasons). Its output (SITREP object, persona tone, scrubbed) is translated into "langs" inside the same job
//     (see Protocol v4). The finished SITREP is attached ONCE to the next /orders reply of that faction: the current reply if the job finished before it is
//     sent, otherwise any later reply, also a gated {"orders":[],"keep":true} one (a rule-fallback {"orders":[]} reply gets it without becoming "keep").
//     The game applies "sitrep" from any reply and shows it to that faction's players only. A failed job is retried after 30 s. Fake provider: template
//     SITREP, immediate, same reply. Log line: "sitrep faction=<f> source=<provider> <ms>ms". GET /api/state: factions[<name>].sitrep =
//     {text, headline, sections:[{id,title,body}], scope:"faction", unit, source:"llm"|"rules", generatedAt (epoch ms), warTimeSec}
//     (the LLM one while it is at most 360 s of war time old, else the game's rules one, else a sidecar-built rules one; null before any).
//
//   Planner split, tiering (design: dev/notes/llm-latency-design.md, B2). Config planner {enabled, intervalSec 60..900} (default true / 180). Enabled (and the manifest has
//     buy or fob intents): the commander call is the ORDERS call (channels orders + missions; its prompt and reply spec have no buy / FOB / logistics text, the user message
//     carries a PLAN block = the planner's last result {buy with assignments, fobs, reasons, lastRunAgoSec} + nextRunInSec; a buy / fobs answer in it is dropped) and
//     BUY + FOBS (FOB, upgrade, RESUPPLY, CONVOY) come from the PLANNER call (role "planner", agent/commanderPrompt.mjs buildPlannerPrompt: economy / FOB / logistics doctrine,
//     catalog + situation, the orders in force with reasons, the HQ journal block, its own wake line; reply b / f / k only, same output mode / response_format / cache handling
//     as the commander call, recorded in llmCalls with sections rules|commands|fobs|logistics|situation|orders|journal|trigger). It is a background job like the SITREP, at
//     most one in flight per faction, parallel to the orders call, checked on EVERY running poll (also gated ones): due on the first running poll, every planner.intervalSec,
//     on a manual wake and on planner triggers (min gap 45 s): objective owner change, an own squad lost, supplies grew by >= the cheapest catalog cost since the last planner
//     run and something is affordable, fobRules.ready turned true, a fob became active / lost or its stock fell below logisticsRules.lowStock. A failed run is retried after 30 s
//     (a manual wake skips that). The validated result waits as pending (expires after 120 s) and is attached ONCE to the next reply of that faction after RE-VALIDATING
//     buy / fobs against that newest situation with validateV2 (supplies may have changed): to normal, keep and gated replies (those become {"orders":[],"keep":true,"buy":[..],
//     "fobs":[..]}), NOT to a failure / rule-fallback {"orders":[]} reply (it stays pending, the game falls back to its rules). Exchange (commander call) has "planner" =
//     {delivered, buy, fobs, planned:{buy,fobs}, droppedOnRevalidate, ageSec, ms, reason, slot, model} | {delivered:false} | null (planner off); re-validation drops are in "dropped"; the
//     exchange also has "slot" (strong|weak that answered) and "escalated". GET /api/state "planner" = per faction {inflight, runs, lastReason, lastAgoSec, lastMs, failed, pending, last}.
//     Log: "planner faction=<f> buy=<n> fobs=<n> <ms>ms (<why>)" per run, "planner faction=<f> delivered ..." per delivery. Fake provider: the planner runs INLINE after the orders part
//     (fakeV2 parts "orders" / "plan"), so buy / fobs ride on the same reply as before. planner disabled = exactly the old combined prompt (buy / fobs in the commander call).
//     Config roles {commander, planner, sitrep, journal, hq, translate} (strong|weak) replaces the fixed role map. escalate {enabled, slot} (default false / strong): an orders call
//     whose wake has an important trigger (contact near a squad, casualties, squad lost / strength loss, objective changed owner, tickets drop) uses escalate.slot instead of
//     roles.commander. roleTimeouts {commander, planner, sitrep, journal, hq} seconds 0..300 (0 = the slot's timeoutSec; hq: replaces agent.timeoutSec, the fast-path cap still
//     applies on top) is the provider timeout of that role's calls. Test env: CLW_PLANNER_MIN_GAP_MS.
//
//   Terse commander reply (config output {mode, terse, reasonWords}, default json_object / true / 12). With terse on, the v2 REPLY paragraph of the prompt is
//     promptKit.terseSpec and the model answers compact tuples: {"o":[[squad,CMD,target|[x,z],reason]],"b":[[type,count,CMD,target|[x,z],reason]],
//     "f":[["ESTABLISH_FOB",site],["UPGRADE_FOB",fob],["RESUPPLY",fob],["CONVOY",origin,dest]],"m":[[squad,[[KIND,target|[x,z],brief]]]],"k":1,"n":[sec,why]}.
//     promptKit.expandTerse turns it (or the old verbose JSON, which still works) into the canonical object before validateV2; reasons are cut to
//     ceil(1.5 x reasonWords) words and 120 chars. mode = response_format for openrouter/openai-compatible: json_object, json_schema (terseSchema) or text
//     (none; a rejecting provider is remembered and retried once without it by agent/llmCall.mjs). maxTokens stays the config value. Protocol v1 is untouched.
//   Prompt layout of the v2 commander call (agent/commanderPrompt.mjs): system = rules | commands + reply spec | persona | conditional FOB / LOGISTICS / HELP CALLS
//     paragraphs (FOBS only when the manifest has fob intents and (fobs[] exist or fobRules.ready !== false or no fobRules); LOGISTICS only with an active fob or a
//     convoy; HELP CALLS only with helpCalls[]); user = situation, WAKE object, environment paragraph, HQ journal. Stable parts first so providers can cache
//     the prefix; config prompt {lean, staticBlock, cache, contactMax 5..60, contactMaxAgeSec 60..1800} (default true,true,true,25,600): cache = cache_control
//     on the system prompt where the provider needs it (OpenRouter anthropic/* and google/gemini*, Anthropic API); lean/staticBlock/contact* are accepted and stored
//     lean (default on): the model gets promptKit.leanSituation (tables {cols, rows}, [x,z] positions, contacts filtered by contactMax / contactMaxAgeSec, per-prompt
//     profile in commanderPrompt.LEAN_PROFILES: the orders call of a planner split gets no catalog / FOB / logistics data) in the commander, planner and SITREP prompts;
//     staticBlock (default on): its static part (map, objective / base / site facts, catalog) is the last section "static" of the SYSTEM prompt (cacheable prefix), else
//     it heads the user message. The raw situation still feeds the rules SITREP, validateV2, the journal, the triggers and the dashboard. Protocol v1 is never lean.
//
//   Call metrics (design: llm-latency-design.md "Metrics"): every provider call is streamed (agent/llmCall.mjs: TTFT, usage, cached tokens) and recorded.
//     GET /api/state: "llmCalls" = ring of 200, newest first, {ts (epoch ms), faction, role (commander|planner|sitrep|journal|translate|hq), slot (strong|weak), model, ok, error,
//     inTok, outTok, cachedTok, ttftMs (null = unknown; hq turns have none), totalMs, promptChars, sections:{<name>:{chars,tokens}} (rules, commands, persona, fobs,
//     logistics, helpCalls, static, situation, wake, env, journal for the commander call; rules/persona/situation/orders for the SITREP call; system/user otherwise), outputMode
//     (response_format actually sent: json_object|json_schema|text), fellBack, terse?, cost?}; "latency" = per role {n, okN, p50Ms, p90Ms, avgTtftMs, avgInTok, avgOutTok,
//     avgCachedTok, lastMs, lastAt} over the ring. Every exchange has "metrics" = the llmCalls entry of its commander call (null for fake/none). One log line per call:
//     "llm role=<r> faction=<f> model=<m> in=<n> out=<n> cached=<n> ttft=<ms>ms total=<ms>ms sections=<name>:<tok>,...". The fake provider is recorded too (model "fake", no tokens).
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
import { chatCall } from "./agent/llmCall.mjs"; // one provider call: streaming, TTFT, usage, response_format, prompt caching
import { sectionStats, approxTokens, terseSchema, expandTerse } from "./agent/promptKit.mjs";
// prompts (commander, SITREP), the built-in manifest and the SITREP template live in agent/commanderPrompt.mjs (persona + environment hooks inside)
import { DEFAULT_MANIFEST, channelOf, orderNamesOf, SITREP_KEYS, SITREP_TITLES, SITREP_CAP, SITREP_HEADLINE_CAP, SITREP_FRESH_SEC, buildCommanderPrompt, buildSitrepPrompt, buildPlannerPrompt, plannerSplit } from "./agent/commanderPrompt.mjs";

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
  addonDirs: [],     // extra addon dirs for map worlds in Workshop mods (an addon dir or a parent of addon dirs); merged after env CLW_ADDON_DIRS
  mapMaxSize: 2048,  // longest side of the served map PNG
  // HQ scheduler: how often the LLM is called (see "HQ scheduler" below)
  wakeup: { dynamic: true, defaultSec: 60, minSec: 20, maxSec: 300, budgetStretch: true },
  // events that wake the HQ before its scheduled time (each tunable; 0 / false = off)
  triggers: { enabled: true, newContact: true, contactNearSquadM: 300, casualties: 2, squadLost: true, strengthDropPct: 25, objectiveReached: true, squadChange: true, cooldownSec: 15,
    objectiveChanged: true, ticketsDropPct: 10, unitSpawned: true, missionEvent: true, idleUnits: true, idleSec: 90 },
  // faction SITREP: its own background LLM call (role sitrep), due on the first commander call, every intervalSec at most, and on big events
  sitrep: { enabled: true, intervalSec: 120 },
  // planner split: buy + FOB / logistics decided by their own background call (role planner) every intervalSec and on planner triggers; the commander call is then the
  // ORDERS call (orders + missions only). disabled = buy / FOB stay in the commander call (the old combined prompt)
  planner: { enabled: true, intervalSec: 180 },
  // role -> slot tiering: every LLM call names a role; "weak" while no weak slot is configured uses the strong slot
  roles: { commander: "strong", planner: "strong", sitrep: "weak", journal: "weak", hq: "strong", translate: "weak" },
  // orders calls woken by an important trigger (contact near a squad, casualties, squad lost, objective lost / changed owner, tickets drop) use escalate.slot instead of roles.commander
  escalate: { enabled: false, slot: "strong" },
  // per-role provider timeout in seconds, 0 = the slot's timeoutSec (hq: the fast-path cap HQ_FAST_TURN_SEC still applies on top)
  roleTimeouts: { commander: 0, planner: 0, sitrep: 0, journal: 0, hq: 0 },
  // commander reply format: mode = response_format sent to openrouter/openai-compatible (json_object | json_schema | text = none); terse = compact tuple replies
  // (promptKit.terseSpec / expandTerse) instead of the verbose JSON; reasonWords = words asked per reason (validation tolerates 1.5x, 120 chars hard cap);
  // reasoning = OpenRouter reasoning for every call incl. /hq (off = reasoning:{enabled:false}, default = model default, low|medium|high = effort).
  // Off by default: reasoning models (e.g. ling-3.1-flash) spent ~90% of a commander call on hidden reasoning (9 s vs 1.5 s, DS e2e 2026-10-15)
  output: { mode: "json_object", terse: true, reasonWords: 12, reasoning: "off" },
  // prompt shaping: lean = tabular lean situation to the model, staticBlock = its static part at the end of the system prompt, cache = cache hints on the (stable) system prompt
  prompt: { lean: true, staticBlock: true, cache: true, contactMax: 25, contactMaxAgeSec: 600 },
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

// Group settings: "bool", [min, max] number range or ["a", "b"] string enum per key. Partial objects are merged onto the current group.
const OUTPUT_MODES = ["json_object", "json_schema", "text"];
const REASONING_MODES = ["off", "default", "low", "medium", "high"];
const SLOT_NAMES = ["strong", "weak"];
const GROUP_SPECS = {
  sitrep: { enabled: "bool", intervalSec: [30, 3600] },
  planner: { enabled: "bool", intervalSec: [60, 900] },
  roles: { commander: SLOT_NAMES, planner: SLOT_NAMES, sitrep: SLOT_NAMES, journal: SLOT_NAMES, hq: SLOT_NAMES, translate: SLOT_NAMES },
  escalate: { enabled: "bool", slot: SLOT_NAMES },
  roleTimeouts: { commander: [0, 300], planner: [0, 300], sitrep: [0, 300], journal: [0, 300], hq: [0, 300] },
  output: { mode: OUTPUT_MODES, terse: "bool", reasonWords: [4, 30], reasoning: REASONING_MODES },
  prompt: { lean: "bool", staticBlock: "bool", cache: "bool", contactMax: [5, 60], contactMaxAgeSec: [60, 1800] },
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
    else if (typeof s[0] === "string") { if (!s.includes(x)) return { error: `${name}.${k} must be one of ${s.join(", ")}` }; }
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
      case "addonDirs": {
        if (!Array.isArray(v) || v.length > 20) return { error: "addonDirs must be an array of at most 20 strings" };
        const list = [];
        for (const d of v) {
          if (typeof d !== "string" || d.length > 400 || /[\r\n]/.test(d)) return { error: "addonDirs entries must be single-line strings (max 400)" };
          if (d.trim() && !list.includes(d.trim())) list.push(d.trim());
        }
        out.addonDirs = list; break;
      }
      case "mapMaxSize":
        if (!Number.isInteger(v) || v < 256 || v > 8192) return { error: "mapMaxSize must be an integer 256..8192" };
        out.mapMaxSize = v; break;
      case "wakeup":
      case "sitrep":
      case "planner":
      case "roles":
      case "escalate":
      case "roleTimeouts":
      case "output":
      case "prompt":
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
  return { ...rest, gameTokenSet: !!gameToken, apiKeySet: !!apiKey, apiKeyLast4: apiKey ? apiKey.slice(-4) : "", fallback: FB.publicFallback(fallback), strong: slot(cfg), weak: slot(weak), translateSlot: weak.provider !== "none" ? "weak" : "strong" };
}

// ---------- state ----------
const state = {
  startedAt: Date.now(),
  factions: {},   // name -> {ts, situation, orders}
  exchanges: [],  // newest first, max RING_MAX
  llmCalls: [],   // every LLM call (commander, sitrep, journal, translate, hq turn), newest first, max LLM_CALLS_MAX; see "LLM call metrics"
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
// + the background SITREP call (see "SITREP as its own background call"): inflight, retryAt (backoff after a failure), pending (finished, not yet delivered), plan (last validated orders)
const sitrepState = {}; // faction -> {lastAt, llm:{value,generatedAt,warTimeSec}|null, rules:{sig,value,generatedAt}|null, inflight, retryAt, pending:{sitrep,i18n}|null, plan:[]}
const srFor = (f) => (sitrepState[f] ??= { lastAt: 0, llm: null, rules: null, inflight: false, retryAt: 0, pending: null, plan: [] });
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
// Roles -> slots. Every LLM call names a role; config "roles" {commander, planner, sitrep, journal, hq, translate} maps it to "strong" | "weak" (defaults below).
// A role on "weak" while no weak slot is configured is answered by the strong slot. escalate.slot overrides the slot of an orders call (see escalation()).
const DEFAULT_ROLES = { commander: "strong", planner: "strong", sitrep: "weak", journal: "weak", hq: "strong", translate: "weak" };
const roleSlotName = (c, role) => (c.roles?.[role] === "weak" || c.roles?.[role] === "strong" ? c.roles[role] : DEFAULT_ROLES[role] ?? "strong");
const hasWeak = (c) => !!(c.weak && c.weak.provider && c.weak.provider !== "none");
/** The slot ("strong"|"weak") that answers `role` right now; slotName (optional) overrides the configured one (escalation). */
const slotOf = (c, role = "strong", slotName) => ((slotName ?? roleSlotName(c, role)) === "weak" && hasWeak(c) ? "weak" : "strong");
function slotCfg(c, role = "strong", slotName) {
  if (slotOf(c, role, slotName) !== "weak") return c;
  const w = c.weak;
  return { ...c, provider: w.provider, model: w.model || c.model, baseUrl: w.baseUrl, apiKey: w.apiKey, timeoutSec: w.timeoutSec || c.timeoutSec };
}
// ---------- model fallback ----------
// Every slot (strong, weak) may carry a backup model (config "fallback", see agent/fallback.mjs). callWithFallback() is the one place that
// routes a call: primary first, ONE retry on the backup after a retryable failure (429, 5xx, timeout, network error, cooling down).
// Cooldown state is per (slot, model): a rate-limited primary does not block the backup, and while it cools down calls go straight to the
// backup until the cooldown expires, then the primary is tried again. No backup configured = exactly the old single-model behaviour.
// The HQ chat role has its OWN cooldown keys ("hq:<slot>|<model>"): commander / planner timeouts never cool down the model /hq uses, and an /hq failure never
// blocks the commander (DS e2e 2026-10-15: commander timeouts cooled the shared model and an /hq turn hung on the backup).
const RATE_COOLDOWN_MS = Number(process.env.CLW_RATE_COOLDOWN_MS) || 20000; // after a 429/5xx the model is skipped for this long (or its Retry-After); the env override is for tests
const cooldowns = new Map();   // "<slot>|<model>" (role hq: "hq:<slot>|<model>") -> until (epoch ms)
const coolKey = (ch, c) => ch.scope + ch.slot + "|" + c.model;
const coolLeftMs = (ch, c) => Math.max(0, (cooldowns.get(coolKey(ch, c)) ?? 0) - Date.now());
/** { slot: "strong"|"weak" (the slot that really answers), scope: cooldown key prefix ("hq:" for role hq, else ""), primary: full provider config, fallback: full provider config | null } */
function slotChain(c, role = "strong", slotName) {
  const slot = slotOf(c, role, slotName), primary = slot === "weak" ? slotCfg(c, role, slotName) : c;
  return { slot, scope: role === "hq" ? "hq:" : "", primary, fallback: primary.provider === "fake" ? null : FB.resolveFallback(primary, slot === "weak" ? c.weak.fallback : c.fallback) };
}
function noteCooldown(ch, c, err, hasFallback) {
  const ms = FB.cooldownFor(err, RATE_COOLDOWN_MS, hasFallback);
  if (ms > 0) cooldowns.set(coolKey(ch, c), Date.now() + ms);
}
function noteFellBack(slot, primaryModel, err) {
  state.counters.modelFallbacks[slot]++;
  console.log(`  ${slot} model ${primaryModel} failed (${String(err?.message ?? err).slice(0, 120)}): using the backup model`);
}
/** True when no model of the role's slot can be called right now (all cooling down). */
function slotBlocked(c, role = "strong", slotName) {
  const ch = slotChain(c, role, slotName);
  return ch.primary.provider !== "fake" && coolLeftMs(ch, ch.primary) > 0 && (!ch.fallback || coolLeftMs(ch, ch.fallback) > 0);
}
const COOLING_ROLES = new Set(["commander", "planner", "journal", "sitrep"]); // without a backup only these calls use the cooldown (as before: commander / journal / sitrep)
/** callProvider through the slot chain. Result: callProvider's {raw, usage, ttftMs, totalMs, metrics} + {modelUsed, provider, fellBack, primaryError}. On total failure the thrown error carries primaryError / fellBack.
 *  opts.test = primary only, no cooldown bookkeeping. opts.slotName = slot override (escalation). config roleTimeouts[role] > 0 sets the provider timeout (seconds) unless opts.timeoutSec is given. */
async function callWithFallback(c, sit, opts = {}) {
  const role = opts.role ?? "strong";
  const ch = slotChain(c, role, opts.slotName);
  const rt = c.roleTimeouts?.[role];
  if (opts.timeoutSec === undefined && isNum(rt) && rt > 0) opts = { ...opts, timeoutSec: rt };
  const cools = !opts.test && (!!ch.fallback || COOLING_ROLES.has(role));
  const attempt = async (cx, fellBack = false) => {
    if (cools && cx.provider !== "fake") { const left = coolLeftMs(ch, cx); if (left > 0) throw Object.assign(new Error(`provider cooling down after rate limit/server error (${Math.ceil(left / 1000)}s left)`), { cooling: true }); }
    try { const p = await callProvider(c, sit, { ...opts, slotCfg: cx, slot: ch.slot, fellBack }); return { ...p, modelUsed: cx.model, provider: cx.provider }; }
    catch (e) { if (cools) noteCooldown(ch, cx, e, !!ch.fallback); throw e; }
  };
  try { return { ...(await attempt(ch.primary)), fellBack: false, primaryError: null }; }
  catch (e) {
    if (opts.test || !ch.fallback || !FB.isRetryable(e)) throw e;
    const primaryError = String(e?.message ?? e).slice(0, 200);
    noteFellBack(ch.slot, ch.primary.model, e);
    try { return { ...(await attempt(ch.fallback, true)), fellBack: true, primaryError }; }
    catch (e2) { e2.fellBack = true; e2.primaryError = primaryError; e2.modelUsed = null; throw e2; }
  }
}
function cooldownsView() { // "<slot>|<model>" ("hq:<slot>|<model>" for the HQ chat) -> seconds left (only the ones still cooling down)
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

// ---------- situation recording (env CLW_RECORD_DIR) ----------
// The last raw v2 situation of each faction that went to a commander call, for refreshing the prompt budget fixture from a real run. Throttled to once per 60 s per faction.
const RECORD_DIR = process.env.CLW_RECORD_DIR ? path.resolve(process.env.CLW_RECORD_DIR) : "";
const RECORD_GAP_MS = 60_000, recordedAt = {};
function recordSituation(faction, sit, now = Date.now()) {
  if (!RECORD_DIR || !sit || typeof sit !== "object" || now - (recordedAt[faction] ?? 0) < RECORD_GAP_MS) return;
  recordedAt[faction] = now;
  try {
    fs.mkdirSync(RECORD_DIR, { recursive: true });
    fs.writeFileSync(path.join(RECORD_DIR, String(faction).replace(/[^A-Za-z0-9_.-]/g, "_") + ".json"), JSON.stringify(sit, null, 1));
  } catch (e) { console.log(`record situation ${faction} failed: ${e.message}`); }
}

// ---------- SITREP as its own background call ----------
// The SITREP is NOT part of the commander call any more (that call is slow because of its output tokens). When one is due, handleOrders starts
// sitrepJob() next to the commander call (parallel, role "sitrep" = weak slot, strong when none is configured; at most one in flight per faction).
// Input = the same situation + the commander's last validated orders with reasons. Output = the SITREP object (normSitrep), then the translation into
// `langs`, all inside the job. The result waits in sitrepState[f].pending and is attached ONCE to the next reply of that faction (the current one when the
// job finished before it is sent): any reply, also a gated {orders:[],keep:true}; a rule-fallback {orders:[]} reply gets the sitrep but never becomes "keep".
const SITREP_MAX_TOKENS = 800, SITREP_RETRY_MS = 30_000;
const sitrepIsFake = (c) => c.provider === "fake" || slotCfg(c, "sitrep").provider === "fake"; // a fake strong provider fakes the whole pipeline (tests keep working with a real weak slot)
async function sitrepJob(faction, c, callSit, orders, langs, warTimeSec) {
  const st = srFor(faction), t0 = Date.now();
  st.inflight = true;
  try {
    const pr = buildSitrepPrompt({ config: c, sit: callSit, orders });
    const sections = sectionStats(pr.sections);
    let sr, source;
    if (sitrepIsFake(c)) {
      sr = normSitrep(fakeSitrep(callSit, { orders })); source = "fake";
      recordFakeCall("sitrep", faction, t0, { promptChars: pr.system.length + pr.user.length, sections });
    } else {
      if (budgetStopped(slotCfg(c, "sitrep"))) throw new Error("budget limit reached");
      session.requests++;
      const p = await callWithFallback(c, { faction }, { role: "sitrep", system: pr.system, userMsg: pr.user, sections, maxTokens: SITREP_MAX_TOKENS,
        responseFormat: c.output?.mode === "text" ? null : { type: "json_object" }, cacheSystem: !!c.prompt?.cache });
      if (p.provider === "openrouter") recordUsage(p.modelUsed, faction, p.usage);
      const a = p.raw.indexOf("{"), b = p.raw.lastIndexOf("}");
      if (a < 0 || b < a) throw new Error("no JSON object in sitrep reply");
      const o = JSON.parse(p.raw.slice(a, b + 1));
      sr = normSitrep(o && typeof o.sitrep === "object" && !Array.isArray(o.sitrep) ? o.sitrep : o);
      source = p.provider;
    }
    if (!sr) { st.retryAt = Date.now() + SITREP_RETRY_MS; console.log(`sitrep faction=${faction}: reply had no usable sitrep object`); return; }
    st.lastAt = Date.now();
    st.llm = { value: sr, generatedAt: st.lastAt, warTimeSec: isNum(warTimeSec) ? warTimeSec : 0 };
    let i18n = null;
    if (langs.length && c.i18n.enabled) { // one weak-LLM translation call per non-English language, parallel, cached; failures omit that language
      const tr = await translator.translateFields(sitrepFields(sr), langs);
      sr = { ...sr, text_by_lang: tr.by_lang };
      st.llm.textByLang = tr.by_lang;
      i18n = { langs, ok: Object.keys(tr.by_lang).filter((l) => l !== "en"), cached: tr.cached, failed: tr.failed, slot: slotCfg(c, "translate") === c ? "strong" : "weak" };
      state.counters.errors += tr.failed.length;
      console.log(`sitrep faction=${faction} translated langs=[${i18n.ok}] cached=[${tr.cached}] failed=[${tr.failed.map((x) => x.lang + ":" + x.reason)}]`);
    }
    st.pending = { sitrep: sr, i18n, at: Date.now() };
    console.log(`sitrep faction=${faction} source=${source} ${Date.now() - t0}ms headline="${sr.headline}"`);
  } catch (e) {
    st.retryAt = Date.now() + SITREP_RETRY_MS;
    console.log(`sitrep faction=${faction} FAILED (${e?.name === "AbortError" ? "timeout" : String(e?.message ?? e).slice(0, 150)}) ${Date.now() - t0}ms`);
  } finally {
    st.inflight = false;
  }
}
/** The finished SITREP waiting for a reply ({sitrep, i18n}) or null; each one is delivered once. */
function takePendingSitrep(faction) {
  const st = sitrepState[faction];
  if (!st?.pending) return null;
  const p = st.pending;
  st.pending = null;
  return p;
}

// ---------- planner (buy + FOB / logistics as their own background call) ----------
// Config planner {enabled, intervalSec 60..900}. Enabled (and the manifest has buy / fob intents): the commander call is the ORDERS call (orders + missions; its
// prompt has no buy / FOB text and carries a PLAN block = the planner's last result + when it runs next), and handleOrders starts plannerJob() as a background
// call (role "planner", at most one in flight per faction, parallel to the orders call). It is due on the first running poll, every planner.intervalSec, on
// planner triggers (min gap 45 s: objective owner change, an own squad lost, supplies grew by >= the cheapest catalog cost and something is affordable,
// fobRules.ready turned true, a fob became active / lost or its stock fell below logisticsRules.lowStock) and on a manual wake.
// Its input = the situation + the commander's last validated orders (orders in force) + the HQ journal block; its output = buy + fobs (terse b / f / k).
// The validated result waits in plannerState[f].pending (expires after 120 s) and is attached ONCE to the next reply of that faction after RE-VALIDATING it
// against the newest situation with validateV2 (supplies may have changed): to normal, keep and gated replies (a keep / gated reply becomes {orders:[],keep:true,buy,fobs});
// NOT to a failure / rule-fallback reply (stays pending, the game falls back to its rules). The fake provider runs the planner inline after the orders part, so
// buy / fobs ride on the same reply as before.
const PLANNER_MAX_AGE_MS = 120_000, PLANNER_RETRY_MS = 30_000;
const PLANNER_MIN_GAP_MS = Number(process.env.CLW_PLANNER_MIN_GAP_MS) || 45_000; // min gap between two TRIGGERED planner runs (the env override is for tests)
const plannerState = {}; // faction -> {inflight, lastAt, retryAt, failed, snap, pending:{buy,fobs,dropped,at,ms,reason,slot,model}|null, last:{buy,fobs,at}|null, runs, lastReason, lastMs}
const plFor = (f) => (plannerState[f] ??= { inflight: false, lastAt: 0, retryAt: 0, failed: false, snap: null, pending: null, last: null, runs: 0, lastReason: "", lastMs: null });
const plannerIsFake = (c) => c.provider === "fake" || slotCfg(c, "planner").provider === "fake"; // a fake strong provider fakes the whole pipeline
const minCatalogCost = (sit) => Math.min(...(Array.isArray(sit?.catalog) ? sit.catalog.map((e) => e?.cost).filter((x) => isNum(x) && x > 0) : []), Infinity);
/** What the planner triggers compare against (taken when a planner run starts). */
function plannerSnap(sit) {
  const lr = sit.logisticsRules ?? {};
  return {
    supplies: isNum(sit.war?.supplies) ? sit.war.supplies : null,
    owners: Object.fromEntries(objList(sit).map((o) => [o.id, String(o.owner)])),
    squads: Object.fromEntries((Array.isArray(sit.squads) ? sit.squads : []).filter((q) => q && typeof q.id === "string" && !q.garrison).map((q) => [q.id, isNum(q.agents) ? q.agents : null])),
    ready: sit.fobRules?.ready === true,
    fobs: Object.fromEntries((Array.isArray(sit.fobs) ? sit.fobs : []).filter((x) => x && typeof x.id === "string").map((x) => [x.id, { state: x.state, low: isNum(x.stock) && isNum(lr.lowStock) && x.stock < lr.lowStock }])),
  };
}
/** Descriptions of what changed since the last planner run (see the list above). */
function plannerTriggers(snap, sit) {
  const out = [], lr = sit.logisticsRules ?? {};
  for (const o of objList(sit)) if (snap.owners[o.id] !== undefined && snap.owners[o.id] !== String(o.owner)) out.push(`objective ${o.id} changed owner (${snap.owners[o.id]} -> ${o.owner})`);
  const down = new Set();
  for (const e of Array.isArray(sit.events) ? sit.events : []) { const m = /^(\S+) stood down/.exec(String(e)); if (m) down.add(m[1]); } // stood down as idle = not a loss
  const now = new Map((Array.isArray(sit.squads) ? sit.squads : []).filter((q) => q && typeof q.id === "string").map((q) => [q.id, q]));
  for (const [id, agents] of Object.entries(snap.squads)) {
    const q = now.get(id);
    if ((!q && !down.has(id)) || (q && isNum(agents) && agents > 0 && isNum(q.agents) && q.agents <= 0)) out.push(`${id} lost`);
  }
  const min = minCatalogCost(sit), sup = sit.war?.supplies;
  if (isNum(snap.supplies) && isNum(sup) && min < Infinity && sup - snap.supplies >= min && sup >= min) out.push(`supplies ${snap.supplies} -> ${sup}`);
  if (!snap.ready && sit.fobRules?.ready === true) out.push("fobs ready");
  for (const x of Array.isArray(sit.fobs) ? sit.fobs : []) {
    if (!x || typeof x.id !== "string") continue;
    const prev = snap.fobs[x.id], low = isNum(x.stock) && isNum(lr.lowStock) && x.stock < lr.lowStock;
    if ((x.state === "active" || x.state === "lost") && (!prev || prev.state !== x.state)) out.push(`fob ${x.id} ${x.state}`);
    else if (x.state === "active" && low && !prev?.low) out.push(`fob ${x.id} stock ${x.stock} below ${lr.lowStock}`);
  }
  return out;
}
/** Why the planner should start now ("" = not due). */
function plannerDue(c, pl, sit, now, manual) {
  if (pl.inflight || (!manual && now < pl.retryAt)) return ""; // a manual wake skips the failure backoff
  if (manual) return "manual";
  if (!pl.lastAt) return "first";
  if (pl.failed) return "retry";
  if (now - pl.lastAt >= c.planner.intervalSec * 1000) return "scheduled";
  if (now - pl.lastAt >= PLANNER_MIN_GAP_MS && pl.snap) { const t = plannerTriggers(pl.snap, sit); if (t.length) return "trigger: " + t.slice(0, 4).join("; "); }
  return "";
}
/** The PLAN block input of the orders prompt (null when the split is off). */
function plannerPlanInfo(c, faction) {
  const pl = plFor(faction), now = Date.now();
  return { buy: pl.last?.buy ?? [], fobs: pl.last?.fobs ?? [], ...(pl.last ? { ageSec: (now - pl.last.at) / 1000 } : {}), running: pl.inflight,
    nextInSec: pl.lastAt ? Math.max(0, c.planner.intervalSec - (now - pl.lastAt) / 1000) : 0 };
}
async function plannerJob(faction, c, sit, manifest, orders, reason) {
  const pl = plFor(faction), t0 = Date.now();
  pl.inflight = true; pl.lastAt = t0; pl.snap = plannerSnap(sit); pl.lastReason = reason; pl.runs++;
  try {
    const terse = !!c.output?.terse;
    const pr = buildPlannerPrompt({ config: c, sit, manifest, journal: journalBlock(faction, c, sit), orders, trigger: reason === "first" || reason === "scheduled" ? "" : reason });
    const sections = sectionStats(pr.sections);
    let obj, p = null, metrics = null;
    if (plannerIsFake(c)) {
      obj = fakeV2(sit, manifest, "plan");
      metrics = recordFakeCall("planner", faction, t0, { promptChars: pr.system.length + pr.user.length, sections, terse });
    } else {
      if (budgetStopped(slotCfg(c, "planner"))) throw new Error("budget limit reached");
      session.requests++;
      p = await callWithFallback(c, { faction }, { role: "planner", system: pr.system, userMsg: pr.user, sections, terse, maxTokens: c.maxTokens,
        responseFormat: responseFormatFor(c, pr.spec), cacheSystem: !!c.prompt?.cache });
      if (p.provider === "openrouter") recordUsage(p.modelUsed, faction, p.usage);
      obj = extractOrders(p.raw, true);
      metrics = p.metrics ?? null;
    }
    const dropped = [];
    const v = validateV2({ buy: obj.buy, fobs: obj.fobs }, sit, dropped, manifest, { reasonWords: terse ? c.output.reasonWords : undefined });
    const ms = Date.now() - t0;
    pl.failed = false;
    pl.last = { buy: v.buy, fobs: v.fobs, at: Date.now(), reason };
    pl.pending = { buy: v.buy, fobs: v.fobs, dropped, at: Date.now(), ms, reason, slot: metrics?.slot ?? slotOf(c, "planner"), model: metrics?.model ?? (p ? p.modelUsed : "fake") };
    pl.lastMs = ms;
    console.log(`planner faction=${faction} buy=${v.buy.length} fobs=${v.fobs.length} ${ms}ms${dropped.length ? ` dropped=${dropped.length}` : ""} (${reason})`);
  } catch (e) {
    pl.failed = true; pl.retryAt = Date.now() + PLANNER_RETRY_MS;
    console.log(`planner faction=${faction} FAILED (${e?.name === "AbortError" ? "timeout" : String(e?.message ?? e).slice(0, 150)}) ${Date.now() - t0}ms`);
  } finally {
    pl.inflight = false;
  }
}
/** The finished planner result for a reply, re-validated against the newest situation (supplies, caps, FOB state may have changed) -> {buy, fobs, dropped, ...} or null. Each result is delivered once. */
function takePlannerResult(faction, sit, manifest) {
  const pl = plannerState[faction];
  if (!pl?.pending) return null;
  const p = pl.pending;
  pl.pending = null;
  if (Date.now() - p.at > PLANNER_MAX_AGE_MS) { console.log(`planner faction=${faction} result expired (${Math.round((Date.now() - p.at) / 1000)}s old), not delivered`); return null; }
  const dropped = [];
  const v = validateV2({ buy: p.buy, fobs: p.fobs }, sit, dropped, manifest);
  return { buy: v.buy, fobs: v.fobs, dropped, planned: { buy: p.buy.length, fobs: p.fobs.length }, ageSec: Math.round((Date.now() - p.at) / 1000), ms: p.ms, reason: p.reason, slot: p.slot, model: p.model };
}
function plannerView() {
  const now = Date.now(), out = {};
  for (const [f, pl] of Object.entries(plannerState)) out[f] = { inflight: pl.inflight, runs: pl.runs, lastReason: pl.lastReason, lastAgoSec: pl.lastAt ? Math.round((now - pl.lastAt) / 1000) : null, lastMs: pl.lastMs, failed: pl.failed,
    pending: pl.pending ? { buy: pl.pending.buy.length, fobs: pl.pending.fobs.length, ageSec: Math.round((now - pl.pending.at) / 1000) } : null, last: pl.last ? { buy: pl.last.buy.length, fobs: pl.last.fobs.length } : null };
  return out;
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
// part: "all" (planner disabled: one combined answer) | "orders" (orders + missions: the commander call of the planner split) | "plan" (buy + fobs: the fake planner).
const xz = (p) => (Array.isArray(p) ? (p.length > 2 ? [p[0], p[2]] : [p[0], p[1]]) : [NaN, NaN]);
const dist2 = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);
function fakeV2(sit, manifest = DEFAULT_MANIFEST, part = "all") {
  const doPlan = part !== "orders", doOrders = part !== "plan";
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
  if (doPlan && canBuy && supplies > 200 && catalog.length && !(isNum(war.aiCap) && isNum(war.liveAI) && war.liveAI >= war.aiCap) && !(war.engineAI && isNum(war.engineAI.headroom) && war.engineAI.headroom < 3)) {
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
  if (doPlan && fobNames.size && isNum(fobRules.cost) && isNum(fobRules.max)) {
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
  if (doPlan && fobNames.has("RESUPPLY") && isNum(logiRules.lowStock) && (logiRules.inFlight ?? 0) < (logiRules.convoyMax ?? 2) && supplies >= (logiRules.minCargo ?? 40) + 40) {
    const convoys = Array.isArray(sit.convoys) ? sit.convoys : [];
    const low = (Array.isArray(sit.fobs) ? sit.fobs : []).find((f) => f?.state === "active" && isNum(f.stock) && f.stock < logiRules.lowStock && !convoys.some((c) => c?.to === f.id && ["planned", "loading", "en_route"].includes(c.state)));
    if (low) out.fobs.push({ command: "RESUPPLY", fob: low.id, reason: `fake provider: ${low.id} stock ${low.stock} is low` });
  }

  if (!doOrders) return out;
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
// v1: needs an orders array. v2: orders/buy/missions/fobs may be omitted, but the reply must contain at least one channel or keep.
// v2 accepts the terse form too ({"o":[[squad,cmd,target|[x,z],reason]],"b":..,"f":..,"m":..,"k":1,"n":[sec,why]}); promptKit.expandTerse turns it
// into the canonical verbose object (a verbose reply passes through unchanged), so validateV2 never sees the difference.
const TERSE_CHANNELS = ["o", "b", "f", "m"];
function extractOrders(text, v2 = false) {
  const a = text.indexOf("{"), b = text.lastIndexOf("}");
  if (a < 0 || b < a) throw new Error("no JSON object in reply");
  const obj = JSON.parse(text.slice(a, b + 1));
  if (!obj || typeof obj !== "object" || Array.isArray(obj)) throw new Error("reply is not a JSON object");
  if (v2) {
    const keep = obj.keep === true || obj.k === 1 || obj.k === true || obj.k === "1";
    if (!["orders", "buy", "missions", "fobs", ...TERSE_CHANNELS].some((k) => Array.isArray(obj[k])) && !keep) throw new Error("reply has no orders/buy/missions/fobs array");
    return expandTerse(obj);
  } else if (!Array.isArray(obj.orders)) throw new Error("reply has no orders array");
  return obj;
}

// ---------- LLM call metrics ----------
// Every LLM call (commander, sitrep, journal, translate, hq agent turn) appends one entry to state.llmCalls (ring of LLM_CALLS_MAX, newest first), prints
// one log line, and feeds the per-role "latency" aggregate of GET /api/state. Entry: {ts (epoch ms), faction, role, slot, model, ok, error, inTok, outTok,
// cachedTok, ttftMs (null = not streamed / unknown), totalMs, promptChars, sections:{<name>:{chars,tokens}}, outputMode, fellBack, terse?, cost?}.
const LLM_CALLS_MAX = 200;
function recordLlmCall(e) {
  const entry = {
    ts: Date.now(), faction: e.faction ?? "?", role: e.role ?? "commander", slot: e.slot ?? slotOf(cfg, e.role ?? "commander"), model: e.model ?? null, ok: e.ok !== false, error: e.error ?? null,
    inTok: e.inTok ?? 0, outTok: e.outTok ?? 0, cachedTok: e.cachedTok ?? 0, ttftMs: isNum(e.ttftMs) ? Math.round(e.ttftMs) : null, totalMs: Math.round(e.totalMs ?? 0),
    promptChars: e.promptChars ?? 0, sections: e.sections ?? {}, outputMode: e.outputMode ?? "text", fellBack: !!e.fellBack,
  };
  if (e.terse !== undefined) entry.terse = !!e.terse;
  if (isNum(e.cost)) entry.cost = e.cost;
  state.llmCalls.unshift(entry);
  if (state.llmCalls.length > LLM_CALLS_MAX) state.llmCalls.length = LLM_CALLS_MAX;
  const secs = Object.entries(entry.sections).map(([k, v]) => `${k}:${v.tokens}`).join(",");
  console.log(`llm role=${entry.role} faction=${entry.faction} model=${entry.model ?? "-"} in=${entry.inTok} out=${entry.outTok} cached=${entry.cachedTok} ttft=${entry.ttftMs === null ? "-" : entry.ttftMs + "ms"} total=${entry.totalMs}ms sections=${secs || "-"}${entry.ok ? "" : ` ERROR="${String(entry.error).slice(0, 100)}"`}`);
  return entry;
}
// Per-role aggregate over the ring: percentiles and averages over the successful calls (all calls when none succeeded).
function latencyView() {
  const by = {};
  for (const e of state.llmCalls) (by[e.role] ??= []).push(e);
  const avg = (xs) => (xs.length ? Math.round(xs.reduce((a, b) => a + b, 0) / xs.length) : null);
  const out = {};
  for (const [role, arr] of Object.entries(by)) {
    const ok = arr.filter((e) => e.ok), base = ok.length ? ok : arr;
    const tot = base.map((e) => e.totalMs).sort((a, b) => a - b);
    const pct = (p) => tot[Math.min(tot.length - 1, Math.max(0, Math.ceil(p * tot.length) - 1))];
    out[role] = { n: arr.length, okN: ok.length, p50Ms: pct(0.5), p90Ms: pct(0.9), avgTtftMs: avg(ok.map((e) => e.ttftMs).filter(isNum)), avgInTok: avg(ok.map((e) => e.inTok)),
      avgOutTok: avg(ok.map((e) => e.outTok)), avgCachedTok: avg(ok.map((e) => e.cachedTok)), lastMs: arr[0].totalMs, lastAt: arr[0].ts };
  }
  return out;
}
// A non-LLM answer (fake provider) in the same ring, so the dashboard and the tests see the pipeline without a key.
function recordFakeCall(role, faction, t0, extra = {}) {
  return recordLlmCall({ faction, role, slot: slotOf(cfg, role), model: "fake", totalMs: Date.now() - t0, ...extra });
}

// Returns {raw, usage:{input,output,cached?,cost?}, ttftMs, totalMs, streamed, responseFormatUsed, metrics}. Throws Error on transport/HTTP failure (err.metrics = the failed call's entry).
// The prompt comes from the caller (opts.system / opts.userMsg: commanderPrompt builders, journal review, translator); without them the commander prompt is built from sit.
// opts: role, slot, slotCfg (already resolved: primary or backup), maxTokens, timeoutSec, responseFormat, cacheSystem, sections (promptKit.sectionStats), outputMode, terse, fellBack.
async function callProvider(c, sit, opts = {}) {
  const role = opts.role ?? "strong";
  c = opts.slotCfg ?? slotCfg(c, role); // role -> slot (translate/sitrep use the weak slot when there is one); opts.slotCfg = already resolved (primary or backup)
  let { system, userMsg } = opts, sections = opts.sections;
  if (system === undefined || userMsg === undefined) {
    const pr = buildCommanderPrompt({ config: c, sit, manifest: opts.manifest, journal: opts.journal });
    system ??= pr.system; userMsg ??= pr.user; sections ??= sectionStats(pr.sections);
  }
  sections ??= sectionStats({ system, user: userMsg });
  const rec = (x) => recordLlmCall({ faction: typeof sit?.faction === "string" ? sit.faction : "?", role, slot: opts.slot, model: c.model, promptChars: system.length + userMsg.length, sections,
    outputMode: opts.responseFormat?.type ?? "text", terse: opts.terse, fellBack: opts.fellBack, ...x });
  const t0 = Date.now();
  try {
    const res = await chatCall({
      provider: c.provider, baseUrl: c.baseUrl, apiKey: c.apiKey, model: c.model, system, user: userMsg, maxTokens: opts.maxTokens ?? c.maxTokens,
      timeoutMs: (opts.timeoutSec ?? c.timeoutSec) * 1000, responseFormat: opts.responseFormat ?? null, cacheSystem: !!opts.cacheSystem, rateCooldownMs: RATE_COOLDOWN_MS,
      reasoning: cfg.output?.reasoning ?? "off",
    });
    const metrics = rec({ inTok: res.usage.input, outTok: res.usage.output, cachedTok: res.usage.cached ?? 0, ttftMs: res.ttftMs, totalMs: res.totalMs, outputMode: res.responseFormatUsed?.type ?? "text", cost: res.usage.cost });
    return { ...res, metrics };
  } catch (e) {
    const metrics = rec({ ok: false, error: e?.name === "AbortError" ? "timeout" : String(e?.message ?? e).slice(0, 200), totalMs: Date.now() - t0 });
    if (e && typeof e === "object") e.metrics = metrics;
    throw e;
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

// Reason text cap: 120 chars hard; with opts.reasonWords (terse output) also at most ceil(1.5 x reasonWords) words.
function clipReason(r, reasonWords) {
  let t = String(r ?? "");
  if (isNum(reasonWords) && reasonWords > 0) { const w = t.split(/\s+/).filter(Boolean), max = Math.ceil(reasonWords * 1.5); if (w.length > max) t = w.slice(0, max).join(" "); }
  return t.slice(0, 120);
}
// Protocol v2 validation. Returns {orders, buy, missions}; every dropped item goes to `dropped` with a reason.
function validateV2(obj, sit, dropped = [], manifest = DEFAULT_MANIFEST, opts = {}) {
  const rw = opts.reasonWords;
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
    res.orders.push({ squad: o.squad, command: o.command, ...(a.target ? { target: a.target } : a.pos ? { pos: a.pos } : {}), reason: clipReason(o.reason, rw) });
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
    res.fobs.push({ command: cmd, ...out, reason: typeof f.reason === "string" ? clipReason(f.reason, rw) : "" });
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
    res.buy.push({ type: b.type, count: b.count, command: b.command, ...(ba.target ? { target: ba.target } : { pos: ba.pos }), reason: typeof b.reason === "string" ? clipReason(b.reason, rw) : "" });
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

// response_format of the commander call: json_object (default) / json_schema (terse spec) / text = none. Only openrouter / openai-compatible send it.
function responseFormatFor(c, spec) {
  const mode = c.output?.mode ?? "json_object";
  if (mode === "text") return null;
  if (mode === "json_schema" && spec) return { type: "json_schema", json_schema: { name: "clausewitz_orders", schema: terseSchema(spec), strict: false } };
  return { type: "json_object" };
}

// Runs one situation through the given config. Never throws. opts: manifest, journal, split (planner split: this is the ORDERS call), plan (planBlock input),
// slotName (escalation: slot override). r.metrics = the llmCalls entry of the commander call; r.slot = the slot that answered.
// In split mode buy / fobs in the model's reply are dropped (the planner owns them).
async function computeOrders(c, sit, isTest = false, opts = {}) {
  const r = { orders: [], buy: [], missions: [], fobs: [], keep: false, dropped: [], rawReply: null, usage: null, ok: true, reason: null, metrics: null };
  try {
    if (!sit || typeof sit !== "object" || !Array.isArray(sit.squads)) throw new Error("no squads array");
    const v2 = isV2(sit);
    const faction = typeof sit.faction === "string" ? sit.faction : "?";
    const terse = v2 && !!c.output?.terse;
    const split = v2 && !!opts.split;
    const pr = buildCommanderPrompt({ config: c, sit, manifest: opts.manifest, journal: opts.journal, split, plan: opts.plan });
    const sections = sectionStats(pr.sections), t0 = Date.now();
    const callOpts = { role: "commander", test: isTest, slotName: opts.slotName, system: pr.system, userMsg: pr.user, sections, terse,
      responseFormat: v2 ? responseFormatFor(c, pr.spec) : null, cacheSystem: v2 && !!c.prompt?.cache }; // v1 protocol: plain prompt, no response_format / cache hints
    let raw;
    if (v2) {
      let obj;
      if (c.provider === "fake") { obj = fakeV2(sit, opts.manifest, split ? "orders" : "all"); r.metrics = recordFakeCall("commander", faction, t0, { promptChars: pr.system.length + pr.user.length, sections, terse }); }
      else {
        session.requests++;
        const p = await callWithFallback(c, sit, callOpts);
        if (p.provider === "openrouter") recordUsage(p.modelUsed, faction, p.usage);
        r.rawReply = p.raw; r.usage = p.usage; r.modelUsed = p.modelUsed; r.fellBack = p.fellBack; r.primaryError = p.primaryError; r.metrics = p.metrics ?? null;
        obj = extractOrders(p.raw, true);
        if (isNum(obj.next_check_sec)) r.nextCheck = { sec: Math.round(obj.next_check_sec), reason: typeof obj.next_check_reason === "string" ? obj.next_check_reason.slice(0, 120) : "" };
      }
      if (split) for (const k of ["buy", "fobs"]) { // the planner owns these channels: a model that answers them anyway is ignored
        if (Array.isArray(obj[k]) && obj[k].length) { r.dropped.push({ [k]: obj[k], reason: "buy/fobs belong to the planner call" }); console.log(`  dropped ${k}: owned by the planner call`); }
        delete obj[k];
      }
      r.slot = r.metrics?.slot ?? null;
      Object.assign(r, validateV2(obj, sit, r.dropped, opts.manifest, { reasonWords: terse ? c.output.reasonWords : undefined }));
      r.keep = obj.keep === true && !r.orders.length && !r.buy.length && !r.missions.length && !r.fobs.length;
      return r;
    }
    if (c.provider === "fake") { raw = fakeOrders(sit); r.metrics = recordFakeCall("commander", faction, t0, { promptChars: pr.system.length + pr.user.length, sections }); }
    else {
      session.requests++;
      const p = await callWithFallback(c, sit, callOpts);
      if (p.provider === "openrouter") recordUsage(p.modelUsed, faction, p.usage);
      r.rawReply = p.raw; r.usage = p.usage; r.modelUsed = p.modelUsed; r.fellBack = p.fellBack; r.primaryError = p.primaryError; r.metrics = p.metrics ?? null;
      const obj = extractOrders(p.raw);
      raw = obj.orders;
      if (isNum(obj.next_check_sec)) r.nextCheck = { sec: Math.round(obj.next_check_sec), reason: typeof obj.next_check_reason === "string" ? obj.next_check_reason.slice(0, 120) : "" };
    }
    r.orders = validate(raw, sit, r.dropped);
  } catch (e) {
    if (e?.fellBack) { r.fellBack = true; r.primaryError = e.primaryError; }
    if (e?.metrics) { r.metrics = e.metrics; r.slot = e.metrics.slot ?? null; }
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
  const hp = slotChain(c, "hq").primary.provider;
  if (!["fake", "openrouter", "openai-compatible"].includes(hp)) return `provider ${hp} has no agent turn`;
  return null;
}
function hqFinish(entry, faction, r) {
  const out = { actions: [], replies: [] }; // this turn's delivery: to the fast request if it is still open, else to the next /orders reply
  entry.state = "done"; entry.ok = r.ok; entry.error = r.error ?? null; entry.latencyMs = r.latencyMs; entry.usage = r.usage; entry.steps = r.steps; entry.tools = r.tools;
  entry.modelUsed = r.ok ? (r.modelUsed ?? entry.model) : null; entry.fellBack = !!r.fellBack; entry.primaryError = r.primaryError ?? null;
  recordLlmCall({ faction, role: "hq", slot: entry.slot ?? "strong", model: r.ok ? (r.modelUsed ?? entry.model ?? "fake") : (entry.model ?? "fake"), ok: r.ok, error: r.ok ? null : r.error, inTok: r.usage?.input ?? 0, outTok: r.usage?.output ?? 0, ttftMs: null, totalMs: r.latencyMs ?? 0, // an agent turn = several calls: usage + latency of the whole turn, no TTFT
    outputMode: "text", fellBack: !!r.fellBack, cost: r.usage?.cost });
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
    const ch = slotChain(c, "hq"); // roles.hq -> slot; cooldown keys "hq:<slot>|<model>" (own cooldown, see the slots section)
    if (ch.primary.provider !== "fake") { session.requests++; }
    const fallback = ch.fallback ? { config: ch.fallback, primaryCooling: () => coolLeftMs(ch, ch.primary) > 0,
      onFailure: (which, err) => noteCooldown(ch, which === "primary" ? ch.primary : ch.fallback, err, true), onFallback: (err) => noteFellBack(ch.slot, ch.primary.model, err) } : null;
    const hqTo = c.roleTimeouts?.hq; // roleTimeouts.hq (> 0) replaces agent.timeoutSec; the fast-path cap applies on top
    const baseSec = isNum(hqTo) && hqTo > 0 ? hqTo : c.agent.timeoutSec;
    const turnSec = entry.fast ? Math.min(baseSec, HQ_FAST_TURN_SEC) : baseSec;
    // hard cap on the whole turn: a backup-model call (primary cooling down after commander timeouts) hung past the agent timeout and
    // the turn never finished (DS e2e 2026-10-15, msg 3); the player then waited for the game's 90 s timeout
    let hardTimer;
    const hard = new Promise((_, reject) => { hardTimer = setTimeout(() => reject(new Error(`turn exceeded ${turnSec + 5} s (hard cap)`)), (turnSec + 5) * 1000); });
    const r = await Promise.race([mod.runAgentTurn({ manifest, situation: sit, message: entry.msg, config: ch.primary, agentConfig: { maxSteps: c.agent.maxSteps, timeoutSec: turnSec }, journal: journalBlock(faction, c, sit), fallback }), hard]).finally(() => clearTimeout(hardTimer));
    if ((r.fellBack ? ch.fallback?.provider : ch.primary.provider) === "openrouter") recordUsage(r.modelUsed ?? ch.primary.model, faction, r.usage);
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
    const hp = slotChain(c, "hq"); // roles.hq decides the slot (default strong)
    const entry = { ts: Date.now(), faction, msg, provider: hp.primary.provider, slot: hp.slot, model: hp.primary.provider === "fake" ? null : hp.primary.model, modelUsed: null, fellBack: false, primaryError: null, state: "running", ok: null, error: null, steps: [], tools: [], actions: [], reply: "", usage: null, latencyMs: null, lang: msg.lang || null, setLanguage: null, historyCount: msg.history.length, path: fast ? "fast" : "situation", fast };
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
  if (why) { journals[faction] = j = J.emptyJournal(); j.resetReason = why; journalBlockKey[faction] = ""; delete plannerState[faction]; console.log(`journal faction=${faction} reset: ${why}`); }
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
    if (c.provider === "fake") { reply = J.fakeReview(); recordFakeCall("journal", faction, t0); }
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
// triggers that escalate an orders call to escalate.slot: contact near a squad, casualties, squad lost (also strength loss), objective lost / changed owner, tickets drop
const IMPORTANT_TRIGGER = /^contact d+ m from|casualties|lost|^objective |^tickets /i;

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
  const plSplit = v2 && plannerSplit(snapshot, manifest); // planner split: buy / FOB / logistics come from their own call
  let plannerOk = false, escalated = false; // plannerOk = a running-war LLM poll (the planner may start and deliver)
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
    if (plSplit && valid) { // planner cadence is checked on EVERY running poll (most are gated for the commander); a fake planner runs inline after the orders part instead
      plannerOk = true;
      const plz = plFor(faction), why = plannerIsFake(snapshot) ? "" : plannerDue(snapshot, plz, sit, now, dec.reason === "manual");
      if (why && (snapshot.provider === "fake" || !slotBlocked(snapshot, "planner"))) void plannerJob(faction, snapshot, sit, manifest, srFor(faction).plan, why);
    }
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
      if (valid && v2) recordSituation(faction, sit);
      const srCfg = snapshot.sitrep, srSt = srFor(faction);
      const wantSitrep = v2 && !!srCfg?.enabled && !srSt.inflight && Date.now() >= srSt.retryAt && (!srSt.lastAt || Date.now() - srSt.lastAt >= srCfg.intervalSec * 1000 || dec.reason === "manual" || dec.triggers.some((t) => /objective|tickets|lost/i.test(t)));
      const srFake = wantSitrep && sitrepIsFake(snapshot);
      // a real SITREP call runs in parallel with the commander call (input: the previous orders); a fake one is instant and runs after it (input: these orders)
      if (wantSitrep && !srFake) void sitrepJob(faction, snapshot, callSit, srSt.plan, langs, sit.time);
      escalated = !!snapshot.escalate?.enabled && dec.triggers.some((t) => IMPORTANT_TRIGGER.test(t)); // important wake: this orders call uses escalate.slot
      r = await computeOrders(snapshot, callSit, false, { manifest, journal: v2 ? journalBlock(faction, snapshot, sit) : "", split: plSplit, plan: plSplit ? plannerPlanInfo(snapshot, faction) : null, slotName: escalated ? snapshot.escalate.slot : undefined });
      if (v2 && r.ok && !r.keep) { journalOrders(faction, sit, r.orders); srSt.plan = r.orders; }
      if (plSplit && plannerOk && r.ok && plannerIsFake(snapshot)) await plannerJob(faction, snapshot, sit, manifest, srSt.plan, "inline (fake)");
      if (srFake && r.ok) await sitrepJob(faction, snapshot, callSit, srSt.plan, langs, sit.time);
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
  // a finished background SITREP rides on this reply (any reply: keep, gated, rule fallback) and is delivered exactly once
  const delivered = v2 ? takePendingSitrep(faction) : null;
  // a finished planner result rides on this reply after re-validation: normal, keep and gated replies, never a failure / rule-fallback one (it stays pending, the game falls back to its rules)
  let plannerDelivery = null;
  if (plSplit && plannerOk && r.ok && !r.rules && !(r.gated && sc.failed)) {
    plannerDelivery = takePlannerResult(faction, sit, manifest);
    if (plannerDelivery) {
      const d = plannerDelivery;
      if (d.buy.length || d.fobs.length) { r.buy = d.buy; r.fobs = d.fobs; } else plannerDelivery = d.dropped.length ? d : null;
      if (d.dropped.length) r.dropped = [...(r.dropped ?? []), ...d.dropped];
      console.log(`planner faction=${faction} delivered buy=${d.buy.length} fobs=${d.fobs.length} (planned ${d.planned.buy}+${d.planned.fobs}, ${d.ageSec}s old${d.dropped.length ? ", " + d.dropped.length + " dropped on re-validation" : ""})${r.gated ? " on a gated reply" : ""}`);
    }
  }
  if (delivered) { r.sitrep = delivered.sitrep; if (delivered.i18n) r.i18n = delivered.i18n; console.log(`sitrep faction=${faction} delivered${r.gated ? " on a gated reply" : ""}, ${Math.round((Date.now() - delivered.at) / 1000)}s after it finished`); }

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
      buy: plannerDelivery?.buy.length ? r.buy : keepOld ? (old?.buy ?? []) : (r.buy ?? []), missions: keepOld ? (old?.missions ?? []) : (r.missions ?? []), fobs: plannerDelivery?.fobs.length || plannerDelivery?.buy.length ? r.fobs : keepOld ? (old?.fobs ?? []) : (r.fobs ?? []), eventLog, prevPos, orderTs };
  }
  if (!r.gated) state.exchanges.unshift({
    ts: state.lastRequestTs, faction, provider: snapshot.enabled ? snapshot.provider : "disabled",
    model: snapshot.provider === "fake" ? null : snapshot.model, modelUsed: r.modelUsed ?? (snapshot.provider === "fake" ? null : snapshot.model), fellBack: !!r.fellBack, primaryError: r.primaryError ?? null, i18n: r.i18n ?? null,
    request: sit, rawReply: r.rawReply, orders: r.orders, buy: r.buy ?? [], missions: r.missions ?? [], fobs: r.fobs ?? [], keep: !!r.keep, sitrep: r.sitrep ?? null, dropped: r.dropped,
    latencyMs, ok: r.ok, reason: r.reason, usage: r.usage, metrics: r.metrics ?? null, slot: r.slot ?? null, escalated,
    planner: plannerDelivery ? { delivered: true, buy: plannerDelivery.buy.length, fobs: plannerDelivery.fobs.length, planned: plannerDelivery.planned, droppedOnRevalidate: plannerDelivery.dropped.length, ageSec: plannerDelivery.ageSec, ms: plannerDelivery.ms, reason: plannerDelivery.reason, slot: plannerDelivery.slot, model: plannerDelivery.model } : plSplit ? { delivered: false } : null,
    wake, nextCheck: r.nextCheck ?? null, intervalSec: interval,
  });
  if (state.exchanges.length > RING_MAX) state.exchanges.length = RING_MAX;

  const nContacts = Array.isArray(sit?.contacts) ? sit.contacts.length : 0;
  if (!r.gated) console.log(`orders faction=${faction} squads=${sit?.squads?.length ?? 0} contacts=${nContacts} orders=${r.orders.length}${v2 ? ` buy=${r.buy?.length ?? 0} missions=${r.missions?.length ?? 0}${r.fobs?.length ? ` fobs=${r.fobs.length}` : ""}` : ""} ${latencyMs}ms${r.skipped ? " (" + r.reason + ")" : ""}${wake ? " wake=" + wake.reason + (wake.triggers.length ? " [" + wake.triggers.join("; ") + "]" : "") : ""}${r.nextCheck ? " next=" + r.nextCheck.sec + "s" : ""}`);
  const reply = r.rules ? { orders: [] } : r.paused || r.phaseGate ? { orders: [], keep: true } : r.gated && sc.failed ? { orders: [] } : r.skipped || r.keep ? { orders: [], keep: true }
    : v2 ? { orders: r.orders, buy: r.buy ?? [], missions: r.missions ?? [], ...(r.fobs?.length ? { fobs: r.fobs } : {}) } : { orders: r.orders };
  if (plannerDelivery) { if (plannerDelivery.buy.length) reply.buy = plannerDelivery.buy; if (plannerDelivery.fobs.length) reply.fobs = plannerDelivery.fobs; } // also on keep / gated replies
  if (r.sitrep) reply.sitrep = r.sitrep; // also on keep / gated / rule-fallback replies; a rule-fallback {orders:[]} stays without "keep"
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
const mapState = { status: "none", key: "", pngPath: null, width: 0, height: 0, source: "", texturePath: "", calibration: null, error: null, ts: 0, detectedGameDir: "", addonDirs: [], addonSkipped: [] };
const warnedAddonDirs = new Set(); // addon dir problems are logged once per path
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
    // extra addon dirs (Workshop mods with their own worlds): env CLW_ADDON_DIRS first, then cfg.addonDirs; each is an addon dir or a parent of addon dirs
    const ad = mod.resolveAddonDirs(mod.mergeAddonDirEntries(process.env.CLW_ADDON_DIRS, cfg.addonDirs));
    mapState.addonDirs = ad.dirs; mapState.addonSkipped = ad.skipped;
    for (const sk of ad.skipped) if (!warnedAddonDirs.has(sk.path)) { warnedAddonDirs.add(sk.path); console.log(`map addon dir skipped: ${sk.path} (${sk.reason})`); }
    const r = await mod.resolveMapImage({
      mapPrefab: info.mapPrefab, worldFile: info.worldFile, gameDir, extraDirs: ad.dirs, maxSize: cfg.mapMaxSize,
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
    addonDirs: { count: mapState.addonDirs.length, paths: mapState.addonDirs, skipped: mapState.addonSkipped, env: process.env.CLW_ADDON_DIRS || "", config: cfg.addonDirs },
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
      exchanges: state.exchanges, hq: hqLog.map(({ fast, ...e }) => e), debugLog: state.debugLog, manifests: manifestsView(), counters: state.counters, llmCalls: state.llmCalls, latency: latencyView(), modelCooldowns: cooldownsView(), config: publicConfig(), auth: { defaultPassword: auth.isDefaultPassword() }, loadouts: { bad: loadouts.badCount(), restartId: loadouts.restartId() }, usage: usageView(), scheduler: schedulerView(), planner: plannerView(), journals: journalsView(), paused,
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
        callModel: async ({ system, userMsg, maxTokens, timeoutSec }) => callProvider(tcfg, { faction: "i18n" }, { system, userMsg, maxTokens, timeoutSec, role: "translate", slotCfg: sc, slot: ch.slot }),
        isFake: () => sc.provider === "fake", timeoutMs: () => tcfg.i18n.translateTimeoutSec * 1000,
      });
      const tr = await one.translateFields({ test: "Enemy infantry sighted near the bridge." }, ["de"], { fresh: true });
      return send(res, 200, { ok: !tr.failed.length, text: tr.by_lang.de?.test ?? null, error: tr.failed[0]?.reason ?? null, latencyMs: Date.now() - t0, provider: sc.provider, model: sc.provider === "fake" ? null : sc.model, slot: ch.slot, which: testWhich });
    }
    const cch = slotChain(tcfg, "commander"); // roles.commander decides the slot of the commander test
    let ccfg = cch.primary;
    if (testWhich === "fallback") {
      if (!cch.fallback) return send(res, 400, { error: "no backup model configured for this slot" });
      ccfg = cch.fallback;
    }
    const sit = {
      faction: "TEST", time: 0,
      squads: [{ id: "TEST_Squad1", pos: [1000, 0, 1000], agents: 8, startAgents: 8, mode: "NONE", humanLed: false }],
      contacts: [{ pos: [1500, 0, 1200], desc: "2x infantry", ageSec: 10 }],
    };
    const t0 = Date.now();
    const r = await computeOrders(ccfg, sit, true);
    return send(res, 200, { ok: r.ok, orders: r.orders, dropped: r.dropped, rawReply: r.rawReply, error: r.reason, latencyMs: Date.now() - t0, provider: ccfg.provider, model: ccfg.provider === "fake" ? null : ccfg.model, slot: cch.slot, which: testWhich });
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

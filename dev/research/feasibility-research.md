# Clausewitz - Feasibility Research

Date: 2026-10-11. Sources: Enfusion API index (via MCP `api_search`), BI wiki (`wiki_read`), GitHub projects and community guides read with Camoufox. Nothing here has been run in the engine yet; items marked **[verify]** need a spike.

## Verdict

**Feasible.** Two LLM High Commands over vanilla AI, with fog of war, is built from parts already shown to work separately. Biggest risks: AI population / server load, vehicle AI quality, and getting LLM output reliable. Voice is the weakest part.

## 1. Prior art

| Project | What it shows | Notes |
|---|---|---|
| [m0nklabs/Reforger-LLM-Squad](https://github.com/m0nklabs/Reforger-LLM-Squad) | Mod -> HTTP -> FastAPI bridge -> LLM; SITREP in, orders out (engage/hold/move/flank/retreat/mount); LLM "Stavka" OPFOR strategic layer; voice STT/TTS | Works on dedicated server. Good lessons file (CLAUDE.md). One faction + player squad, not a full AI-vs-AI war. |
| [nuclearbombmods/reforger-chain-of-command](https://github.com/nuclearbombmods/reforger-chain-of-command) | Two opposing chains of command, 17 ranks, per-officer memory, OPORDs, comms posture, sitrep distortion, trust, deception, campaign persistence; Node/Hono + Ollama sidecar + ~30 `.c` files | Closest to our vision. Author says **unproven under load**, hallucination issues. Excellent `DESIGN.md` to read before designing. No license: read for ideas, do not copy. |
| "LLM Commander" (vlog, [NeoGAF thread](https://www.neogaf.com/threads/can-claude-ai-command-troops-in-arma-reforger.1692991/)) | LLM as strategic layer above vanilla AI; periodic situation reports in, orders out; simulated vision/sound inputs; sockets | PvE opponent, single side, early. Little technical detail. |
| [MitoAtGit/reforger-modding-knowledge](https://github.com/MitoAtGit/reforger-modding-knowledge) (`docs/50-scripting/ai-scripting.md`) | Field-tested ranking of safe ways to influence AI | See section 3. |
| Workshop: BattleGen High Command, Advanced AI Command, AI Commanding | Player-side command of AI; automatic AI commander in BattleGen | Not LLM; useful as UX/behaviour reference. |
| [CptGalaxy/llm-commander](https://github.com/CptGalaxy/llm-commander) (Workshop "ClaudeCommander") | One LLM strategic commander over vanilla AI, 9 order types, Flask proxy, debrief | All rights reserved. Best lessons file (`CLAUDE.md`), see below. One side only. |
| [DCO_AI](https://817r.github.io/DCO_AIRoadmap/) | Rule-based soldier/squad/vehicle AI + AI commander (QRF, arty, resupply) | Very active (v0.4.136, Sep 2026, 34k dl). Possible later compat target; may fight our waypoints. |
| [Conflict HQ PvPvE](https://reforger.armaplatform.com/workshop/69BA7B45F088EA15-ConflictHQPvPvE) | Rule-based BLUFOR/OPFOR AI commanders (recon, attack, defend, base building) + FIA raiders, on vanilla HQ Commander | WIP. Closest rule-based analogue: study for our RuleCommander. |
| [AI Kommandanten](https://reforger.armaplatform.com/workshop/A7C3E91B5D2F4601-AIKommandanten) | Two AI commanders, HQ + outposts, squads bought with points, supply trucks | "TEST". Study the rule-based war loop. |
| [Raven AI Commander](https://reforger.armaplatform.com/workshop/6A3A112604EF7286-RavenAICommander) | Autonomous AI economy, logistics, convoys for Conflict | Active (1.8, Aug 2026), **APL licence: reuse allowed**. Logistics reference. |
| [OpFor Warfare](https://reforger.armaplatform.com/workshop/7F8A7AE18C17C220-OpForWarfare) | Arma 2 Warfare CTI vs an AI commander | Early. Not inspected. |
| [BGC_Core Battlegroup Command](https://reforger.armaplatform.com/workshop/32C5A359B406A788-BGC_Core-BattlegroupCommand) | Tactical-map UI commanding many AI groups, faction commander role, any faction, DS | Active. UX reference for a human commander / spectator map. Not inspected. |
| [Dynamic Operations 2](https://reforger.armaplatform.com/workshop/6A74115AE1FF6652-DynamicOperations2) | Persistent PvE campaign generator | Not inspected. Persistence reference. |
| Overthrow, Everon Life | Dynamic campaign / life mods with persistence | Not inspected. |
| [yaluft/llmm-reforger](https://github.com/yaluft/llmm-reforger) | Showed up for "reforger llm" (last push 2025-07) | Not inspected. |
| [DCO GPT](https://steamcommunity.com/sharedfiles/filedetails/?id=2965142417) (Arma 3, 2023) | ChatGPT NPC conversations: auto-generated personality profiles (cached, ~1000 pre-generated), NPCs know their mission and injuries; GPT radio assistant (locations, gear, squad) with 5 min memory | Not a commander. Bridge = Node.js process + INIDBI2 **file exchange** (proof that a file-based bridge works). Idea for later: HQ/officer personas, profile caching. |
| [TAC-LEX](https://github.com/TreeoM-ManhheiM/TAC-LEX) (Arma 3, MIT, v2.4 alpha) | Natural language from chat → LLM tool calling → structured command → missions (kill/rescue/defend, chained campaigns), spawns, weather | Not a commander. Useful patterns: game-agnostic core + per-game adapters, **content packs per mod (vanilla/RHS/CUP)** for faction mapping, event bus + rules engine, persistent campaign state JSON, missions/results JSONL dataset. Reforger adapter is paper only (no Enforce runtime). |

Workshop has ~121 hits for "AI commander" (only the first 16 were looked at): crowded, our niche is the LLM + two-sided fog of war.

Infrastructure worth reusing:
- [Arkensor/EnfusionDatabaseFramework](https://github.com/Arkensor/EnfusionDatabaseFramework) (EDF) + Enfusion Persistence Framework (EPF): production `RestContext` web-proxy driver (reference for our bridge, section 2), and a candidate for saving campaign state / virtual battlefield records later.
- acemod docker-reforger: Dockerised dedicated server, handy for repeatable DS tests. Not inspected.
- Vanilla: 1.6+ **HQ Commander** (player commander role, AI supply transport groups, supply scarcity, command posts) and `SCR_AmbientPatrolSystem` / proximity spawners (how vanilla spawns AI near players).

Takeaway: nobody has shipped a full two-sided LLM war with real fog of war, resource budget and logistics, or run one live for an hour. The pieces exist.

### Lessons from prior art (round 2, 2026-10-11)
- **Never wait on the LLM inside a request.** CptGalaxy reports a ~10 s RestContext timeout (conflicts with `SetTimeout` existing: verify) vs 8-15 s LLM replies. Pattern: game posts state, sidecar answers immediately, game polls orders every few s. Sidecar keeps a durable order queue (m0nklabs lost orders on bridge restart).
- **Server only.** Guard bridge code with `Replication.IsServer()`; a client running it stole orders and duplicated SITREPs.
- **Order stability.** Don't re-order a group within ~30 s; vanilla combat behaviour (utility 70-90) beats a default waypoint (30); blocked groups need priority escalation.
- **Vehicles:** dismount passengers only, never crews; mounted groups can't use patrol waypoints; use road-snapped move waypoints.
- **Text, not images.** Map-tile vision to the LLM cost too many tokens and was dropped.
- **Hallucination is unsolved:** per-verb schema + existence/authority checks + HOLD fallback. Local models return fenced/truncated/non-dict JSON: extract the first `{...}` and type-check it.
- **Cost:** commanders wake on events, idle = zero calls; tiered models (chain-of-command targets ~2.5k tokens in / 500 out for the top level).
- **Spawn:** add AI to its group, then `ActivateAI()` ~500 ms later, or the group utility component is null.
- **Override vs augment** vanilla Conflict/HQC: running both fights over the same groups. Vanilla 1.6+ HQ Commander is a player role; there is no vanilla AI faction commander.

## 2. REST bridge (game <-> LLM)

- `GetGame().GetRestApi().GetContext(url)` -> `RestContext` with `GET`, `POST`, `PUT`, `DELETE` (async with `RestCallback`, plus blocking `_now` variants), `SetHeaders(string)`, `SetTimeout(int)`.
- Response/request data **under 1 MB**. Callbacks are async (do not block the game thread).
- **Headers work; the wiki "no custom headers" note is stale.** Arkensor's Enfusion Database Framework (EPF backend) ships a production web-proxy driver using `SetHeaders` + JSON bodies with POST/PUT/DELETE. Header format is **comma-separated pairs**: `"Content-Type,application/json,User-Agent,Clausewitz"` (not `Key: Value`). Source: `EDF_WebProxyDbDriverBase.c` in github.com/Arkensor/EnfusionDatabaseFramework.
- **Empty POST body** (Reforger-LLM-Squad, `Content-Length: 0`): their code never called `SetHeaders`, so likely a missing `Content-Type`. **[verify in spike, with and without Content-Type, Workbench + dedicated]**. Keep a GET `?data=` fallback.
- Reforger sends `Connection: Upgrade` on plain requests; Uvicorn chokes on it unless patched. Pick/patch the sidecar HTTP server accordingly.
- `GetContext` URL needs a trailing `/` or the `_now` variants break (EDF). Use `SetOnSuccess`/`SetOnError` (the `OnSuccess`/`OnError` overrides are obsolete); keep callbacks in a `static ref set<>` until done. `Print` truncates at 1024 chars. HTTPS via URL scheme.
- Unknown: concurrency limits, whether clients can call REST, real timeout cap (see ~10 s report below).
- Callback objects must be kept in a `ref` member (inline `new RestCallback` gets garbage-collected before the response).
- `RestContext` is a mod-written-script feature, not Workbench-only; used on dedicated servers by the projects above.
- Alternative mentioned by the LLM Commander vlog: sockets. Not found in the script API index; plan on REST.
- Polling pattern (game polls for orders) plus the game pushing SITREPs works. No inbound connections into the game are needed.

## 3. Influencing AI (what the engine gives us)

AI architecture: brain agent + body entity, behaviour trees (Workbench-authored), utility scoring, groups are agents too (group leader coordinates). `SCR_AIWorld` hosts navmesh and the **AI population budget**.

Safe levers (mildest first), per community notes:
1. **Group waypoints** (`AIGroup.AddWaypoint`, `SCR_AIWaypoint` + subclasses `SCR_EntityWaypoint`, `SCR_TimedWaypoint`, `SCR_TransportWaypoint`, `SCR_BoardingWaypoint`, `SCR_AIWaypointArtillerySupport`, `SCR_SuppliesTransferWaypoint`, `SCR_SmartActionWaypoint`, ...). The vanilla command layer. Always safe.
2. Share a contact into group perception (`SCR_AIGroupPerception.AddOrUpdateTarget`).
3. Orders via the agent mailbox (stance, movement type, ...).
4. Goal messages (attack / defend / move / suppress) competing in the utility system.
5. Target assignment bias (`SetAssignedTargets`): strongest, must be maintained.

Useful classes found in the API index:
- `AIGroup` (agents, waypoints, formation), `ChimeraAIGroup` (dormant/expand/spawn members, importance), `SCR_AIGroup` (rally points, many group APIs).
- `SCR_AIGroupUtilityComponent`: `SetCombatMode`, `SetMaxAutonomousDistance`, `GetThreatMeasure`, group perception, `m_Mailbox`, settings component.
- `AIGroupMovementComponent`: movement type, formation handlers.
- `SCR_AIGroupSettingsComponent` / settings classes (e.g. `SCR_AIGroupCharactersMovementSpeedSetting`).

Design rule from the field: every mutation has a revert path, rate-limit commands, ship a dry-run mode first, keep it all server-side. Leadership resolves a few seconds after spawn; wait before classifying roles.

## 4. Perception -> fog of war

- `PerceptionComponent` (per AI body): target lists by `ETargetCategory`, last seen, closest target, perception factor.
- `SCR_AIGroupPerception`: aggregates member contacts into `SCR_AITargetInfo` list and **target clusters** (`SCR_AIGroupTargetCluster`), events `GetOnEnemyDetected`, `GetOnEnemyDetectedFiltered`, `GetOnNoEnemy`, `GetOnTargetClusterDeleted`; ~2 s update; forget/lost thresholds.
- Plan: subscribe per group, convert to `Contact` records (position, type, count, heading, timestamps, confidence), merge per faction, decay over time. The LLM only ever receives its faction's contact list -> genuine fog of war. Our own units' states are known (with optional report delay).
- Gunshots are tracked too (`AddOrUpdateGunshot`), useful for sound-based intel.

## 5. Map markers (auto sightings, stale removal)

- **Vanilla already does it:** `SCR_AIEnemyMarkingSystem` (GameSystem) - AI groups mark enemy targets on the map; configurable `m_fTimeUntilMarkerDeletion`, `m_fGroupMarkingCooldown`, min mark distance (infantry/vehicles), `m_fBaseInaccuracy`, `m_fInaccuracyByDistance`; uses military symbol icons and faction identity. `MarkTarget(targetInfo, spottingGroup)` is public.
- `SCR_MapMarkerManagerComponent` (on the game mode): `InsertStaticMarkerByType`, `InsertStaticMarker`, `GetStaticMarkers`, dynamic markers, server markers, faction flags; markers are replicated. Use for our own markers if the vanilla system is too limited. Delete = our stale timer.
- Players' manual markers: readable through `GetStaticMarkers()` (owner ID, type, config ID, position, text). Parsing them into contacts is plausible. **[verify]** which marker types/text fields are available and how to detect "enemy" markers (probably via a dedicated custom marker config/icon set).
- A radial "report contact" menu is custom UI + input action: `SCR_MapMarkerManagerComponent` API for the marker, raycast / view direction for position. **[verify]** feasibility of range estimation (e.g. use a trace along the view direction).

## 6. Chat and voice

- Text: `SCR_ChatComponent` (`SendMessage`, `OnNewMessage(msg, channelId, senderId)`, `RadioProtocolMessage`). Server can receive/handle messages; mod can parse them and reply to a player. **Works as a first channel.**
- Voice: `VoNComponent`/`SCR_VoNComponent` handle capture (`SetCapture`), comm method, radio transceiver; the wiki describes an OPUS audio pipeline `AudioCaptureDevice -> server -> clients`. **No script API found for reading raw audio buffers** (API search for AudioCapture returned nothing). Scripts can't hand audio to an external STT directly. **[verify]**
- Practical routes:
  1. **Companion app** on the player's PC: push-to-talk key, local STT (Whisper), sends text via a sidecar endpoint (player ID + token) or injects chat. Reforger-LLM-Squad already did Whisper STT + edge-TTS as a PTT tool outside the game.
  2. Server-side VoN tap: no known way. Skip unless a mod/engine feature appears.
  3. TTS for HQ replies: via the same companion, or just text/radio-style UI in game.
- Voice-to-text searches on the web failed (search tool timeouts); redo later.

## 7. Economy, building, logistics

- Resources: `SCR_ResourceComponent` + containers/consumers/generators (supply types: `EResourceType.SUPPLIES`...). Conflict has a working supply economy; Game Master does not. Trucks and depots exist in vanilla Conflict, with supply transfer (`SCR_SuppliesTransferWaypoint`).
- Building: `SCR_CampaignBuildingManagerComponent` (54 methods: providers, refund, disassemble rules, outline manager). Community note: "the campaign building system is fully script-drivable server-side - spawn a composition in its unfinished state, bind to a building provider, drive the layout's building value until it completes, with real supply consumption when the supply economy is enabled." No vanilla AI builder behaviour exists, so AI engineers need our scripted behaviour (move to site, then "build" by driving progress) = natural fake/real hybrid.
- Fortifications: compositions via the same system. AI use of fortifications is **[verify]**.
- Our own ledger (per faction and per base) is simpler and safer than depending on vanilla Conflict internals; map it to real containers where cheap.

## 8. Population, cleanup, size of war

- `SCR_AIWorld`: AI population budget (main perf lever); too many AI degrades everyone.
- `SCR_GarbageSystem` / `GarbageSystem`: `Insert(entity, lifetime)`, `Withdraw`, `Bump`, `Flush`, `GetRemainingLifetime`, `FetchTrackedEntities`, blacklist; overridable hooks `OnInsertRequested`, `OnBeforeDelete`. Use it for bodies and wrecks with custom lifetimes.
- `ChimeraAIGroup` dormant APIs: `IsDormant`, `GetDormantAliveCount`, `GetDormantDeadCount`, `SetDormantCounts`, `SetImportance`, `HasObserverInRange`, `SpawnMembers`, `DespawnMembersFromC`, `ExpandOneMember` - the engine has an expand/collapse concept for groups that matches "pause far units". **[verify]** how it behaves for groups with waypoints, and whether it is usable on a dedicated server for arbitrary groups.
- Game Master note: destroyed entities keep consuming editor budgets until cleared (editor-specific).
- **Caps (confirmed):** server `operating.aiLimit` / `-aiLimit` (default unlimited); 1.7+ per-faction active-AI limit (`GetAILimitForFaction`, `CanLimitedAIBeAddedForFaction`). Importance tiers (`SCR_EAISpawnImportance`): LOW may use 50 % of the active limit, NORMAL 70 %, HIGH 90 %, CRITICAL 100 %. Over budget, the engine evicts groups to dormant. Hosters suggest `aiLimit` 50-80 for Conflict; no real benchmarks found. chain-of-command claims ~1250 AI at 60 FPS (author's test only). `-logStats` prints FPS + AI count.
- **Dormancy = despawn/respawn**, not cheap simulation: `SCR_AIGroup.DespawnMembers()` stores alive/dead counts and deletes bodies, `SpawnMembers()` recreates them. `SCR_EAIGroupLifecyclePolicy`: `Manual` (default) or `ProximityDriven` (spawn/despawn by distance to observers). 1.8 exposed the ObserversSystem API and fixed far-group despawning.
- **Implication:** with no players nearby, proximity-driven groups would all despawn, so they'd never fight. An AI-vs-AI war needs either our own "observer" logic (keep the front active) or **abstract combat resolution** between collapsed groups. Whether waypoints survive despawn/respawn: **[verify]**.
- Our design: faction-level strength records; collapse far/idle groups into a record, re-expand near contact; hard cap on live AI.

## 8a. Prior art for virtualisation: ALiVE (Arma 3)

Source: ALiVE.OS source, `addons/sys_profile` ([GitHub](https://github.com/ALiVEOS/ALiVE.OS/tree/master/addons/sys_profile), e.g. `fnc_profileSimulator.sqf`, `CfgVehicles.hpp`); the wiki pages returned 404.
- **Profiles:** hash records per group ("entity") and vehicle in a central handler with a spatial grid. They hold position, per-man damage array, waypoints, active/locked flags, speed, spawn data, combat state and last-sim time. Vehicles keep per-hitpoint damage and crew.
- **Movement:** distance integration along waypoints, `speed × (now - lastSim)`, with an optional grid/road pathfinder. A per-frame handler simulates 4 profiles and 3 attacks per frame, time-scaled per profile.
- **Virtual combat:**
  - Inactive profiles scan about 225 m for enemies and create an attack record.
  - Each cycle, damage = output × rate × modifier, spread randomly over the target's units.
  - There is no ammo model. Virtual armour was overpowered (issue #792).
  - Spawned profiles never fight virtually; their pending attacks are removed.
- **Spawning:**
  - Spawn radius 1500 m for ground and helicopters; jets and UAVs are off by default.
  - Spawns are queued with a smooth spawn of 0.3 s, and active groups are capped at 144, nearest to a player first.
  - At most one despawn is processed per frame.
- **Despawn guards:**
  - Never despawn while airborne.
  - Combat extends the spawned time by 60 s, up to five times.
  - 300 s grace after a player was occupant; 120 s within 500 m after a player death.
- **Spawn safety:** units are invulnerable until the group is complete, then a safe-position search, road snap within 100 m, and a 15 s vehicle settle time.
- **Known bugs:** vehicles spawning in bad spots (#850), units spawning in terrain or falling through objects (#913, #1020), armour not moving after spawn (#720), hitches when registering kills (#971), debug mode halving FPS (#838), short or corrupt records after loading saves on dedicated servers, and persistence not saving on server close (#585).
- **Reforger:** I found no ALiVE-like mod, only simple proximity spawners (`AI_Spawn_OA`, devinSpitz/AiAndEnvSpawnFramework). That search was shallow.
- **Lessons:** see VISION.md section 9, "Experimental: virtual battlefield".

## 9. Vehicles / mechanized

- Vanilla AI can drive vehicles on roads and execute waypoint chains (Move -> Get In -> Move -> Get Out), per a version-pinned guide (Stable 1.8.0.10, Aug 2026). Same guide warns: partial road blocks can cause bad reroutes, combat interrupts progress, remote clients may see different state, mod-generated route logic may not survive reload. Treat as a testable state machine; prove one group + one vehicle first.
- Community note: "vehicle AI usage is limited - verify against vanilla capabilities before designing missions around AI drivers."
- Conclusion: mechanized is **feasible but risky**; schedule a dedicated vehicle spike (convoy, dismount, off-road, bridge, stuck recovery) before committing to the mechanized scope. Provide a stuck-detector and fallback (teleport/despawn-respawn at destination as a last resort).
- **1.9 (experimental since Sep 2026) rewrites the whole vehicle powertrain + steering** (Dev Report #24); trucks were very slow uphill in 1.7/1.8. Vehicle AI behaviour will shift: do vehicle work after 1.9 is stable.
- Relevant classes: `SCR_TransportWaypoint`, `SCR_BoardingWaypoint`, `SCR_EntityWaypoint`, `SCR_TimedWaypoint`, `SCR_AIWaypointArtillerySupport`.

## 10. Custom factions and RHS/WCS

- Factions live in `SCR_FactionManager` / `SCR_Faction`; unit prefabs, groups, vehicles are listed in **entity catalogs** (`SCR_EntityCatalog`, `SCR_EntityCatalogEntry` with labels, data types, editor/AI patrol data). `SCR_EntityCatalog.GetEntityListWithLabel / WithData / FullFiltered` supports building role lists dynamically.
- Mods like RHS register their own faction + catalogs; so reading catalogs at runtime makes the mod faction-agnostic. Role classification will need heuristics + override tables per pack. **[verify]** against RHS/WCS data on disk.
- MCP tools available for this work: `faction_list_units`, `component_search`, `asset_search`, `game_read`.

## 11. Existing vanilla hooks worth knowing

- `SCR_FactionCommanderHandlerComponent` + `SCR_BaseAIFactionCommander` (+ `SCR_EstablishBaseAIFactionCommander`): vanilla AI faction commander slot (Conflict). Minimal logic today, but a clean place to hang a commander, or to study.
- `SCR_MapMarkerManagerComponent`, `SCR_AIEnemyMarkingSystem`, `SCR_AIWorld`, `SCR_GarbageSystem`: all above.
- Rally points on `SCR_AIGroup` (`SetRallyPoint`) tied to military bases.

## 12. Risks and unknowns (ordered)

1. REST POST body / timeout cap - probably fine (EDF does it), settle in 30 minutes with a test.
2. Live AI capacity on target hardware; collapse/expand behaviour; AI-vs-AI fights far from any player (see section 8).
3. Vehicle AI behaviour for assaults and convoys.
4. LLM reliability: hallucinations (reported in chain-of-command), need schema validation + ground-truth checks; cost/latency of two commanders.
5. Voice: no raw audio from scripts; needs companion app.
6. Player-marker parsing and in-game report menu (UI work).
7. Role classification for arbitrary mod factions.
8. Dedicated server + Workshop publication quirks (stale cache, mismatched scripts; mods must be published for DS to load them).
9. Replication/authority bugs when mixing server scripts and client UI.

## 13. Suggested first spikes (in order)

1. **REST test**: GET + POST + headers from a game-mode script to a local echo server. (Resolves risk 1.)
2. **Perception tap**: print per-group enemy clusters from `SCR_AIGroupPerception` on two opposing AI groups; build the `Contact` record.
3. **Order loop**: sidecar returns a canned `MOVE` order; mod creates a waypoint on the group and logs progress.
4. **Vehicle spike**: one IFV + squad: board, drive 2 km on road, dismount, report.
5. **Population spike**: spawn N groups, measure server FPS; test dormant/expand APIs.
6. **Marker spike**: spawn/delete a server marker; read player markers; hook `SCR_AIEnemyMarkingSystem`.

## 14. Sources

- BI wiki: REST API Usage, JsonApiStruct Usage, Audio: Voice over Network (via MCP wiki tools).
- Enfusion API index via MCP: AIGroup, ChimeraAIGroup, SCR_AIGroup, SCR_AIGroupUtilityComponent, SCR_AIGroupPerception, PerceptionComponent, SCR_AIWaypoint, SCR_FactionCommanderHandlerComponent, SCR_BaseAIFactionCommander, SCR_MapMarkerManagerComponent, SCR_AIEnemyMarkingSystem, SCR_ChatComponent, VoNComponent, SCR_GarbageSystem, SCR_ResourceComponent, SCR_CampaignBuildingManagerComponent, SCR_EntityCatalog, SCR_AIWorld, RestContext.
- GitHub: m0nklabs/Reforger-LLM-Squad (README, CLAUDE.md), nuclearbombmods/reforger-chain-of-command, MitoAtGit/reforger-modding-knowledge (ai-scripting.md), ArgA-Reforger/arga_reforger_agent_skills (REST skill).
- Community: gamers.wiki "Game Master AI Routes: Waypoints and Resets", NeoGAF "Can Claude AI command troops in Arma Reforger?".
- Round 2 (2026-10-11):
  - GitHub: CptGalaxy/llm-commander (`CLAUDE.md`), Arkensor/EnfusionDatabaseFramework (`EDF_WebProxyDbDriverBase.c`, `docs/drivers/proxy-mongodb.md`), m0nklabs `AGENTS.md`, nuclearbombmods `DESIGN.md` sections 7.3 and 8.
  - BI wiki: `Arma_Reforger:Server_Config` (`aiLimit`), `Arma_Reforger:Startup_Parameters` (`-aiLimit`, `-aiPartialSim`, `-logStats`), `Arma_Reforger:Server_Hosting` / `Server_Management` (RCON).
  - Patch notes: [1.6 HQ Commander](https://reforger.armaplatform.com/news/update-october-30-2025), [1.6 changelog](https://reforger.armaplatform.com/news/changelog-october-30-2025), 1.7.0.41 and 1.7.0.54 changelogs (per-faction AI limit, observer fixes), 1.8 (13 Aug 2026, ObserversSystem API, dormant fixes), 1.9 experimental and Dev Report #24 (powertrain rewrite), all on reforger.armaplatform.com/news.
  - Installed game scripts via MCP: `SCR_AIGroup.c` (Despawn/SpawnMembers), `SCR_EAISpawnImportance.c`, `SCR_EAIGroupLifecyclePolicy`, `SCR_DefendWaypoint`.
  - Hoster guides (XGamingServer, Terabit) for `aiLimit` 50-80.
  - [DCO_AI roadmap](https://817r.github.io/DCO_AIRoadmap/).

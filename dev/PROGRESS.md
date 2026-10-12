# Clausewitz - Progress

Updated: 2026-10-12. Roadmap: `VISION.md` section 13. Decisions: `notes/`.

## Done: M0 - Skeleton, M1 - LLM bridge, M1.5 - Dashboard

- [x] Vision and feasibility research
- [x] Empty addon (`addon/addon.gproj`, GUID `C1A05E7B3D9F4A28`)
- [x] Decision: own game mode instead of Conflict (`notes/decision-game-mode.md`)
- [x] Branch `main` in workspace and mod repo; MCP project path -> `Clausewitz/addon`
- [x] MCP verified: `project browse` resolves to Clausewitz/addon
- [x] `CLW_GameMode` script + prefab (inherits GameMode_Base.et), world `Worlds/CLW_Arland.ent` (sub-scene of vanilla Arland): US vs USSR factions, loadouts, spawn points, AIWorld with Arland navmeshes; Play runs, marker logged
- [x] One AI rifle squad per side spawns (US 9 agents, USSR 6)
- [x] Research: vanilla player flow lives in GameMode_Base.et, not Conflict (`research/player-flow-components.md`)
- [x] Player spawns as US soldier (SCR_AutoSpawnLogic, m_sForcedFaction "US") and joins the AI rifle squad: needs `m_bPlayable 1` on the scene-placed SCR_AIGroup plus `SCR_GroupsManagerComponent.RegisterGroup(group)` (GroupID is -1 until then), then `AddPlayerToGroup`. Verified: `player 1 in squad=1`; the AI squad then follows the player in formation. The player becomes the squad leader (AI follows). USSR-side join not tested
- [x] Spike: REST GET/POST/headers to local echo server: verified 2026-10-11 (headers incl. Authorization pass via SetHeaders "Name,Value,...", async callbacks fire; see notes/rest-spike.md)
- [x] Spike: perception tap, per-group enemy clusters -> `Contact` record (`CLW_Contact.FromGroup`, reads `SCR_AIGroupUtilityComponent.m_Perception.m_aTargetClusters`): verified, US squad reported the USSR squad at its exact position (400 m apart, player walked within ~100-150 m). Without the player, squads 400 m apart did not detect each other
- [x] Spike: order loop, canned MOVE -> waypoint on a group: verified, US leader walked 150 m east in <60 s on the Arland navmesh (CLW_GameMode.SpikeOrderLoop)
- [x] Contact book (merge 60 m, expire 120 s) + faction map markers (`CLW_ContactBook`, `CLW_ContactMarkers`): contacts verified in the log; map markers confirmed visible by the user (2026-10-11); expiry on the map not yet watched
- [x] RuleCommander v0 (`CLW_RuleCommander`): verified in play, patrol MOVE with no contacts, ATTACK (search-and-destroy) on nearest contact; DEFEND waypoint seen working e2e, but the automatic switch at the 50% strength threshold (WEAK_STRENGTH) is not verified. Second AI-only squad per side added. Game Master: `CLW_EditorSettings` (SCR_EditorSettingsEntity, unlimited editor legal); hold Y 0.5 s opens it (EditorToggle)

- [ ] LATER (M3): human reports via map-marker parsing, `/report` chat command, quick menu (VISION 8)
- [x] DECIDED 2026-10-12: player-request approach (VISION 5a): both, layered (`notes/decision-command-tools.md`). GM functions checked: `research/m3-bases-fobs-requests.md` §4

- [x] Squad movement verified in the log (per-squad leader position + waypoint logging): AI-only squads walk their patrol/attack waypoints. A joining player becomes squad LEADER automatically (`playerLeader=1`; the `leaderId=-1` seen right after join was read too early), so the squad follows the player and HQ skips it: it needs the mission path (M2).
- [ ] Verify later (needs 2 players or a way to join as non-leader): does an AI leader obey HQ waypoints while a player is a plain member? RuleCommander already skips only squads with a human LEADER

## Current milestone: M2 - Real war + mechanized
Design: `notes/m2-design.md` (decided 2026-10-12). Branch `feature/m2-real-war`. Phases P1 war foundation, P2 forces +
mechanized, P3 commander v1 + protocol v2, P4 missions, S sidecar/dashboard; implemented code-only by subagents, one
Workbench e2e test at the end. LLM: fake + OpenRouter tested e2e; Anthropic provider deferred.

### M2 status (2026-10-12, branch `feature/m2-real-war`, build #27)
Implemented P1-P6 + S, then e2e-tested in Workbench (vanilla, RHS test project, WCS test project; real LLM via OpenRouter):
- [x] Setup menu (generated from mode params + faction pickers) looks good and starts the war; no AI before the war starts (scene squads removed, start forces from data).
- [x] Faction choice incl. RHS_USAF / RHS_AFRF / RHS_ION without dependency; WCS vehicles appear in vanilla US/USSR catalogs (auto-discovery); profiles load from .conf.
- [x] Control points capture, income, tickets (kills + bleed), mission offers in the J menu (refresh/expire), truck dismount + RTB, vehicles drive on both sides (crews re-affiliated to the buying faction), road-slot vehicle placement, player joins a side squad after the war start.
- [x] LLM (OpenRouter) commands in protocol v2: orders on objectives, buys with reasons, sees vehicle units; game REST timeout 30 s.
- [ ] Known issues: RHS open-top UAZ469 refuses cargo seating (fallback: infantry walk, UAZ RTB); classifier: armed armoured cars (M1025 M2) come out `mech` (should be `light`), K-17 Berezok seat count 0 from UI info (needs probe for APCs); editor loadout manager gives only a rifleman per faction (vanilla had 6 roles: add them as data to `CLW_LoadoutManager.et`).
- [x] End of war + end screen confirmed by the owner (2026-10-12).
- [ ] Not yet seen in play: mission accept/complete/reward popups, stuck watchdog dismount path, unstuck helper, DEFEND switch at 50 % strength, marker expiry.
- Test packs: junctions in `Documents\My Games\ArmaReforgerWorkbench\addons` (RHS 3, WCS 12, Clausewitz); see `dev/testpacks/README.md`.

### After the first e2e (2026-10-12, builds #28-#31), merged into main as v0.3.0 (local tag, not published)
- [x] Activity management (relevance-based AI LOD; agents must be lifted with SetLOD(max-1) before PreventMaxLOD): far units act without a human near.
- [x] Squad join: HQ squads registered as vanilla groups; deploy menu, respawn menu and groups menu verified; one join service (Establish/Release).
- [x] Vehicle unstuck escalation, server overrides file `$profile:Clausewitz/overrides.json`, classifier fixes, vanilla role loadouts, ticket HUD, dashboard side order + identity colours.
- [x] LLM/rules flip-flop fixed (health scale in the prompt; rules don't override fresh LLM orders).
- [ ] Open: mission completion/reward not yet watched; RHS open-top UAZ seating; harmless vanilla UI exception when the deploy menu closes during a squad list update; dedicated-server checks (group mirror RPC, remote commanding, SetIsPlayable replication).

### UX + SITREP batch -> v0.4.0 (2026-10-12, branch `feature/ux-sitrep`, builds #33-#35)
Implemented by subagents (code only), then one Workbench e2e (vanilla, OpenRouter on, small battle). Designs: `notes/sitrep-design.md`, `research/sitrep-and-briefing.md`.
- [x] Battle-size layout (small): HQs 956 m apart (authored 1911 m), D/E dropped, spawn point and setup-phase player moved, markers rebuilt. Objective spacing fix verified: every `points layout check` ok, closest pair 248 m (>= 220 m). The "F 211 m from HQ" seen in build #34 was a log bug (distance of the pre-snap wish).
- [x] SITREP: rules version at T+0, LLM version at ~15 s, delivered to the own side only (`CLW: sitrep deliver / client received`); new prose style for rules and LLM (sizes/times in words, Intel title, place names, no ids/coordinates). Contact counts fixed (live targets, vehicles vs infantry), seen as 1 and 7 infantry.
- [x] Dashboard: compare tables, SITREP cards, missions page (empty for sides without player-led squads, now explained), map dormant shading.
- [x] H re-show confirmed by the owner (2026-10-12).
- [ ] Not yet seen: 100 m grid `025-285` (changed after the run, not compiled yet), `/sitrep` in game, journal line breaks, no-sidecar run, plausibility of `dormant`, medium layout unchanged, dedicated server (SITREP per side, player teleport at war start).
- Plan only: loadouts/arsenals (`notes/loadouts-arsenal-plan.md`, reviewed against `research/mod-agnostic-asset-discovery.md`). Vision added: in-game friendly map picture (VISION 8a), per-player-squad briefings (4), dashboard accounts + soldier tablet view (12a).
- Known: units take 30-60 s to move at start (first tick 10 s, LLM answer up to 30 s, rules only on error); proposed: one rule-commander pass at war start, LLM overrides it (`LLM_PRIORITY_SEC`). Owner decision 2026-10-12: leave as is, no fix. LLM intel sometimes states more than the contacts show (owner: leave as is).
- Watch: `dev/tools/sidecar-config.json` selects openrouter and `CLW_LLM=fake` does not override it; local tests must use a temp config.

### Dedicated server smoke test (2026-10-12, build #36, no players)
Local DS in `D:\GitHub\ReforgerMods\_dedicated\` (junction to `addon/`, no pack needed); findings and join guide: `notes/dedicated-server.md`.
- [x] Boots in ~25 s, scripts compile (no `SCRIPT (E)`), game mode + scenario + bases/points + setup phase, auto start after 180 s, war RUNNING with 141 AI / 8 vehicles at 60 FPS, sidecar (fake) 5 requests 0 errors, LLM orders and SITREPs applied.
- [x] With one joined client (owner, 2026-10-12, `_dedicated/start-client.ps1`): connect + faction join, setup menu sent to the owner (war then auto-started at 180 s), squad join (group id 1003, player leader, 3 AI moved), mission offers in the J menu, AI commanding (radial menu), groups menu (group mirror / registration replicate), ticket HUD, SITREP rules v2 + LLM v3/v4 delivered to the US player only; sidecar (OpenRouter) driven by the DS. No `SCRIPT (E)`.
- [x] Unstuck helper seen on DS (2026-10-12): two soldiers stuck on an LAV-25 turret teleported off. Setup menu Start war pressed by the owner (values applied, small layout, `humans moved=1`).
- [ ] Still open on DS: two players on opposite sides (SITREP isolation per side), late join after war start, setup menu "Start war" pressed by a human, battle-size teleport of players at war start.
- [ ] Before publish: `addon/Missions/CLW_Arland.conf` (`SCR_MissionHeader`) for `scenarioId` / host menu.
- [ ] Sidecar config path is hard-wired to the script folder: add a `--config` / env override (would also fix the fake-vs-openrouter test gotcha).
- Noise to look at later: every unit placement logs "via fallback"; one `ATTACK ... failed: waypoints lost` 40 s after start.

### M3 spike S0 (2026-10-12, Workbench builds #37-#39, branch `spike/m3-services`)
- [x] Script-placed vanilla services work in our mode (register ONLINE, faction, base); containers fill from script. Vanilla
  interaction layer (build menu, depot request actions) is Conflict-bound -> own actions + registry. Details: `notes/m3-design.md` §2a.
- [x] Arsenal kit cost works against nearby filled containers (owner: 5000 shown, drops when taking items). Range / base link: verify in phase B.
- [x] FIXED (branch `fix/spawn-activity-budget`, verified build #40, medium battle, real LLM): purchases failed `no members
  arrived` from ~90 active AI. Cause: the engine spawn queue drops NORMAL-importance requests above ~70 % of the global
  active-AI limit (128); purchases now spawn CRITICAL (+ 2 member retries, 60 s activity grace for new groups). Verified READY at
  109-123/128.
- [ ] Engine ceiling: at 128/128 even CRITICAL requests wait and get refunded (seen t=390 s+). Fix in progress: refuse purchases
  without engine headroom (no spend), `engineAI` in the situation so HQ stops buying.
- [x] Debug chat off by default (`debugChat` in `$profile:Clausewitz/overrides.json`), debug lines go to the dashboard "Debug log"
  tab; real LLM orders move units. Owner verified all green (2026-10-12, build #40).
- [ ] Fake LLM provider: units seemed not to move (owner, build #40); check the fake orders path.

## Next up
M3 phase R (action registry) coded 2026-10-12, not compiled or tested yet: `CLW_ActionRegistry` / `CLW_ActionsCore` / `CLW_Capabilities`, `Configs/CLW/Actions/Actions.conf`, protocol v3 (manifest POST + hash, sidecar prompt/validator from the manifest). Check first at compile: `string.ToType()` + `typename.Spawn()` in `CLW_ActionRegistry.Register`, `[Attribute] ref array<ref CLW_ActionParam>` conf load, `ref Managed m_Payload`. In the e2e: log `CLW: action registry: 7 of 7 entries`, sidecar log `manifest faction=...`, LLM orders still applied.
M3 phase B (bases + services + arsenal) coded 2026-10-12, not compiled or tested yet: `CLW_Services`, `CLW_SupplyLink`, `CLW_ArsenalPool`, service data in the faction profiles, setup switches `cap_logistics` / `cap_kitCost`. Check first in the e2e: services spawn + base link, container sync log lines (`CLW: services supply ...`), arsenal item counts.
M3 phase Q (player requests + reports, fast path) coded 2026-10-12, not compiled or tested yet: `CLW_RequestService` / `CLW_ActionsRequests` / `CLW_Reports` / `CLW_RequestPlayerController` / `CLW_RequestTerminal` + `Prefabs/CLW/CLW_RequestTerminal.et`, dashboard Requests tab, situation `requests[]`. Check first: `CLW: request entries registered`, `/request` list and a squad/vehicle/resupply grant in the log (`CLW: request #n ... GRANTED`), terminal actions visible next to the command post (`CLW: request terminal placed`), `/report 3 inf 300 NE` -> `CLW: report ... ACCEPTED` and a contact marker, an enemy symbol placed on the map -> `CLW: report marker`. Chat does not reach handlers in Workbench: test `/request` on the dedicated server, the terminal in Workbench.
M3 phase F (FOBs + build executor) coded 2026-10-12, not compiled or tested yet: `CLW_Fob` (state + `CLW_FobManager`), `CLW_FobSites`, `CLW_BuildExecutor`, `CLW_ActionsFob` (ESTABLISH_FOB / UPGRADE_FOB + doctrine), mode params `fobMax/fobBuild/fobCost/fobBuildSec`, `m_aFobSites` in Arland.conf, `fobs` reply channel in sidecar + dashboard markers. Check first at compile: `DamageManagerComponent.GetState()`, `ScriptInvoker.Insert(static method)`, `QueryEntitiesBySphere(presence.OnEntity)`. In the e2e: `CLW: fob site pool ready`, `CLW: fob FOB1 planned/truck assigned/build 25 pct/established`, respawn at the FOB, loss by holding the zone.
M3 "Logistics, bases & requests": draft design `notes/m3-design.md` (open decisions in its section 11), command/tool decision proposed in `notes/decision-command-tools.md`. Before or alongside: dedicated-server join test with one client (`notes/dedicated-server.md`).

## Log
- 2026-10-11: Vision and research committed; game-mode decision made; roadmap condensed to M0-M5; branch naming unified on `main`.
- 2026-10-11: Human participation made central (VISION 2a): soldiers and squad leaders get HQ orders as missions.  Players pick from several offered assignments.
- 2026-10-11: Player-request approach (fixed request commands vs tool registry) drafted in VISION 5a; decision still open.
- 2026-10-11: First playable world. Gotchas (no AIWorld in sub-scenes, Diag "Resources are leaking" assert, base-game GUID lookup) recorded in workspace skills and `_tools/guid-lookup.ps1`.
- 2026-10-11: Order loop verified in play (MOVE waypoint followed, waypoint cleared on arrival). Navmesh works. Earlier failed runs used stale scripts: after editing, run wb_reload and check the log for 'Reloading game scripts'.
- 2026-10-11: Commanding radial menu works for the player after adding `SCR_CommandingManagerComponent` to `CLW_GameMode.et` (without it the menu is empty and the game shows "commanding is only available to group leaders"). Build-stamp line (`CLW: build #N`) is printed at game start; `_tools/stamp-build.ps1` bumps it. Contacts, map markers, RuleCommander (MOVE/ATTACK/DEFEND) verified in play.
- 2026-10-11: Debug chat (`CLW_Debug.Say`, global, `ENABLED` switch), mission stand-in (human-led squads get a message + faction map marker instead of waypoints) and REST spike added. REST verified in log; chat visibility and mission marker on the map still need a look in game.
- 2026-10-11: Verified in game: debug chat visible to all; missions for human-led squads appear as vanilla tasks in the J menu with a map marker (SCR_TaskSystem.CreateTask with BaseTask.et, needs a real GroupID via SCR_GroupsManagerComponent.AssignGroupID; scene-placed squads have -1). The custom mission marker class was removed. REST spike verified (headers via SetHeaders "Name,Value,...").
- 2026-10-11: M1 LLM bridge verified end-to-end in fake mode: `CLW_RemoteCommander` POSTs the situation to `dev/tools/llm_sidecar.mjs` (127.0.0.1:8788, `node dev/tools/llm_sidecar.mjs`, CLW_LLM=fake|anthropic), parses/validates orders, applies via `CLW_RuleCommander.ApplyOrder`; falls back to rules on error/empty reply (log `fallback to rule commander`). `CLW_RemoteCommander.ENABLED` toggles it. Anthropic mode (ANTHROPIC_API_KEY) untested. Obsolete warnings left: SCR_JsonLoadContext/ImportFromString, RestCallback overrides.
- 2026-10-11: Read-only web panel added to the sidecar (VISION 12a basic): http://127.0.0.1:8788/ with HQ view (map, SITREP, exchange log, status strip) and Configuration view (provider fake|anthropic|openai-compatible, model, base URL, key, timeout, max tokens, enabled switch, Test connection). Files in `dev/tools/dashboard/`; settings persist to `dev/tools/sidecar-config.json` (git-ignored). Verified end-to-end (2026-10-12).
- 2026-10-12: Dynamic squad sweep: `CLW_SquadRegistry` (AIWorld.GetAIAgents -> parent groups, 5 s sweep) replaces the hardcoded squad names; commanders keep per-squad state by id (`CLW_SquadState`). Verified end-to-end (2026-10-12). See VISION 4b.
- 2026-10-12: Adaptive HQ wakeup (VISION 1b basic): game polls every 10 s (rule fallback throttled to 25 s), sidecar scheduler per faction (scheduled / trigger / manual wake, `next_check_sec` from the model, budget stretch), config groups `wakeup` and `triggers`, `POST /api/wake`, dashboard countdown chips + Wakeup/Triggers settings. Verified end-to-end (2026-10-12).

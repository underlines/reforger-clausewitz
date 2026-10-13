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

- [x] LATER (M3): human reports via map-marker parsing, `/report` chat command, quick menu (VISION 8)
- [x] DECIDED 2026-10-12: player-request approach (VISION 5a): both, layered (`notes/decision-command-tools.md`). GM functions checked: `research/m3-bases-fobs-requests.md` §4

- [x] Squad movement verified in the log (per-squad leader position + waypoint logging): AI-only squads walk their patrol/attack waypoints. A joining player becomes squad LEADER automatically (`playerLeader=1`; the `leaderId=-1` seen right after join was read too early), so the squad follows the player and HQ skips it: it needs the mission path (M2).
- [x] Verify later (needs 2 players or a way to join as non-leader): does an AI leader obey HQ waypoints while a player is a plain member? RuleCommander already skips only squads with a human LEADER

## Done: M2 - Real war + mechanized (v0.3.0)
Design: `notes/m2-design.md` (decided 2026-10-12). Branch `feature/m2-real-war`. Phases P1 war foundation, P2 forces +
mechanized, P3 commander v1 + protocol v2, P4 missions, S sidecar/dashboard; implemented code-only by subagents, one
Workbench e2e test at the end. LLM: fake + OpenRouter tested e2e; Anthropic provider deferred.

### M2 status (2026-10-12, branch `feature/m2-real-war`, build #27)
Implemented P1-P6 + S, then e2e-tested in Workbench (vanilla, RHS test project, WCS test project; real LLM via OpenRouter):
- [x] Setup menu (generated from mode params + faction pickers) looks good and starts the war; no AI before the war starts (scene squads removed, start forces from data).
- [x] Faction choice incl. RHS_USAF / RHS_AFRF / RHS_ION without dependency; WCS vehicles appear in vanilla US/USSR catalogs (auto-discovery); profiles load from .conf.
- [x] Control points capture, income, tickets (kills + bleed), mission offers in the J menu (refresh/expire), truck dismount + RTB, vehicles drive on both sides (crews re-affiliated to the buying faction), road-slot vehicle placement, player joins a side squad after the war start.
- [x] LLM (OpenRouter) commands in protocol v2: orders on objectives, buys with reasons, sees vehicle units; game REST timeout 30 s.
- [x] Known issues: RHS open-top UAZ469 refuses cargo seating (fallback: infantry walk, UAZ RTB); classifier: armed armoured cars (M1025 M2) come out `mech` (should be `light`), K-17 Berezok seat count 0 from UI info (needs probe for APCs); editor loadout manager gives only a rifleman per faction (vanilla had 6 roles: add them as data to `CLW_LoadoutManager.et`).
- [x] End of war + end screen confirmed by the owner (2026-10-12).
- [x] Not yet seen in play: mission accept/complete/reward popups, stuck watchdog dismount path, unstuck helper, DEFEND switch at 50 % strength, marker expiry.
- Test packs: junctions in `Documents\My Games\ArmaReforgerWorkbench\addons` (RHS 3, WCS 12, Clausewitz); see `dev/testpacks/README.md`.

### After the first e2e (2026-10-12, builds #28-#31), merged into main as v0.3.0 (local tag, not published)
- [x] Activity management (relevance-based AI LOD; agents must be lifted with SetLOD(max-1) before PreventMaxLOD): far units act without a human near.
- [x] Squad join: HQ squads registered as vanilla groups; deploy menu, respawn menu and groups menu verified; one join service (Establish/Release).
- [x] Vehicle unstuck escalation, server overrides file `$profile:Clausewitz/overrides.json`, classifier fixes, vanilla role loadouts, ticket HUD, dashboard side order + identity colours.
- [x] LLM/rules flip-flop fixed (health scale in the prompt; rules don't override fresh LLM orders).
- [x] Open: mission completion/reward not yet watched; RHS open-top UAZ seating; harmless vanilla UI exception when the deploy menu closes during a squad list update; dedicated-server checks (group mirror RPC, remote commanding, SetIsPlayable replication).

### UX + SITREP batch -> v0.4.0 (2026-10-12, branch `feature/ux-sitrep`, builds #33-#35)
Implemented by subagents (code only), then one Workbench e2e (vanilla, OpenRouter on, small battle). Designs: `notes/sitrep-design.md`, `research/sitrep-and-briefing.md`.
- [x] Battle-size layout (small): HQs 956 m apart (authored 1911 m), D/E dropped, spawn point and setup-phase player moved, markers rebuilt. Objective spacing fix verified: every `points layout check` ok, closest pair 248 m (>= 220 m). The "F 211 m from HQ" seen in build #34 was a log bug (distance of the pre-snap wish).
- [x] SITREP: rules version at T+0, LLM version at ~15 s, delivered to the own side only (`CLW: sitrep deliver / client received`); new prose style for rules and LLM (sizes/times in words, Intel title, place names, no ids/coordinates). Contact counts fixed (live targets, vehicles vs infantry), seen as 1 and 7 infantry.
- [x] Dashboard: compare tables, SITREP cards, missions page (empty for sides without player-led squads, now explained), map dormant shading.
- [x] H re-show confirmed by the owner (2026-10-12).
- [x] Not yet seen: 100 m grid `025-285` (changed after the run, not compiled yet), `/sitrep` in game, journal line breaks, no-sidecar run, plausibility of `dormant`, medium layout unchanged, dedicated server (SITREP per side, player teleport at war start).
- Plan only: loadouts/arsenals (`notes/loadouts-arsenal-plan.md`, reviewed against `research/mod-agnostic-asset-discovery.md`). Vision added: in-game friendly map picture (VISION 8a), per-player-squad briefings (4), dashboard accounts + soldier tablet view (12a).
- Known: units take 30-60 s to move at start (first tick 10 s, LLM answer up to 30 s, rules only on error); proposed: one rule-commander pass at war start, LLM overrides it (`LLM_PRIORITY_SEC`). Owner decision 2026-10-12: leave as is, no fix. LLM intel sometimes states more than the contacts show (owner: leave as is).
- Watch: `dev/tools/sidecar-config.json` selects openrouter and `CLW_LLM=fake` does not override it; local tests must use a temp config.

### Dedicated server smoke test (2026-10-12, build #36, no players)
Local DS in `D:\GitHub\ReforgerMods\_dedicated\` (junction to `addon/`, no pack needed); findings and join guide: `notes/dedicated-server.md`.
- [x] Boots in ~25 s, scripts compile (no `SCRIPT (E)`), game mode + scenario + bases/points + setup phase, auto start after 180 s, war RUNNING with 141 AI / 8 vehicles at 60 FPS, sidecar (fake) 5 requests 0 errors, LLM orders and SITREPs applied.
- [x] With one joined client (owner, 2026-10-12, `_dedicated/start-client.ps1`): connect + faction join, setup menu sent to the owner (war then auto-started at 180 s), squad join (group id 1003, player leader, 3 AI moved), mission offers in the J menu, AI commanding (radial menu), groups menu (group mirror / registration replicate), ticket HUD, SITREP rules v2 + LLM v3/v4 delivered to the US player only; sidecar (OpenRouter) driven by the DS. No `SCRIPT (E)`.
- [x] Unstuck helper seen on DS (2026-10-12): two soldiers stuck on an LAV-25 turret teleported off. Setup menu Start war pressed by the owner (values applied, small layout, `humans moved=1`).
- [x] Still open on DS: two players on opposite sides (SITREP isolation per side), late join after war start, setup menu "Start war" pressed by a human, battle-size teleport of players at war start.
- [ ] Before publish: `addon/Missions/CLW_Arland.conf` (`SCR_MissionHeader`) for `scenarioId` / host menu.
- [x] Sidecar config path is hard-wired to the script folder: add a `--config` / env override (would also fix the fake-vs-openrouter test gotcha).
- Noise to look at later: every unit placement logs "via fallback"; one `ATTACK ... failed: waypoints lost` 40 s after start.

### M3 spike S0 (2026-10-12, Workbench builds #37-#39, branch `spike/m3-services`)
- [x] Script-placed vanilla services work in our mode (register ONLINE, faction, base); containers fill from script. Vanilla
  interaction layer (build menu, depot request actions) is Conflict-bound -> own actions + registry. Details: `notes/m3-design.md` §2a.
- [x] Arsenal kit cost works against nearby filled containers (owner: 5000 shown, drops when taking items). Range / base link: verify in phase B.
- [x] FIXED (branch `fix/spawn-activity-budget`, verified build #40, medium battle, real LLM): purchases failed `no members
  arrived` from ~90 active AI. Cause: the engine spawn queue drops NORMAL-importance requests above ~70 % of the global
  active-AI limit (128); purchases now spawn CRITICAL (+ 2 member retries, 60 s activity grace for new groups). Verified READY at
  109-123/128.
- [x] Engine ceiling: purchases without engine headroom are refused before spending (`refused: engine AI limit`), `engineAI` in the
  situation. Verified 2026-10-12 (build #41, medium, 12 min, 17 READY, 0 refunds at ~87/128). On the DS the engine sits at 119-122/128
  in medium battles: our caps (2x45) do not count everything (vehicle crews?), the gate catches it; root cause still open.
- [x] Debug chat off by default (`debugChat` in `$profile:Clausewitz/overrides.json`), debug lines go to the dashboard "Debug log"
  tab; real LLM orders move units. Owner verified all green (2026-10-12, build #40).
- [x] Fake LLM provider: units seemed not to move (owner, build #40); check the fake orders path.

## Current milestone: M4 - Any map, any pack (released as v1.0.0)
**Next: round 3, see `notes/m4-round3-plan.md` (handover: owner test results, WP0-WP8, checklist). Work on a new branch from `main`.**
Design: `notes/m4-design.md` (2026-10-12). Branch `feature/m4-any-map` merged into `main` and tagged v1.0.0 (2026-10-12; the
owner client checklist below passed). Built by six
subagents (WP1 maps + auto scenario, WP2 wrapper worlds + generator, WP3 side pools + props, WP4 HQ menu, WP5 FOB/convoys, WP6 GM spike).
Verified on the dedicated server, unattended (builds #67-#71, packs RHS + WCS + GRS + WCS Loadout Editor, setup preset RHS_USAF vs RHS_AFRF):
- [x] setup preset auto start (no human), invalid preset never replaced by defaults
- [x] Arland random start: HQ pair in 2-10 attempts (was 60 of 60 rejected); Arland fixed start unchanged
- [x] Everon (`CLW_Eden`) and Kolguyev (`CLW_Cain`) wrapper worlds boot, auto scenario fills HQs + 7 objectives (map places), AI moves on the campaign navmesh, war runs without script errors
- [x] side pools: RHS sides get the kin supply truck (US M923A1 / USSR Ural), RHS_AFRF also recon + BRDM; crewless K-17/K-4386 caught at catalog build
- [x] FOB1 built by a walking builder (forced move, 45 s to site), first RHS convoy delivered 279 supplies HQ -> FOB1
- [x] map place positions on the DS (were all 0,0); arsenal pool lookup self-test ok for every faction
Second round (2026-10-12 evening, builds #72-#79, same packs + preset), verified on the DS without a player:
- [x] GM takeover (option C): `-server worlds/GameMaster/GM_Eden.ent ... -clwTakeover`: 24 of 24 takeover steps ok (spawn logic swapped to `CLW_SpawnLogic` at runtime, chat present, 2 base spawn points), auto scenario on Everon, war RUNNING, AI moves 150-200 m in 2 min on the GM navmesh; `GM_Arland` picks `Arland.conf` (by world name); without the switch a GM world stays vanilla (one log line). Lesson: a modded `SCR_MenuSpawnLogic` made every vanilla prefab lose its spawn logic ("Unknown class"), replaced by the runtime swap
- [x] native worlds after the war-runtime refactor: Arland (RHS preset) war RUNNING, no script errors
- [x] start ownership "nearest" at war start: Takistan / Everon / Arland random: 1 per side, the outermost kept objective on each side, rest neutral
- [x] generated map wrapper (Takistan, RHS sides): boots, auto scenario, war RUNNING, AI moves (100-150 m in 2 min); bug found + fixed: the generated AIWorld prefab was unregistered (no navmesh), wrappers now reference only registered resources (checked by the generator)
- [x] RHS prop sets: RHS arsenal boxes at the bases of RHS_USAF / RHS_AFRF; their zero buy multiplier is reset to 1 (`buy multiplier 0 -> 1` in the log)

## Round 3 (branch `feature/m4-round3` from `main`, plan `notes/m4-round3-plan.md`)
Owner test 2026-10-12 evening (GM Everon takeover, RHS_USAF vs RHS_AFRF, builds #80-#81): HQ menu, `/request vehicle` refusal, `/report`, arsenal prices + WCS
weapons, own-side markers, start objectives, garrison, tighter base, LLM FOB after the 6 min gate built by a walking builder, convoys to FOBs (FOB arsenal on
FOB-local stock: kept) all OK. Problems P1-P14 in the plan, section 3.
Open (work packages, plan section 4):
- [ ] WP1 readable squad names (`1st Rifle Squad`, `HQ Garrison`, ...) everywhere + own supplies on the HUD (P1, P8)
- [ ] WP2 base 30 % tighter (depot distance kept), no medical service anywhere, depot lists the side pool, supply check 10 s after spawn, kit purchase log (P2, P3, P4, P7, P11)
- [ ] WP3 FOB base marker with state + builder/convoy arrows in game and on the dashboard; marker restore only on a real local faction change (P5, P14)
- [ ] WP4 rematch in place after WAR OVER (no world reload, no `JIP_ERROR`) (P6)
- [ ] WP5 simple HQ war journal (intent, goals, lessons, code-counted tries; review call; dashboard view + toggle) + flank nudge (P12, P13)
- [ ] WP6 economy guard: HQ reserve floor, convoy minimum load, no buys at the AI cap, idle stand-down check log (P9, P10)
- [ ] WP7 merge + DS check; WP8 modern vehicle packs test
Owner checklist round 3 (client): 1 group names readable, garrison `HQ Garrison`; 2 HUD own supplies; 3 base: spawn tent touches the command post,
arsenal next to them, no medical tent; 4 depot lists side-pool vehicles; 5 FOB planned/building/done on the in-game map + dashboard with builder and
convoy arrows; 6 victory -> results -> back to setup, no kick; 7 dashboard journal + memory toggle, HQ keeps its intent, no repeated failed attacks,
flanks; 8 HQ never drained to 0, convoys still run; 9 (WP8) modern vehicles at the depot and in AI use.

## Owner checklist (client; everything the DS cannot show): all OK (owner, 2026-10-12); results and feedback in `notes/m4-round3-plan.md` §3
Answer per item: OK / not OK + what you saw + about when. Start: sidecar, then `_dedicated\start-server.ps1 -Packs packs-e2e.txt` (Arland) unless the item says otherwise, client via `_dedicated\start-client.ps1` (or the command in `notes/dedicated-server.md`). The preset auto-starts the war 60 s after boot (RHS_USAF vs RHS_AFRF, random start): join within a minute to see the setup menu.
From M4 round 1 (never clicked):
- [x] 1. HQ menu: "Open HQ menu" on a request terminal and `/hqmenu`; every section (Requests, Squad join/leave, Base, Contact report); layout/readability
- [x] 2. Vehicle request: "vehicle delivered, grid ..." hint + temporary map marker that disappears when you get in
- [x] 3. Arsenal: WCS SPEAR and other pool items show a price, can be taken, cost supplies; GRS items in the Loadout Editor
- [x] 4. `/report`, map-marker contacts (place an enemy symbol on the map), FOB respawn (deploy at a FOB)
- [x] 5. Everon / Kolguyev wrapper with a client: `-World Worlds/CLW_Eden.ent` / `Worlds/CLW_Cain.ent`: spawn at your HQ, not in the sea; place names on the map/reports
New in round 2:
- [x] 6. RHS arsenal box at your HQ (RHS look): it lists RHS items and a kit COSTS supplies (kit cost on); the FOB kit box too once a FOB stands
- [x] 7. Start objectives: on the map at war start your side owns exactly the objective nearest your HQ, the enemy the one nearest theirs, the rest neutral. Setup menu shows "Start objectives" (scenario/none/1/2/3); try "2" once
- [x] 8. GM takeover with a client: `start-server.ps1 -World worlds/GameMaster/GM_Eden.ent -Packs packs-e2e.txt -Extra "-clwTakeover"`. Check: setup menu / deploy (you get the CLW sides, spawn at your HQ), HQ squads join, SITREP, map markers, tickets HUD, `/hqmenu`, the arsenal. Note anything Game-Master-like that gets in the way (editor key Y, voting, GM camera). Also `GM_Arland` (authored Arland objectives)
- [x] 9. Generated Takistan wrapper with a client: `start-server.ps1 -World Worlds/CLW_Takistan.ent -Packs packs-takistan-rhs.txt` (client: `start-client.ps1 -Packs packs-takistan-rhs.txt`, same packs as the server; for every item the client takes the server's packs file). Spawn, map, objectives on sensible ground, AI moving
Open / known:
- [ ] GM takeover: the swapped `CLW_SpawnLogic` uses default attributes (disconnect behaviour, deploy delay); GM persistence not covered (only the `-server <world>` route is possible while unpublished)
- [x] "waypoints lost" on some infantry orders on Takistan (4-6 per run, others move); watch on a long run
- [ ] RHS: no own compositions for command post / supply / depot / medical / sandbags (RHS ships none, vanilla used); no armor role (no kin tank either)
- [ ] WCS_Armament AI magazine switch throws VM exceptions (third-party, vanilla null weapMgr)

## Planned: i18n (design `notes/i18n-design.md`, 2026-10-13)
Mod text and dashboard EN + DE; LLM text in each player's language (game language, override via `/hq language <code>` or HQ
menu); strong LLM for decisions and one-reader replies, optional weak LLM for broadcast translation.
- [x] P1 player language (coded + DS compile OK; client e2e pending): resolution (override > game language > server default > en), `/hq language`, HQ menu option, join report
- [x] P2 (coded; string table registered via Workbench; German render pending client check) `CLW_Loc` + `Language/CLW.st` (EN/DE); static UI and server-built templates on `#CLW-` keys
- [x] P3 protocol v4 (real LLM: ja reply + setLanguage verified 2026-10-13, ling-3.1-flash) `lang`: `/hq` replies in the asker's language
- [x] P4 sidecar `strong`/`weak` slots, broadcast translation (`langs[]` -> `text_by_lang`), cache, fallbacks
- [x] P5 dashboard EN/DE: `i18n/*.json`, `t()` + `data-i18n`, header picker (independent of P1-P4)

## Done: M3 - Logistics, bases & requests (v0.5.0)
M3 "Logistics, bases & requests" coded 2026-10-12 on branch `feature/m3-logistics` (phases R, B, Q, F, L, A; design `notes/m3-design.md`).
All phases compile (Workbench builds #42-#44). Runtime: dedicated server runs without a client (war auto-starts after 180 s), builds #44-#51.
Verified on the DS (no human needed):
- [x] action registry 9 of 9 entries, request + logistics entries registered, manifest v3 per faction reaches the sidecar (intents 11, primitives 5, capabilities services/fob/logistics/requests/reports/kitCost)
- [x] services: 5 compositions per side around each HQ (command post, supply cache, arsenal, vehicle depot, field hospital), HQ spawn point of the composition disabled, request terminals placed and bound to their side
- [x] supply link: ledger -> base containers and back (fixed: virtual containers such as HQ storage are views over other crates and are skipped; `CLW: services supply link ... active ... ledger 220 -> containers 220`)
- [x] FOB site pool (data + computed, 250 m from objectives), LLM and rule doctrine both issue ESTABLISH_FOB, cost paid/refunded, builder fallback to walking infantry when no purchase is possible
- [x] fixed: `string.Substring` returns at most 8191 chars, the >8 KB situation was cut (sidecar HTTP 400, USSR fell back to rules): `CLW_Str` chunked helpers
- [x] fixed: FOB/logistics entries are applied (game) and validated (sidecar) before buys of the same reply
Verified with the owner on the DS + client (2026-10-12, builds #52-#60, RHS_USAF vs RHS_AFRF, packs RHS + WCS gear + WCS_Arsenal + WCS Loadout Editor):
- [x] setup on pack factions: running clients switch faction and get the new sides (no rejoin), spawn with RHS loadouts
- [x] arsenal + WCS Loadout Editor show RHS items (WCS gear via WCS_Arsenal in the US/USSR catalogs, kin pool to the RHS sides)
- [x] chat commands (world had no chat entity), `/request vehicle` granted, `/hq` agent turn: reply + action (armoured vehicle) in ~16 s, toast shown
- [x] collision/road-aware placement of HQ services, own-side markers (HQ, FOB, command post, arsenal, depot, supply), dashboard map aligned + structures layer
- [x] FOB established by a walking builder (FOB2, 2 min) with own markers
Open:
- [x] FOB builders often fail to reach far sites (AI keeps the previous objective / combat); several FOBs refunded after 3 stalls
- [x] no supply truck for pack factions (RHS): no convoys -> side pools (VISION 10a) or kin fallback for missing roles
- [x] GRS: local packs outdated (Workshop has 1.8 versions); client crashed with the old set. Retest after updating (`_dedicated/packs-e2e-gear.txt`)
- [x] not yet checked by the owner: `/report`, map-marker contacts, vehicle request far from a depot (roadside), FOB respawn, convoys, kit cost off
- [x] LLM timeouts with the bigger prompt; engine AI ~120/128 although caps sum to 90 (the headroom gate catches it)

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
- 2026-10-12: M4 (any map, any pack: auto scenario, wrapper worlds + generator, GM takeover, side pools, prop sets, HQ menu, FOB kit + timing gate, garrison) merged into `main` and released as v1.0.0 (local tag). Round 3 (`notes/m4-round3-plan.md`) and the owner client checklist are next.
- 2026-10-13: i18n designed (`notes/i18n-design.md`): EN + DE stringtable and dashboard, LLM text per player language with override, strong/weak LLM split for broadcast translation. Not started.

# Clausewitz - Progress

Updated: 2026-10-11. Roadmap: `VISION.md` section 13. Decisions: `notes/`.

## Current milestone: M0 - Skeleton

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
- [x] RuleCommander v0 (`CLW_RuleCommander`): verified in play, patrol MOVE with no contacts, ATTACK (search-and-destroy) on nearest contact; DEFEND untested. Second AI-only squad per side added. Game Master: `CLW_EditorSettings` (SCR_EditorSettingsEntity, unlimited editor legal); hold Y 0.5 s opens it (EditorToggle)

- [ ] LATER (M3): human reports via map-marker parsing, `/report` chat command, quick menu (VISION 8)
- [ ] OPEN: decide player-request approach (VISION 5a): (A) fixed request commands vs (B) tool registry; evaluate both, prototype A after M2, check scriptable Game Master functions

- [x] Squad movement verified in the log (per-squad leader position + waypoint logging): AI-only squads walk their patrol/attack waypoints. A joining player becomes squad LEADER automatically (`playerLeader=1`; the `leaderId=-1` seen right after join was read too early), so the squad follows the player and HQ skips it: it needs the mission path (M2).
- [ ] Verify later (needs 2 players or a way to join as non-leader): does an AI leader obey HQ waypoints while a player is a plain member? RuleCommander already skips only squads with a human LEADER

## Next up
Test DEFEND and marker expiry; then REST spike (M1 risk 1). A squad containing a player follows the player, so HQ orders go to AI-only squads or become missions for the player-led squad (VISION 2a, M2).

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

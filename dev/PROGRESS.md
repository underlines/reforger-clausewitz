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
- [x] Player spawns as US soldier (SCR_AutoSpawnLogic, m_sForcedFaction "US") and joins the AI rifle squad: needs `m_bPlayable 1` on the scene-placed SCR_AIGroup plus `SCR_GroupsManagerComponent.RegisterGroup(group)` (GroupID is -1 until then), then `AddPlayerToGroup`. Verified: `player 1 in squad=1`; the AI squad then follows the player in formation. Open: player is not made leader (leaderId=-1), AI-leader hand-over and USSR-side join not tested
- [ ] Spike: REST GET/POST/headers to local echo server (risk 1)
- [x] Spike: perception tap, per-group enemy clusters -> `Contact` record (`CLW_Contact.FromGroup`, reads `SCR_AIGroupUtilityComponent.m_Perception.m_aTargetClusters`): verified, US squad reported the USSR squad at its exact position (400 m apart, player walked within ~100-150 m). Without the player, squads 400 m apart did not detect each other
- [x] Spike: order loop, canned MOVE -> waypoint on a group: verified, US leader walked 150 m east in <60 s on the Arland navmesh (CLW_GameMode.SpikeOrderLoop)
- [x] Contact book (merge 60 m, expire 120 s) + faction map markers (`CLW_ContactBook`, `CLW_ContactMarkers`): contacts verified in the log; marker visibility on the map NOT yet confirmed by eye
- [x] RuleCommander v0 (`CLW_RuleCommander`): verified in play, patrol MOVE with no contacts, ATTACK (search-and-destroy) on nearest contact; DEFEND untested. Second AI-only squad per side added. Game Master: `CLW_EditorSettings` (SCR_EditorSettingsEntity, unlimited editor legal); hold Y 0.5 s opens it (EditorToggle)

- [ ] LATER (M3): human reports via map-marker parsing, `/report` chat command, quick menu (VISION 8)
- [ ] OPEN: decide player-request approach (VISION 5a): (A) fixed request commands vs (B) tool registry; evaluate both, prototype A after M2, check scriptable Game Master functions

## Next up
RuleCommander v0 (step 4): consume Contact records, issue MOVE/ATTACK/DEFEND to the AI-led squad. Note: a squad containing a player follows the player, so HQ orders must go to AI-only squads or become missions for the player-led squad (VISION 2a). Also: REST spike.

## Log
- 2026-10-11: Vision and research committed; game-mode decision made; roadmap condensed to M0-M5; branch naming unified on `main`.
- 2026-10-11: Human participation made central (VISION 2a): soldiers and squad leaders get HQ orders as missions.  Players pick from several offered assignments.
- 2026-10-11: Player-request approach (fixed request commands vs tool registry) drafted in VISION 5a; decision still open.
- 2026-10-11: First playable world. Gotchas (no AIWorld in sub-scenes, Diag "Resources are leaking" assert, base-game GUID lookup) recorded in workspace skills and `_tools/guid-lookup.ps1`.
- 2026-10-11: Order loop verified in play (MOVE waypoint followed, waypoint cleared on arrival). Navmesh works. Earlier failed runs used stale scripts: after editing, run wb_reload and check the log for 'Reloading game scripts'.

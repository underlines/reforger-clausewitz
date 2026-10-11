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
- [ ] Player can join a faction, spawn as soldier, join an AI squad (section 2a); verify which base-game spawn/loadout/group components are reusable
- [ ] Spike: REST GET/POST/headers to local echo server (risk 1)
- [ ] Spike: perception tap, per-group enemy clusters -> `Contact` record
- [x] Spike: order loop, canned MOVE -> waypoint on a group: verified, US leader walked 150 m east in <60 s on the Arland navmesh (CLW_GameMode.SpikeOrderLoop)
- [ ] RuleCommander v0: one squad per side, MOVE/ATTACK/DEFEND

- [ ] OPEN: decide player-request approach (VISION 5a): (A) fixed request commands vs (B) tool registry; evaluate both, prototype A after M2, check scriptable Game Master functions

## Next up
CLW_GameMode skeleton, then the perception and order spikes.

## Log
- 2026-10-11: Vision and research committed; game-mode decision made; roadmap condensed to M0-M5; branch naming unified on `main`.
- 2026-10-11: Human participation made central (VISION 2a): soldiers and squad leaders get HQ orders as missions.  Players pick from several offered assignments.
- 2026-10-11: Player-request approach (fixed request commands vs tool registry) drafted in VISION 5a; decision still open.
- 2026-10-11: First playable world. Gotchas (no AIWorld in sub-scenes, Diag "Resources are leaking" assert, base-game GUID lookup) recorded in workspace skills and `_tools/guid-lookup.ps1`.
- 2026-10-11: Order loop verified in play (MOVE waypoint followed, waypoint cleared on arrival). Navmesh works. Earlier failed runs used stale scripts: after editing, run wb_reload and check the log for 'Reloading game scripts'.

# Clausewitz - Progress

Updated: 2026-10-11. Roadmap: `VISION.md` section 13. Decisions: `notes/`.

## Current milestone: M0 - Skeleton

- [x] Vision and feasibility research
- [x] Empty addon (`addon/addon.gproj`, GUID `C1A05E7B3D9F4A28`)
- [x] Decision: own game mode instead of Conflict (`notes/decision-game-mode.md`)
- [x] Branch `main` in workspace and mod repo; MCP project path -> `Clausewitz/addon`
- [x] MCP verified: `project browse` resolves to Clausewitz/addon
- [ ] `CLW_GameMode` + minimal world (two factions, spawn points), compiles and Plays
- [ ] Spike: REST GET/POST/headers to local echo server (risk 1)
- [ ] Spike: perception tap, per-group enemy clusters -> `Contact` record
- [ ] Spike: order loop, canned MOVE -> waypoint on a group
- [ ] RuleCommander v0: one squad per side, MOVE/ATTACK/DEFEND

## Next up
CLW_GameMode skeleton, then the perception and order spikes.

## Log
- 2026-10-11: Vision and research committed; game-mode decision made; roadmap condensed to M0-M5; branch naming unified on `main`.

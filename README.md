# Clausewitz

AI High Command for Arma Reforger (built-in rule-based commander, optional LLM): AI vs AI wars with real fog of war, SITREPs, chain of command, resources and logistics, for infantry and mechanized forces, with support for custom factions (RHS, WCS, ...). Human players fight on either side as soldiers or squad leaders inside the same chain of command; HQ offers them missions.

## Features (v0.4.0)
- Own game mode `CLW_GameMode` (not Conflict) with a setup menu for the first player: factions for side A/B (any playable faction, incl. mod packs such as RHS), war mode, battle size and mode parameters.
- War: control points, tickets + bleed, supplies + income, bases and respawn, end of war (tickets, hold all, time limit) with end screen, ticket HUD.
- Forces as data: faction profiles (`Configs/CLW/Factions/`) plus auto-discovery from faction catalogs; infantry, motorized and mechanized units (mount, dismount, overwatch, RTB, stuck watchdog and unstuck); force purchases with caps.
- HQ commanders: rule commander (default and fallback) and an LLM commander through a local Node sidecar (protocol v2, OpenRouter tested); adaptive wakeup and triggers.
- Humans: join HQ squads from the deploy/groups menu (`CLW_PlayerSquad`), mission offers for player squad leaders (J menu, popups, rewards), faction SITREP briefings (journal, H, `/sitrep`).
- Fog of war: per-faction contact book from AI perception, enemy contact markers on the map.
- Activity management (relevance-based AI LOD) so far units keep fighting without a player nearby.
- Web dashboard served by the sidecar: HQ map, forces, compare tables, SITREPs, missions, exchange log, configuration.

## Layout
- `addon/` - the Reforger addon (`addon.gproj`, GUID `C1A05E7B3D9F4A28`). Only this is packaged and published.
- `dev/` - never shipped.
  - `VISION.md` - vision, scope, features, roadmap (section 13)
  - `PROGRESS.md` - live milestone checklist and log
  - `notes/` - design decisions (`m2-design.md`, `sitrep-design.md`, `squad-join-design.md`, `loadouts-arsenal-plan.md`, ...)
  - `research/` - API and content research
  - `tools/` - LLM sidecar (`node dev/tools/llm_sidecar.mjs`) and dashboard (http://127.0.0.1:8788/)
  - `testpacks/` - never-shipped test projects that load Clausewitz with RHS / WCS without a dependency
- `CHANGELOG.md`, `VERSION` - releases via `_tools/release.ps1` (workspace)

## Status
v0.4.0 (local tag, not published). M0 skeleton, M1 LLM bridge, M1.5 dashboard, M2 real war + mechanized and the UX + SITREP batch are done and tested in Workbench (vanilla, RHS and WCS test projects). Next: dedicated-server smoke test, then M3 (logistics, FOBs, bases and player requests); see `dev/PROGRESS.md`.

## Dependencies
None (base game only). RHS / WCS factions and vehicles work when loaded alongside, without a hard dependency.

## Publish
Not yet.

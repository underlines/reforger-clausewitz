# Clausewitz

AI High Command for Arma Reforger (built-in rule-based commander, optional LLM): AI vs AI wars with real fog of war, SITREPs, chain of command, resources and logistics, for infantry and mechanized forces, on any map and with custom factions (RHS, WCS, ...). Human players fight on either side as soldiers or squad leaders inside the same chain of command; HQ offers them missions and takes their requests.

## Features (v1.0.0)
- Own game mode `CLW_GameMode` (not Conflict) with a setup menu for the first player: factions for side A/B (any playable faction, incl. mod packs such as RHS), war mode, battle size, start positions (fixed or random), start objectives and mode parameters. A server setup preset in `overrides.json` starts the war unattended.
- Any map: scenario per world, else an auto scenario built at game start (HQs from a random-layout search, objectives from named map places). Wrapper worlds for Everon and Kolguyev, a generator for wrappers of map mods, and an opt-in Game Master takeover (`-clwTakeover`) that runs the war inside a vanilla GM world.
- War: control points, tickets + bleed, supplies + income, bases and respawn, end of war (tickets, hold all, time limit) with end screen, ticket HUD.
- Forces as data: faction profiles (`Configs/CLW/Factions/`) plus auto-discovery from faction catalogs; side pools fill missing roles (supply trucks, ...) from kin factions; infantry, motorized and mechanized units (mount, dismount, overwatch, RTB, stuck watchdog and unstuck); force purchases with caps and orders; HQ garrison, idle stand-down.
- HQ commanders: rule commander (default and fallback) and an LLM commander through a local Node sidecar (protocol v3 with an action manifest, OpenRouter tested); action registry behind every command; adaptive wakeup and triggers.
- Logistics and bases: base service compositions (command post, supply, arsenal, vehicle depot), supply link, arsenal kin pool and pack rules, prop style sets per pack, FOBs (timing gate, builders, kit, respawn), per-FOB stock and supply convoys.
- Humans: join HQ squads from the deploy/groups menu (`CLW_PlayerSquad`), mission offers for player squad leaders (J menu), faction SITREP briefings (journal, H, `/sitrep`), requests and reports (`/request`, `/report`, request terminals, HQ menu `/hqmenu`, `/hq` free-text radio chat via the LLM agent), own-side map markers.
- Fog of war: per-faction contact book from AI perception and player reports, enemy contact markers on the map.
- Activity management (relevance-based AI LOD) so far units keep fighting without a player nearby.
- Web dashboard served by the sidecar: HQ map (structures, FOBs, convoys), forces, compare tables, SITREPs, missions, requests, HQ chat, debug log, exchange log, configuration.

## Layout
- `addon/` - the Reforger addon (`addon.gproj`, GUID `C1A05E7B3D9F4A28`). Only this is packaged and published.
- `dev/` - never shipped.
  - `VISION.md` - vision, scope, features, roadmap (section 13)
  - `PROGRESS.md` - live milestone checklist and log
  - `notes/` - design decisions (`m2-design.md`, `m3-design.md`, `m4-design.md`, `m4-round3-plan.md`, `overrides.md`, `maps.md`, `dedicated-server.md`, ...)
  - `research/` - API and content research
  - `tools/` - LLM sidecar (`node dev/tools/llm_sidecar.mjs`, `npm install` once for the HQ chat agent), dashboard (http://127.0.0.1:8788/), map wrapper generator
  - `testpacks/` - never-shipped test projects that load Clausewitz with RHS / WCS without a dependency
- `CHANGELOG.md`, `VERSION` - releases via `_tools/release.ps1` (workspace)

## Status
v1.0.0 (local tag, not published). M0 skeleton, M1 LLM bridge, M1.5 dashboard, M2 real war + mechanized, UX + SITREP, M3 logistics/bases/requests and M4 any map/any pack are done; M4 verified on the local dedicated server (vanilla, RHS, WCS, GRS packs; Arland, Everon, Kolguyev, Takistan, GM takeover). Open: the owner client checklist for M4 and the round 3 polish (`dev/notes/m4-round3-plan.md`); see `dev/PROGRESS.md`.

## Dependencies
None (base game only). RHS / WCS / GRS factions, vehicles and gear work when loaded alongside, without a hard dependency.

## Publish
Not yet.

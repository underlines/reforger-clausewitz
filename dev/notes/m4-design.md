# M4 - Any map, any pack (design, 2026-10-12)

Roadmap row: VISION section 13. Branch `feature/m4-any-map`. Implemented by subagents (one per work package, code only), e2e on the
dedicated server at the end (`reforger-workbench-test`, section "Dedicated server"). Live status: `PROGRESS.md`.

Goal: Clausewitz runs on Everon, Arland, Kolguyev and any map mod, with any faction packs, without hand-written scenario data,
and the pack factions get complete forces (supply trucks, crews), bases and FOBs.

## Shared rules for every work package
- Read first: `CLAUDE.md` (workspace), `.claude/skills/reforger-script-dev/SKILL.md`, `dev/VISION.md` sections 6a, 10, 10a, 13,
  and the header comments of every file you touch. Match the surrounding style: header comment per file, `CLW_` prefix, tabs,
  log lines `CLW: <area> ...`, server-only code guarded with `Replication.IsServer()`.
- No vanilla hard-wiring in code (prefabs, faction keys, coordinates, seat counts): content lives in `.conf` data or is discovered at runtime.
- Enforce gotchas: `string.Substring` returns at most 8191 chars (`CLW_Str` has chunked helpers); no field initializers on `[Attribute]`
  members; `Managed` already has `ToString()`; prefer `while` loops over `for (;...)`; check the API with the enfusion-mcp tools
  (`api_search`, `game_read`, `wiki_search`) instead of guessing, mark unverified API calls `// [verify]`.
- NO Workbench, no `wb_*` tools, no dedicated server, no game client, no e2e runs. Static checks (`script_lint`, `script_analyze`) are fine.
- NO git commands, no edits to `CHANGELOG.md`, `dev/PROGRESS.md`, `CLW_Build.c`, `dev/.build-counter`: the coordinator does those.
  End your report with proposed CHANGELOG lines (Added / Changed / Fixed).
- Only edit the files your package owns (table below). Need a change elsewhere? Use the existing public API, or describe the needed
  change in your report instead of making it. New files are fine (name them after your package).
- Heavy logging on every decision path, so one e2e run can diagnose it. New config goes into data (`.conf` classes with `[Attribute]`,
  or `overrides.json` keys documented in `dev/notes/overrides.md`).

## File ownership
| WP | owns (edit) | new files |
|---|---|---|
| 1 Maps + auto scenario | `CLW_GameMode.c`, `CLW_ScenarioConfig.c`, `CLW_RandomLayout.c`, `CLW_Places.c`, `CLW_FobSites.c`, `CLW_Bases.c`, `Configs/CLW/Scenarios/*` | `CLW_AutoScenario.c`, `Configs/CLW/Scenarios/_Auto.conf` |
| 2 Wrapper worlds + generator | `Worlds/*`, `Prefabs/AI/*`, `Prefabs/GameModes/*` | `Worlds/CLW_Eden.ent`, `Worlds/CLW_Cain.ent` (+ layers, metas), `Prefabs/AI/CLW_AIWorld_Eden.et`, `..._Cain.et`, `dev/tools/new-map-wrapper.mjs`, `dev/notes/maps.md` |
| 3 Side pools + props + overrides | `CLW_ForceCatalog.c`, `CLW_ForceSpawner.c`, `CLW_FactionProfile.c`, `CLW_PropResolver.c`, `CLW_Services.c`, `CLW_CatalogOverrides.c`, `dev/notes/overrides.md`, `Configs/CLW/Factions/*` | `CLW_SidePool.c`, `Configs/CLW/Props/*` |
| 4 HQ menu + delivery feedback | `CLW_RequestTerminal.c`, `CLW_RequestService.c`, `CLW_RequestPlayerController.c`, `Prefabs/CLW/CLW_RequestTerminal.et` | `CLW_HqMenu*.c`, `UI/layouts/CLW/HqMenu*.layout`, `Configs/CLW/UI/HqMenu*.conf` |
| 5 FOB builders + convoys | `CLW_Fob.c`, `CLW_Logistics.c`, `CLW_BuildExecutor.c`, `CLW_SupplyStore.c` | - |
| 6 GM takeover spike | nothing in `addon/` | `dev/research/gm-takeover-spike.md` |

## WP1 - Maps + auto scenario
1. **Scenario per world.** `CLW_GameMode.LoadScenario`: keep the prefab/world attribute `m_ScenarioConfig` when set (Arland). Otherwise look
   for `Configs/CLW/Scenarios/<world file name without extension>.conf` (e.g. `CLW_Eden`), then a scenario whose `m_aWorlds` lists the world,
   else the **auto scenario** (`_Auto.conf` + `CLW_AutoScenario`). Log the choice: `CLW: scenario <name> (source attribute|by-world|auto)`.
2. **Auto scenario** (`CLW_AutoScenario`, data in `_Auto.conf`): the template holds everything that is not map specific (two side slots with
   default vanilla factions as defaults only, economy, modes, start forces, random-layout limits, objective count per battle size).
   At game start (before players spawn, the bases need positions in SETUP) it fills the map specific parts:
   - HQ A / HQ B: the random-layout HQ search (reuse `CLW_RandomLayout`, extract a public "find HQ pair" function), seed logged.
   - Objectives: named map places from `CLW_Places` (towns/villages first, then other named places) in the band between the HQs, spread
     out (min spacing), count from the battle size; fall back to evenly spaced points on the HQ line validated by `CLW_FreeSpot`.
   - Vehicle slots: free spots near each HQ (`CLW_FreeSpot`).
   - FOB sites: none in data; `CLW_FobSites` computes them (make sure the computed path alone gives enough sites).
   - The map's own scene AI groups / spawn points are handled by the existing mechanisms (`CLW_Bases.PruneForeignSpawns`, scene group removal).
   With the auto scenario the setup's `startPositions=random` re-runs the same search; `fixed` keeps the positions found at game start.
3. **Random layout fix** (round-2 finding, Arland: 60 of 60 attempts rejected, 25 "water at the wish"): sample HQ wishes from land only (a cheap
   pre-check that does not use up attempts, e.g. sample until dry within a budget), raise the default attempts, and prefer wishes from the
   map places / road network over uniform points. Log the rejection histogram once per search.
4. Keep `fixed` on Arland byte-for-byte unchanged in behaviour.

## WP2 - Wrapper worlds + generator
1. `Worlds/CLW_Eden.ent` (Everon) and `Worlds/CLW_Cain.ent` (Kolguyev): sub-scenes like `Worlds/CLW_Arland.ent` (parent world + one layer
   with the same nine entities: AIWorld, PerceptionManager, FactionManager, GameMode, EditorSettings, two SpawnPoints, LoadoutManager, Chat).
   Parent GUIDs: `{853E92315D1D9EFE}worlds/Eden/Eden.ent`, `{1EA95DAE3230BEB0}worlds/Cain/Cain.ent`. AIWorld prefabs
   `Prefabs/AI/CLW_AIWorld_Eden.et` / `_Cain.et` inherit `{70CCCF16487C927F}Prefabs/AI/SCR_AIWorld_Eden.et` / `{672531FAC0834C06}Prefabs/AI/SCR_AIWorld_Cain.et`
   and set the campaign navmeshes: Eden `{A07353FE17EB2C49}worlds/MP/Navmeshes/CTI_Campaign_Eden.nmn` + `{3D08F0F26F771347}..._Eden_vehicles.nmn`,
   Cain `{B51562AFF15F6CCB}worlds/MP/Navmeshes/CTI_Campaign_Cain.nmn` + `{7D656156ECDE1C1C}..._Cain_vehicles.nmn` (same component layout as
   `CLW_AIWorld_Arland.et`). The GameMode entity in these worlds sets NO scenario config (WP1 picks the auto scenario by world).
   Every new resource gets a `.meta` with a fresh random 16-hex GUID in the same format as the existing metas. More GUIDs: `_tools/guid-lookup.ps1`.
2. `dev/tools/new-map-wrapper.mjs`: generates a tiny compat addon for ANY map mod: input = map addon folder (packed or loose) or its world path;
   it finds the world `.ent` and `.nmn` navmesh files (use `_tools/readpak.mjs`' pak reader, or the rdb like `guid-lookup.ps1`), asks/accepts
   CLI args for the choice when ambiguous, and writes `<out>/<Name>/addon.gproj` (dependencies: Clausewitz `C1A05E7B3D9F4A28` + the map addon),
   the wrapper world + layer + AIWorld prefab + metas. Without a navmesh it still writes the world and warns (AI will not move).
   Document usage, limits and how to load the result on the local DS in `dev/notes/maps.md`.
3. Optional if trivial: `_dedicated/start-server.ps1` already takes `-World`; document the worlds there (`_dedicated/README.md` is git-ignored workspace tooling, may be edited).

## WP3 - Side pools + props + admin overrides (VISION 10a)
1. **Side pools for forces** (`CLW_SidePool`, used by `CLW_ForceCatalog.Build`): a side's buy catalog = own faction entries, then kin factions
   (vanilla friendly lists, plus optional explicit kin in data), deduped by role + vehicle model, never the enemy side. Own entries win, kin
   entries only fill roles the own faction lacks (at least: logistics/supply truck, and any empty kind). Mark the source in the entry and the log.
   This fixes "no supply truck for RHS" (convoys) and missing roles.
2. **Crew check**: vehicles whose prefab has no default crew (seen: RHS_AFRF `APC_K17_unarmed.et`, `K4386.et` unarmed, "vehicle has no default
   crew to spawn" at purchase) must not reach the buy mix as crewed units: either crew them from the cargo group (driver + gunner taken from
   the infantry group, if the spawner supports it cleanly) or mark them crewless at catalog build (probe) and exclude them from the AI buy mix
   (still usable for player vehicle requests). Log per entry.
3. **Prop resolution chain** (`CLW_PropResolver`): admin override (`overrides.json` key `props`: per role a prefab, `"vanilla"`, or a style set)
   > shipped preference list (`Configs/CLW/Props/PropSets.conf`: ordered style sets of role -> candidate prefabs, used only when the prefab
   loads) > today's chain (profile, fallback faction, kin) > none. Validate every candidate (prefab loads). One log line per side + role with
   the source. Ship a vanilla set; pack sets only with verified GUIDs (find them with the enfusion tools, or leave the set empty with a comment).
4. **Admin overrides for sides** (`overrides.json` key `sides`): per faction key `ownOnly` (no kin in pools), `excludeFactions` (never use these
   kin), `excludePathContains` (prefab path texts never used). Document in `dev/notes/overrides.md`.

## WP4 - HQ menu + delivery feedback (VISION 6a, light version)
1. **HQ menu**: an action "Open HQ menu" on the request terminal (it already stands beside the command post and depot) opens one dialog with
   tabs or sections: Request (the five `/request` kinds with cost and the refusal reason), Squad (join/leave HQ squads, SITREP), Base (side
   stock, FOB list, convoys in flight), Report (contact report: count, type, distance, direction). Every button calls the SAME registry
   entries / services as the chat commands (`CLW_RequestService`, report path, sitrep), so rules and logs stay in one place. Server
   authoritative, data to the client over RPC as flat text like the setup menu (`CLW_SetupServer.BuildWire` / `CLW_SetupDialog` are the
   pattern to copy, incl. their layout). Keep the chat commands working.
2. **Vehicle delivery feedback**: when a vehicle request is granted, the requester gets "vehicle delivered, grid ..." (hint + chat) and a
   temporary own-side map marker at the vehicle (removed after a few minutes or when the requester enters it).

## WP5 - FOB builders + convoys for pack factions
1. FOB builders fail to reach far sites (round 1: "stalled 120 s at 237 m: no valid ground near the builder"; several FOBs refunded after 3
   stalls). Make it robust: relocate to valid ground near the builder with a wider search before giving up, re-plan the route via the road
   network, let a walking builder take a closer site, and clear the builder's previous objective/combat waypoints when it is assigned.
2. Convoys use the logistics entry from the side pool (WP3 makes `CLW_ForceCatalog` find kin supply trucks; you only consume the catalog API).
   Make the truck pick, load and unload robust for pack factions, and log clearly why a convoy is not started.

## WP6 - Game Master scenario takeover spike (research only)
Can we load a map addon + Clausewitz + the map's own Game Master scenario by its scenario ID and turn it into a Clausewitz war at runtime
(spawn our game mode logic / managers into a running GM world, no wrapper world)? Read the vanilla GM game mode and world setup with the
enfusion tools, list what a GM world has and lacks (AIWorld + navmesh, faction manager, loadout manager, spawn points), what we would need to
spawn at runtime, and the blockers. Output `dev/research/gm-takeover-spike.md` with a verdict (go / no-go / needs a test) and a minimal
test plan for the DS.

## Coordinator (not a subagent)
Merge order 3 -> 1 -> 5 -> 4 -> 2 (compile after each on the DS), CHANGELOG + PROGRESS, build stamp, one e2e run per map on the DS
(Arland fixed + random, Everon auto, Kolguyev auto; packs RHS/WCS/GRS) with the setup preset for unattended starts.

# Game Master scenario takeover spike (M4 WP6, research only, 2026-10-12)

Question: can a server load a map addon + Clausewitz + the map's own Game Master (GM) scenario by scenario ID and turn it into a
Clausewitz war at runtime, without a wrapper world? (VISION section 13, M4 "spike option C".)

Method: enfusion-mcp `game_read` / `game_browse` on the vanilla pak, `api_search`, local source. No game, Workbench or DS run.
Evidence marked **[read]** was read from game data, **[inferred]** follows from it, **[verify]** needs the DS test below.
Limit: the `.ent` world files are binary (EBIN); only class and component names could be extracted from `GM_Cain.ent`, `GM_Arland.ent`,
`GM_Eden.ent`, not property values or the exact entity list.

## Verdict: NEEDS A TEST (leaning go, medium effort)

Nothing found rules it out. The GM world already contains almost everything our hand-built wrapper world contains, and the missing
piece (our game mode logic) can be attached from script. But the attach point, an engine assumption (one game mode) and a few
GM side effects (persistence, editor game mode behaviour) can only be settled on the DS. The code work is a refactor of
`CLW_GameMode`, not a new feature: see "Options". Do the DS test (section "Test plan") before any refactor.

## 1. What a vanilla GM scenario is

Mission headers **[read]** `Missions/21_GM_Eden.conf`, `22_GM_Arland.conf`, `27_GM_Cain.conf` (`SCR_MissionHeader`):
- `World "{BEF094A5F7F3211B}worlds/GameMaster/GM_Eden.ent"` (Cain: `{6228BD33933C8F81}worlds/GameMaster/GM_Cain.ent`)
- `SystemsConfig "{8DDC2A311929D52F}Configs/Systems/GameMasterSystems.conf"`: `SystemSettings` inheriting `BaseGameModeSystems.conf`, adds
  one `SCR_PersistenceSystem` (server, config `Configs/Systems/Persistence/GameMode/GameMaster.conf`). So GM scenarios save/restore the world.
- `m_sGameMode "#AR-Scenario_GameMode_GameMaster"`, `m_iPlayerCount 64`. Plain `SCR_MissionHeader`, no custom fields.

GM world files **[read]** `worlds/GameMaster/GM_Arland.ent` (31 KB), `GM_Eden.ent` (100 KB), `GM_Cain.ent` (1.3 KB):
- All are `SubScene` with `Parent` = the map world (same mechanism as our `Worlds/CLW_Arland.ent`).
- Cain (smallest, class names visible): `SCR_AIWorld` with `NavmeshWorldComponent` + `NavmeshSettings` + `NavmeshWorld` +
  `NavmeshFilesConfig` + `ChimeraNavmeshWorld`, a `SCR_GameModeEditor` entity (named `GameMode_Editor_Full1`),
  `SCR_EditorCameraDefaultsComponent` with `SCR_CameraCoordinates` positions, `SCR_MapLocator` (`SCR_MapLocationQuadHint`), `MusicManager`.
- Arland / Eden additionally: `PerceptionManager`, the game mode entity named `GameMode_GameMaster_Full`, and dozens of
  `SCR_SiteSlotEntity` composition slots (`Slots_Airfield_*`, `Slots_Military_Hill`, ...) used by GM to place compositions.
  `PerceptionManager` is not visible in the Cain file [inferred: Cain relies on the parent world, or lacks it: check].
- Navmeshes ship per GM world **[read]** `worlds/GameMaster/Navmeshes/GM_{Arland,Cain,Eden}.nmn` + `_vehicles.nmn`
  (Eden/Cain 4.3 MB each, Arland 451 KB). So **a GM world has an AIWorld with navmesh**, and the AI walks (GM places AI all the time).

## 2. The GM game mode prefab

**[read]** `Prefabs/MP/Modes/Editor/GameMode_Editor_Full.et` (class `SCR_GameModeEditor`, inherits `GameMode_Editor.et`, which inherits
`Prefabs/MP/Modes/GameMode_Base.et`, the same base as our `Prefabs/GameModes/CLW_GameMode.et`):
- Children (prefab entities): `SCR_FactionManager` from `Prefabs/MP/Managers/Factions/FactionManager_Editor.et`, `RadioManagerEntity`,
  `ScriptedChatEntity`, `SCR_LoadoutManager` from `Prefabs/MP/Managers/Loadouts/LoadoutManager_Editor.et`.
- Components of interest: `SCR_RespawnSystemComponent` with `m_SpawnLogic SCR_MenuSpawnLogic` (deploy menu), `SCR_RespawnTimerComponent`,
  `SCR_PlayerSpawnPointManagerComponent` (player radio spawn points, `m_bEnablePlayerSpawnPoints 0`), `SCR_ScoringSystemComponent`,
  `SCR_VotingManagerComponent` (editor in/out/withdraw votes), `SCR_RestoreEditorModesComponent`, `SCR_RewindComponent`,
  `SCR_CampaignBuildingManagerComponent`, `SCR_CommandingManagerComponent` (we also have it), `SCR_NightModeGameModeComponent`,
  `SCR_InitWeatherComponent`. Child entity `SCR_EditorSettingsEntity` (`m_bIsUnlimitedEditorLegal 1`).
- `PlayerControllerPrefab ...DefaultPlayerControllerMP_Factions.et`.
- `FactionManager_Editor.et` factions: US, USSR, FIA, CIV, all `m_bIsPlayable 0` (GM players are not faction-bound).
  `LoadoutManager_Editor.et`: one `SCR_FactionPlayerLoadout` + one `SCR_PlayerArsenalLoadout` per faction (US/USSR/FIA/CIV rifleman etc.).
- Script API **[read]** `api_search SCR_GameModeEditor`: inherits `SCR_BaseGameMode`; overrides `OnGameStart`, `OnGameEnd`, `OnPlayerKilled`,
  `OnPlayerDeleted`, `OnPlayerEntityChanged_S`; `GetGameMasterTarget()` (how players get GM rights).

## 3. GM world vs. what Clausewitz needs (our `CLW_Arland.ent` wrapper)

| Need | Our wrapper world | GM world | Gap |
|---|---|---|---|
| AIWorld + campaign navmesh | `SCR_AIWorld CLW_AIWorld` (`CLW_AIWorld_Arland.et`, campaign nmn) | `SCR_AIWorld` with `GM_<Map>.nmn` + `_vehicles.nmn` | none; the GM navmesh is a different file from the campaign one [verify it is as good for vehicles on roads] |
| PerceptionManager | placed | Arland/Eden yes, Cain not visible | spawn `Prefabs/World/Game/PerceptionManager.et` at runtime if none [verify how to detect one] |
| FactionManager | `CLW_FactionManager.et` = **inherits `FactionManager_Editor.et`** | the same `FactionManager_Editor.et` inside the GM game mode prefab | none. `CLW_Sides` already flips playable/hostile at runtime (`SetIsPlayable`, `SetFactionsHostile`) and handles replication, so the GM factions work as in our wrapper |
| LoadoutManager | `CLW_LoadoutManager.et` | `LoadoutManager_Editor.et` (the loadout manager our prefab is based on, see `Prefabs/Managers/`) | none expected [verify our CLW_LoadoutManager.et parent] |
| Pack factions (RHS / WCS) | via the packs' own faction/loadout entries | not in the GM world | same problem as in our wrapper today: handled by whatever M4 WP3 does; takeover adds nothing |
| Spawn points | 2 `SCR_SpawnPoint` + runtime base spawns | none known (radio spawn points disabled); GM uses the deploy menu | none: `CLW_Bases` creates base spawn points at runtime and prunes foreign ones (`PruneForeignSpawns`) |
| Respawn system / spawn logic | `SCR_MenuSpawnLogic` subclass `CLW_SpawnLogic` (setup autospawn, forced faction, HQ squad wait) | plain `SCR_MenuSpawnLogic` | **gap**: the prefab's component attribute cannot be swapped from script (see 4.3) |
| Game mode entity | `CLW_GameMode` (OnGameStart builds everything) | `SCR_GameModeEditor` | **the main gap** |
| Chat / Radio | `ScriptedChatEntity` placed | in the game mode prefab | none |
| EditorSettings | `SCR_EditorSettingsEntity` placed | in game mode prefab | none |
| Persistence | none | `SCR_PersistenceSystem` (GameMaster.conf) | **risk**: may save/restore our spawned war entities, or restore a previous war on restart [verify] |
| Composition slots | none | `SCR_SiteSlotEntity` in Arland/Eden | harmless, but GM `m_aAuthoredLabels` slots are used for GM building only |

Conclusion: the GM world is a superset of our wrapper world except for the game mode entity and the spawn logic. The wrapper world
exists only because our game mode is a different entity class.

## 4. Can script attach to / replace the running game mode?

### 4.1 One game mode per world
`ChimeraGame.GetGameMode()` returns a single `BaseGameMode` **[read]** `api_search GetGameMode`. The engine finds the game mode entity
in the world; the first one is used [inferred, not documented]. Spawning a second `CLW_GameMode` prefab (`GetGame().SpawnEntityPrefab`)
beside `SCR_GameModeEditor` is therefore the unsafe route: the engine would not route `OnGameStart`, player events, respawn and
`SCR_RespawnSystemComponent` to it, and its replication would be a second game mode RplComponent [inferred]. Do not rely on it
(only as a DS test variant, 5.B).

### 4.2 Modded class on the GM game mode (preferred)
`modded class SCR_GameModeEditor` (or `modded class SCR_BaseGameMode`, which also covers `CLW_GameMode`, so the hook must tell them apart):
- `override void OnGameStart()` : `super.OnGameStart();` then, if the takeover flag is on and `Replication.IsServer()`, start the war systems.
  This is exactly the sequence in `CLW_GameMode.OnGameStart` (CLW_GameMode.c, lines ~196-270): reset statics, action registry, scenario
  load, `CLW_Bases/ControlPoints/WarState/ForceCatalog/VehicleManager/ForceSpawner`, `CLW_MissionBoard.Install`, `CLW_SquadJoin.Init`,
  `CLW_SquadGroups.LockNewGroups`, registry start, `BuildFactionObjects`, call-later timers.
- Other CLW overrides that need the same hook on the GM class: `CanPlayerSpawn_S` (ticket gate), `OnControllableDestroyedEx`,
  `OnPlayerKilledEx` (kill accounting), `OnPlayerSpawnFinalize_S` (start squad join), `OnPlayerRegistered` listener. All exist on
  `SCR_BaseGameMode` (CLW_GameMode.c overrides the same methods), so the GM class can override them (`SCR_GameModeEditor` itself
  overrides `OnPlayerKilled` / `OnPlayerEntityChanged_S`, which must still call `super`).
- **Flag gating** (so a Clausewitz mod loaded on a normal GM server does not hijack it): a server-side switch read in `OnGameStart`.
  Candidates: `$profile:Clausewitz/overrides.json` key (already the pattern for admin overrides, see `dev/notes/overrides.md`), or a
  command line parameter (`System.GetCLIParam`). `-config` JSON `gameProperties` cannot carry custom keys [inferred]. Default off.
  Client side needs nothing but the same addon (UI, HUD are components / classes in our scripts).
- State access: `CLW_GameMode.GetInstance()` and the static getters (`GetBases`, `GetWarState`, `GetRegistry`, ...) are used in 222
  places across 50 files. Keep that API; the game mode logic must move into a holder both game mode classes delegate to (see Options).

### 4.3 Spawn logic cannot be replaced from script
`m_SpawnLogic` is a prefab attribute on `SCR_RespawnSystemComponent`. `CLW_SpawnLogic` (our subclass of `SCR_MenuSpawnLogic`)
carries the setup-phase autospawn and the "wait for HQ squads" rule. Script cannot swap it on the GM prefab. Options:
(a) `modded class SCR_MenuSpawnLogic` with the same gated overrides (`DoSpawn_S`, `OnPlayerEntityLost_S`, `GetForcedFaction`,
`OnPlayerSpawnFailed_S`) [note: CLW_SpawnLogic.c header says a modded SCR_*SpawnLogic used BY NAME in a prefab was "unknown class";
modding the vanilla class that vanilla prefabs already use is the normal pattern, but verify]; (b) skip the setup autospawn and
accept the vanilla deploy menu for the takeover mode (the setup dialog would then need another trigger); (c) a tiny override mod
patching `GameMode_Editor.et`/`_Full.et` through a prefab override in our addon (`reforger-patch-thirdparty-mod`-style): works for a
vanilla-based GM only, but the map mod's GM world may use its own prefab [inferred]. (a) is the least invasive.

### 4.4 Scenario data
`LoadScenario` needs a scenario config: no prefab attribute on the GM class. Use the WP1 resolution (by world file name, then
`m_aWorlds`, then auto scenario). The world name here is `GM_Eden` etc.; the world file name differs from the Clausewitz one,
so the lookup key must be configurable (flag file) or based on the map parent world (`Eden`, `Cain`) rather than `GM_Eden` [WP1 coordination].

## 5. How the DS picks such a scenario

- JSON config **[read]** wiki Server Config + `_dedicated/configs/clausewitz.json`: `game.scenarioId` = `"{GUID}Missions/21_GM_Eden.conf"`,
  `game.mods` = list of Workshop addons (`modId`, `name`, optional `version`). The scenario file is found by the mission header GUID;
  for vanilla GM (base game, no mod needed). For a map mod's GM scenario: mods = [map mod, Clausewitz (+ packs)], scenarioId = the
  map mod's mission header path.
- The map-mod pattern [inferred from the vanilla files, no map mod header was read]: a map mod ships `Missions/<Name>.conf`
  (`SCR_MissionHeader` with `World` = its GM `.ent`, `SystemsConfig = GameMasterSystems.conf`) and a sub-scene `.ent` parented on its
  terrain world, containing `SCR_AIWorld` + navmesh + the vanilla GM game mode prefab, like `GM_Cain.ent`. Whether every map mod ships a
  GM scenario is not guaranteed; many ship only the terrain. (Check with `server_scenario_picker` / `server_mod_list` on the DS, [verify].)
- **Constraint** (`_dedicated/README.md`, verified 2026-10-12): `-config` cannot be combined with `-addons`, and `game.mods` must
  be Workshop addons ("Addon ... was not found on workshop"). So while Clausewitz is **unpublished**, the `scenarioId` route is
  unavailable for it: the local test cannot load `Missions/21_GM_Eden.conf` + Clausewitz through `-config`.
  What does work locally: `ArmaReforgerServer.exe -server <world.ent> -addonsDir ... -addons C1A05E7B3D9F4A28 ...`, where the `.ent`
  path can be any world resource, **including `worlds/GameMaster/GM_Eden.ent` from the base game**: the world loads, its game mode
  prefab runs, our modded class applies. This tests the takeover without a mission header and without publishing (limitation: no
  `SystemsConfig` from the header, so no persistence system; fine for a first test, and it avoids the persistence risk entirely).
  The full path (scenarioId + persistence system) needs Clausewitz published (even as a private/unlisted Workshop item), or a mission
  header inside our addon (`Missions/CLW_GM_Eden.conf` with `World` = the vanilla GM world and our own `SystemsConfig`), which `-server`
  cannot use either; both only matter at release time.

## 6. Options

| | What | Cost | Risk |
|---|---|---|---|
| A. Wrapper worlds (M4 WP2, plan of record) | sub-scene per map with our nine entities | generator already designed | needs a world per map; map mods change = regenerate |
| B. Spawn `CLW_GameMode` into the GM world | `SpawnEntityPrefab` at runtime | small | high: two game modes, engine routing (4.1); probably does not work |
| C. Modded `SCR_GameModeEditor` (+ `SCR_MenuSpawnLogic`) gated by a flag | extract war bootstrap from `CLW_GameMode` into `CLW_WarRuntime` (static or `ref` holder), both entity classes delegate | medium refactor of one file (`CLW_GameMode.c`, 658 lines), keeps `CLW_GameMode.GetInstance()` API | GM side effects (editor, voting, persistence), unknown map-mod GM variations |
| D. Own mission header on the vanilla GM world | `Missions/CLW_*.conf` -> vanilla GM `.ent`, with option C's hook | trivial after C | needs `-config`, so only after publish |

Option C also gives "runtime takeover of other scenario types" later (VISION "Later": option B in the roadmap wording): any scenario with
a `SCR_BaseGameMode` subclass can be hooked the same way, because the war logic no longer depends on its own entity.

## 7. Blockers and risks (ranked)

1. **Unpublished mod + `-config`** (confirmed by README): no scenarioId path locally; use `-server GM_x.ent` for the test, publish for the real thing.
2. **Spawn logic swap** (4.3): `CLW_SpawnLogic` cannot be put on the GM prefab; modded `SCR_MenuSpawnLogic` needed, class-name gotcha noted in our code.
3. **GM side effects**: GM rights/voting (`SCR_VotingManagerComponent`), `SCR_RestoreEditorModesComponent`, `SCR_PersistenceSystem`
   (restores a previous session's entities and may save our war entities; war state itself is not persisted by us), editor "rewind".
   The GM can already spawn/delete AI, which our registry adopts (`CLW_SquadRegistry`, VISION: "GM authority per group"), so coexistence is by design.
4. **Scenario lookup key** (4.4) differs from the wrapper's world name: WP1 must accept a configured scenario.
5. **Navmesh quality** (`GM_*.nmn` vs `CTI_Campaign_*.nmn`): both are the "playable" navmeshes; the sizes match for Eden/Cain (4.3 MB) [verify]; Arland GM navmesh is 451 KB, tiny, check.
6. **Map mods** may not ship a GM scenario, or ship a different game mode prefab (then option C hooks the wrong class: hook `SCR_BaseGameMode` and gate on the entity not being `CLW_GameMode`).
7. **PerceptionManager** missing in the Cain GM world (not visible in the binary).
8. Engine assumption "one game mode" is only a problem for option B.

## 8. Minimal DS test plan (needs a test)

Pure code of the hook is ~40 lines, written in a scratch branch/file and not shipped:

Prep (all in `addon/`, test-only file `Scripts/Game/CLW_GmTakeoverSpike.c`, removed afterwards):
```
modded class SCR_GameModeEditor
{
	override void OnGameStart()
	{
		super.OnGameStart();
		if (!Replication.IsServer()) return;
		Print("CLW-SPIKE: SCR_GameModeEditor.OnGameStart, world=" + GetGame().GetWorldFile(), LogLevel.NORMAL);
		// then: dump managers, try the real bootstrap
	}
}
```
Steps:
1. **Hook fires + inventory** (30 s): `start-server.ps1 -World worlds/GameMaster/GM_Eden.ent` (or `-server` with that path; script may need a `-World` string that
   is the resource path). Expect in `console.log`: `Compiling Game scripts` OK, `CLW-SPIKE ...` line. In the same hook log:
   `GetGame().GetFactionManager()` (class, faction keys, `IsPlayable`), `SCR_LoadoutManager`, `GetGame().GetGameMode()` class name
   (must be `SCR_GameModeEditor`), `AIWorld`/`SCR_AIWorld` present, `PerceptionManager` present, count of `SCR_SpawnPoint`, respawn system component + spawn logic class.
   Repeat with `worlds/GameMaster/GM_Cain.ent` and `GM_Arland.ent` (Cain: perception manager question).
2. **Navmesh/AI** (2 min): from the hook spawn one AI group (`CLW_ForceSpawner`) 200 m away and order a move; log arrival. Verifies `GM_*.nmn`
   is usable (infantry + one vehicle).
3. **Real bootstrap** (5 min): behind a `static const bool` or `overrides.json` flag, call a copy of the `CLW_GameMode.OnGameStart` server block with
   `CLW_ScenarioConfig.Load(<Arland-like scenario or WP1 auto scenario>)`. Needs `CLW_GameMode.s_Instance` equivalent (copy the block into a
   scratch class that sets `s_Instance` through a small test accessor; no refactor yet). Success = `CLW: war RUNNING`, bases created, forces
   spawned, tickets ticking, a client can join, deploys at a base spawn point.
4. **Spawn logic**: log what `SCR_MenuSpawnLogic.DoSpawn_S` does for the first joining client (deploy menu appears? faction tiles show the two CLW sides after `CLW_Sides` flips playable?).
5. **Persistence / second run**: only if a mission header route is available (published mod): scenarioId GM_Eden + Clausewitz in `game.mods`;
   restart the server and check that no stale entities come back.
6. **Decision**: if steps 1-4 pass -> GO: refactor `CLW_GameMode` into `CLW_WarRuntime` + two thin entity classes (one task for the next milestone,
   Arland `fixed` behaviour unchanged). If step 1 shows two game modes or the hook cannot see the managers -> NO-GO, stay with wrapper worlds (A).

Pass/fail needs one DS boot each (~25 s) plus a client for steps 3-4; fits one e2e session. No Workbench needed.

## 9. Recommendation for M4

Keep the wrapper-world plan (A) for M4; it is independent. Add the step-1/2 test to the M4 e2e session (cheap) to decide whether option C
is worth scheduling after M4. Do not touch `CLW_GameMode` for this before the test passes.

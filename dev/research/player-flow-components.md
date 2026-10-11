# Player flow: vanilla components we can reuse (2026-10-11)

Source: MCP `api_search`, `component_search`, `game_read` on base-game data. Items marked `[verify]` are not yet proven in play.

## Verdict
Joining a side, spawning, loadouts, respawn, squads/groups, map markers and tasks all live in **`Prefabs/MP/Modes/GameMode_Base.et`** (base of every vanilla mode), not in Conflict. Our own game-mode prefab inherits it and gets the player flow for free. This confirms the "own game mode" decision (`notes/decision-game-mode.md`); the "revisit if M0 takes >1 week" trigger is unlikely to fire.

## What GameMode_Base.et already contains
- `SCR_RespawnSystemComponent` with spawn handlers `SCR_FreeSpawnHandlerComponent`, `SCR_PossessSpawnHandlerComponent`, `SCR_SpawnPointSpawnHandlerComponent`; spawn logic is a slot (`m_SpawnLogic`), e.g. `SCR_AutoSpawnLogic` (used by TeamDeathmatch_Auto).
- `SCR_GroupsManagerComponent` (player groups, `AddPlayerToGroup`, `MovePlayerToGroup`, `SetGroupLeader`, leader volunteering; default group prefab `Prefabs/Groups/PlayableGroup.et`), `SCR_VotingManagerComponent` (group-leader votes).
- `SCR_MapMarkerManagerComponent`, `SCR_MapConfigComponent`, `SCR_CallsignManagerComponent`, `SCR_ArsenalManagerComponent`, `SCR_EntityCatalogManagerComponent` (factionless catalogs), `SCR_SpawnerAIGroupManagerComponent`, `SCR_TextsTaskManagerComponent`, `SCR_TaskManagerUIComponent`, `Persistence`.
- Not in the base (added by modes): `SCR_RespawnTimerComponent` (TeamDeathmatch uses 10 s), scoring, nametags, factions manager, loadout manager (`Prefabs/MP/Managers/Factions/FactionManager_*.et`, `Managers/Loadouts/LoadoutManager_*.et`), spawn points (`Prefabs/MP/Spawning/SpawnPoint_*.et`).

## Lightest references
`Prefabs/MP/Modes/Plain/GameMode_Plain.et`, `TeamDeathmatch/GameMode_TeamDeathmatch_{Base,Auto,Selection}.et` (a few components over the base). Other `SCR_BaseGameMode` subclasses: `SCR_GameModeSectorControl`, `SCR_GameModeLastStand`, `SCR_GameModeCleanSweep`, `SCR_GameModeCampaign` (Conflict), `SCR_GameModeEditor`.

## Hooks on SCR_BaseGameMode (server `_S`)
`OnGameStart`, `OnGameModeStart`, `OnPlayerRegistered`, `OnPlayerSpawnFinalize_S`, `OnPlayerKilled`, `OnPlayerFactionSet_S`, `OnPlayerLoadoutSet_S`, `CanPlayerSpawn_S`, `PreparePlayerEntity_S`, `OnControllableSpawned`, `OnControllableDestroyed`, `GetPlayerRemainingRespawnTime`.

## Mission delivery: vanilla task system exists
`SCR_TaskSystem` + `SCR_Task`: tasks visible in the task list and on the map, assignable to executors (`TaskExecutorFromPlayerID`, `TaskExecutorFromEntity`, `TaskExecutorFromGroup`), group-leader checks (`IsGroupLeader`), `GetOnTaskCreated/Added/Removed`, `AssignTask` via `SCR_TaskSystemNetworkComponent`, per-task visibility `IsTaskVisibleFor`. So VISION 2a "orders as missions" and "choose between assignments" can sit on the vanilla task system `[verify: create a task from script, offer N tasks, visibility per group]`.

## AI side
- `SCR_AIGroup` (spawn members `SetNumberOfMembersToSpawn`, rally point, utility component); `SCR_AIGroupPerception` exposes `m_aTargetsSCR_AITargetInfo`, `m_aTargetClusters`, `m_MostDangerousCluster`, events `GetOnEnemyDetected`, `GetOnNoEnemy`, `GetOnTargetClusterDeleted` -> direct input for our `Contact` records (perception spike).
- `SCR_FactionCommanderHandlerComponent` (+ Campaign subclass): vanilla faction-commander role, a slot we can study; not needed for M0.

## Plan for M0 player join
1. `CLW_GameMode` prefab inheriting `GameMode_Base.et` (+ respawn timer, `SCR_AutoSpawnLogic` or selection like TeamDeathmatch).
2. Faction manager for two factions (start with US vs USSR: `FactionManager_USxUSSR.et`) and `LoadoutManager_USxFIA.et`-style loadouts `[verify USSR loadouts exist]`.
3. Minimal world with spawn points per faction and one AI squad per side (`Prefabs/Groups/...`).
4. Later: make the player join an existing AI group instead of a new playable group `[verify: AddPlayerToGroup works on AI groups, AI leader hand-over]`.

## Open `[verify]` items
- Player joins an AI-led `SCR_AIGroup`, and takes leadership with AI members following.
- Script-created `SCR_Task` visible per group; several offered tasks.
- Which world to use as the base (a base-data world like `GM_Eden.ent`, or our own small world).

// Dynamic sides (phase P6). The scenario's two faction setups are SIDE SLOTS (base, identity, spawn settings, start forces);
// the factions fighting on them are chosen in the war setup (battle values factionA / factionB) and assigned at StartWar.
// This class holds the mechanisms; no faction key, prefab or coordinate lives here. Server only.
//
// Re-keying order inside CLW_Sides.Apply (all BEFORE the war mode starts and BEFORE CLW_WarState.GetOnWarStarted fires):
//   1. scenario sides (CLW_ScenarioConfig.ApplySides: GetFactionKeys / IsClwFaction / GetEnemyKey / FindFaction follow)
//   2. factions: playable / hostile (SCR_Faction.SetIsPlayable, SCR_FactionManager.SetFactionsHostile)
//   3. bases + spawn points (CLW_Bases.Rekey)
//   4. control points: owners from the objective start owners, markers (CLW_ControlPoints.Rekey)
//   5. war state ledgers (CLW_WarState.OnSidesChanged)
//   6. scene groups of non-participating factions deleted (scenario m_bRemoveUnusedSceneGroups)
//   7. squad registry: faction filter + re-sweep (CLW_SquadRegistry.Rekey)
//   8. force catalogs of both sides built (CLW_ForceCatalog.Build), loadout check
//   9. players of other factions are moved to the default side (delayed, best effort)
class CLW_Sides
{
	static const int MOVE_PLAYERS_DELAY_MS = 1500;

	// Called by CLW_WarState.Init: the default sides are playable and hostile during SETUP (players spawn on side A).
	static void PrepareSetup(notnull CLW_ScenarioConfig scenario)
	{
		if (!Replication.IsServer())
			return;

		SetPlayableAndHostile(scenario);
	}

	// Assigns keyA/keyB to the sides and re-keys everything. False (nothing changed) when invalid.
	static bool Apply(notnull CLW_WarState war, notnull CLW_ScenarioConfig scenario, string keyA, string keyB)
	{
		if (!Replication.IsServer())
			return false;

		string error = scenario.CheckSides(keyA, keyB);
		if (!error.IsEmpty())
		{
			Print("CLW: sides refused: " + CLW_Loc.Resolve(error), LogLevel.ERROR);
			return false;
		}

		Print(string.Format("CLW: sides applying %1 (side A) vs %2 (side B)", keyA, keyB), LogLevel.NORMAL);
		if (!scenario.ApplySides(keyA, keyB))
			return false;

		SetPlayableAndHostile(scenario);
		BroadcastSides(keyA, keyB);

		CLW_Bases bases = CLW_GameMode.GetBases();
		if (bases)
			bases.Rekey(scenario);
		else
			Print("CLW: sides WARNING no bases object to re-key", LogLevel.WARNING);

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (points)
			points.Rekey(scenario);

		war.OnSidesChanged();

		if (scenario.m_bRemoveUnusedSceneGroups)
			RemoveUnusedSceneGroups(scenario);

		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (registry)
			registry.Rekey();

		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		array<string> keys = {};
		scenario.GetActiveKeys(keys);
		foreach (string key : keys)
		{
			if (catalog)
			{
				int entries = catalog.Build(key);
				Print(string.Format("CLW: sides catalog %1: %2 enabled entries", key, entries), LogLevel.NORMAL);
			}

			CheckLoadouts(key);
		}

		GetGame().GetCallqueue().CallLater(MovePlayers, MOVE_PLAYERS_DELAY_MS);
		Print(string.Format("CLW: sides applied: %1 vs %2", keyA, keyB), LogLevel.NORMAL);
		return true;
	}

	// ---- factions ----

	// SetIsPlayable does not reach running clients (vanilla replicates it only through Game Master faction delegates): their deploy
	// menu kept listing the setup sides (DS e2e 2026-10-12: "US Army / Soviet Army" after an RHS war start; a rejoin was fine).
	// Every connected client applies the same playable/hostile state locally; later joiners get it from the server as usual.
	protected static void BroadcastSides(string keyA, string keyB)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
			if (controller)
				controller.CLW_SyncSides_S(keyA, keyB);
		}

		Print(string.Format("CLW: sides broadcast %1 vs %2 to %3 client(s)", keyA, keyB, ids.Count()), LogLevel.NORMAL);
	}

	// Client side of BroadcastSides: the two sides playable and hostile, every other faction not playable.
	static void ApplySidesLocal(string keyA, string keyB)
	{
		SCR_FactionManager manager = SCR_FactionManager.Cast(GetGame().GetFactionManager());
		if (!manager)
			return;

		array<Faction> factions = {};
		manager.GetFactionsList(factions);
		foreach (Faction faction : factions)
		{
			SCR_Faction scrFaction = SCR_Faction.Cast(faction);
			if (!scrFaction)
				continue;

			string key = scrFaction.GetFactionKey();
			bool isSide = key == keyA || key == keyB;
			if (scrFaction.IsPlayable() != isSide)
				scrFaction.SetIsPlayable(isSide, false);
		}

		SCR_Faction first = SCR_Faction.Cast(manager.GetFactionByKey(keyA));
		SCR_Faction second = SCR_Faction.Cast(manager.GetFactionByKey(keyB));
		if (first && second && first.DoCheckIfFactionFriendly(second))
			manager.SetFactionsHostile(first, second, -1, true);

		Print(string.Format("CLW: sides client applied %1 vs %2", keyA, keyB), LogLevel.NORMAL);
	}

	// The two sides playable and hostile to each other, every other faction not playable. [verify] SetIsPlayable replicates.
	protected static void SetPlayableAndHostile(notnull CLW_ScenarioConfig scenario)
	{
		SCR_FactionManager manager = SCR_FactionManager.Cast(GetGame().GetFactionManager());
		if (!manager)
		{
			Print("CLW: sides no SCR_FactionManager: playable/hostile not set", LogLevel.ERROR);
			return;
		}

		array<string> keys = {};
		scenario.GetActiveKeys(keys);
		array<Faction> factions = {};
		manager.GetFactionsList(factions);
		foreach (Faction faction : factions)
		{
			SCR_Faction scrFaction = SCR_Faction.Cast(faction);
			if (!scrFaction)
				continue;

			bool isSide = keys.Contains(scrFaction.GetFactionKey());
			if (scrFaction.IsPlayable() != isSide)
			{
				scrFaction.SetIsPlayable(isSide, false);
				Print(string.Format("CLW: sides faction %1 playable=%2", scrFaction.GetFactionKey(), isSide), LogLevel.NORMAL);
			}
		}

		if (keys.Count() < 2)
			return;

		SCR_Faction first = SCR_Faction.Cast(manager.GetFactionByKey(keys[0]));
		SCR_Faction second = SCR_Faction.Cast(manager.GetFactionByKey(keys[1]));
		if (!first || !second)
		{
			Print("CLW: sides cannot set hostility: faction missing", LogLevel.ERROR);
			return;
		}

		manager.SetFactionsHostile(first, second, -1, true);
		Print(string.Format("CLW: sides %1 and %2 hostile=%3", keys[0], keys[1], !first.DoCheckIfFactionFriendly(second)), LogLevel.NORMAL);
	}

	// ---- scene groups ----

	// Deletes scene-placed (and any other existing) AI groups of factions that are neither side nor on the ignore list,
	// together with their members. Groups with a player in them are kept.
	protected static void RemoveUnusedSceneGroups(notnull CLW_ScenarioConfig scenario)
	{
		AIWorld aiWorld = GetGame().GetAIWorld();
		if (!aiWorld)
			return;

		array<AIAgent> agents = {};
		aiWorld.GetAIAgents(agents);
		array<SCR_AIGroup> groups = {};
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			SCR_AIGroup group = SCR_AIGroup.Cast(agent);
			if (!group)
				group = SCR_AIGroup.Cast(agent.GetParentGroup());
			if (group && group.IsSlave())
				group = group.GetMaster();
			if (group && !groups.Contains(group))
				groups.Insert(group);
		}

		int removedGroups;
		int removedMembers;
		foreach (SCR_AIGroup victim : groups)
		{
			string key = CLW_SquadRegistry.GetFactionKey(victim);
			if (key.IsEmpty() || scenario.GetSideIndex(key) >= 0 || scenario.IsIgnoredFaction(key))
				continue;

			if (victim.GetPlayerCount() > 0)
			{
				Print(string.Format("CLW: sides keeps group %1 (%2): has players", victim.GetName(), key), LogLevel.WARNING);
				continue;
			}

			Print(string.Format("CLW: sides removing unused scene group '%1' faction=%2 agents=%3", victim.GetName(), key, victim.GetAgentsCount()), LogLevel.NORMAL);
			array<IEntity> members = CLW_SquadInfo.Members(victim);
			foreach (IEntity member : members)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(member);
				removedMembers++;
			}

			SCR_EntityHelper.DeleteEntityAndChildren(victim);
			removedGroups++;
		}

		Print(string.Format("CLW: sides removed %1 unused scene groups (%2 members)", removedGroups, removedMembers), LogLevel.NORMAL);
	}

	// ---- loadouts ----

	// Loadouts a player of the faction can be spawned with (rifleman-like, not the saved-arsenal ones).
	static int GetSpawnLoadouts(string factionKey, notnull array<SCR_BasePlayerLoadout> result)
	{
		FactionManager factionManager = GetGame().GetFactionManager();
		SCR_LoadoutManager loadoutManager = GetGame().GetLoadoutManager();
		if (!factionManager || !loadoutManager)
			return 0;

		Faction faction = factionManager.GetFactionByKey(factionKey);
		if (!faction)
			return 0;

		array<ref SCR_BasePlayerLoadout> all = {};
		loadoutManager.GetPlayerLoadoutsByFaction(faction, all);
		foreach (SCR_BasePlayerLoadout loadout : all)
		{
			if (loadout && !SCR_PlayerArsenalLoadout.Cast(loadout))
				result.Insert(loadout);
		}

		return result.Count();
	}

	protected static void CheckLoadouts(string factionKey)
	{
		array<SCR_BasePlayerLoadout> loadouts = {};
		int count = GetSpawnLoadouts(factionKey, loadouts);
		if (count == 0)
			Print(string.Format("CLW: sides ERROR faction %1 has NO player loadout in the LoadoutManager: its players cannot spawn (the pack must add one to LoadoutManager_Editor.et)", factionKey), LogLevel.ERROR);
		else
			Print(string.Format("CLW: sides faction %1 has %2 player loadout(s)", factionKey, count), LogLevel.NORMAL);
	}

	// ---- players ----

	// Default side of the scenario: where CLW_SpawnLogic auto-spawns players during SETUP (forced faction). False when no scenario is active.
	static bool GetPlayerFaction(out Faction faction)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario)
			return false;

		FactionManager manager = GetGame().GetFactionManager();
		if (!manager)
			return false;

		string key = scenario.GetDefaultPlayerFactionKey();
		if (key.IsEmpty())
			return false;

		faction = manager.GetFactionByKey(key);
		return faction != null;
	}

	// Players of a faction that is neither side (spawned during SETUP on a default side that was replaced): remove their
	// character; CLW_SpawnLogic then opens the deploy menu (the two real sides). Best effort, every step logged.
	protected static void MovePlayers()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		PlayerManager players = GetGame().GetPlayerManager();
		if (!scenario || !players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			Faction faction = SCR_FactionManager.SGetPlayerFaction(playerId);
			if (!faction)
				continue;

			string key = faction.GetFactionKey();
			if (scenario.GetSideIndex(key) >= 0)
				continue;

			IEntity entity = players.GetPlayerControlledEntity(playerId);
			Print(string.Format("CLW: sides moving player %1 from %2 to %3 (character %4)", playerId, key, scenario.GetDefaultPlayerFactionKey(), entity), LogLevel.NORMAL);

			// the player's own faction must change too: otherwise it stays on the replaced side, which owns no spawn point
			// any more. Not SetFaction_S on the server: the running client never learned it (DS e2e 2026-10-12: deploy menu with
			// vanilla US loadouts, spawn point disabled). The client requests it itself, as the deploy menu would.
			FactionManager factionManager = GetGame().GetFactionManager();
			Faction target = factionManager.GetFactionByKey(scenario.GetDefaultPlayerFactionKey());
			SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
			if (target && controller)
			{
				controller.CLW_SwitchFaction_S(factionManager.GetFactionIndex(target));
				Print(string.Format("CLW: sides player %1: client asked to switch to %2", playerId, target.GetFactionKey()), LogLevel.NORMAL);
			}
			else
			{
				Print(string.Format("CLW: sides player %1: cannot switch faction (target %2, controller %3)", playerId, target, controller), LogLevel.WARNING);
			}

			if (entity)
				SCR_EntityHelper.DeleteEntityAndChildren(entity);	// [verify] -> OnPlayerEntityLost_S -> DoSpawn_S with the new forced faction
		}
	}
}

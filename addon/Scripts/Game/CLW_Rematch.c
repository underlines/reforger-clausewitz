// Rematch in place (M4 round 3, owner problem P6). After the war is over the session goes back to the setup phase WITHOUT the engine game
// end and WITHOUT a world reload: EndGameMode made the server reload the world ~30 s later and the reconnecting client was kicked with
// JIP_ERROR. Server only. Log prefix: "CLW: rematch".
//
// Switch (server file $profile:Clausewitz/overrides.json, top-level keys, read again at every war end; documented in dev/notes/overrides.md):
//   "rematch":         "inPlace" (default) | "reload" (the old behaviour: EndGameMode, the server reloads the world) | "off" (the war stays ENDED,
//                      nothing happens afterwards; the admin restarts the server)
//   "rematchDelaySec": seconds the results stay before the next round (default 60, minimum 5)
//
// inPlace sequence (CLW_WarState.Finish -> OnWarOver -> Run after the delay):
//   results   the vanilla end screen only exists through EndGameMode, so there is none: the results are the HUD (phase ENDED shows "<faction> wins"),
//             the "WAR OVER" announcement from the war state and a "next round in n s" announcement from here.
//   1 stop        commanders, contact poll, activity tick, contact markers (nothing plans or buys during the reset)
//   2 services    base / FOB services, request terminals, structure markers, supply links (already removed at war end, repeated for safety)
//   3 FOB / convoy state, vehicle units (crews, cargo, vehicles, returning / abandoned), spawner jobs, squads (AI, slave groups, humans freed),
//     player requested vehicles / boxes, pending orders, idle-watch ledger, SITREPs, objective + HQ markers, spawn points
//   4 restart     CLW_WarRuntime.RematchRestart: scenario reloaded, bases / points / war state (SETUP, setup preset + auto start) / catalog /
//                 spawner / registry / commanders rebuilt, mission board reinstalled (its tasks deleted)
//   5 players     default sides sent to the clients, players on a side of the finished war switch to the default faction, their characters are
//                 deleted (or the spawn logic is nudged when they have none): CLW_SpawnLogic auto-spawns them in SETUP and the setup menu opens
//                 for the setup owner, exactly as at the first start.
// Every step logs "CLW: rematch: reset <manager> (<n> entities)"; entity counts before / after are logged as "census".
class CLW_RematchFlags
{
	string rematch;
	int rematchDelaySec = -1;
}

class CLW_Rematch
{
	static const string MODE_IN_PLACE = "inPlace";
	static const string MODE_RELOAD = "reload";
	static const string MODE_OFF = "off";
	static const int DEFAULT_DELAY_SEC = 60;
	static const int MIN_DELAY_SEC = 5;
	static const int COUNTDOWN_SEC = 10;		// last "next round in n s" announcement
	static const int PLAYER_SETTLE_MS = 1500;	// faction switch RPC before the characters are removed (same delay as CLW_Sides.MovePlayers)

	protected static string s_sMode = MODE_IN_PLACE;
	protected static int s_iDelaySec = DEFAULT_DELAY_SEC;
	protected static int s_iRound = 1;			// the round that is running / just ended
	protected static bool s_bPending;

	// ---- config ----

	protected static void LoadConfig()
	{
		s_sMode = MODE_IN_PLACE;
		s_iDelaySec = DEFAULT_DELAY_SEC;
		if (!FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
			return;

		string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
		if (text.IsEmpty())
			return;

		SCR_JsonLoadContext ctx = new SCR_JsonLoadContext();
		CLW_RematchFlags flags = new CLW_RematchFlags();
		if (!ctx.ImportFromString(text) || !ctx.ReadValue("", flags))	// [verify] unknown keys are skipped, absent keys keep the defaults
			return;

		string wanted = flags.rematch;
		wanted.ToLower();
		if (wanted == "inplace" || wanted.IsEmpty())
			s_sMode = MODE_IN_PLACE;
		else if (wanted == "reload")
			s_sMode = MODE_RELOAD;
		else if (wanted == "off")
			s_sMode = MODE_OFF;
		else
			Print(string.Format("CLW: rematch: unknown overrides.json rematch '%1' (inPlace | reload | off), using inPlace", flags.rematch), LogLevel.WARNING);

		if (flags.rematchDelaySec >= 0)
			s_iDelaySec = Math.Max(MIN_DELAY_SEC, flags.rematchDelaySec);
	}

	static int GetRound()
	{
		return s_iRound;
	}

	// ---- war over ----

	// Called by CLW_WarState.Finish after the "WAR OVER" announcement. Returns true when the caller must end the game the old way
	// (EndGameMode, mode "reload"); false when the session stays up (inPlace schedules the next round, off does nothing).
	static bool OnWarOver(CLW_WarState war, string winnerKey, string why)
	{
		LoadConfig();
		Print(string.Format("CLW: rematch: choice '%1' (overrides.json rematch: inPlace | reload | off), delay %2 s, round %3 just ended", s_sMode, s_iDelaySec, s_iRound), LogLevel.NORMAL);
		if (s_sMode == MODE_RELOAD)
		{
			Print("CLW: rematch: reload: the engine game end (EndGameMode) runs, the server reloads the world; clients may fail to rejoin with JIP_ERROR", LogLevel.WARNING);
			return true;
		}

		if (s_sMode == MODE_OFF)
		{
			Print("CLW: rematch: off: the war stays ENDED, no new round and no engine game end (restart the server for a new war)", LogLevel.NORMAL);
			return false;
		}

		Print(string.Format("CLW: rematch: war over, results for %1 s", s_iDelaySec), LogLevel.NORMAL);
		Print("CLW: rematch: no EndGameMode, so no vanilla end screen: the results are the HUD (winner label), the WAR OVER announcement and this log", LogLevel.NORMAL);
		string winner = winnerKey;
		if (winner.IsEmpty())
			winner = "draw";
		CLW_Debug.Announce(string.Format("Results: %1 (%2). The next round starts in %3 s: back to the setup menu, no reconnect needed.", winner, why, s_iDelaySec));
		s_bPending = true;
		GetGame().GetCallqueue().Remove(Run);
		GetGame().GetCallqueue().Remove(Countdown);
		GetGame().GetCallqueue().CallLater(Run, s_iDelaySec * 1000);
		if (s_iDelaySec > COUNTDOWN_SEC * 2)
			GetGame().GetCallqueue().CallLater(Countdown, (s_iDelaySec - COUNTDOWN_SEC) * 1000);

		return false;
	}

	protected static void Countdown()
	{
		if (s_bPending)
			CLW_Debug.Announce(string.Format("The next round starts in %1 s.", COUNTDOWN_SEC));
	}

	// ---- the reset sequence ----

	protected static void Run()
	{
		s_bPending = false;
		if (!Replication.IsServer())
			return;

		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		CLW_WarState war;
		if (runtime)
			war = runtime.GetWarState();
		if (!runtime || !war || !war.IsEnded())
		{
			Print(string.Format("CLW: rematch: skipped, runtime=%1 war=%2 ended=%3 (the war state is not ENDED any more)", runtime != null, war != null, war && war.IsEnded()), LogLevel.WARNING);
			return;
		}

		array<string> keys = {};
		war.GetFactionKeys(keys);
		Print(string.Format("CLW: rematch: reset begins (round %1 ended, %2 vs %3)", s_iRound, keys[0], keys[1]), LogLevel.NORMAL);
		Print("CLW: rematch: census before: " + Census(keys), LogLevel.NORMAL);

		// 1. nothing may plan, buy or order any more
		int commanders = runtime.RematchStop();
		Print(string.Format("CLW: rematch: reset commanders (%1 stopped, contact poll / think / activity timers removed)", commanders), LogLevel.NORMAL);

		// 2. services and their bookkeeping (CLW_WarState.Finish already did this at war end; idempotent)
		int services;
		foreach (string serviceKey : keys)
		{
			array<IEntity> serviceEntities = {};
			services += CLW_Services.GetAll(serviceKey, "", serviceEntities);
		}

		CLW_Services.OnWarEnded();
		CLW_Services.Reset();
		Print(string.Format("CLW: rematch: reset services (%1 entities still present, links / terminals / structure markers cleared)", services), LogLevel.NORMAL);

		// FOB / convoy state: their entities are services, spawn points and vehicle units, removed by the steps around
		int fobs;
		int convoys;
		foreach (string fobKey : keys)
		{
			fobs += CLW_FobManager.CountLive(fobKey);
			convoys += CLW_Convoys.CountLive(fobKey);
		}

		CLW_FobManager.Reset();
		CLW_Convoys.Reset();
		Print(string.Format("CLW: rematch: reset FOBs (%1 live, state + sites cleared) and convoys (%2 in flight, stock cleared)", fobs, convoys), LogLevel.NORMAL);

		// 3. entities
		CLW_VehicleManager vehicles = runtime.GetVehicleManager();
		if (vehicles)
			Print(string.Format("CLW: rematch: reset vehicle units (%1 vehicles deleted)", vehicles.RematchReset()), LogLevel.NORMAL);
		CLW_ForceSpawner spawner = runtime.GetForceSpawner();
		if (spawner)
			Print(string.Format("CLW: rematch: reset spawner (%1 purchase jobs dropped)", spawner.RematchReset()), LogLevel.NORMAL);
		CLW_SquadRegistry registry = runtime.GetRegistry();
		if (registry)
			Print(string.Format("CLW: rematch: reset squad registry (%1 squads deleted, humans freed, vanilla group entries left %2)", registry.RematchReset(), CLW_SquadGroups.Count()), LogLevel.NORMAL);
		Print(string.Format("CLW: rematch: reset requests (%1 issued vehicles / boxes deleted, history + cooldowns + /hq + reports cleared)", CLW_RequestService.RematchReset()), LogLevel.NORMAL);
		CLW_PendingOrders.RematchReset();
		CLW_BaseWatch.RematchReset();
		CLW_SitrepStore.RematchReset();
		Print("CLW: rematch: reset pending orders, idle-watch ledger, SITREP store (state only)", LogLevel.NORMAL);
		CLW_StructureMarkers.RemoveAll();
		CLW_StructureMarkers.Reset();
		CLW_RequestTerminals.RemoveAll();
		CLW_RequestTerminals.Reset();
		CLW_ControlPoints points = runtime.GetControlPoints();
		if (points)
			Print(string.Format("CLW: rematch: reset control points (%1 map markers removed)", points.RematchReset()), LogLevel.NORMAL);
		CLW_Bases bases = runtime.GetBases();
		if (bases)
			Print(string.Format("CLW: rematch: reset bases (%1 spawn points deleted)", bases.RematchReset()), LogLevel.NORMAL);
		CLW_Places.Invalidate();
		Print("CLW: rematch: census after the reset: " + Census(keys), LogLevel.NORMAL);

		// 4. a new round of the war systems
		s_iRound++;
		if (!runtime.RematchRestart())
		{
			Print("CLW: rematch: ERROR the war systems could not be restarted (no scenario), the session stays without a war", LogLevel.ERROR);
			return;
		}

		CLW_WarState newWar = runtime.GetWarState();
		if (newWar)
		{
			newWar.NotifyStatus();
			Print(string.Format("CLW: rematch: back to setup (round %1), phase %2, setup menu driver restarted", s_iRound, newWar.GetPhaseName()), LogLevel.NORMAL);
		}

		CLW_Debug.Announce(string.Format("Round %1: back to the setup phase. The setup owner opens the menu (/war setup) or the server preset starts the war.", s_iRound));

		// 5. players
		SyncSides();
		GetGame().GetCallqueue().CallLater(RecyclePlayers, PLAYER_SETTLE_MS);
	}

	// One line with the entity counts that must go back to the setup values.
	protected static string Census(notnull array<string> keys)
	{
		int agents;
		AIWorld aiWorld = GetGame().GetAIWorld();
		if (aiWorld)
		{
			array<AIAgent> agentList = {};
			aiWorld.GetAIAgents(agentList);
			agents = agentList.Count();
		}

		int markers;
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (manager)
			markers = manager.GetStaticMarkers().Count();

		int squads;
		int vehicleUnits;
		int fobs;
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		foreach (string key : keys)
		{
			if (runtime && runtime.GetRegistry())
			{
				array<string> ids = {};
				runtime.GetRegistry().GetIds(key, ids);
				squads += ids.Count();
			}

			if (runtime && runtime.GetVehicleManager())
				vehicleUnits += runtime.GetVehicleManager().CountVehicles(key);

			fobs += CLW_FobManager.CountLive(key);
		}

		return string.Format("aiAgents=%1 squads=%2 vehicleUnits=%3 liveFobs=%4 spawnPoints=%5 staticMarkers=%6 players=%7", agents, squads, vehicleUnits, fobs, SCR_SpawnPoint.CountSpawnPoints(), markers, GetGame().GetPlayerManager().GetPlayerCount());
	}

	// ---- players ----

	// The default sides of the new round (playable / hostile) to every client; the server side was set by CLW_Sides.PrepareSetup in the war state init.
	protected static void SyncSides()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		PlayerManager players = GetGame().GetPlayerManager();
		if (!scenario || !players)
			return;

		string keyA = scenario.GetFactionKey(0);
		string keyB = scenario.GetFactionKey(1);
		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
			if (controller)
				controller.CLW_SyncSides_S(keyA, keyB);
		}

		Print(string.Format("CLW: rematch: default sides %1 vs %2 sent to %3 client(s)", keyA, keyB, ids.Count()), LogLevel.NORMAL);
	}

	// Players alive in the finished war go back through the setup flow: a player on a faction that is not the default side switches to it
	// (client-driven, like CLW_Sides.MovePlayers), then the character is deleted so the spawn logic auto-spawns the player at the new base;
	// a player without a character (dead, deploy menu open) is nudged. CLW_SpawnLogic does the spawn in SETUP.
	protected static void RecyclePlayers()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		PlayerManager players = GetGame().GetPlayerManager();
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!scenario || !players || !runtime || !runtime.GetWarState() || runtime.GetWarState().GetPhase() != CLW_EWarPhase.SETUP)
		{
			Print("CLW: rematch: players not recycled (no scenario / war not in SETUP)", LogLevel.WARNING);
			return;
		}

		FactionManager factionManager = GetGame().GetFactionManager();
		string defaultKey = scenario.GetDefaultPlayerFactionKey();
		Faction target;
		if (factionManager)
			target = factionManager.GetFactionByKey(defaultKey);

		CLW_SpawnLogic logic;	// the prefab attribute of CLW_GameMode, or the instance swapped in by the takeover
		SCR_RespawnSystemComponent respawn = SCR_RespawnSystemComponent.GetInstance();
		if (respawn)
			logic = CLW_SpawnLogic.Cast(respawn.GetSpawnLogic());

		array<int> ids = {};
		players.GetPlayers(ids);
		int switched;
		int removed;
		int nudged;
		foreach (int playerId : ids)
		{
			SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
			Faction current = SCR_FactionManager.SGetPlayerFaction(playerId);
			string currentKey;
			if (current)
				currentKey = current.GetFactionKey();

			if (target && controller && currentKey != defaultKey)
			{
				controller.CLW_SwitchFaction_S(factionManager.GetFactionIndex(target));
				switched++;
			}

			IEntity entity = players.GetPlayerControlledEntity(playerId);
			Print(string.Format("CLW: rematch: player %1 (%2) faction=%3 -> %4, character %5", playerId, players.GetPlayerName(playerId), currentKey, defaultKey, entity), LogLevel.NORMAL);
			if (entity)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(entity);	// [verify] -> OnPlayerEntityLost_S -> DoSpawn_S -> AutoSpawn_S (setup)
				removed++;
			}
			else if (logic)
			{
				logic.OnPlayerEntityLost_S(playerId);	// no character (dead / deploy menu): request the setup spawn directly
				nudged++;
			}
		}

		Print(string.Format("CLW: rematch: players recycled: %1 faction switches, %2 characters removed, %3 spawns requested (spawn logic %4)", switched, removed, nudged, logic), LogLevel.NORMAL);
	}

	// ---- statics (world reload) ----

	// CLW_WarRuntime.OnGameModeEnd: forget every static of the finished session so a reloaded world starts clean.
	static void ClearStatics()
	{
		GetGame().GetCallqueue().Remove(Run);
		GetGame().GetCallqueue().Remove(Countdown);
		GetGame().GetCallqueue().Remove(RecyclePlayers);
		s_bPending = false;
		s_iRound = 1;
		CLW_Services.Reset();
		CLW_FobManager.Reset();
		CLW_Convoys.Reset();
		CLW_RequestService.OnGameStart();
		CLW_PendingOrders.RematchReset();
		CLW_BaseWatch.RematchReset();
		CLW_SitrepStore.RematchReset();
		CLW_StructureMarkers.Reset();
		CLW_RequestTerminals.Reset();
		Print("CLW: rematch: statics cleared (world end)", LogLevel.NORMAL);
	}
}

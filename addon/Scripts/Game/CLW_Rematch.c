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
	static const int SWEEP1_MS = 1000;			// leftover sweeps after the reset (SETUP only) ...
	static const int SWEEP2_MS = 6000;
	static const float BASE_AREA_M = 350;		// ... and empty-faction vehicles (depot vehicles, wrecks) this close to an old HQ
	static const float FOB_AREA_M = 120;
	static const int PLAYER_SETTLE_MS = 1500;	// faction switch RPC before the characters are removed (same delay as CLW_Sides.MovePlayers)

	protected static string s_sMode = MODE_IN_PLACE;
	protected static int s_iDelaySec = DEFAULT_DELAY_SEC;
	protected static int s_iRound = 1;			// the round that is running / just ended
	protected static bool s_bPending;
	protected static bool s_bForceRestart;		// RestartNow: the war end that is being forced skips the results delay and the rematch switch
	protected static ref array<string> s_aPrevKeys = {};			// factions of the war that just ended (leftover sweeps)
	protected static ref array<vector> s_aPrevAreas = {};			// its HQ and FOB positions
	protected static ref array<float> s_aPrevAreaRadius = {};
	protected static ref array<CLW_RemoteCommander> s_aOldCommanders = {};	// weak: must be gone after the reset

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
		if (s_bForceRestart)
		{
			Print("CLW: rematch: restart requested (dashboard), no results delay", LogLevel.NORMAL);
			return false;
		}

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
		if (winnerKey.IsEmpty())
			CLW_Debug.Announce(CLW_Loc.W("#CLW-Rematch_ResultsDraw", why, CLW_Loc.N(s_iDelaySec)));
		else
			CLW_Debug.Announce(CLW_Loc.W("#CLW-Rematch_Results", CLW_Loc.FactionName(winnerKey), why, CLW_Loc.N(s_iDelaySec)));
		s_bPending = true;
		GetGame().GetCallqueue().Remove(Run);
		GetGame().GetCallqueue().Remove(Countdown);
		GetGame().GetCallqueue().CallLater(Run, s_iDelaySec * 1000);
		if (s_iDelaySec > COUNTDOWN_SEC * 2)
			GetGame().GetCallqueue().CallLater(Countdown, (s_iDelaySec - COUNTDOWN_SEC) * 1000);

		return false;
	}

	// Dashboard restart (CLW_LoadoutSync, loadouts plan R5): a running war ends as a draw and the reset sequence runs at once, whatever the
	// rematch switch says (inPlace / reload / off) and without the results delay. False in SETUP (nothing to restart) or without a runtime.
	// The next war start re-runs the loadout sync (CLW_WarRuntime.OnWarStarted).
	static bool RestartNow(string whyWire)
	{
		if (!Replication.IsServer())
			return false;

		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		CLW_WarState war;
		if (runtime)
			war = runtime.GetWarState();
		if (!runtime || !war || war.GetPhase() == CLW_EWarPhase.SETUP)
			return false;

		if (war.IsRunning())
		{
			s_bForceRestart = true;
			war.Finish("", EGameOverTypes.ENDREASON_DRAW, whyWire);
			s_bForceRestart = false;
		}

		if (!war.IsEnded())
			return false;

		GetGame().GetCallqueue().Remove(Run);
		GetGame().GetCallqueue().Remove(Countdown);
		Run();
		return true;
	}

	protected static void Countdown()
	{
		if (s_bPending)
			CLW_Debug.Announce(CLW_Loc.W("#CLW-Rematch_Countdown", CLW_Loc.N(COUNTDOWN_SEC)));
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

		RememberWar(runtime, keys);
		CensusDetail("before");

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
		CLW_FriendlyPicture.RematchReset();
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
		CensusDetail("after the reset (deletions may still be pending)");

		// 4. a new round of the war systems
		s_iRound++;
		if (!runtime.RematchRestart())
		{
			Print("CLW: rematch: ERROR the war systems could not be restarted (no scenario), the session stays without a war", LogLevel.ERROR);
			return;
		}

		LogCommanders(runtime, "round start");
		GetGame().GetCallqueue().CallLater(SweepSetup, SWEEP1_MS, false, "sweep 1");
		GetGame().GetCallqueue().CallLater(SweepSetup, SWEEP2_MS, false, "sweep 2");
		CLW_WarState newWar = runtime.GetWarState();
		if (newWar)
		{
			newWar.NotifyStatus();
			Print(string.Format("CLW: rematch: back to setup (round %1), phase %2, setup menu driver restarted", s_iRound, newWar.GetPhaseName()), LogLevel.NORMAL);
		}

		CLW_Debug.Announce(CLW_Loc.W("#CLW-Rematch_Round", CLW_Loc.N(s_iRound)));

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


	// ---- leftovers ----

	// Before the reset: the finished war's factions, HQ / FOB areas and (weak) commanders, for the sweeps and the commander check.
	protected static void RememberWar(CLW_WarRuntime runtime, notnull array<string> keys)
	{
		s_aPrevKeys.Clear();
		s_aPrevAreas.Clear();
		s_aPrevAreaRadius.Clear();
		s_aOldCommanders.Clear();
		CLW_Bases bases = runtime.GetBases();
		foreach (string key : keys)
		{
			s_aPrevKeys.Insert(key);
			if (bases && bases.GetBasePos(key) != vector.Zero)
			{
				s_aPrevAreas.Insert(bases.GetBasePos(key));
				s_aPrevAreaRadius.Insert(BASE_AREA_M);
			}

			array<CLW_Fob> fobList = {};
			CLW_FobManager.GetFobs(key, fobList, false, true);
			foreach (CLW_Fob fob : fobList)
			{
				s_aPrevAreas.Insert(fob.m_vPos);
				s_aPrevAreaRadius.Insert(FOB_AREA_M);
			}

			CLW_RemoteCommander commander = runtime.GetCommander(key);
			if (commander)
				s_aOldCommanders.Insert(commander);
		}
	}

	// "commanders alive n": the commanders of the new round, and how many objects of the old round are still referenced (must be 0).
	protected static void LogCommanders(CLW_WarRuntime runtime, string when)
	{
		int alive;
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario)
		{
			array<string> keys = {};
			scenario.GetFactionKeys(keys);
			foreach (string key : keys)
			{
				if (runtime.GetCommander(key))
					alive++;
			}
		}

		int old;
		foreach (CLW_RemoteCommander previous : s_aOldCommanders)
		{
			if (previous)
				old++;
		}

		Print(string.Format("CLW: rematch: commanders alive %1 (%2; previous round objects still referenced: %3, must be 0)", alive, when, old), LogLevel.NORMAL);
	}

	// CLW_WarRuntime.OnWarStarted, before the start forces: nothing of the previous round may be left on the map.
	static void OnWarStart()
	{
		if (s_iRound <= 1 || !Replication.IsServer())
			return;

		Sweep("round start", false);
		CensusDetail("round start (after the sweep)");
	}

	protected static void SweepSetup(string when)
	{
		Sweep(when, true);
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (runtime)
			LogCommanders(runtime, when);
	}

	protected static bool IsPlayerEntity(IEntity entity)
	{
		return entity && GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(entity) > 0;
	}

	protected static string VehicleFactionKey(IEntity vehicle)
	{
		FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(vehicle.FindComponent(FactionAffiliationComponent));
		if (!affiliation)
			return "";

		Faction faction = affiliation.GetAffiliatedFaction();
		if (faction)
			return faction.GetFactionKey();

		return affiliation.GetDefaultFactionKey();
	}

	protected static bool HasPlayerAboard(IEntity vehicle)
	{
		BaseCompartmentManagerComponent compartments = BaseCompartmentManagerComponent.Cast(vehicle.FindComponent(BaseCompartmentManagerComponent));
		if (!compartments)
			return false;

		array<BaseCompartmentSlot> slots = {};
		compartments.GetCompartments(slots);
		foreach (BaseCompartmentSlot slot : slots)
		{
			if (slot && IsPlayerEntity(slot.GetOccupant()))
				return true;
		}

		return false;
	}

	protected static bool NearOldArea(vector pos)
	{
		foreach (int i, vector area : s_aPrevAreas)
		{
			if (vector.DistanceXZ(pos, area) <= s_aPrevAreaRadius[i])
				return true;
		}

		return false;
	}

	protected static bool IsWreck(IEntity vehicle)
	{
		DamageManagerComponent damage = DamageManagerComponent.Cast(vehicle.FindComponent(DamageManagerComponent));
		return damage && damage.GetState() == EDamageState.DESTROYED;
	}

	protected static void FindVehicles(notnull array<IEntity> outVehicles)
	{
		BaseWorld world = GetGame().GetWorld();
		vector mins, maxs;
		world.GetBoundBox(mins, maxs);
		vector center = (mins + maxs) * 0.5;
		CLW_RematchVehicleQuery query = new CLW_RematchVehicleQuery();
		world.QueryEntitiesBySphere(center, vector.Distance(mins, maxs) * 0.5 + 10, query.OnEntity, null, EQueryEntitiesFlags.DYNAMIC);
		outVehicles.InsertAll(query.m_aFound);
	}

	protected static string CountText(notnull map<string, int> counts)
	{
		string text;
		foreach (string key, int count : counts)
		{
			text += key + "=" + count + " ";
		}

		return text;
	}

	protected static void CountUp(notnull map<string, int> counts, string key)
	{
		if (key.IsEmpty())
			key = "none";
		int n;
		counts.Find(key, n);
		counts.Set(key, n + 1);
	}

	// Agent -> its group (master for a slave) and character entity (null for a group agent).
	protected static SCR_AIGroup GroupOfAgent(AIAgent agent, out IEntity character)
	{
		SCR_AIGroup group = SCR_AIGroup.Cast(agent);
		if (!group)
		{
			group = SCR_AIGroup.Cast(agent.GetParentGroup());
			character = agent.GetControlledEntity();
		}

		if (group && group.IsSlave())
			group = group.GetMaster();

		return group;
	}

	protected static string AgentFactionKey(SCR_AIGroup group, IEntity character)
	{
		string key;
		if (group)
			key = CLW_SquadRegistry.GetFactionKey(group);
		if (key.IsEmpty())
		{
			SCR_ChimeraCharacter chimera = SCR_ChimeraCharacter.Cast(character);
			if (chimera)
				key = chimera.GetFactionKey();
		}

		return key;
	}

	// Entity counts by category: groups, agents and vehicles per faction, wrecks.
	protected static void CensusDetail(string when)
	{
		map<string, int> agentsBy = new map<string, int>();
		map<string, int> vehiclesBy = new map<string, int>();
		array<SCR_AIGroup> seen = {};
		AIWorld aiWorld = GetGame().GetAIWorld();
		if (aiWorld)
		{
			array<AIAgent> agents = {};
			aiWorld.GetAIAgents(agents);
			foreach (AIAgent agent : agents)
			{
				if (!agent)
					continue;

				IEntity character;
				SCR_AIGroup group = GroupOfAgent(agent, character);
				if (group && !seen.Contains(group))
					seen.Insert(group);
				if (character)
					CountUp(agentsBy, AgentFactionKey(group, character));
			}
		}

		int wrecks;
		array<IEntity> vehicles = {};
		FindVehicles(vehicles);
		foreach (IEntity vehicle : vehicles)
		{
			CountUp(vehiclesBy, VehicleFactionKey(vehicle));
			if (IsWreck(vehicle))
				wrecks++;
		}

		Print(string.Format("CLW: rematch: census %1: groups=%2 agents{ %3} vehicles{ %4} wrecks=%5", when, seen.Count(), CountText(agentsBy), CountText(vehiclesBy), wrecks), LogLevel.NORMAL);
	}

	// Deletes what the finished war left behind: AI of its factions that are not players (groups, orphan agents), vehicles of its factions
	// and empty-faction vehicles / wrecks inside its old base areas (depot vehicles). requireSetup: only while the new war has not started.
	protected static void Sweep(string when, bool requireSetup)
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime || !runtime.GetWarState() || s_aPrevKeys.IsEmpty())
			return;

		if (requireSetup && runtime.GetWarState().GetPhase() != CLW_EWarPhase.SETUP)
			return;

		int groups;
		int loose;
		AIWorld aiWorld = GetGame().GetAIWorld();
		if (aiWorld)
		{
			array<AIAgent> agents = {};
			aiWorld.GetAIAgents(agents);
			array<SCR_AIGroup> doomed = {};
			array<IEntity> orphans = {};
			foreach (AIAgent agent : agents)
			{
				if (!agent)
					continue;

				IEntity character;
				SCR_AIGroup group = GroupOfAgent(agent, character);
				if (!s_aPrevKeys.Contains(AgentFactionKey(group, character)) || IsPlayerEntity(character))
					continue;

				if (group)
				{
					if (group.GetPlayerCount() == 0 && !doomed.Contains(group))
						doomed.Insert(group);
				}
				else if (character && !orphans.Contains(character))
					orphans.Insert(character);
			}

			foreach (SCR_AIGroup victim : doomed)
			{
				string prefab;
				EntityPrefabData prefabData = victim.GetPrefabData();
				if (prefabData)
					prefab = prefabData.GetPrefabName();
				Print(string.Format("CLW: rematch: leftover group '%1' (%2) faction=%3 agents=%4 at %5 deleted (%6)", victim.GetName(), prefab, CLW_SquadRegistry.GetFactionKey(victim), victim.GetAgentsCount(), victim.GetOrigin(), when), LogLevel.NORMAL);
				SCR_AIGroup slave = victim.GetSlave();
				CLW_ForceSpawner.DeleteGroupAndMembers(victim);
				if (slave)
					SCR_EntityHelper.DeleteEntityAndChildren(slave);
				groups++;
			}

			foreach (IEntity orphan : orphans)
			{
				Print(string.Format("CLW: rematch: leftover agent %1 at %2 deleted (%3)", orphan, orphan.GetOrigin(), when), LogLevel.NORMAL);
				SCR_EntityHelper.DeleteEntityAndChildren(orphan);
				loose++;
			}
		}

		int vehiclesDeleted;
		int wrecksDeleted;
		array<IEntity> vehicles = {};
		FindVehicles(vehicles);
		foreach (IEntity vehicle : vehicles)
		{
			string key = VehicleFactionKey(vehicle);
			vector pos = vehicle.GetOrigin();
			bool ours = s_aPrevKeys.Contains(key);
			if (!ours && !(key.IsEmpty() && NearOldArea(pos)))
				continue;

			if (HasPlayerAboard(vehicle))
				continue;

			bool wreck = IsWreck(vehicle);
			string vehiclePrefab;
			EntityPrefabData vehicleData = vehicle.GetPrefabData();
			if (vehicleData)
				vehiclePrefab = vehicleData.GetPrefabName();
			Print(string.Format("CLW: rematch: leftover vehicle (%1) faction='%2' wreck=%3 at %4 deleted (%5)", vehiclePrefab, key, wreck, pos, when), LogLevel.NORMAL);
			SCR_EntityHelper.DeleteEntityAndChildren(vehicle);
			vehiclesDeleted++;
			if (wreck)
				wrecksDeleted++;
		}

		Print(string.Format("CLW: rematch: %1: leftovers deleted: %2 groups, %3 orphan agents, %4 vehicles (%5 wrecks)", when, groups, loose, vehiclesDeleted, wrecksDeleted), LogLevel.NORMAL);
	}

	// ---- statics (world reload) ----

	// CLW_WarRuntime.OnGameModeEnd: forget every static of the finished session so a reloaded world starts clean.
	static void ClearStatics()
	{
		GetGame().GetCallqueue().Remove(Run);
		GetGame().GetCallqueue().Remove(Countdown);
		GetGame().GetCallqueue().Remove(RecyclePlayers);
		GetGame().GetCallqueue().Remove(SweepSetup);
		s_aPrevKeys.Clear();
		s_aPrevAreas.Clear();
		s_aPrevAreaRadius.Clear();
		s_aOldCommanders.Clear();
		s_bPending = false;
		s_iRound = 1;
		CLW_Services.Reset();
		CLW_FobManager.Reset();
		CLW_Convoys.Reset();
		CLW_RequestService.OnGameStart();
		CLW_PendingOrders.RematchReset();
		CLW_BaseWatch.RematchReset();
		CLW_SitrepStore.RematchReset();
		CLW_FriendlyPicture.RematchReset();
		CLW_StructureMarkers.Reset();
		CLW_RequestTerminals.Reset();
		Print("CLW: rematch: statics cleared (world end)", LogLevel.NORMAL);
	}
}

// QueryEntitiesBySphere callback holder: every Vehicle in the query volume.
class CLW_RematchVehicleQuery
{
	ref array<IEntity> m_aFound = {};

	bool OnEntity(IEntity entity)
	{
		if (Vehicle.Cast(entity))
			m_aFound.Insert(entity);

		return true;
	}
}

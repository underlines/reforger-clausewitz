// Clausewitz game mode: AI High Command war with human players on both sides.
// Inherits the whole player flow (spawn, groups, tasks, markers) from GameMode_Base.et.

class CLW_GameModeClass : SCR_BaseGameModeClass
{
}

class CLW_GameMode : SCR_BaseGameMode
{
	protected static const bool RUN_ORDER_SPIKE = false;
	protected static const bool RUN_REST_SPIKE = false;	// M1 test (needs dev/tools/echo_server), off for normal play
	protected static const int SQUAD_RETRY_MS = 5000;		// player squad search interval
	protected static const int SQUAD_RETRY_MAX = 24;		// 2 min, then a free infantry squad is spawned
	protected static const int SQUAD_RETRY_EXTRA = 12;		// further tries for that squad to appear

	[Attribute("", UIWidgets.ResourceNamePicker, "Scenario config: factions, bases, objectives, economy (spec 0a). Empty = CLW_WarConfig.DEFAULT_SCENARIO", "conf")]
	protected ResourceName m_ScenarioConfig;

	protected static CLW_GameMode s_Instance;

	// M2 war systems (server only, started in this order: scenario -> bases -> control points -> war state -> registry/commanders).
	protected ref CLW_Bases m_Bases;
	protected ref CLW_ControlPoints m_Points;
	protected ref CLW_WarState m_War;
	protected ref CLW_ForceCatalog m_Catalog;
	protected ref CLW_VehicleManager m_Vehicles;
	protected ref CLW_ForceSpawner m_Spawner;
	protected ref CLW_Activity m_Activity = new CLW_Activity();

	// Which AI groups HQ commands (scene-placed and added at runtime, e.g. from Game Master); the commanders and markers query it.
	protected ref CLW_SquadRegistry m_Registry = new CLW_SquadRegistry();
	// One contact picture, marker set and commander per scenario faction, keyed by faction key. The keys can change at war start
	// (faction choice in the setup), so these are (re)created from the scenario in BuildFactionObjects, never cached at init.
	protected ref map<string, ref CLW_ContactBook> m_mBooks = new map<string, ref CLW_ContactBook>();
	protected ref map<string, ref CLW_ContactMarkers> m_mMarkers = new map<string, ref CLW_ContactMarkers>();
	protected ref map<string, ref CLW_RemoteCommander> m_mCommanders = new map<string, ref CLW_RemoteCommander>();
	protected ref array<string> m_aBuiltKeys = {};

	static CLW_GameMode GetInstance()
	{
		return s_Instance;
	}

	static CLW_ScenarioConfig GetScenario()
	{
		return CLW_ScenarioConfig.Get();
	}

	static CLW_Bases GetBases()
	{
		if (!s_Instance)
			return null;

		return s_Instance.m_Bases;
	}

	static CLW_ControlPoints GetControlPoints()
	{
		if (!s_Instance)
			return null;

		return s_Instance.m_Points;
	}

	static CLW_WarState GetWarState()
	{
		if (!s_Instance)
			return null;

		return s_Instance.m_War;
	}

	static CLW_SquadRegistry GetRegistry()
	{
		if (!s_Instance)
			return null;

		return s_Instance.m_Registry;
	}

	static CLW_ForceCatalog GetForceCatalog()
	{
		if (!s_Instance)
			return null;

		return s_Instance.m_Catalog;
	}

	static CLW_ForceSpawner GetForceSpawner()
	{
		if (!s_Instance)
			return null;

		return s_Instance.m_Spawner;
	}

	// The faction's commander (LLM bridge wrapping the rule commander), null for an unknown key.
	static CLW_RemoteCommander GetCommander(string factionKey)
	{
		if (!s_Instance)
			return null;

		CLW_RemoteCommander commander;
		if (s_Instance.m_mCommanders.Find(factionKey, commander))
			return commander;

		return null;
	}

	static CLW_VehicleManager GetVehicleManager()
	{
		if (!s_Instance)
			return null;

		return s_Instance.m_Vehicles;
	}

	// War SETUP -> RUNNING: build the force catalogs (factions may have been chosen in the setup) and field the start forces.
	protected void OnWarStarted()
	{
		Print("CLW: war started: building force catalogs and spawning start forces", LogLevel.NORMAL);
		if (m_Catalog)
			m_Catalog.BuildAll();
		if (m_Spawner)
			m_Spawner.SpawnStartForces();
		CLW_SquadGroups.LockNewGroups();	// HQ squads are the only player groups; the registry registers them in its next sweep

		// the factions are final now: rebuild books, markers and commanders from the scenario's current keys
		BuildFactionObjects();
		CLW_MissionHook.Get().OnWarStarted();
	}

	// (Re)create the per-faction contact book, marker set and commander from the scenario's current faction keys.
	protected void BuildFactionObjects()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario)
			return;

		foreach (string oldKey, CLW_ContactMarkers oldMarkers : m_mMarkers)
		{
			if (oldMarkers)
				oldMarkers.RemoveAll();
		}

		m_mBooks.Clear();
		m_mMarkers.Clear();
		m_mCommanders.Clear();
		m_aBuiltKeys.Clear();
		array<string> keys = {};
		scenario.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			CLW_ContactBook book = new CLW_ContactBook();
			CLW_RemoteCommander commander = new CLW_RemoteCommander();
			commander.Init(key, book, m_Registry);
			m_mBooks.Set(key, book);
			m_mMarkers.Set(key, new CLW_ContactMarkers());
			m_mCommanders.Set(key, commander);
			m_aBuiltKeys.Insert(key);
		}

		Print("CLW: faction objects (contact books, markers, commanders) built for " + keys.Count() + " factions", LogLevel.NORMAL);
	}

	// Rebuild when the scenario's faction keys differ from the ones the objects were built for.
	protected void EnsureFactionObjects()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario)
			return;

		array<string> keys = {};
		scenario.GetFactionKeys(keys);
		bool same = keys.Count() == m_aBuiltKeys.Count();
		if (same)
		{
			foreach (string key : keys)
			{
				if (!m_aBuiltKeys.Contains(key))
					same = false;
			}
		}

		if (!same)
		{
			Print("CLW: scenario faction keys changed, rebuilding faction objects", LogLevel.NORMAL);
			BuildFactionObjects();
		}
	}

	protected override void OnGameStart()
	{
		super.OnGameStart();
		s_Instance = this;
		Print("CLW: build " + CLW_Build.STAMP, LogLevel.NORMAL);
		Print("CLW: game mode started", LogLevel.NORMAL);
		CLW_PlayerSquad.ApplyCommandingLimit();

		if (Replication.IsServer())
		{
			if (RUN_REST_SPIKE)
				GetGame().GetCallqueue().CallLater(CLW_RestSpike.Run, 10000);
			if (RUN_ORDER_SPIKE)
				GetGame().GetCallqueue().CallLater(SpikeOrderLoop, 15000);

			// 1. scenario data (factions, bases, objectives, economy)
			CLW_ScenarioConfig scenario = LoadScenario();
			if (scenario)
			{
				// 2. bases + respawn points, 3. control points, 4. war state
				m_Bases = new CLW_Bases();
				m_Bases.Init(scenario);
				m_Points = new CLW_ControlPoints();
				m_Points.Init(scenario);
				m_War = new CLW_WarState();
				m_War.Init(scenario, m_Points);
				// 5. forces: catalog (built at war start), vehicle units, spawner; start forces when the war starts
				m_Catalog = new CLW_ForceCatalog();
				m_Registry.SetCatalog(m_Catalog);
				m_Vehicles = new CLW_VehicleManager();
				m_Vehicles.Init(m_Registry);
				m_Spawner = new CLW_ForceSpawner();
				m_Spawner.Init(m_Catalog, m_Registry, m_Vehicles);
				m_War.GetOnWarStarted().Insert(OnWarStarted);
				// 6. missions for human squad leaders (replaces the rule commander's legacy forced task)
				CLW_MissionBoard.Install();
			}
			else
			{
				Print("CLW: NO SCENARIO: war systems, registry adoption and commanders are disabled", LogLevel.ERROR);
			}

			// Squad join service: vanilla group events -> commandable squads; HQ squads are the only player groups.
			CLW_SquadJoin.Init();
			CLW_SquadGroups.LockNewGroups();
			GetOnPlayerRegistered().Insert(OnClwPlayerRegistered);
			GetGame().GetCallqueue().CallLater(PruneVanillaGroups, 3000);	// after the groups manager made its predefined groups

			// Adopt the scene-placed squads now (before the first commander tick), then keep sweeping for new/lost groups.
			m_Registry.Start();
			// The commanders take their squads from the registry; human-led squads get missions instead of waypoints.
			if (scenario)
				BuildFactionObjects();

			GetGame().GetCallqueue().CallLater(CLW_WorldInfo.Start, 5000);
			GetGame().GetCallqueue().CallLater(SayBuild, 8000);
			if (scenario)
			{
				GetGame().GetCallqueue().CallLater(PollContacts, 20000, true);
				m_Activity.Start();
				GetGame().GetCallqueue().CallLater(ThinkCommanders, 10000, true);
			}
		}
	}

	// Ticket counter HUD: server -> every client (also run locally for the listen-server host; the broadcast does not loop back).
	void CLW_BroadcastWarStatus(string wire)
	{
		if (!Replication.IsServer())
			return;

		Rpc(CLW_RpcAsk_WarStatus, wire);
		if (SCR_PlayerController.GetLocalPlayerId() > 0)
			CLW_WarHud.Update(wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Broadcast)]
	protected void CLW_RpcAsk_WarStatus(string wire)
	{
		CLW_WarHud.Update(wire);
	}

	// Scenario from the prefab attribute, else the default path (logged).
	protected CLW_ScenarioConfig LoadScenario()
	{
		CLW_ScenarioConfig scenario;
		if (!m_ScenarioConfig.IsEmpty())
		{
			scenario = CLW_ScenarioConfig.Load(m_ScenarioConfig);
			if (scenario)
				return scenario;

			Print("CLW: scenario attribute failed, trying the default path", LogLevel.WARNING);
		}
		else
		{
			Print("CLW: no scenario attribute on the game mode prefab, using the default path", LogLevel.WARNING);
		}

		return CLW_ScenarioConfig.Load(CLW_WarConfig.DEFAULT_SCENARIO);
	}

	// Respawn gate: a faction without tickets (or a finished war) cannot spawn players.
	override bool CanPlayerSpawn_S(SCR_SpawnRequestComponent requestComponent, SCR_SpawnHandlerComponent handlerComponent, SCR_SpawnData data, out SCR_ESpawnResult result = SCR_ESpawnResult.SPAWN_NOT_ALLOWED)
	{
		if (!super.CanPlayerSpawn_S(requestComponent, handlerComponent, data, result))
			return false;

		if (!m_War || !requestComponent)
			return true;

		Faction faction = SCR_FactionManager.SGetPlayerFaction(requestComponent.GetPlayerId());
		if (faction && CLW_ScenarioConfig.IsClwFaction(faction.GetFactionKey()) && !m_War.CanFactionSpawn(faction.GetFactionKey()))
		{
			Print(string.Format("CLW: respawn denied for player %1 faction %2 (no tickets / war over)", requestComponent.GetPlayerId(), faction.GetFactionKey()), LogLevel.NORMAL);
			result = SCR_ESpawnResult.SPAWN_NOT_ALLOWED;
			return false;
		}

		return true;
	}

	// Kill accounting: every destroyed controllable (AI, player, vehicle) costs its faction tickets.
	protected override void OnControllableDestroyedEx(notnull SCR_InstigatorContextData instigatorContextData)
	{
		super.OnControllableDestroyedEx(instigatorContextData);
		if (m_War && Replication.IsServer())
			m_War.OnEntityDestroyed(instigatorContextData.GetVictimEntity(), instigatorContextData.GetVictimPlayerID());
	}

	// Players: the war state de-duplicates against OnControllableDestroyedEx by entity.
	protected override void OnPlayerKilledEx(notnull SCR_InstigatorContextData instigatorContextData)
	{
		super.OnPlayerKilledEx(instigatorContextData);
		if (m_War && Replication.IsServer())
			m_War.OnEntityDestroyed(instigatorContextData.GetVictimEntity(), instigatorContextData.GetVictimPlayerID());
	}

	protected void SayBuild()
	{
		CLW_Debug.Say("build " + CLW_Build.STAMP);
	}

	// Perception -> contact book -> map markers, for both factions.
	protected void PollContacts()
	{
		EnsureFactionObjects();
		foreach (string key : m_aBuiltKeys)
		{
			UpdateFaction(key, m_mBooks.Get(key), m_mMarkers.Get(key));
		}
	}

	protected void UpdateFaction(string factionKey, CLW_ContactBook book, CLW_ContactMarkers markers)
	{
		if (!book || !markers)
			return;

		array<string> squads = {};
		m_Registry.GetIds(factionKey, squads);
		foreach (string name : squads)
		{
			SCR_AIGroup group = m_Registry.Find(name);
			if (!group)
				continue;

			array<ref CLW_Contact> sightings = {};
			CLW_Contact.FromGroup(group, name, sightings);
			book.Report(factionKey, sightings);

			vector leaderPos;
			IEntity squadLeader = CLW_SquadInfo.LeaderEntity(group);
			if (squadLeader)
				leaderPos = squadLeader.GetOrigin();
			array<IEntity> members = CLW_SquadInfo.Members(group);
			float maxDist;
			foreach (IEntity member : members)
			{
				maxDist = Math.Max(maxDist, vector.Distance(leaderPos, member.GetOrigin()));
			}
			Print(string.Format("CLW: squad %1 leader=%2 players=%3 firstPlayerLeader=%4 humanLeader=%5 leaderId=%6 waypoint=%7", name, leaderPos, group.GetPlayerCount(), group.GetFirstPlayerLeaderID(), CLW_RuleCommander.HasHumanLeader(group), group.GetLeaderID(), group.GetCurrentWaypoint() != null), LogLevel.NORMAL);
			SCR_AIGroup slave = group.GetSlave();
			if (slave)
			{
				vector slaveWp;
				AIWaypoint slaveCurrent = slave.GetCurrentWaypoint();
				if (slaveCurrent)
					slaveWp = slaveCurrent.GetOrigin();
				Print(string.Format("CLW: squad %1 slave=%2 slaveAgents=%3 slaveWaypoint=%4 slaveWpPos=%5 groupId=%6", name, slave, slave.GetAgentsCount(), slaveCurrent != null, slaveWp, group.GetGroupID()), LogLevel.NORMAL);
			}
			else
			{
				Print(string.Format("CLW: squad %1 slave=none groupId=%2", name, group.GetGroupID()), LogLevel.NORMAL);
			}
			PrintFormat("CLW: squad %1 agents=%2 alive=%3 aiMembers=%4 maxDist=%5", name, group.GetAgentsCount(), CLW_SquadInfo.AliveCount(group), members.Count(), Math.Round(maxDist), level: LogLevel.NORMAL);
			if (CLW_SquadInfo.AliveCount(group) <= 2)
				CLW_SquadInfo.LogMembers(name, group);
		}

		array<int> removed = {};
		book.Expire(removed);
		markers.Remove(removed);

		array<CLW_Contact> known = {};
		book.GetContacts(known);
		markers.Sync(factionKey, known);
		Print(string.Format("CLW: %1 book contacts=%2 removed=%3", factionKey, known.Count(), removed.Count()), LogLevel.NORMAL);
		foreach (CLW_Contact contact : known)
		{
			Print("CLW: contact " + contact.Describe(), LogLevel.NORMAL);
		}
	}

	protected void ThinkCommanders()
	{
		EnsureFactionObjects();
		foreach (string key : m_aBuiltKeys)
		{
			CLW_RemoteCommander commander = m_mCommanders.Get(key);
			if (commander)
				commander.Think();
		}
	}

	// A client connected: tell it which HQ squads are in the vanilla group lists (after its controller is up).
	protected void OnClwPlayerRegistered(int playerId)
	{
		GetGame().GetCallqueue().CallLater(CLW_SquadGroups.MirrorAll, 3000, false, playerId);
	}

	protected void PruneVanillaGroups()
	{
		if (m_Registry)
			CLW_SquadGroups.PruneVanillaGroups(m_Registry);
	}

	// Player flow: after the spawn (auto in SETUP, deploy menu later) the player is in an HQ squad (menu choice) or gets the fallback
	// auto-join into the AI rifle squad of their faction.
	override void OnPlayerSpawnFinalize_S(SCR_SpawnRequestComponent requestComponent, SCR_SpawnHandlerComponent handlerComponent, SCR_SpawnData data, IEntity entity)
	{
		super.OnPlayerSpawnFinalize_S(requestComponent, handlerComponent, data, entity);

		int playerId = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(entity);
		Faction faction = SCR_FactionManager.SGetPlayerFaction(playerId);
		Print(string.Format("CLW: player %1 spawned faction=%2 at %3", playerId, faction, entity.GetOrigin()), LogLevel.NORMAL);
		if (m_War)
			m_War.OnHumanPlayerSpawned(playerId);
		if (!faction)
			return;

		CLW_SquadGroups.MirrorAll(playerId);	// idempotent safety net for clients that connected before the squads existed
		TryJoinPlayer(playerId, faction.GetFactionKey(), 0);
	}

	// First adopted infantry squad of the faction that is led by a human (more players join it), else the first free one.
	// The scenario's preferred squad name wins when it is adopted and of this faction.
	protected SCR_AIGroup FindStartSquad(string factionKey)
	{
		m_Registry.Sweep();
		string preferredSquad;
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario)
		{
			CLW_FactionSetup factionSetup = scenario.FindFaction(factionKey);
			if (factionSetup)
				preferredSquad = factionSetup.m_sPreferredSquad;
		}

		if (!preferredSquad.IsEmpty())
		{
			SCR_AIGroup preferred = m_Registry.Find(preferredSquad);
			if (preferred && CLW_SquadRegistry.GetFactionKey(preferred) == factionKey && !preferred.IsFull())
				return preferred;
		}

		array<string> ids = {};
		m_Registry.GetIds(factionKey, ids);
		SCR_AIGroup firstFree;
		foreach (string id : ids)
		{
			SCR_AIGroup group = m_Registry.Find(id);
			if (!group || !m_Registry.IsJoinable(group) || group.IsFull())
				continue;

			if (CLW_SquadInfo.HumanLeaderEntity(group))
				return group;

			if (!firstFree)
				firstFree = group;
		}

		return firstFree;
	}

	// Join the player to a squad; while there is none (start forces come with the war start) retry every 5 s for 2 min, then spawn
	// a free infantry squad of the faction and join that. Only counts attempts while the war is running (the setup phase keeps the
	// player alone at the base).
	protected void TryJoinPlayer(int playerId, string factionKey, int attempt)
	{
		if (!GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId))
			return;	// player gone / dead: the next spawn starts a new search

		Faction current = SCR_FactionManager.SGetPlayerFaction(playerId);
		if (current)
			factionKey = current.GetFactionKey();

		bool running = m_War && m_War.IsRunning();
		SCR_AIGroup squad;
		if (running)
		{
			// entry point 1/2: the player already picked an HQ squad (deploy menu / groups menu): the hooks establish it, we only
			// make sure (idempotent). Anything else (no group, a vanilla group) = fallback auto-join below.
			SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
			SCR_AIGroup chosen;
			if (groups)
				chosen = groups.GetPlayerGroup(playerId);
			if (chosen && m_Registry.IsJoinable(chosen))
			{
				Print(string.Format("CLW: player %1 is in HQ squad %2 (chosen): establishing", playerId, chosen), LogLevel.NORMAL);
				CLW_PlayerSquad.Establish(chosen, playerId);
				return;
			}

			squad = FindStartSquad(factionKey);
		}

		if (squad)
		{
			Print(string.Format("CLW: player %1 joins squad %2 (after %3 retries)", playerId, squad, attempt), LogLevel.NORMAL);
			CLW_PlayerSquad.Join(squad, playerId);
			Print(string.Format("CLW: player %1 in squad=%2 players=%3 leaderId=%4", playerId, squad.IsPlayerInGroup(playerId), squad.GetPlayerCount(), squad.GetLeaderID()), LogLevel.NORMAL);
			return;
		}

		if (!running)
		{
			if (attempt == 0)
				Print(string.Format("CLW: player %1 waits for the war start to get a squad (setup: alone at the base)", playerId), LogLevel.NORMAL);
			GetGame().GetCallqueue().CallLater(TryJoinPlayer, SQUAD_RETRY_MS, false, playerId, factionKey, attempt);
			return;
		}

		attempt++;
		if (attempt == SQUAD_RETRY_MAX && m_Spawner && m_Catalog)
		{
			CLW_ForceEntry best;
			array<CLW_ForceEntry> entries = {};
			m_Catalog.GetEntries(factionKey, entries);
			foreach (CLW_ForceEntry entry : entries)
			{
				if (entry.m_sKind != CLW_ForceKind.INFANTRY || entry.HasVehicle())
					continue;

				if (!best || entry.m_iGroupMembers > best.m_iGroupMembers)
					best = entry;
			}

			string error;
			if (best && m_Spawner.SpawnFree(factionKey, best.m_sType, "player squad (none available)", error))
				Print(string.Format("CLW: player %1 got no squad in %2 retries: spawning a free %3", playerId, attempt, best.m_sType), LogLevel.WARNING);
			else
				Print(string.Format("CLW: player %1 got no squad and no infantry entry could be spawned: %2", playerId, error), LogLevel.ERROR);
		}
		else if (attempt > SQUAD_RETRY_MAX + SQUAD_RETRY_EXTRA)
		{
			Print(string.Format("CLW: player %1 gave up waiting for a squad", playerId), LogLevel.ERROR);
			return;
		}

		GetGame().GetCallqueue().CallLater(TryJoinPlayer, SQUAD_RETRY_MS, false, playerId, factionKey, attempt);
	}

	// M0 spike: squads spawned, then a canned MOVE order -> waypoint, then check the squad moved.
	protected void SpikeOrderLoop()
	{
		LogSquad("CLW_Squad_US_1");
		LogSquad("CLW_Squad_USSR_1");

		SCR_AIGroup group = FindSquad("CLW_Squad_US_1");
		if (!group || !group.GetLeaderEntity())
			return;

		// Advance on the enemy squad to about 100 m short of it, so perception has something to see.
		vector target = group.GetLeaderEntity().GetOrigin() + Vector(0, 0, -300);
		SCR_AIGroup enemy = FindSquad("CLW_Squad_USSR_1");
		if (enemy && enemy.GetLeaderEntity())
		{
			vector toEnemy = enemy.GetLeaderEntity().GetOrigin() - group.GetLeaderEntity().GetOrigin();
			toEnemy[1] = 0;
			target = enemy.GetLeaderEntity().GetOrigin() - toEnemy.Normalized() * 100;
		}
		target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
		if (IssueMove(group, target))
			Print("CLW: MOVE issued to CLW_Squad_US_1 -> " + target.ToString(), LogLevel.NORMAL);

		LogOrderState(group);
		foreach (int delay : {5000, 15000, 30000, 60000})
		{
			GetGame().GetCallqueue().CallLater(LogOrderState, delay, false, group);
		}
	}

	protected void LogOrderState(SCR_AIGroup group)
	{
		array<AIWaypoint> waypoints = {};
		group.GetWaypoints(waypoints);
		AIWaypoint current = group.GetCurrentWaypoint();
		vector leaderPos;
		if (group.GetLeaderEntity())
			leaderPos = group.GetLeaderEntity().GetOrigin();

		Print(string.Format("CLW: order state waypoints=%1 current=%2 leader=%3", waypoints.Count(), current, leaderPos), LogLevel.NORMAL);
	}

	protected bool IssueMove(SCR_AIGroup group, vector target)
	{
		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = target;

		AIWaypoint waypoint = AIWaypoint.Cast(GetGame().SpawnEntityPrefab(Resource.Load(CLW_WarConfig.WAYPOINT_MOVE), GetGame().GetWorld(), params));
		if (!waypoint)
		{
			Print("CLW: failed to spawn move waypoint", LogLevel.ERROR);
			return false;
		}

		group.AddWaypoint(waypoint);
		return true;
	}

	protected SCR_AIGroup FindSquad(string name)
	{
		return SCR_AIGroup.Cast(GetGame().GetWorld().FindEntityByName(name));
	}

	protected void LogSquad(string name)
	{
		SCR_AIGroup group = FindSquad(name);
		if (!group)
		{
			Print("CLW: squad " + name + " not found", LogLevel.WARNING);
			return;
		}

		vector leaderPos;
		if (group.GetLeaderEntity())
			leaderPos = group.GetLeaderEntity().GetOrigin();

		Print(string.Format("CLW: squad %1 faction=%2 agents=%3 leader=%4", name, group.GetFaction().GetFactionKey(), group.GetAgentsCount(), leaderPos), LogLevel.NORMAL);
	}
}

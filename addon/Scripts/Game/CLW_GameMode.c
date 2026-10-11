// Clausewitz game mode: AI High Command war with human players on both sides.
// Inherits the whole player flow (spawn, groups, tasks, markers) from GameMode_Base.et.

class CLW_GameModeClass : SCR_BaseGameModeClass
{
}

class CLW_GameMode : SCR_BaseGameMode
{
	protected static const bool RUN_ORDER_SPIKE = false;
	protected static const bool RUN_REST_SPIKE = true;
	protected static const ref array<string> SQUADS_US = {"CLW_Squad_US_1", "CLW_Squad_US_2"};
	protected static const ref array<string> SQUADS_USSR = {"CLW_Squad_USSR_1", "CLW_Squad_USSR_2"};

	// One contact picture, marker set and rule commander per faction.
	protected ref CLW_ContactBook m_BookUS = new CLW_ContactBook();
	protected ref CLW_ContactBook m_BookUSSR = new CLW_ContactBook();
	protected ref CLW_ContactMarkers m_MarkersUS = new CLW_ContactMarkers();
	protected ref CLW_ContactMarkers m_MarkersUSSR = new CLW_ContactMarkers();
	protected ref CLW_RemoteCommander m_CommanderUS = new CLW_RemoteCommander();
	protected ref CLW_RemoteCommander m_CommanderUSSR = new CLW_RemoteCommander();
	protected static const ResourceName WAYPOINT_MOVE = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";

	protected override void OnGameStart()
	{
		super.OnGameStart();
		Print("CLW: build " + CLW_Build.STAMP, LogLevel.NORMAL);
		Print("CLW: game mode started", LogLevel.NORMAL);

		if (Replication.IsServer())
		{
			if (RUN_REST_SPIKE)
				GetGame().GetCallqueue().CallLater(CLW_RestSpike.Run, 10000);
			if (RUN_ORDER_SPIKE)
				GetGame().GetCallqueue().CallLater(SpikeOrderLoop, 15000);
			// RuleCommander drives only AI-only squads; each side's first squad is the player squad.
			array<string> commandedUS = {"CLW_Squad_US_1", "CLW_Squad_US_2"};
			array<string> commandedUSSR = {"CLW_Squad_USSR_1", "CLW_Squad_USSR_2"};
			m_CommanderUS.Init("US", m_BookUS, commandedUS);
			m_CommanderUSSR.Init("USSR", m_BookUSSR, commandedUSSR);
			array<vector> patrolUS = {Vector(2030, 0, 2630), Vector(2150, 0, 2630)};
			array<vector> patrolUSSR = {Vector(2030, 0, 3030), Vector(2150, 0, 3030)};
			m_CommanderUS.SetPatrolPoints(patrolUS);
			m_CommanderUSSR.SetPatrolPoints(patrolUSSR);

			GetGame().GetCallqueue().CallLater(SayBuild, 8000);
			GetGame().GetCallqueue().CallLater(PollContacts, 20000, true);
			GetGame().GetCallqueue().CallLater(ThinkCommanders, 25000, true);
		}
	}

	protected void SayBuild()
	{
		CLW_Debug.Say("build " + CLW_Build.STAMP);
	}

	// Perception -> contact book -> map markers, for both factions.
	protected void PollContacts()
	{
		UpdateFaction("US", SQUADS_US, m_BookUS, m_MarkersUS);
		UpdateFaction("USSR", SQUADS_USSR, m_BookUSSR, m_MarkersUSSR);
	}

	protected void UpdateFaction(string factionKey, array<string> squads, CLW_ContactBook book, CLW_ContactMarkers markers)
	{
		foreach (string name : squads)
		{
			SCR_AIGroup group = FindSquad(name);
			if (!group)
				continue;

			array<ref CLW_Contact> sightings = {};
			CLW_Contact.FromGroup(group, name, sightings);
			book.Report(factionKey, sightings);

			vector leaderPos;
			if (group.GetLeaderEntity())
				leaderPos = group.GetLeaderEntity().GetOrigin();
			array<IEntity> members = CLW_SquadInfo.Members(group);
			float maxDist;
			foreach (IEntity member : members)
			{
				maxDist = Math.Max(maxDist, vector.Distance(leaderPos, member.GetOrigin()));
			}
			Print(string.Format("CLW: squad %1 leader=%2 players=%3 firstPlayerLeader=%4 humanLeader=%5 leaderId=%6 waypoint=%7", name, leaderPos, group.GetPlayerCount(), group.GetFirstPlayerLeaderID(), CLW_RuleCommander.HasHumanLeader(group), group.GetLeaderID(), group.GetCurrentWaypoint() != null), LogLevel.NORMAL);
			PrintFormat("CLW: squad %1 agents=%2 alive=%3 aiMembers=%4 maxDist=%5", name, group.GetAgentsCount(), CLW_SquadInfo.AliveCount(group), members.Count(), Math.Round(maxDist), level: LogLevel.NORMAL);
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
		m_CommanderUS.Think();
		m_CommanderUSSR.Think();
	}

	// Player flow: after the auto-spawn, put the player into the AI rifle squad of their faction.
	override void OnPlayerSpawnFinalize_S(SCR_SpawnRequestComponent requestComponent, SCR_SpawnHandlerComponent handlerComponent, SCR_SpawnData data, IEntity entity)
	{
		super.OnPlayerSpawnFinalize_S(requestComponent, handlerComponent, data, entity);

		int playerId = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(entity);
		Faction faction = SCR_FactionManager.SGetPlayerFaction(playerId);
		Print(string.Format("CLW: player %1 spawned faction=%2 at %3", playerId, faction, entity.GetOrigin()), LogLevel.NORMAL);
		if (!faction)
			return;

		SCR_AIGroup squad = FindSquad("CLW_Squad_" + faction.GetFactionKey() + "_1");
		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		if (!squad || !groups)
		{
			Print("CLW: no squad/groups manager to join", LogLevel.WARNING);
			return;
		}

		if (squad.GetGroupID() < 0)
			groups.RegisterGroup(squad);
		// Do NOT AssignGroupID here or anywhere on the player squad: a real group ID silences the commander voice and move marker (bisected 2026-10-12).
		Print(string.Format("CLW: squad %1 groupId=%2 playable=%3", squad, squad.GetGroupID(), squad.IsPlayable()), LogLevel.NORMAL);
		groups.AddPlayerToGroup(squad.GetGroupID(), playerId);
		array<SCR_AIGroup> factionGroups = groups.GetPlayableGroupsByFaction(faction);
		Print(string.Format("CLW: playable groups of faction %1: %2", faction.GetFactionKey(), factionGroups.Count()), LogLevel.NORMAL);
		Print(string.Format("CLW: player %1 in squad=%2 players=%3 leaderId=%4", playerId, squad.IsPlayerInGroup(playerId), squad.GetPlayerCount(), squad.GetLeaderID()), LogLevel.NORMAL);
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

		AIWaypoint waypoint = AIWaypoint.Cast(GetGame().SpawnEntityPrefab(Resource.Load(WAYPOINT_MOVE), GetGame().GetWorld(), params));
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

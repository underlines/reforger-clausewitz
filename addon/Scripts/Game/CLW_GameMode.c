// Clausewitz game mode: AI High Command war with human players on both sides.
// Inherits the whole player flow (spawn, groups, tasks, markers) from GameMode_Base.et.

class CLW_GameModeClass : SCR_BaseGameModeClass
{
}

class CLW_GameMode : SCR_BaseGameMode
{
	protected static const ResourceName WAYPOINT_MOVE = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";

	protected override void OnGameStart()
	{
		super.OnGameStart();
		Print("CLW: game mode started", LogLevel.NORMAL);

		if (Replication.IsServer())
			GetGame().GetCallqueue().CallLater(SpikeOrderLoop, 15000);
	}

	// M0 spike: squads spawned, then a canned MOVE order -> waypoint, then check the squad moved.
	protected void SpikeOrderLoop()
	{
		LogSquad("CLW_Squad_US_1");
		LogSquad("CLW_Squad_USSR_1");

		SCR_AIGroup group = FindSquad("CLW_Squad_US_1");
		if (!group || !group.GetLeaderEntity())
			return;

		vector target = group.GetLeaderEntity().GetOrigin() + Vector(150, 0, 0);
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

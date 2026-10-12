// Waypoint helpers shared by the vehicle units and (later) the commanders. Server only.
// Waypoint prefabs are engine-level defaults from CLW_WarConfig, not faction content.
class CLW_Waypoints
{
	// Remove and delete every waypoint of the group (and of its slave group, where player-led squads keep their AI).
	static void Clear(SCR_AIGroup group)
	{
		if (!group)
			return;

		ClearOne(group);
		SCR_AIGroup slave = group.GetSlave();
		if (slave)
			ClearOne(slave);
	}

	protected static void ClearOne(notnull SCR_AIGroup group)
	{
		array<AIWaypoint> waypoints = {};
		group.GetWaypoints(waypoints);
		foreach (AIWaypoint old : waypoints)
		{
			group.RemoveWaypoint(old);
			SCR_EntityHelper.DeleteEntityAndChildren(old);
		}
	}

	// Spawn a waypoint prefab at pos (Y replaced by the surface height unless keepY). Null (and a log) on failure.
	static AIWaypoint Spawn(ResourceName prefab, vector pos, bool keepY = false)
	{
		if (prefab.IsEmpty())
		{
			Print("CLW: waypoints empty prefab name", LogLevel.ERROR);
			return null;
		}

		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
		{
			Print("CLW: waypoints prefab failed to load: " + prefab, LogLevel.ERROR);
			return null;
		}

		if (!keepY)
			pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);

		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = pos;
		AIWaypoint waypoint = AIWaypoint.Cast(GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params));
		if (!waypoint)
			Print("CLW: waypoints prefab is not an AIWaypoint: " + prefab, LogLevel.ERROR);

		return waypoint;
	}

	// Spawn a waypoint of the given kind prefab and append it to the group. radius > 0 sets the completion radius.
	static AIWaypoint Add(SCR_AIGroup group, ResourceName prefab, vector pos, float radius = -1)
	{
		if (!group)
			return null;

		AIWaypoint waypoint = Spawn(prefab, pos);
		if (!waypoint)
			return null;

		if (radius > 0)
			waypoint.SetCompletionRadius(radius);

		group.AddWaypoint(waypoint);
		return waypoint;
	}

	static AIWaypoint AddMove(SCR_AIGroup group, vector pos, float radius = -1)
	{
		return Add(group, CLW_WarConfig.WAYPOINT_MOVE, pos, radius);
	}

	// Move that ignores combat (fall back).
	static AIWaypoint AddForcedMove(SCR_AIGroup group, vector pos, float radius = -1)
	{
		return Add(group, CLW_WarConfig.WAYPOINT_FORCED_MOVE, pos, radius);
	}

	static AIWaypoint AddAttack(SCR_AIGroup group, vector pos, float radius = -1)
	{
		return Add(group, CLW_WarConfig.WAYPOINT_ATTACK, pos, radius);
	}

	static AIWaypoint AddDefend(SCR_AIGroup group, vector pos, float radius = -1)
	{
		return Add(group, CLW_WarConfig.WAYPOINT_DEFEND, pos, radius);
	}

	// Waypoint of a command kind: MOVE/RECON -> Move, ATTACK -> SearchAndDestroy, DEFEND -> Defend, FALL_BACK -> ForcedMove.
	static AIWaypoint AddForCommand(SCR_AIGroup group, CLW_ECommand command, vector pos, float radius = -1)
	{
		switch (command)
		{
			case CLW_ECommand.ATTACK: return AddAttack(group, pos, radius);
			case CLW_ECommand.DEFEND: return AddDefend(group, pos, radius);
			case CLW_ECommand.FALL_BACK: return AddForcedMove(group, pos, radius);
		}

		return AddMove(group, pos, radius);
	}

	// Make the vehicle a usable vehicle of the group (Get-In/Get-Out waypoints and AI driving need it).
	static bool AddUsableVehicle(SCR_AIGroup group, IEntity vehicle)
	{
		if (!group || !vehicle)
			return false;

		SCR_AIGroupUtilityComponent utility = group.GetGroupUtilityComponent();
		if (!utility)
		{
			Print("CLW: waypoints group has no SCR_AIGroupUtilityComponent, vehicle not registered as usable", LogLevel.WARNING);
			return false;
		}

		IEntity owner;
		SCR_AIVehicleUsageComponent usage = SCR_AIVehicleUsageComponent.FindOnNearestParent(vehicle, owner);
		if (!usage)
		{
			Print("CLW: waypoints vehicle has no SCR_AIVehicleUsageComponent, AI cannot drive it", LogLevel.ERROR);
			return false;
		}

		utility.AddUsableVehicle(usage);
		return true;
	}

	// Walk to the vehicle and board it (SCR_BoardingEntityWaypoint with SetEntity). Allowance = which seat types may be used.
	static AIWaypoint AddGetIn(SCR_AIGroup group, IEntity vehicle, bool driver = false, bool gunner = false, bool cargo = true)
	{
		if (!group || !vehicle)
			return null;

		AIWaypoint waypoint = Spawn(CLW_WarConfig.WAYPOINT_GET_IN, vehicle.GetOrigin());
		if (!waypoint)
			return null;

		SCR_BoardingEntityWaypoint boarding = SCR_BoardingEntityWaypoint.Cast(waypoint);
		if (!boarding)
		{
			Print("CLW: waypoints GetIn prefab is not a SCR_BoardingEntityWaypoint", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(waypoint);
			return null;
		}

		boarding.SetEntity(vehicle);
		boarding.SetAllowance(driver, gunner, cargo);
		group.AddWaypoint(waypoint);
		return waypoint;
	}

	// Leave the vehicle at pos (the group's members walk out). allowance: which seat types leave (cargo only keeps the crew seated).
	static AIWaypoint AddGetOut(SCR_AIGroup group, vector pos, bool driver = false, bool gunner = false, bool cargo = true)
	{
		if (!group)
			return null;

		AIWaypoint waypoint = Spawn(CLW_WarConfig.WAYPOINT_GET_OUT, pos);
		if (!waypoint)
			return null;

		SCR_BoardingWaypoint boarding = SCR_BoardingWaypoint.Cast(waypoint);
		if (boarding)
			boarding.SetAllowance(driver, gunner, cargo);
		else
			Print("CLW: waypoints GetOut prefab is not a SCR_BoardingWaypoint (allowance not set)", LogLevel.WARNING);

		group.AddWaypoint(waypoint);
		return waypoint;
	}
}

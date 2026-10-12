// Generic unstuck helper for infantry that rode in a vehicle and did not really dismount: still seated in a
// compartment, or standing ON TOP of a vehicle (bed / roof). Data free, static.

// Finds the nearest vehicle (entity with a compartment manager) around a point.
class CLW_UnstuckVehicleQuery
{
	IEntity m_Best;
	float m_fBestDist = 1000000;
	vector m_vOrigin;
	IEntity m_Ignore;

	bool OnEntity(IEntity entity)
	{
		if (!entity || entity == m_Ignore)
			return true;

		if (!entity.FindComponent(BaseCompartmentManagerComponent))
			return true;

		float dist = vector.Distance(entity.GetOrigin(), m_vOrigin);
		if (dist < m_fBestDist)
		{
			m_fBestDist = dist;
			m_Best = entity;
		}

		return true;
	}
}

class CLW_Unstuck
{
	static const float ON_TOP_MIN_HEIGHT_M = 1.2;	// above the terrain surface
	static const float VEHICLE_QUERY_M = 8;
	static const float VEHICLE_NEAR_M = 6;
	static const float SPOT_MIN_M = 4;				// beside the vehicle hull
	static const float SPOT_MAX_M = 8;

	// Free every living AI member of the group that is stuck in / on a vehicle. mounted = the group is meant to
	// sit in its vehicle right now (then compartments are left alone). Returns how many members were freed.
	static int FreeMembers(SCR_AIGroup group, string why, bool mounted = false)
	{
		if (!group)
			return 0;

		string groupId = group.GetGroupID().ToString();
		int freed;
		array<IEntity> members = CLW_SquadInfo.Members(group);
		foreach (IEntity member : members)
		{
			if (!CLW_VehicleUnit.IsAlive(member) || IsPlayer(member))
				continue;

			if (!mounted && ForceOutOfCompartment(member))
			{
				Print(string.Format("CLW: unstuck %1 %2 forced out of compartment (%3)", groupId, member, why), LogLevel.NORMAL);
				freed++;
				continue;
			}

			IEntity vehicle;
			if (!IsOnTopOfVehicle(member, vehicle))
				continue;

			vector spot;
			if (!FindSpotBeside(vehicle, spot))
				continue;

			if (TeleportCharacter(member, spot))
			{
				Print(string.Format("CLW: unstuck %1 %2 teleported off %3 to %4 (%5)", groupId, member, vehicle, spot, why), LogLevel.NORMAL);
				freed++;
			}
		}

		return freed;
	}

	static bool IsPlayer(IEntity entity)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		return players && players.GetPlayerIdFromControlledEntity(entity) > 0;
	}

	protected static bool ForceOutOfCompartment(IEntity member)
	{
		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(member.FindComponent(SCR_CompartmentAccessComponent));
		if (!access || !access.IsInCompartment())
			return false;

		return access.GetOutVehicle(EGetOutType.TELEPORT, -1, ECloseDoorAfterActions.INVALID, false);
	}

	// Standing high above the terrain with a vehicle close by.
	protected static bool IsOnTopOfVehicle(IEntity member, out IEntity vehicle)
	{
		vector pos = member.GetOrigin();
		float ground = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
		if (pos[1] - ground <= ON_TOP_MIN_HEIGHT_M)
			return false;

		CLW_UnstuckVehicleQuery query = new CLW_UnstuckVehicleQuery();
		query.m_vOrigin = pos;
		query.m_Ignore = member;
		GetGame().GetWorld().QueryEntitiesBySphere(pos, VEHICLE_QUERY_M, query.OnEntity, null, EQueryEntitiesFlags.ALL);
		if (!query.m_Best || query.m_fBestDist > VEHICLE_NEAR_M)
			return false;

		vehicle = query.m_Best;
		return true;
	}

	// Free ground 4-8 m beside the vehicle hull (random side), surface Y.
	protected static bool FindSpotBeside(IEntity vehicle, out vector spot)
	{
		vector mins, maxs;
		vehicle.GetBounds(mins, maxs);
		float hull = Math.Max(maxs[0] - mins[0], maxs[2] - mins[2]) * 0.5;
		vector center = vehicle.GetOrigin();
		BaseWorld world = GetGame().GetWorld();

		for (int attempt = 0; attempt < 6; attempt++)
		{
			float angle = Math.RandomFloat(0, Math.PI2);
			float dist = hull + Math.RandomFloat(SPOT_MIN_M, SPOT_MAX_M);
			vector candidate = center + Vector(Math.Cos(angle) * dist, 0, Math.Sin(angle) * dist);
			candidate[1] = world.GetSurfaceY(candidate[0], candidate[2]);

			vector found;
			if (SCR_WorldTools.FindEmptyTerrainPosition(found, candidate, 2, 0.5, 2))
			{
				found[1] = world.GetSurfaceY(found[0], found[2]);
				candidate = found;
			}

			if (SurfaceIsWater(world, candidate))
				continue;

			spot = candidate;
			return true;
		}

		return false;
	}

	protected static bool SurfaceIsWater(BaseWorld world, vector pos)
	{
		return world.GetOceanHeight(pos[0], pos[2]) > pos[1];
	}

	// ChimeraCharacter.Teleport(mat[4]) is the character-aware engine teleport (keeps physics / AI in sync).
	protected static bool TeleportCharacter(IEntity member, vector spot)
	{
		ChimeraCharacter character = ChimeraCharacter.Cast(member);
		if (!character)
			return false;

		vector mat[4];
		member.GetWorldTransform(mat);
		mat[3] = spot;
		character.Teleport(mat);
		return true;
	}
}

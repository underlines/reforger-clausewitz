// Vehicle depot menu (WP11): the CLW depot prefabs (Prefabs/CLW/CLW_VehicleDepot_*.et) carry ONE user action instead of the vanilla
// SCR_CatalogSpawnerUserAction per vehicle (those need catalog membership, faction equality and spawner slots, and showed nothing in game).
// The action opens the HQ menu (CLW_HqMenuDialog) on its paged vehicle list; the depot position travels as "terminalPos" like a request
// terminal, the server resolves it with CLW_DepotLookup (side, base) and spawns the bought vehicle next to this depot
// (CLW_RequestService.FindVehicleSpot / CLW_FreeSpot.FindDepotSpot). Log prefix: "CLW: depot".

class CLW_OpenDepotAction : ScriptedUserAction
{
	override bool HasLocalEffectOnlyScript()
	{
		return true;
	}

	override bool CanBeShownScript(IEntity user)
	{
		return true;
	}

	override bool CanBePerformedScript(IEntity user)
	{
		return true;
	}

	override bool GetActionNameScript(out string outName)
	{
		outName = "Request vehicle";
		return true;
	}

	override void PerformAction(IEntity pOwnerEntity, IEntity pUserEntity)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print("CLW: depot action: opening the HQ menu at the depot", LogLevel.NORMAL);
		controller.CLW_HqMenuOpen(pOwnerEntity.GetOrigin());
	}
}

class CLW_DepotLookup
{
	static const float FIND_M = 3;			// a client-reported depot position matches a depot within this
	static const float USE_M = 25;			// the requester must stand within this of the depot (the action reach is a few metres)

	// The vehicle depot of the side at (within FIND_M of) the position, null when none. Server.
	static CLW_ServiceInstance FindAt(string sideKey, vector pos)
	{
		if (sideKey.IsEmpty() || pos == vector.Zero)
			return null;

		array<CLW_ServiceInstance> list = {};
		CLW_Services.GetInstances(sideKey, "", list);
		CLW_ServiceInstance best;
		float bestDist = FIND_M;
		foreach (CLW_ServiceInstance inst : list)
		{
			if (inst.m_sKind != CLW_ServiceKind.VEHICLE_DEPOT || !inst.m_Entity)
				continue;

			float dist = vector.Distance(inst.m_Entity.GetOrigin(), pos);
			if (dist <= bestDist)
			{
				bestDist = dist;
				best = inst;
			}
		}

		return best;
	}

	// Server: the requester is near the depot. False with a refusal text otherwise.
	static bool PlayerNear(int playerId, CLW_ServiceInstance depot, out string refusal)
	{
		vector playerPos;
		if (!CLW_RequestService.PlayerPos(playerId, playerPos))
		{
			refusal = "you are not deployed";
			return false;
		}

		if (vector.Distance(playerPos, depot.m_Entity.GetOrigin()) > USE_M)
		{
			refusal = "you are too far from the vehicle depot";
			return false;
		}

		return true;
	}
}

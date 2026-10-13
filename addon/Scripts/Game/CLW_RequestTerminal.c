// Request terminal (M3 phase Q): a small prop (Prefabs/CLW/CLW_RequestTerminal.et, vanilla crate mesh + ActionsManagerComponent +
// the user actions below) that CLW_Services spawns next to the command post and the vehicle depot of a side. The actions are LOCAL
// (HasLocalEffectOnly): PerformAction runs on the machine of the player, which sends the request to the server through the player
// controller RPC (same path as the chat command /request), so it works on a dedicated server and on the listen-server host alike.
// The server decides (CLW_RequestService, role / cooldown / cost); the answer comes back as hint + chat line.

class CLW_RequestActionBase : ScriptedUserAction
{
	protected string KindName()
	{
		return "";
	}

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
		outName = CLW_RequestClient.ActionLabel(KindName());
		return true;
	}

	override void PerformAction(IEntity pOwnerEntity, IEntity pUserEntity)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print("CLW: request terminal action " + KindName(), LogLevel.NORMAL);
		controller.CLW_SendRequest(KindName(), pOwnerEntity.GetOrigin());	// the terminal position tells the server which terminal (side, base) was used
	}
}

class CLW_RequestSquadAction : CLW_RequestActionBase
{
	protected override string KindName()
	{
		return CLW_RequestKind.SQUAD;
	}
}

class CLW_RequestVehicleAction : CLW_RequestActionBase
{
	protected override string KindName()
	{
		return CLW_RequestKind.VEHICLE;
	}
}

class CLW_RequestResupplyAction : CLW_RequestActionBase
{
	protected override string KindName()
	{
		return CLW_RequestKind.RESUPPLY;
	}
}

// M4 WP4: "Open HQ menu" opens the dialog (CLW_HqMenuDialog) with requests, squad, base and contact report; the terminal position
// is sent along, so requests made in the menu are bound to this terminal's side and base like the plain actions above.
class CLW_RequestMenuAction : CLW_RequestActionBase
{
	protected override string KindName()
	{
		return "menu";
	}

	override bool GetActionNameScript(out string outName)
	{
		outName = CLW_Loc.Fmt("#CLW-Terminal_OpenHq");
		return true;
	}

	override void PerformAction(IEntity pOwnerEntity, IEntity pUserEntity)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print("CLW: request terminal action menu", LogLevel.NORMAL);
		controller.CLW_HqMenuOpen(pOwnerEntity.GetOrigin());
	}
}

// One spawned terminal: the entity plus the side and base it was bound to at spawn (M3 phase L fix A).
class CLW_TerminalInfo
{
	IEntity m_Entity;
	string m_sSide;
	string m_sBase;
	vector m_vPos;
}

// Server: spawns the terminals (hook in CLW_Services.SpawnOne), looks them up for the request handler, removes them with a lost FOB
// (fix B) and at war end.
class CLW_RequestTerminals
{
	static const ResourceName PREFAB = "{BD560817364C995D}Prefabs/CLW/CLW_RequestTerminal.et";
	static const float GAP_M = 2.5;			// distance between the service footprint and the terminal
	static const float FIND_M = 3;			// a client-reported terminal position matches a terminal within this

	protected static ref array<ref CLW_TerminalInfo> s_aTerminals = {};

	static void Reset()
	{
		s_aTerminals.Clear();
	}

	static void RemoveAll()
	{
		foreach (CLW_TerminalInfo info : s_aTerminals)
		{
			if (info.m_Entity)
				SCR_EntityHelper.DeleteEntityAndChildren(info.m_Entity);
		}

		s_aTerminals.Clear();
	}

	// The terminal at (or within FIND_M of) the position, null when none. Server.
	static CLW_TerminalInfo FindAt(vector pos)
	{
		CLW_TerminalInfo best;
		float bestDist = FIND_M;
		foreach (CLW_TerminalInfo info : s_aTerminals)
		{
			if (!info.m_Entity)
				continue;

			float dist = vector.Distance(info.m_Entity.GetOrigin(), pos);
			if (dist <= bestDist)
			{
				bestDist = dist;
				best = info;
			}
		}

		return best;
	}

	// Fix B: removes the terminals of one base (a lost FOB). Returns how many were deleted.
	static int RemoveBase(string sideKey, string baseId)
	{
		int removed;
		for (int i = s_aTerminals.Count() - 1; i >= 0; i--)
		{
			CLW_TerminalInfo info = s_aTerminals[i];
			if (info.m_sSide != sideKey || info.m_sBase != baseId)
				continue;

			if (info.m_Entity)
				SCR_EntityHelper.DeleteEntityAndChildren(info.m_Entity);

			s_aTerminals.Remove(i);
			removed++;
		}

		if (removed > 0)
			Print(string.Format("CLW: request terminals %1 %2: %3 removed (base lost)", sideKey, baseId, removed), LogLevel.NORMAL);

		return removed;
	}

	// CLW_FobManager.GetOnLost() callback (fobId, side).
	static void OnFobLost(string fobId, string sideKey)
	{
		RemoveBase(sideKey, fobId);
	}

	// A service of kind command_post was placed at servicePos (footprint radius): put a terminal beside it (the vehicle depot gets none).
	static void OnServicePlaced(string sideKey, string baseId, string kind, vector servicePos, float radius)
	{
		if (!Replication.IsServer())
			return;

		if (kind == CLW_ServiceKind.VEHICLE_DEPOT)
		{
			// owner 2026-10-13: only the command post gets a terminal (the depot is reached through it, CLW_RequestService.FindDepot)
			Print(string.Format("CLW: terminals skipped at %1 (only the command post gets one) side=%2 base=%3", kind, sideKey, baseId), LogLevel.NORMAL);
			return;
		}

		if (kind != CLW_ServiceKind.COMMAND_POST)
			return;

		if (!CLW_Capabilities.Has("requests"))
			return;

		Resource resource = Resource.Load(PREFAB);
		if (!resource || !resource.IsValid())
		{
			Print("CLW: request terminal prefab does not load: " + PREFAB, LogLevel.ERROR);
			return;
		}

		vector spot;
		string via = "free";
		float yaw;
		// collision-free spot beside the service (CLW_FreeSpot), the old cylinder search only when nothing clear exists
		if (!CLW_FreeSpot.FindSmall("terminal " + sideKey + " " + baseId + " " + kind, servicePos, radius + GAP_M, radius + GAP_M + 10, 0.9, 1.8, spot, yaw))
		{
			CLW_Placement.FindGround(servicePos, radius + GAP_M, radius + GAP_M + 8, 0.8, 1.6, CLW_WarConfig.PLACE_CHAR_CLEAR_M, spot, via);
			yaw = CLW_Placement.YawFromDir(servicePos[0] - spot[0], servicePos[2] - spot[2]);
		}
		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		Math3D.AnglesToMatrix(Vector(yaw, 0, 0), params.Transform);
		params.Transform[3] = spot;
		IEntity terminal = GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params);
		if (!terminal)
		{
			Print(string.Format("CLW: request terminal spawn failed for %1 %2 %3", sideKey, baseId, kind), LogLevel.ERROR);
			return;
		}

		CLW_TerminalInfo info = new CLW_TerminalInfo();
		info.m_Entity = terminal;
		info.m_sSide = sideKey;
		info.m_sBase = baseId;
		info.m_vPos = spot;
		s_aTerminals.Insert(info);
		Print(string.Format("CLW: request terminal placed %1 %2 beside %3 at %4 via=%5 (bound to side %1)", sideKey, baseId, kind, spot, via), LogLevel.NORMAL);
	}
}

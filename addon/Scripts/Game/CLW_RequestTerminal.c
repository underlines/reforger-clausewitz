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
		controller.CLW_SendRequest(KindName());
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

// Server: spawns the terminals (hook in CLW_Services.SpawnOne) and removes them at war end.
class CLW_RequestTerminals
{
	static const ResourceName PREFAB = "{BD560817364C995D}Prefabs/CLW/CLW_RequestTerminal.et";
	static const float GAP_M = 2.5;			// distance between the service footprint and the terminal

	protected static ref array<IEntity> s_aTerminals = {};

	static void Reset()
	{
		s_aTerminals.Clear();
	}

	static void RemoveAll()
	{
		foreach (IEntity terminal : s_aTerminals)
		{
			if (terminal)
				SCR_EntityHelper.DeleteEntityAndChildren(terminal);
		}

		s_aTerminals.Clear();
	}

	// A service of kind command_post or vehicle_depot was placed at servicePos (footprint radius): put a terminal beside it.
	static void OnServicePlaced(string sideKey, string baseId, string kind, vector servicePos, float radius)
	{
		if (!Replication.IsServer())
			return;

		if (kind != CLW_ServiceKind.COMMAND_POST && kind != CLW_ServiceKind.VEHICLE_DEPOT)
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
		string via;
		CLW_Placement.FindGround(servicePos, radius + GAP_M, radius + GAP_M + 8, 0.8, 1.6, CLW_WarConfig.PLACE_CHAR_CLEAR_M, spot, via);
		float yaw = CLW_Placement.YawFromDir(servicePos[0] - spot[0], servicePos[2] - spot[2]);
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

		s_aTerminals.Insert(terminal);
		Print(string.Format("CLW: request terminal placed %1 %2 beside %3 at %4 via=%5", sideKey, baseId, kind, spot, via), LogLevel.NORMAL);
	}
}

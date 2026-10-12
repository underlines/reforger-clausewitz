// Executors of the player request entries on the action registry (M3 phase Q): REQUEST_VEHICLE, REQUEST_SQUAD, REQUEST_RESUPPLY,
// REQUEST_REINFORCE, REQUEST_RECON. Authority "player", capability "requests". Registered from script (CLW_ActionsRequests.Register,
// called by CLW_GameMode right after the registry reset). The executors hold the grant rules (role, cooldown, ledger, caps, engine
// headroom); CLW_RequestService does parsing, history and the answer. Executors are shared per entry: no per-call state in members.
// Execute returns true = granted, result = the sentence the requester reads ("... issued ...") or the refusal reason.

// Common checks: war running, living requester on the call's faction, role, cooldown.
class CLW_Action_RequestBase : CLW_ActionExecutor
{
	protected string KindName()
	{
		return "";
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
		{
			error = "the war is not running";
			return false;
		}

		int playerId = call.m_iPlayerId;
		if (playerId <= 0)
		{
			error = "no requesting player";
			return false;
		}

		vector pos;
		if (!CLW_RequestService.PlayerPos(playerId, pos))
		{
			error = "you are not deployed";
			return false;
		}

		if (CLW_RequestService.FactionKeyOf(playerId) != call.m_sFaction)
		{
			error = "you are not on that side";
			return false;
		}

		string kind = KindName();
		if (CLW_RequestKind.NeedsLeader(kind) && !CLW_RequestService.IsSquadLeader(playerId))
		{
			error = "only squad leaders may request a " + kind + " (you are a soldier; soldiers may request resupply and a vehicle)";
			return false;
		}

		float left = CLW_RequestService.CooldownLeft(playerId, kind);
		if (left > 0)
		{
			error = "cooldown, try again in " + Math.Round(left) + " s";
			return false;
		}

		return true;
	}

	protected void Granted(CLW_ActionCall call)
	{
		call.m_iApplied = 1;
		CLW_RequestService.MarkGranted(call.m_iPlayerId, KindName());
	}
}

// ---- REQUEST_VEHICLE type? ----
class CLW_Action_RequestVehicle : CLW_Action_RequestBase
{
	protected override string KindName()
	{
		return CLW_RequestKind.VEHICLE;
	}

	override bool IsAvailable(string factionKey)
	{
		return CLW_GameMode.GetForceCatalog() != null;
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (!super.Validate(call, error))
			return false;

		if (!CLW_RequestService.PickVehicle(call.m_sFaction, call.GetString("type")))
		{
			error = "no such vehicle in the faction's catalog";
			return false;
		}

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		string faction = call.m_sFaction;
		CLW_ForceEntry entry = CLW_RequestService.PickVehicle(faction, call.GetString("type"));
		if (!war || !spawner || !entry)
		{
			result = "no vehicle can be issued";
			return false;
		}

		int issued = CLW_RequestService.CountIssuedVehicles(faction);
		if (issued >= CLW_RequestConfig.MAX_ISSUED_VEHICLES)
		{
			result = "too many requested vehicles in the field (" + issued + "), use or lose one first";
			return false;
		}

		int cap = war.GetVehicleCap(faction);
		int live = spawner.GetLiveVehicles(faction) + issued;
		if (cap > 0 && live + 1 > cap)
		{
			result = string.Format("vehicle cap reached (%1 + 1 > %2)", live, cap);
			return false;
		}

		int cost = CLW_RequestService.VehicleCost(entry);
		if (!CLW_SupplyStore.TrySpend(faction, call.GetString("base"), cost))
		{
			result = string.Format("not enough supplies (%1 costs %2, the base has %3)", entry.GetVehicleName(), cost, CLW_SupplyStore.Get(faction, call.GetString("base")));
			return false;
		}

		vector spot;
		float yaw;
		string via;
		string where;
		if (!CLW_RequestService.FindVehicleSpot(faction, spot, yaw, via, where, call.GetString("base")))
		{
			CLW_SupplyStore.Refund(faction, call.GetString("base"), cost);
			result = "no free parking spot at the " + where;
			return false;
		}

		IEntity vehicle = CLW_RequestService.SpawnVehicle(entry.m_VehiclePrefab, spot, yaw);
		if (!vehicle)
		{
			CLW_SupplyStore.Refund(faction, call.GetString("base"), cost);
			result = "the vehicle could not be spawned";
			return false;
		}

		war.RegisterVehicle(vehicle, faction);
		FactionManager factions = GetGame().GetFactionManager();
		if (factions)
		{
			Faction side = factions.GetFactionByKey(faction);
			FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(vehicle.FindComponent(FactionAffiliationComponent));
			if (side && affiliation)
				affiliation.SetAffiliatedFaction(side);
		}

		CLW_RequestService.TrackVehicle(faction, vehicle);
		Granted(call);
		war.PushEvent(faction, "player " + CLW_RequestService.PlayerName(call.m_iPlayerId) + " requested a " + entry.GetVehicleName());
		result = string.Format("%1 issued at the %2, grid %3 (cost %4). It is empty: bring your own crew.", entry.GetVehicleName(), where, CLW_Places.Grid(spot), cost);
		Print(string.Format("CLW: request vehicle %1 spawned %2 at %3 via=%4 where=%5 cost=%6", faction, entry.m_sType, spot, via, where, cost), LogLevel.NORMAL);
		return true;
	}
}

// ---- REQUEST_SQUAD type? ----
class CLW_Action_RequestSquad : CLW_Action_RequestBase
{
	protected override string KindName()
	{
		return CLW_RequestKind.SQUAD;
	}

	override bool IsAvailable(string factionKey)
	{
		return CLW_GameMode.GetForceSpawner() != null && CLW_GameMode.GetForceCatalog() != null;
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (!super.Validate(call, error))
			return false;

		if (!CLW_RequestService.GroupOf(call.m_iPlayerId))
		{
			error = "you are not in a squad";
			return false;
		}

		if (!CLW_RequestService.PickInfantry(call.m_sFaction, call.GetString("type")))
		{
			error = "no such infantry team in the faction's catalog";
			return false;
		}

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		CLW_ForceEntry entry = CLW_RequestService.PickInfantry(call.m_sFaction, call.GetString("type"));
		if (!spawner || !entry)
		{
			result = "no team can be issued";
			return false;
		}

		// the spawner checks supplies, the AI cap and the engine AI headroom and charges the ledger (refunded if the spawn fails)
		string error;
		if (!spawner.BuyFor(call.m_sFaction, entry.m_sType, "player request " + CLW_RequestService.PlayerName(call.m_iPlayerId), call.m_iPlayerId, error))
		{
			result = error;
			return false;
		}

		Granted(call);
		result = string.Format("%1 approved (cost %2). It is forming at the base and will join your squad when complete.", entry.m_sName, entry.m_iCost);
		return true;
	}
}

// ---- REQUEST_RESUPPLY ----
class CLW_Action_RequestResupply : CLW_Action_RequestBase
{
	protected override string KindName()
	{
		return CLW_RequestKind.RESUPPLY;
	}

	// The box prefab is the "arsenal" service of the faction data (profile services, kin fallback): nothing is named here.
	protected ResourceName BoxPrefab(string faction, out string source)
	{
		array<ref CLW_ServiceEntry> entries = {};
		source = CLW_Services.ResolveEntries(faction, entries);
		foreach (CLW_ServiceEntry entry : entries)
		{
			if (entry.m_sKind == CLW_ServiceKind.ARSENAL && !entry.m_Prefab.IsEmpty())
				return entry.m_Prefab;
		}

		return "";
	}

	override bool IsAvailable(string factionKey)
	{
		string source;
		return !BoxPrefab(factionKey, source).IsEmpty();
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (!super.Validate(call, error))
			return false;

		string source;
		if (BoxPrefab(call.m_sFaction, source).IsEmpty())
		{
			error = "the faction has no resupply box in its data";
			return false;
		}

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_Bases bases = CLW_GameMode.GetBases();
		string faction = call.m_sFaction;
		string source;
		ResourceName prefab = BoxPrefab(faction, source);
		vector playerPos;
		if (!war || prefab.IsEmpty() || !CLW_RequestService.PlayerPos(call.m_iPlayerId, playerPos))
		{
			result = "no box can be delivered";
			return false;
		}

		vector anchor;
		if (bases)
			anchor = bases.GetBasePos(faction);

		// near a friendly base / service: drop at the requester, else at the HQ
		bool near;
		if (anchor != vector.Zero && vector.DistanceXZ(playerPos, anchor) <= CLW_RequestConfig.RESUPPLY_NEAR_M + 40)
			near = true;
		array<CLW_ServiceInstance> services = {};
		CLW_Services.GetInstances(faction, "", services);
		foreach (CLW_ServiceInstance inst : services)
		{
			if (vector.DistanceXZ(playerPos, inst.m_vPos) <= CLW_RequestConfig.RESUPPLY_NEAR_M)
				near = true;
		}

		vector center = playerPos;
		string where = "at your position";
		if (!near)
		{
			if (anchor == vector.Zero)
			{
				result = "no friendly base to deliver from";
				return false;
			}

			center = anchor;
			where = "at the HQ base";
		}

		int cost = CLW_RequestConfig.RESUPPLY_COST;
		if (!CLW_SupplyStore.TrySpend(faction, call.GetString("base"), cost))
		{
			result = string.Format("not enough supplies (resupply costs %1, the base has %2)", cost, CLW_SupplyStore.Get(faction, call.GetString("base")));
			return false;
		}

		vector spot;
		string via;
		CLW_Placement.FindGround(center, 2, 9, 0.8, 1.2, CLW_WarConfig.PLACE_CHAR_CLEAR_M, spot, via);
		Resource resource = Resource.Load(prefab);
		IEntity box;
		if (resource && resource.IsValid())
		{
			EntitySpawnParams params = new EntitySpawnParams();
			params.TransformMode = ETransformMode.WORLD;
			params.Transform[3] = spot;
			box = GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params);
		}

		if (!box)
		{
			CLW_SupplyStore.Refund(faction, call.GetString("base"), cost);
			result = "the box could not be spawned";
			return false;
		}

		array<IEntity> tree = {};
		CLW_Services.CollectHierarchy(box, tree);
		CLW_Services.AdoptFaction(tree, source, faction);
		CLW_RequestService.TrackBox(call.m_iPlayerId, box);
		Granted(call);
		result = string.Format("resupply box delivered %1, grid %2 (cost %3, removed after %4 min).", where, CLW_Places.Grid(spot), cost, Math.Round(CLW_RequestConfig.RESUPPLY_LIFETIME_MS / 60000.0));
		Print(string.Format("CLW: request resupply %1 box %2 at %3 via=%4 near=%5 cost=%6", faction, prefab.GetPath(), spot, via, near, cost), LogLevel.NORMAL);
		return true;
	}
}

// ---- REQUEST_REINFORCE / REQUEST_RECON: HQ orders the nearest free AI group ----
class CLW_Action_RequestOrder : CLW_Action_RequestBase
{
	protected string OrderCommand()
	{
		return "DEFEND";
	}

	// Target of the request: "" / "here" = the requester, an objective id, or a map grid "123-456" (100 m cells, as the map shows).
	protected bool ResolveWhere(CLW_ActionCall call, string where, out vector pos, out string label)
	{
		string text = where;
		text = text.Trim();
		text.ToLower();
		vector playerPos;
		if (!CLW_RequestService.PlayerPos(call.m_iPlayerId, playerPos))
			return false;

		if (text.IsEmpty() || text == "here")
		{
			pos = playerPos;
			label = "your position";
			return true;
		}

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		CLW_ControlPoint point;
		if (points)
			point = points.Find(where.Trim());
		if (point)
		{
			pos = point.m_vPos;
			label = point.m_sName;
			return true;
		}

		string digits = "";
		for (int i = 0; i < text.Length(); i++)
		{
			string c = text.Get(i);
			if ("0123456789".Contains(c))
				digits += c;
			else if (c != "-" && c != " " && c != "/" && c != ",")
				return false;
		}

		if (digits.Length() != 6)
			return false;

		int gx = digits.Substring(0, 3).ToInt();
		int gz = digits.Substring(3, 3).ToInt();
		pos = Vector(gx * 100 + 50, 0, gz * 100 + 50);
		pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
		label = "grid " + CLW_Places.Grid(pos);
		return true;
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (!super.Validate(call, error))
			return false;

		if (!call.m_Rules)
		{
			error = "HQ has no commander";
			return false;
		}

		vector pos;
		string label;
		if (!ResolveWhere(call, call.GetString("where"), pos, label))
		{
			error = "target not understood: use here, an objective id or a map grid like 123-456";
			return false;
		}

		return true;
	}

	// Nearest group HQ may redirect: not human-led, not weak, no fresh order; groups with an active move / defend order count as
	// further away (idle ones first); groups attacking or reconning are left alone.
	protected string FindGroup(CLW_ActionCall call, vector pos, out float distance)
	{
		CLW_RuleCommander rules = call.m_Rules;
		CLW_Orders orders = rules.GetOrders();
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		array<string> ids = {};
		registry.GetCommandableIds(call.m_sFaction, ids);
		string best;
		float bestScore = 1000000000;
		foreach (string id : ids)
		{
			if (!rules.IsCommandable(id) || rules.IsHumanLed(id) || registry.IsHidden(id))
				continue;

			if (!orders.CanReorder(id) || orders.IsWeak(id))
				continue;

			float penalty = 0;
			CLW_UnitOrder order = orders.GetOrder(id);
			if (order && order.m_sStatus == CLW_Orders.STATUS_ACTIVE)
			{
				if (order.m_eCommand == CLW_ECommand.ATTACK || order.m_eCommand == CLW_ECommand.RECON)
					continue;

				penalty = CLW_RequestConfig.ORDER_BUSY_PENALTY_M;
			}

			vector unitPos;
			if (!orders.UnitPos(id, unitPos))
				continue;

			float score = vector.DistanceXZ(unitPos, pos) + penalty;
			if (score < bestScore)
			{
				bestScore = score;
				best = id;
				distance = vector.DistanceXZ(unitPos, pos);
			}
		}

		return best;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		vector pos;
		string label;
		if (!ResolveWhere(call, call.GetString("where"), pos, label))
		{
			result = "target not understood";
			return false;
		}

		float distance;
		string squad = FindGroup(call, pos, distance);
		if (squad.IsEmpty())
		{
			result = "no free group is available right now (all are on missions or too weak)";
			return false;
		}

		string why;
		string reason = "player request " + CLW_RequestService.PlayerName(call.m_iPlayerId) + " (" + KindName() + ")";
		if (!call.m_Rules.ApplyOrder(squad, OrderCommand(), "", pos, reason, why))
		{
			result = "HQ could not order " + squad + ": " + why;
			return false;
		}

		Granted(call);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.PushEvent(call.m_sFaction, squad + " sent to " + label + " at the request of " + CLW_RequestService.PlayerName(call.m_iPlayerId));

		result = string.Format("%1 is %2 to %3, %4 m away.", squad, Verb(), label, Math.Round(distance));
		Print(string.Format("CLW: request %1 %2 -> %3 %4 at %5 (%6 m)", call.m_sFaction, KindName(), squad, OrderCommand(), pos, Math.Round(distance)), LogLevel.NORMAL);
		return true;
	}

	protected string Verb()
	{
		return "moving";
	}
}

class CLW_Action_RequestReinforce : CLW_Action_RequestOrder
{
	protected override string KindName()
	{
		return CLW_RequestKind.REINFORCE;
	}

	protected override string OrderCommand()
	{
		return "DEFEND";
	}

	protected override string Verb()
	{
		return "reinforcing you, moving";
	}

	// reinforce always means "to me"
	override bool Validate(CLW_ActionCall call, out string error)
	{
		call.SetString("where", "here");
		return super.Validate(call, error);
	}
}

class CLW_Action_RequestRecon : CLW_Action_RequestOrder
{
	protected override string KindName()
	{
		return CLW_RequestKind.RECON;
	}

	protected override string OrderCommand()
	{
		return "RECON";
	}

	protected override string Verb()
	{
		return "sent to recon, moving";
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (!call.HasArg("where"))
		{
			error = "recon needs a target: /request recon <grid|here|objective>";
			return false;
		}

		return super.Validate(call, error);
	}
}

class CLW_ActionsRequests
{
	// Script registration of the request entries. Call right after CLW_ActionRegistry.Reset() (the registry forgets script entries).
	static void Register()
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		Add(registry, "REQUEST_VEHICLE", "A player asks HQ for an empty vehicle from the faction catalog, parked at the vehicle depot (or HQ). Soldiers and leaders; supplies, caps and a cooldown apply.", new CLW_Action_RequestVehicle(), 0).AddParam("type", "string", false, "catalog type, kind or part of the name; empty = cheapest transport");
		Add(registry, "REQUEST_SQUAD", "A player (squad leader) asks HQ for an AI infantry team that joins the player's squad.", new CLW_Action_RequestSquad(), 0).AddParam("type", "string", false, "catalog infantry type, kind or part of the name; empty = main infantry unit");
		Add(registry, "REQUEST_RESUPPLY", "A player asks HQ for an ammo / resupply box at the player's position (near a friendly base) or at the HQ.", new CLW_Action_RequestResupply(), CLW_RequestConfig.RESUPPLY_COST);
		Add(registry, "REQUEST_REINFORCE", "A player (squad leader) asks HQ to send the nearest free AI group to the player's position.", new CLW_Action_RequestReinforce(), 0);
		Add(registry, "REQUEST_RECON", "A player (squad leader) asks HQ to send the nearest free AI group to look at a map grid, an objective or the player's position.", new CLW_Action_RequestRecon(), 0).AddParam("where", "string", false, "grid 'ddd-ddd' (100 m cells), objective id or 'here'");
		Print("CLW: request entries registered (REQUEST_VEHICLE, REQUEST_SQUAD, REQUEST_RESUPPLY, REQUEST_REINFORCE, REQUEST_RECON)", LogLevel.NORMAL);
	}

	protected static CLW_ActionEntry Add(CLW_ActionRegistry registry, string name, string description, CLW_ActionExecutor executor, int cost)
	{
		CLW_ActionEntry entry = CLW_ActionEntry.Create(name, CLW_ActionEntry.LAYER_PRIMITIVE, description, executor);
		entry.m_sAuthority = CLW_ActionEntry.AUTH_PLAYER;
		entry.m_sChannel = "requests";
		entry.m_iCost = cost;
		entry.m_aRequires.Insert("requests");
		entry.m_aTags.Insert("request");
		registry.Register(entry);
		return entry;
	}
}

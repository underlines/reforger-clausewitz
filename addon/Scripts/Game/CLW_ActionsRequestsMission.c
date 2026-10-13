// Player-authority request entries for the HQ chat agent: REQUEST_MISSION and REQUEST_STATUS (registered by CLW_ActionsRequests.Register).
// Same structure as the other request executors (CLW_Action_RequestBase: war running, living requester on the side, cooldown; result =
// English text for log / history, call.m_sResultWire = the same for the player as CLW_Loc wires). Nothing here names a vanilla prefab,
// faction or objective: the offers come from the rule commander's plan (CLW_RuleCommander.BuildOffers) and are published by the
// existing mission board, the status from the faction's SITREP, contact book and objectives.

// ---- REQUEST_MISSION: the asker wants a task; HQ offers one through the mission board (to his squad only) ----
class CLW_Action_RequestMission : CLW_Action_RequestBase
{
	protected override string KindName()
	{
		return CLW_RequestKind.MISSION;
	}

	override bool IsAvailable(string factionKey)
	{
		return CLW_GameMode.GetRegistry() != null;
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (!super.Validate(call, error))
			return false;

		SCR_AIGroup group = CLW_RequestService.GroupOf(call.m_iPlayerId);
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!group || !registry)
			return Refuse(call, error, "you are not in a squad", CLW_Loc.W("#CLW-Req_NoSquad"));

		CLW_MissionBoard board = CLW_MissionBoard.Cast(CLW_MissionHook.Get());
		if (!board)
			return Refuse(call, error, "HQ has no mission board running", CLW_Loc.W("#CLW-ReqMission_NoBoard"));

		string squad = registry.GetId(group);
		string state = board.RequestState(call.m_sFaction, squad, call.m_iPlayerId);
		if (state == "other")
			return Refuse(call, error, "only your squad leader can take a mission: ask him to request one", CLW_Loc.W("#CLW-ReqMission_LeaderOnly"));
		if (state == "none")
			return Refuse(call, error, "HQ has not registered your squad yet, try again in a few seconds", CLW_Loc.W("#CLW-ReqMission_NotReady"));
		if (state == "accepted")
			return Refuse(call, error, "you already have a mission: finish or abandon it first", CLW_Loc.W("#CLW-ReqMission_Active"));

		if (board.PendingOffers(call.m_sFaction, squad) >= CLW_RequestConfig.MISSION_MAX_PENDING)
			return Refuse(call, error, "you already have mission offers waiting: open the task list (map / J menu)", CLW_Loc.W("#CLW-ReqMission_Pending"));

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		SCR_AIGroup group = CLW_RequestService.GroupOf(call.m_iPlayerId);
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		CLW_MissionBoard board = CLW_MissionBoard.Cast(CLW_MissionHook.Get());
		if (!group || !registry || !board || !call.m_Rules)
			return Refuse(call, result, "HQ has no mission to offer right now", CLW_Loc.W("#CLW-ReqMission_None"));

		string squad = registry.GetId(group);
		array<ref CLW_MissionOffer> offers = {};
		call.m_Rules.BuildOffers(squad, offers);
		if (offers.IsEmpty())
			return Refuse(call, result, "HQ has no sensible objective for your squad right now, try again later", CLW_Loc.W("#CLW-ReqMission_None"));

		if (!board.QueueNow(call.m_sFaction, squad, offers))
			return Refuse(call, result, "HQ could not prepare a mission for your squad", CLW_Loc.W("#CLW-ReqMission_NotReady"));

		string kinds;
		foreach (CLW_MissionOffer offer : offers)
		{
			if (!kinds.IsEmpty())
				kinds += ", ";
			kinds += offer.m_sKind;
			if (offer.m_bHasPos)
				kinds += " " + CLW_Places.Grid(offer.m_vPos);
			else
				kinds += " " + offer.m_sTarget;
		}

		Granted(call);
		result = string.Format("%1 mission offer(s) for your squad (%2): check the task list (map / J menu) in a few seconds.", offers.Count(), kinds);
		call.m_sResultWire = CLW_Loc.W("#CLW-ReqMission_Offered", CLW_Loc.N(offers.Count()));
		Print(string.Format("CLW: request mission %1 player=%2 squad=%3 offers=%4 (%5)", call.m_sFaction, call.m_iPlayerId, squad, offers.Count(), kinds), LogLevel.NORMAL);
		return true;
	}
}

// ---- REQUEST_STATUS: compact, fog-limited picture for the asker's faction (read only, no cost) ----
class CLW_Action_RequestStatus : CLW_Action_RequestBase
{
	protected override string KindName()
	{
		return CLW_RequestKind.STATUS;
	}

	protected string StateKey(CLW_ControlPoint point, string faction)
	{
		if (point.m_bContested)
			return "#CLW-ReqStatus_StateContested";
		if (point.m_sOwner == faction)
			return "#CLW-ReqStatus_StateHeld";
		if (point.m_sOwner.IsEmpty())
			return "#CLW-ReqStatus_StateNeutral";

		return "#CLW-ReqStatus_StateEnemy";
	}

	protected string ContactKindKey(CLW_Contact contact)
	{
		if (contact.m_sKind == CLW_Contact.KIND_VEHICLE)
			return "#CLW-ReqStatus_KindVehicle";
		if (contact.m_sKind == CLW_Contact.KIND_INFANTRY)
			return "#CLW-ReqStatus_KindInfantry";

		return "#CLW-ReqStatus_KindUnknown";
	}

	protected string Add(string text, string line)
	{
		if (line.IsEmpty())
			return text;
		if (text.IsEmpty())
			return line;

		return text + "\n" + line;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		string faction = call.m_sFaction;
		vector pos;
		if (!war || !CLW_RequestService.PlayerPos(call.m_iPlayerId, pos))
			return Refuse(call, result, "no status available", CLW_Loc.W("#CLW-ReqStatus_None"));

		string wire;

		// 1. the faction's SITREP headline (same data as the rule SITREP the players read)
		CLW_Sitrep sitrep = CLW_SitrepStore.Current(faction);
		if (sitrep)
			wire = Add(wire, sitrep.m_sHeadline);

		// 2. supplies and own squads
		int squads;
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (registry && call.m_Rules)
		{
			CLW_Orders orders = call.m_Rules.GetOrders();
			array<string> ids = {};
			registry.GetCommandableIds(faction, ids);
			foreach (string id : ids)
			{
				if (orders && orders.UnitAlive(id) > 0)
					squads++;
			}
		}

		wire = Add(wire, CLW_Loc.W("#CLW-ReqStatus_Force", CLW_Loc.N(war.GetSupplies(faction)), CLW_Loc.N(squads)));

		// 3. objectives near the player: held / contested / enemy / neutral, nearest first
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		array<CLW_ControlPoint> shown = {};
		if (points)
		{
			for (int n = 0; n < CLW_RequestConfig.STATUS_MAX_OBJECTIVES; n++)
			{
				CLW_ControlPoint best;
				float bestDist = CLW_RequestConfig.STATUS_OBJECTIVE_RANGE_M;
				best = null;
				for (int i = 0; i < points.Count(); i++)
				{
					CLW_ControlPoint point = points.Get(i);
					if (!point || shown.Contains(point))
						continue;

					float d = vector.DistanceXZ(pos, point.m_vPos);
					if (d <= bestDist)
					{
						bestDist = d;
						best = point;
					}
				}

				if (!best)
					break;

				shown.Insert(best);
				int objDist = Math.Round(bestDist);
				wire = Add(wire, CLW_Loc.W("#CLW-ReqStatus_Objective", best.m_sName + " (" + best.m_sId + ")", StateKey(best, faction), CLW_Loc.N(objDist)));
			}
		}

		if (shown.IsEmpty())
			wire = Add(wire, CLW_Loc.W("#CLW-ReqStatus_NoObjective", CLW_Loc.N(Math.Round(CLW_RequestConfig.STATUS_OBJECTIVE_RANGE_M))));

		// 4. last known enemy contact near the player (only what this faction's units reported)
		CLW_Contact nearest;
		float nearestDist = CLW_RequestConfig.STATUS_CONTACT_RANGE_M;
		float now = CLW_Orders.Now();
		if (call.m_Rules && call.m_Rules.GetBook())
		{
			array<CLW_Contact> contacts = {};
			call.m_Rules.GetBook().GetContacts(contacts);
			foreach (CLW_Contact contact : contacts)
			{
				if (!contact || now - contact.m_fLastSeen > CLW_RequestConfig.STATUS_CONTACT_MAX_AGE_SEC)
					continue;

				float cd = vector.DistanceXZ(pos, contact.m_vPos);
				if (cd <= nearestDist)
				{
					nearestDist = cd;
					nearest = contact;
				}
			}
		}

		if (nearest)
		{
			int contactDist = Math.Round(nearestDist);
			int age = Math.Round(Math.Max(0, now - nearest.m_fLastSeen));
			wire = Add(wire, CLW_Loc.W("#CLW-ReqStatus_Contact", ContactKindKey(nearest), CLW_Loc.N(nearest.m_iCount), CLW_SitrepBuilder.Where(nearest.m_vPos, points, faction), CLW_Places.Grid(nearest.m_vPos), CLW_Loc.N(contactDist), CLW_Loc.N(age)));
		}
		else
		{
			wire = Add(wire, CLW_Loc.W("#CLW-ReqStatus_NoContact", CLW_Loc.N(Math.Round(CLW_RequestConfig.STATUS_CONTACT_RANGE_M))));
		}

		Granted(call);
		result = CLW_Loc.Resolve(wire);	// English on the server: log, history and the sidecar
		call.m_sResultWire = wire;
		Print(string.Format("CLW: request status %1 player=%2 objectives=%3 contact=%4", faction, call.m_iPlayerId, shown.Count(), nearest != null), LogLevel.NORMAL);
		return true;
	}
}

// Executors of the M2 command set on the action registry (M3 phase R): the orders MOVE / ATTACK / DEFEND / RECON / FALL_BACK,
// BUY and mission offers. They call the same code paths the LLM bridge used before (CLW_RuleCommander.ApplyOrder,
// CLW_ForceSpawner.Buy, CLW_MissionHook.OnOffers): no behaviour change. Data side: Configs/CLW/Actions/Actions.conf.
// Executors are shared per entry and hold no state; everything per call is in CLW_ActionCall.

// ORDERS: squad (unit), target (objective id) or pos [x, z], reason. One executor serves all five command entries, the entry name
// (call.m_sAction) is the command.
class CLW_Action_Order : CLW_ActionExecutor
{
	override bool Validate(CLW_ActionCall call, out string error)
	{
		CLW_ECommand command;
		if (!CLW_Command.FromName(call.m_sAction, command))
		{
			error = "unknown command";
			return false;
		}

		bool hasPos = call.HasPos("pos");
		string target = call.GetString("target");
		if (!target.IsEmpty())
		{
			CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
			if (points && points.Find(target))
				return true;	// objective id wins over a pos given with it

			if (!hasPos)
			{
				error = "unknown objective " + target;
				return false;
			}

			call.SetString("target", "");	// unknown id but a valid pos: use the pos
			return true;
		}

		if (!hasPos && command != CLW_ECommand.FALL_BACK)
		{
			error = "needs a target or pos";
			return false;
		}

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		if (!call.m_Rules)
		{
			result = "no commander";
			call.m_sResultWire = CLW_Loc.W("#CLW-Req_NoCommander");
			return false;
		}

		string why;
		string squad = call.GetString("squad");
		if (!call.m_Rules.ApplyOrder(squad, call.m_sAction, call.GetString("target"), call.GetPos("pos"), call.m_sReason, why))
		{
			if (why.IsEmpty())
				why = "not applied";
			result = why;
			return false;
		}

		call.m_iApplied = 1;
		result = squad + " " + call.m_sAction;
		call.m_sResultWire = CLW_Loc.W("#CLW-Act_Ordered", squad, call.m_sAction);
		return true;
	}
}

// BUY: type (catalog type), count 1..3 (clamped), reason.
class CLW_Action_Buy : CLW_ActionExecutor
{
	override bool IsAvailable(string factionKey)
	{
		return CLW_GameMode.GetForceSpawner() != null && CLW_GameMode.GetForceCatalog() != null;
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!spawner || !catalog)
		{
			error = "no spawner / catalog";
			return false;
		}

		string type = call.GetString("type");
		if (!catalog.Find(call.m_sFaction, type))
		{
			error = "unknown catalog type " + type;
			return false;
		}

		// economy guard (WP6): no purchase that will be refused (HQ reserve, AI cap); the text reaches the LLM as the action result
		if (!EconomyGate(call, type, error))
			return false;

		// spawn on assignment (owner playtest 2026-10-12): an LLM purchase must come with the first order of the new units, so nothing is
		// bought just to wait at the base. The order is checked here and applied when the unit is ready (CLW_PendingOrders).
		if (call.m_sSource == "llm" && call.m_sAuthority == CLW_ActionEntry.AUTH_HQ)
			return ValidateAssignment(call, error);

		return true;
	}

	// WP6 buy gate for AI purchases (this action is the LLM / rules path; player requests use CLW_RequestService, not this): the HQ stock
	// must stay at or above hqReserve after paying, and the side must not be at its AI cap. False with the refusal text in error.
	protected bool EconomyGate(CLW_ActionCall call, string type, out string error)
	{
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		CLW_ForceEntry entry;
		if (catalog)
			entry = catalog.Find(call.m_sFaction, type);
		if (!entry)
			return true;

		string side = call.m_sFaction;
		int live;
		int cap;
		if (CLW_EconomyGuard.AiCapFull(side, live, cap))
		{
			error = string.Format("AI cap (%1 live + pending, cap %2)", live, cap);
			if (CLW_EconomyGuard.LogChanged("buycap-llm", side, "" + live + "/" + cap))
				Print(string.Format("CLW: RemoteCommander %1: no buy, AI cap %2/%3", side, live, cap), LogLevel.NORMAL);
			return false;
		}

		CLW_EconomyGuard.LogChanged("buycap-llm", side, "ok");
		string why;
		if (!CLW_EconomyGuard.CanAiSpend(side, entry.m_iCost, why))
		{
			error = why;
			if (CLW_EconomyGuard.LogChanged("buyreserve-llm", side, why))
				Print(string.Format("CLW: RemoteCommander %1: buy %2 refused: %3 (cost %4)", side, type, why, entry.m_iCost), LogLevel.NORMAL);
			return false;
		}

		CLW_EconomyGuard.LogChanged("buyreserve-llm", side, "ok");
		return true;
	}

	// command (MOVE | ATTACK | DEFEND | RECON) + a known objective id (target) or a position (pos).
	protected bool ValidateAssignment(CLW_ActionCall call, out string error)
	{
		string commandName = call.GetString("command");
		CLW_ECommand command;
		if (commandName.IsEmpty() || !CLW_Command.FromName(commandName, command) || command == CLW_ECommand.FALL_BACK)
		{
			error = "a purchase needs an assignment: command MOVE|ATTACK|DEFEND|RECON for the new unit";
			return false;
		}

		string target = call.GetString("target");
		if (!target.IsEmpty())
		{
			CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
			if (!points || !points.Find(target))
			{
				error = "assignment target is not an objective: " + target;
				return false;
			}

			return true;
		}

		if (!call.HasPos("pos"))
		{
			error = "a purchase needs an assignment: target (objective id) or pos [x, z]";
			return false;
		}

		return true;
	}

	// True when at least one unit was bought (call.m_iApplied = units). A refusal is logged here, result then holds the reason.
	override bool Execute(CLW_ActionCall call, out string result)
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (!spawner)
		{
			result = "no spawner";
			call.m_sResultWire = CLW_Loc.W("#CLW-Act_NoSpawner");
			return false;
		}

		string type = call.GetString("type");
		int count = Math.Clamp(call.GetInt("count", 1), 1, 3);
		string prefix = "LLM";
		if (call.m_sSource != "llm")
			prefix = call.m_sSource;

		// the assignment of the new units (validated for LLM buys; other sources may buy without one)
		string commandName = call.GetString("command");
		CLW_ECommand assignCommand;
		bool assigned = !commandName.IsEmpty() && CLW_Command.FromName(commandName, assignCommand);
		if (assigned)
			commandName = CLW_Command.ToName(assignCommand);
		string assignTarget = call.GetString("target");
		vector assignPos = call.GetPos("pos");

		int applied;
		string summary;
		for (int n = 0; n < count; n++)
		{
			string error;
			string tag;
			bool started;
			if (n > 0 && !EconomyGate(call, type, error))
			{
				// the first unit passed Validate; later ones re-check reserve / cap after the earlier purchases
				Print(string.Format("CLW: RemoteCommander %1: buy %2 (%3/%4) stopped: %5", call.m_sFaction, type, n + 1, count, error), LogLevel.NORMAL);
				break;
			}

			if (assigned)
			{
				tag = CLW_PendingOrders.Add(call.m_sFaction, commandName, assignTarget, assignPos, call.m_sReason, CLW_Orders.SOURCE_LLM);
				started = spawner.BuyTagged(call.m_sFaction, type, prefix + ": " + call.m_sReason, tag, error);
				if (!started)
					CLW_PendingOrders.Remove(tag);
			}
			else
			{
				started = spawner.Buy(call.m_sFaction, type, prefix + ": " + call.m_sReason, error);
			}

			if (!started)
			{
				Print(string.Format("CLW: RemoteCommander %1: buy %2 (%3/%4) refused: %5", call.m_sFaction, type, n + 1, count, error), LogLevel.NORMAL);
				if (applied == 0)
					result = error;
				break;
			}

			applied++;
			if (!summary.IsEmpty())
				summary += ", ";
			summary += "buy " + type;
			if (assigned)
				summary += " -> " + commandName + " " + assignTarget;
			Print(string.Format("CLW: RemoteCommander %1: BUY %2 (%3/%4) for %5 %6: %7", call.m_sFaction, type, n + 1, count, commandName, assignTarget, call.m_sReason), LogLevel.NORMAL);
		}

		call.m_iApplied = applied;
		if (applied > 0)
		{
			result = summary;
			call.m_sResultWire = CLW_Loc.W("#CLW-Act_Bought", type, CLW_Loc.N(applied));
		}

		return applied > 0;
	}
}

// OFFER_MISSION: squad (a human-led squad) + payload = the CLW_MissionDto of the reply (up to MAX_OFFERS offers, each validated).
class CLW_Action_OfferMission : CLW_ActionExecutor
{
	override bool Validate(CLW_ActionCall call, out string error)
	{
		CLW_MissionDto mission = CLW_MissionDto.Cast(call.m_Payload);
		if (!mission || !mission.offers || mission.offers.IsEmpty())
		{
			error = "no offers";
			return false;
		}

		if (!call.m_Rules)
		{
			error = "no commander";
			return false;
		}

		string squad = call.GetString("squad");
		if (!call.m_Rules.IsCommandable(squad))
		{
			error = "unknown squad";
			return false;
		}

		if (!call.m_Rules.IsHumanLed(squad))
		{
			error = "not human-led (missions are for human-led squads only)";
			return false;
		}

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		CLW_MissionDto mission = CLW_MissionDto.Cast(call.m_Payload);
		if (!mission || !mission.offers)
		{
			result = "no offers";
			call.m_sResultWire = CLW_Loc.W("#CLW-Act_NoOffers");
			return false;
		}

		string squad = call.GetString("squad");
		array<ref CLW_MissionOffer> offers = {};
		foreach (CLW_OfferDto dto : mission.offers)
		{
			if (!dto || offers.Count() >= CLW_RemoteCommander.MAX_OFFERS)
				continue;

			CLW_MissionOffer offer = BuildOffer(call, dto);
			if (offer)
				offers.Insert(offer);
		}

		if (offers.IsEmpty())
		{
			result = "no valid offer";
			call.m_sResultWire = CLW_Loc.W("#CLW-Act_NoOffers");
			return false;
		}

		CLW_MissionHook.Get().OnOffers(call.m_sFaction, squad, offers);
		call.m_iApplied = offers.Count();
		result = squad + " " + offers.Count() + " offer(s)";
		call.m_sResultWire = CLW_Loc.W("#CLW-Act_Offers", squad, CLW_Loc.N(offers.Count()));
		Print(string.Format("CLW: RemoteCommander %1: MISSION offers for %2: %3", call.m_sFaction, squad, offers.Count()), LogLevel.NORMAL);
		return true;
	}

	protected CLW_MissionOffer BuildOffer(CLW_ActionCall call, CLW_OfferDto dto)
	{
		if (dto.kind != "CAPTURE" && dto.kind != "DEFEND" && dto.kind != "RECON" && dto.kind != "DESTROY")
		{
			Print(string.Format("CLW: RemoteCommander %1: offer rejected (bad kind %2)", call.m_sFaction, dto.kind), LogLevel.WARNING);
			return null;
		}

		CLW_MissionOffer offer = new CLW_MissionOffer();
		offer.m_sKind = dto.kind;
		offer.m_sSource = call.m_sSource;
		offer.m_sBrief = dto.brief;
		if (offer.m_sBrief.Length() > CLW_RemoteCommander.MAX_BRIEF)
			offer.m_sBrief = offer.m_sBrief.Substring(0, CLW_RemoteCommander.MAX_BRIEF);

		// resolve to an objective first (id, display name, position inside the capture radius + margin): only an offer that matches
		// no objective stays a bare position. A positional CAPTURE completes only when the leader stands at the point (M4.6).
		bool hasPos = false;
		float x;
		float z;
		if (dto.pos && dto.pos.Count() >= 2)
		{
			x = dto.pos[0];
			z = dto.pos[1];
			hasPos = x == x && z == z && Math.AbsFloat(x) < 100000 && Math.AbsFloat(z) < 100000;
		}

		// a position names an objective only for CAPTURE / DEFEND (ownership matters there); RECON / DESTROY keep their exact point
		bool posMayResolve = hasPos && (dto.kind == "CAPTURE" || dto.kind == "DEFEND");
		string how;
		CLW_ControlPoint resolved = CLW_MissionTarget.Resolve(dto.target, posMayResolve, x, z, how);
		if (resolved)
		{
			offer.m_sTarget = resolved.m_sId;
			Print(string.Format("CLW: mission offer %1 '%2' pos=%3 resolved to objective %4 by %5", dto.kind, dto.target, hasPos, resolved.m_sId, how), LogLevel.NORMAL);
			return offer;
		}

		if (hasPos)
		{
			offer.m_bHasPos = true;
			offer.m_vPos = Vector(x, 0, z);
			Print(string.Format("CLW: mission offer %1 '%2' matches no objective, kept as position %3/%4", dto.kind, dto.target, Math.Round(x), Math.Round(z)), LogLevel.NORMAL);
			return offer;
		}

		Print(string.Format("CLW: RemoteCommander %1: offer rejected (no valid target / pos, target='%2')", call.m_sFaction, dto.target), LogLevel.WARNING);
		return null;
	}
}

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

		return true;
	}

	// True when at least one unit was bought (call.m_iApplied = units). A refusal is logged here, result then holds the reason.
	override bool Execute(CLW_ActionCall call, out string result)
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (!spawner)
		{
			result = "no spawner";
			return false;
		}

		string type = call.GetString("type");
		int count = Math.Clamp(call.GetInt("count", 1), 1, 3);
		string prefix = "LLM";
		if (call.m_sSource != "llm")
			prefix = call.m_sSource;

		int applied;
		string summary;
		for (int n = 0; n < count; n++)
		{
			string error;
			if (!spawner.Buy(call.m_sFaction, type, prefix + ": " + call.m_sReason, error))
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
			Print(string.Format("CLW: RemoteCommander %1: BUY %2 (%3/%4): %5", call.m_sFaction, type, n + 1, count, call.m_sReason), LogLevel.NORMAL);
		}

		call.m_iApplied = applied;
		if (applied > 0)
			result = summary;
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
			return false;
		}

		CLW_MissionHook.Get().OnOffers(call.m_sFaction, squad, offers);
		call.m_iApplied = offers.Count();
		result = squad + " " + offers.Count() + " offer(s)";
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

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (!dto.target.IsEmpty() && points && points.Find(dto.target))
		{
			offer.m_sTarget = dto.target;
			return offer;
		}

		if (dto.pos && dto.pos.Count() >= 2)
		{
			float x = dto.pos[0];
			float z = dto.pos[1];
			if (x == x && z == z && Math.AbsFloat(x) < 100000 && Math.AbsFloat(z) < 100000)
			{
				offer.m_bHasPos = true;
				offer.m_vPos = Vector(x, 0, z);
				return offer;
			}
		}

		Print(string.Format("CLW: RemoteCommander %1: offer rejected (no valid target / pos, target='%2')", call.m_sFaction, dto.target), LogLevel.WARNING);
		return null;
	}
}

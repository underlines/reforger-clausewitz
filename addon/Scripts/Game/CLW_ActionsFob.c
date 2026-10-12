// FOB intents on the action registry (M3 phase F): ESTABLISH_FOB <site> and UPGRADE_FOB <fob>. Entries live in Configs/CLW/Actions/Actions.conf
// (channel "fob", requires capability `fob`, authority hq); the executors below do the work through CLW_FobManager. Also the rule
// commander's FOB doctrine (CLW_FobDoctrine), which calls the same registry entries as the LLM. Executors hold no per-call state.

// ESTABLISH_FOB: site (a candidate id from the situation's fobSites), reason. Pays the cost, then a builder drives out and builds.
class CLW_Action_EstablishFob : CLW_ActionExecutor
{
	// Drops out of the manifest when the mode allows no FOBs (fobMax 0). Does not depend on the current FOB count (no manifest churn).
	override bool IsAvailable(string factionKey)
	{
		return CLW_FobManager.GetMax() > 0;
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		string siteId = call.GetString("site");
		CLW_FobSite site = CLW_FobSites.FindSite(siteId);
		if (!site)
		{
			error = "unknown FOB site " + siteId;
			return false;
		}

		return CLW_FobManager.CanEstablish(call.m_sFaction, site, error);
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		string siteId = call.GetString("site");
		string why;
		if (!CLW_FobManager.Establish(call.m_sFaction, siteId, call.m_sSource, call.m_sReason, why))
		{
			result = why;
			return false;
		}

		call.m_iApplied = 1;
		result = "ESTABLISH_FOB " + siteId;
		return true;
	}
}

// UPGRADE_FOB: fob (id of an active FOB at tier 1). Adds the vehicle depot and medical services for half the FOB cost.
class CLW_Action_UpgradeFob : CLW_ActionExecutor
{
	override bool IsAvailable(string factionKey)
	{
		return CLW_FobManager.GetMax() > 0;
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		return CLW_FobManager.CanUpgrade(call.m_sFaction, call.GetString("fob"), error);
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		string fobId = call.GetString("fob");
		string why;
		if (!CLW_FobManager.Upgrade(call.m_sFaction, fobId, call.m_sSource, why))
		{
			result = why;
			return false;
		}

		call.m_iApplied = 1;
		result = "UPGRADE_FOB " + fobId;
		return true;
	}
}

// Rule commander doctrine (called from CLW_RuleCommander.Think before it spends): with the `fob` capability on, fewer FOBs than fobMax,
// none in progress and supplies for the cost plus the doctrine reserve, establish one at the first candidate site (frontline, toward the
// contested centre). An active FOB at tier 1 is upgraded when supplies are plentiful. Same registry entries as the LLM.
class CLW_FobDoctrine
{
	static const float START_DELAY_SEC = 45;		// the start forces field first
	static const float INTERVAL_SEC = 45;			// per side
	static const int UPGRADE_EXTRA_SUPPLIES = 100;	// kept on top of the upgrade cost and the reserve

	protected static ref map<string, float> s_mLast = new map<string, float>();

	static void Think(CLW_RuleCommander rules, string side, CLW_WarState war)
	{
		if (!rules || !war || !CLW_Capabilities.Has("fob") || CLW_FobManager.GetMax() <= 0)
			return;

		if (war.GetElapsedSec() < START_DELAY_SEC)
			return;

		float now = CLW_Orders.Now();
		float last = -1000;
		s_mLast.Find(side, last);
		if (now < last)
			last = -1000;	// a new session restarted the clock

		if (now - last < INTERVAL_SEC)
			return;

		s_mLast.Set(side, now);
		if (CLW_FobManager.CountInProgress(side) > 0)
			return;

		string result;
		if (CLW_FobManager.CountLive(side) < CLW_FobManager.GetMax())
		{
			int needed = CLW_FobManager.GetCost() + CLW_RuleCommander.SUPPLY_RESERVE;
			if (war.GetSupplies(side) < needed)
				return;

			array<CLW_FobSite> sites = {};
			if (CLW_FobSites.PickSites(side, 1, sites) == 0)
			{
				Print(string.Format("CLW: fob doctrine %1: wants a FOB but no site is usable now", side), LogLevel.NORMAL);
				return;
			}

			CLW_ActionCall call = NewCall("ESTABLISH_FOB", side, rules);
			call.SetString("site", sites[0].m_sId);
			call.m_sReason = "doctrine: one forward base toward the contested centre";
			call.SetString("reason", call.m_sReason);
			if (CLW_ActionRegistry.Get().Run(call, result))
				Print(string.Format("CLW: fob doctrine %1: ESTABLISH_FOB %2 (%3)", side, sites[0].m_sId, sites[0].m_sName), LogLevel.NORMAL);
			else
				Print(string.Format("CLW: fob doctrine %1: ESTABLISH_FOB %2 refused: %3", side, sites[0].m_sId, result), LogLevel.NORMAL);

			return;
		}

		// logistics off: the one pool must be plentiful; logistics on: the upgrade is paid from the FOB's own stock (checked per FOB below)
		bool perBase = CLW_Capabilities.Has("logistics");
		if (!perBase && war.GetSupplies(side) < CLW_FobManager.GetUpgradeCost() + CLW_RuleCommander.SUPPLY_RESERVE + UPGRADE_EXTRA_SUPPLIES)
			return;

		array<CLW_Fob> own = {};
		CLW_FobManager.GetFobs(side, own, true);
		foreach (CLW_Fob fob : own)
		{
			if (fob.m_iTier >= 2)
				continue;

			if (perBase && CLW_SupplyStore.Get(side, fob.m_sId) < CLW_FobManager.GetUpgradeCost() + UPGRADE_EXTRA_SUPPLIES)
				continue;

			CLW_ActionCall upgrade = NewCall("UPGRADE_FOB", side, rules);
			upgrade.SetString("fob", fob.m_sId);
			upgrade.m_sReason = "doctrine: supplies are plentiful";
			if (CLW_ActionRegistry.Get().Run(upgrade, result))
				Print(string.Format("CLW: fob doctrine %1: UPGRADE_FOB %2", side, fob.m_sId), LogLevel.NORMAL);
			else
				Print(string.Format("CLW: fob doctrine %1: UPGRADE_FOB %2 refused: %3", side, fob.m_sId, result), LogLevel.NORMAL);

			return;
		}
	}

	protected static CLW_ActionCall NewCall(string action, string side, CLW_RuleCommander rules)
	{
		CLW_ActionCall call = new CLW_ActionCall();
		call.m_sAction = action;
		call.m_sFaction = side;
		call.m_sAuthority = CLW_ActionEntry.AUTH_HQ;
		call.m_sSource = "rules";
		call.m_Rules = rules;
		return call;
	}
}

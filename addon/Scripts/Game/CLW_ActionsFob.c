// FOB intents on the action registry (M3 phase F): ESTABLISH_FOB and UPGRADE_FOB. Entries live in Configs/CLW/Actions/Actions.conf
// (channel "fob", requires capability `fob`, authority hq); the ESTABLISH_FOB parameters and description are replaced in script
// (CLW_FobActionSpec, applied by CLW_ActionRegistry.Register) for the M4.6 two-step tool. The executors below do the work through
// CLW_FobManager. Also the rule commander's FOB doctrine (CLW_FobDoctrine), which calls the same registry entries as the LLM. Executors hold no
// per-call state.

// Parameters / description of ESTABLISH_FOB as the LLM sees them (manifest) and the game validates them (M4.6, dev/notes/fob-tool-design.md).
class CLW_FobActionSpec
{
	static const string NAME = "ESTABLISH_FOB";
	static const string AREA_NONE = "none";

	static void Apply(CLW_ActionEntry entry)
	{
		if (!entry || entry.m_sName != NAME)
			return;

		entry.Normalize();
		entry.m_sDescription = "Place a forward operating base (FOB), or decline to. A FOB gives a spawn point, a command post, a supply cache and an arsenal near the front; it costs fobRules.cost supplies, a transport with an escort drives out and builds it. TWO STEPS. Step 1: look at fobMap (the whole map: terrain, roads, towns, objectives by cell, front, known contacts) and reply with area = the cell id (e.g. D5) where the base should stand plus intent = what it is for (cover a flank, shorten the way to an objective, ...); or area = \"none\" with a reason when no FOB is worth it now. Step 2: the game then shows you that cell at 100 m resolution and you pick the exact sub-cell; the game validates the spot (dry, flat, road within reach, clear of HQs, objectives, other FOBs and enemy contacts) and moves it to the nearest valid ground, or tells you why not so you can retry once. Only one FOB may be in progress per side, max war.mode.params.fobMax per side.";
		entry.m_aParams.Clear();
		entry.m_aParams.Insert(CLW_ActionParam.Create("area", "string", false, "step 1: cell id from fobMap (column letter + row number), or none to decline"));
		entry.m_aParams.Insert(CLW_ActionParam.Create("intent", "string", false, "step 1: what the base is for, short"));
		entry.m_aParams.Insert(CLW_ActionParam.Create("pos", "position", false, "step 2 (set by the game from your zoom pick): [x, z]"));
		entry.m_aParams.Insert(CLW_ActionParam.Create("site", "string", false, "legacy: a site id of the rule commander's pool"));
		entry.m_aParams.Insert(CLW_ActionParam.Create("reason", "string", false, "short text"));
	}
}

// ESTABLISH_FOB, three shapes:
//   area (+ intent)   step 1 of the LLM tool: asks the sidecar for the zoom (CLW_RemoteCommander.StartFobZoom); area "none" = decline
//   pos               the validated zoom pick: CLW_FobPlacer.Resolve snaps it to valid ground and makes a pool site, then as `site`
//   site              a pool site id (rule doctrine, legacy LLM replies): pays the cost, then a builder drives out and builds
class CLW_Action_EstablishFob : CLW_ActionExecutor
{
	// Drops out of the manifest when the mode allows no FOBs (fobMax 0). Does not depend on the current FOB count (no manifest churn).
	override bool IsAvailable(string factionKey)
	{
		return CLW_FobManager.GetMax() > 0;
	}

	protected bool IsDecline(CLW_ActionCall call)
	{
		string area = call.GetString("area");
		area.ToLower();
		return area == CLW_FobActionSpec.AREA_NONE;
	}

	// AI callers only (the LLM and the doctrine reach this action; players do not): the HQ keeps its reserve.
	protected bool CheckReserve(CLW_ActionCall call, out string error)
	{
		string reserveWhy;
		if (!CLW_EconomyGuard.CanAiSpend(call.m_sFaction, CLW_FobManager.GetCost(), reserveWhy))
		{
			error = reserveWhy;
			if (CLW_EconomyGuard.LogChanged("fobreserve", call.m_sFaction, reserveWhy))
				Print(string.Format("CLW: fob %1 ESTABLISH_FOB refused: %2", call.m_sFaction, reserveWhy), LogLevel.NORMAL);

			return false;
		}

		return true;
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (IsDecline(call))
			return true;	// "no FOB now" needs no gate

		if (call.HasArg("site"))
		{
			string siteId = call.GetString("site");
			CLW_FobSite site = CLW_FobSites.FindSite(siteId);
			if (!site)
			{
				error = "unknown FOB site " + siteId;
				return false;
			}

			if (!CLW_FobManager.CanEstablish(call.m_sFaction, site, error))
				return false;

			return CheckReserve(call, error);
		}

		if (!CLW_FobManager.CanStart(call.m_sFaction, error))
			return false;

		if (!CheckReserve(call, error))
			return false;

		if (call.HasPos("pos"))
		{
			// the zoom pick: validate + snap, then continue as a pool site (the id goes back into the call for Execute)
			CLW_FobSite placed;
			if (!CLW_FobPlacer.Resolve(call.m_sFaction, call.GetPos("pos"), placed, error))
				return false;

			call.SetString("site", placed.m_sId);
			if (!CLW_FobManager.CanEstablish(call.m_sFaction, placed, error))
			{
				CLW_FobSites.RemoveSite(placed.m_sId);
				call.SetString("site", "");
				return false;
			}

			return true;
		}

		string area = call.GetString("area");
		if (area.IsEmpty())
		{
			error = "ESTABLISH_FOB needs an area (step 1) or a pos (step 2)";
			return false;
		}

		if (!CLW_MapDigest.AreaValid(area))
		{
			error = "unknown area '" + area + "': use a cell id from fobMap (column letter + row number)";
			return false;
		}

		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(call.m_sFaction);
		if (!commander)
		{
			error = "no commander link for " + call.m_sFaction;
			return false;
		}

		if (commander.IsFobZoomBusy())
		{
			error = "a FOB spot request is already in progress";
			return false;
		}

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		if (IsDecline(call))
		{
			call.m_iApplied = 1;
			result = "ESTABLISH_FOB declined";
			call.m_sResultWire = CLW_Loc.W("#CLW-Act_FobDeclined");
			if (CLW_FobPlanner.NoteFailure(call.m_sFaction, "declined", call.m_sReason))
				CLW_FobDoctrine.Fallback(call.m_Rules, call.m_sFaction);

			return true;
		}

		string siteId = call.GetString("site");
		if (siteId.IsEmpty())
		{
			// step 1: the sidecar is asked for the zoom; the answer arrives later (CLW_RemoteCommander.OnFobZoomReply)
			string area = call.GetString("area");
			area.ToUpper();
			CLW_RemoteCommander commander = CLW_GameMode.GetCommander(call.m_sFaction);
			string zoomWhy;
			if (!commander || !commander.StartFobZoom(area, call.GetString("intent"), call.m_sReason, zoomWhy))
			{
				result = zoomWhy;
				return false;
			}

			call.m_iApplied = 1;
			result = "ESTABLISH_FOB zoom " + area;
			call.m_sResultWire = CLW_Loc.W("#CLW-Act_FobZoom", area);
			return true;
		}

		string why;
		if (!CLW_FobManager.Establish(call.m_sFaction, siteId, call.m_sSource, call.m_sReason, why))
		{
			CLW_FobSite failed = CLW_FobSites.FindSite(siteId);
			if (failed && failed.m_sSource == "llm")
				CLW_FobSites.RemoveSite(siteId);

			result = why;
			return false;
		}

		CLW_FobPlanner.NoteSuccess(call.m_sFaction);
		call.m_iApplied = 1;
		result = "ESTABLISH_FOB " + siteId;
		call.m_sResultWire = CLW_Loc.W("#CLW-Act_FobOrdered", siteId);
		return true;
	}
}

// UPGRADE_FOB: fob (id of an active FOB at tier 1). Adds the vehicle depot service for half the FOB cost.
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
		call.m_sResultWire = CLW_Loc.W("#CLW-Act_FobUpgrade", fobId);
		return true;
	}
}

// Rule commander doctrine (called from CLW_RuleCommander.Think before it spends): with the `fob` capability on, fewer FOBs than fobMax,
// none in progress, the timing gate open (CLW_FobManager.IsReady: fobEarliestMin of war time + front knowledge), a front objective far
// enough from the HQ to need a forward base, and supplies for the cost plus the doctrine reserve, establish one at the first candidate
// site (CLW_FobSites.PickSites: toward the front objective, not behind the HQ; M4.6: the pool has a candidate per objective and side, and an
// objective an own FOB already covers comes last, so every flank gets one in turn). An active FOB at tier 1 is upgraded when supplies are
// plentiful. Same registry entries as the LLM. Fallback() is the LLM commander's second chance: two failed / declined LLM FOB decisions in a row.
class CLW_FobDoctrine
{
	static const float START_DELAY_SEC = 45;		// the start forces field first
	static const float INTERVAL_SEC = 45;			// per side
	static const int UPGRADE_EXTRA_SUPPLIES = 100;	// kept on top of the upgrade cost and the reserve

	protected static ref map<string, float> s_mLast = new map<string, float>();
	protected static ref map<string, string> s_mState = new map<string, string>();

	// The rule doctrine places a FOB now (no interval gate; every other gate still applies).
	static void Fallback(CLW_RuleCommander rules, string side)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!rules || !war)
		{
			Print(string.Format("CLW: fob fallback %1: no rule commander to run the doctrine", side), LogLevel.WARNING);
			return;
		}

		Think(rules, side, war, true);
	}

	static void Think(CLW_RuleCommander rules, string side, CLW_WarState war, bool force = false)
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

		if (!force && now - last < INTERVAL_SEC)
			return;

		s_mLast.Set(side, now);
		if (CLW_FobManager.CountInProgress(side) > 0)
			return;

		string result;
		if (CLW_FobManager.CountLive(side) < CLW_FobManager.GetMax())
		{
			int needed = CLW_FobManager.GetCost() + Math.Max(CLW_RuleCommander.SUPPLY_RESERVE, CLW_EconomyGuard.HqReserve());
			if (war.GetSupplies(side) < needed)
				return;

			string gateWhy;
			if (!CLW_FobManager.IsReady(side, gateWhy))
				return;	// the gate logs its state once per change

			// a FOB only pays off when the front is far from the HQ (the existing scaled FOB distance logic)
			CLW_ControlPoint front = CLW_FobSites.FrontObjective(side);
			CLW_Bases bases = CLW_GameMode.GetBases();
			if (front && bases)
			{
				float frontDist = vector.DistanceXZ(front.m_vPos, bases.GetBasePos(side));
				float frontMin = bases.ScaleDistance(CLW_FobManager.FRONT_MIN_M);
				if (frontDist < frontMin)
				{
					LogDoctrine(side, "near", string.Format("front objective %1 is only %2 m from the HQ (needs %3 m): no FOB needed yet", front.m_sId, Math.Round(frontDist), Math.Round(frontMin)));
					return;
				}
			}

			array<CLW_FobSite> sites = {};
			if (CLW_FobSites.PickSites(side, 1, sites) == 0)
			{
				LogDoctrine(side, "nosite", "wants a FOB but no site is usable now");
				return;
			}

			s_mState.Set(side, "go");

			CLW_ActionCall call = NewCall("ESTABLISH_FOB", side, rules);
			call.SetString("site", sites[0].m_sId);
			if (force)
				call.m_sReason = "doctrine: the LLM commander gave no usable FOB";
			else
				call.m_sReason = "doctrine: one forward base toward the contested centre";
			call.SetString("reason", call.m_sReason);
			if (CLW_ActionRegistry.Get().Run(call, result))
				Print(string.Format("CLW: fob doctrine %1: ESTABLISH_FOB %2 (%3) objective '%4'", side, sites[0].m_sId, sites[0].m_sName, sites[0].m_sObjective), LogLevel.NORMAL);
			else
				Print(string.Format("CLW: fob doctrine %1: ESTABLISH_FOB %2 refused: %3", side, sites[0].m_sId, result), LogLevel.NORMAL);

			return;
		}

		// logistics off: the one pool must be plentiful; logistics on: the upgrade is paid from the FOB's own stock (checked per FOB below)
		bool perBase = CLW_Capabilities.Has("logistics");
		if (!perBase && war.GetSupplies(side) < CLW_FobManager.GetUpgradeCost() + Math.Max(CLW_RuleCommander.SUPPLY_RESERVE, CLW_EconomyGuard.HqReserve()) + UPGRADE_EXTRA_SUPPLIES)
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

	// Doctrine state log, once per change per side.
	protected static void LogDoctrine(string side, string key, string text)
	{
		string last;
		if (s_mState.Find(side, last) && last == key)
			return;

		s_mState.Set(side, key);
		Print(string.Format("CLW: fob doctrine %1: %2", side, text), LogLevel.NORMAL);
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

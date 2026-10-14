// Mode "short": the spec section 2 rules. Tickets, ticket bleed for the side with fewer points, supplies income,
// win by tickets / holding all points / time limit (more tickets, equal = draw). Defaults come from CLW_WarModeConfig_Short.
class CLW_WarMode_ShortConflict : CLW_WarMode
{
	protected ref CLW_WarModeConfig_Short m_Config;
	protected float m_fElapsed;
	protected float m_fBleedAcc;
	protected float m_fIncomeAcc;
	protected float m_fHoldAll;
	protected string m_sHoldFaction;

	void CLW_WarMode_ShortConflict(CLW_WarModeConfig_Short config)
	{
		m_Config = config;
		array<string> sizes = {};
		if (config.m_aBattleSizes)
			sizes.Copy(config.m_aBattleSizes);
		if (sizes.IsEmpty())
		{
			sizes.Insert("small");
			sizes.Insert("medium");
			sizes.Insert("large");
		}

		m_aParams.Insert(CLW_ModeParam.CreateNumber("timeLimitMin", "#CLW-Param_timeLimitMin", "#CLW-Param_timeLimitMin_Desc", CLW_ModeParam.TYPE_INT, 5, 180, 5, config.m_iTimeLimitMin));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("startTickets", "#CLW-Param_startTickets", "#CLW-Param_startTickets_Desc", CLW_ModeParam.TYPE_INT, 100, 2000, 50, config.m_iStartTickets));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("startSupplies", "#CLW-Param_startSupplies", "#CLW-Param_startSupplies_Desc", CLW_ModeParam.TYPE_INT, 0, 2000, 50, config.m_iStartSupplies));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("incomeMultiplier", "#CLW-Param_incomeMultiplier", "#CLW-Param_incomeMultiplier_Desc", CLW_ModeParam.TYPE_FLOAT, 0.25, 3, 0.25, config.m_fIncomeMultiplier));
		m_aParams.Insert(CLW_ModeParam.CreateChoice("battleSize", "#CLW-Param_battleSize", "#CLW-Param_battleSize_Desc", sizes, config.m_sBattleSize));
		array<string> startModes = {"fixed", "random"};
		string startDefault = config.m_sStartPositions;
		if (startDefault.IsEmpty())
			startDefault = "fixed";
		m_aParams.Insert(CLW_ModeParam.CreateChoice("startPositions", "#CLW-Param_startPositions", "#CLW-Param_startPositions_Desc", startModes, startDefault));
		array<string> ownerModes = {"scenario", "none", "1", "2", "3"};
		m_aParams.Insert(CLW_ModeParam.CreateChoice("startObjectives", "#CLW-Param_startObjectives", "#CLW-Param_startObjectives_Desc", ownerModes, "scenario"));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("randomSeed", "#CLW-Param_randomSeed", "#CLW-Param_randomSeed_Desc", CLW_ModeParam.TYPE_INT, 0, 9999999, 1, config.m_iRandomSeed));
		m_aParams.Insert(CLW_ModeParam.CreateBool("ticketBleed", "#CLW-Param_ticketBleed", "#CLW-Param_ticketBleed_Desc", config.m_bTicketBleed));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("holdAllWinMin", "#CLW-Param_holdAllWinMin", "#CLW-Param_holdAllWinMin_Desc", CLW_ModeParam.TYPE_INT, 0, 15, 1, config.m_iHoldAllWinMin));
		// capability switches (CLW_Capabilities.Has): declared in GetCapabilities, "cap_<name>" turns one off in the setup menu
		// FOBs (M3 phase F, CLW_FobManager): the intent ESTABLISH_FOB drops out of the manifest when fobMax is 0
		array<string> fobBuildModes = {"instant", "timed"};
		string fobBuildDefault = config.m_sFobBuild;
		if (fobBuildDefault.IsEmpty())
			fobBuildDefault = "timed";
		m_aParams.Insert(CLW_ModeParam.CreateNumber("fobMax", "#CLW-Param_fobMax", "#CLW-Param_fobMax_Desc", CLW_ModeParam.TYPE_INT, 0, 4, 1, config.m_iFobMax));
		m_aParams.Insert(CLW_ModeParam.CreateChoice("fobBuild", "#CLW-Param_fobBuild", "#CLW-Param_fobBuild_Desc", fobBuildModes, fobBuildDefault));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("fobCost", "#CLW-Param_fobCost", "#CLW-Param_fobCost_Desc", CLW_ModeParam.TYPE_INT, 0, 1000, 25, config.m_iFobCost));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("fobBuildSec", "#CLW-Param_fobBuildSec", "#CLW-Param_fobBuildSec_Desc", CLW_ModeParam.TYPE_INT, 10, 600, 10, config.m_iFobBuildSec));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("fobEarliestMin", "#CLW-Param_fobEarliestMin", "#CLW-Param_fobEarliestMin_Desc", CLW_ModeParam.TYPE_INT, 0, 60, 1, 6));
		// spawn on assignment (owner playtest 2026-10-12): few idle AI at the bases, see CLW_BaseWatch
		m_aParams.Insert(CLW_ModeParam.CreateNumber("garrisonSquads", "#CLW-Param_garrisonSquads", "#CLW-Param_garrisonSquads_Desc", CLW_ModeParam.TYPE_INT, 0, 4, 1, 1));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("idleDespawnMin", "#CLW-Param_idleDespawnMin", "#CLW-Param_idleDespawnMin_Desc", CLW_ModeParam.TYPE_INT, 0, 60, 1, 6));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("convoyMax", "#CLW-Param_convoyMax", "#CLW-Param_convoyMax_Desc", CLW_ModeParam.TYPE_INT, 1, 4, 1, 2));
		// economy guard (M4 round 3 WP6): CLW_EconomyGuard.HqReserve() / ConvoyMinLoad() are the getters other code uses
		m_aParams.Insert(CLW_ModeParam.CreateNumber("hqReserve", "#CLW-Param_hqReserve", "#CLW-Param_hqReserve_Desc", CLW_ModeParam.TYPE_INT, 0, 1000, 25, 100));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("convoyMinLoad", "#CLW-Param_convoyMinLoad", "#CLW-Param_convoyMinLoad_Desc", CLW_ModeParam.TYPE_INT, 10, 400, 10, 60));
		m_aParams.Insert(CLW_ModeParam.CreateBool("cap_logistics", "#CLW-Param_cap_logistics", "#CLW-Param_cap_logistics_Desc", true));
		m_aParams.Insert(CLW_ModeParam.CreateBool("cap_kitCost", "#CLW-Param_cap_kitCost", "#CLW-Param_cap_kitCost_Desc", true));
		m_aParams.Insert(CLW_ModeParam.CreateBool("cap_serverKitCost", "#CLW-Param_cap_serverKitCost", "#CLW-Param_cap_serverKitCost_Desc", true));	// loadouts plan R3: spawning with a server loadout costs supplies
		// friendly units on the player map (CLW_FriendlyMapSettings): mapMe, mapSquadmates, mapSquads, mapVehicles
		CLW_FriendlyMapSettings.AddParams(m_aParams);
		// weather and time (CLW_Environment), commander traits per side (CLW_HqPersona)
		CLW_Environment.AddParams(m_aParams);
		CLW_HqPersona.AddParams(m_aParams);
	}

	override string GetId()
	{
		return "short";
	}

	override string GetName()
	{
		return "#CLW-Mode_Short_Name";
	}

	override string GetDescription()
	{
		return "#CLW-Mode_Short_Desc";
	}

	override void GetCapabilities(notnull array<string> caps)
	{
		caps.Insert("services");
		caps.Insert("fob");
		caps.Insert("logistics");
		caps.Insert("requests");
		caps.Insert("reports");
		caps.Insert("kitCost");
		caps.Insert("serverKitCost");
	}

	override float GetDistanceScale()
	{
		array<float> scales = m_Config.m_aDistanceScales;
		if (!scales || scales.IsEmpty())
			return 1;

		int index = Param("battleSize").GetInt();
		if (index < 0 || index >= scales.Count())
			index = scales.Count() - 1;
		return scales[index];
	}

	override int GetBattleSizeIndex()
	{
		return Param("battleSize").GetInt();
	}

	override string GetBattleSizeName()
	{
		return Param("battleSize").GetChoice();
	}

	override float GetMinHqDistanceM()
	{
		return m_Config.m_fMinHqDistanceM;
	}

	override float GetHqObjectiveClearM()
	{
		return m_Config.m_fHqObjectiveClearM;
	}

	override float GetObjectiveGapM()
	{
		return m_Config.m_fObjectiveGapM;
	}

	override void OnWarStart(CLW_WarState war)
	{
		ApplyBattleLayout();
		m_fElapsed = 0;
		m_fBleedAcc = 0;
		m_fIncomeAcc = 0;
		m_fHoldAll = 0;
		m_sHoldFaction = "";
		array<string> keys = {};
		war.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			war.SetTickets(key, Param("startTickets").GetInt());
			war.SetSupplies(key, Param("startSupplies").GetInt());
		}
	}

	override void OnTick(CLW_WarState war, float dt)
	{
		m_fElapsed += dt;
		CLW_EconomySetup economy = war.GetEconomy();

		if (Param("ticketBleed").GetBool())
		{
			m_fBleedAcc += dt;
			if (m_fBleedAcc >= economy.m_iBleedIntervalSec)
			{
				m_fBleedAcc = 0;
				Bleed(war, economy);
			}
		}

		m_fIncomeAcc += dt;
		if (m_fIncomeAcc >= economy.m_iIncomeIntervalSec)
		{
			m_fIncomeAcc = 0;
			array<string> keys = {};
			war.GetFactionKeys(keys);
			foreach (string key : keys)
			{
				war.AddSupplies(key, GetIncome(war, key));
			}
		}

		TrackHoldAll(war, dt);
	}

	override void OnUnitKilled(CLW_WarState war, string factionKey, bool isPlayer, bool isVehicle, bool isHeavyVehicle)
	{
		CLW_EconomySetup economy = war.GetEconomy();
		if (isVehicle)
		{
			if (isHeavyVehicle)
				war.LoseTickets(factionKey, economy.m_iCostVehicleHeavy, "heavy vehicle destroyed");
			else
				war.LoseTickets(factionKey, economy.m_iCostVehicleLight, "light vehicle destroyed");
		}
		else if (isPlayer)
			war.LoseTickets(factionKey, economy.m_iCostPlayerKilled, "player killed");
		else
			war.LoseTickets(factionKey, economy.m_iCostAiKilled, "soldier killed");
	}

	override bool CheckEnd(CLW_WarState war, out string winnerFaction, out EGameOverTypes reason)
	{
		array<string> keys = {};
		war.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			if (war.GetTickets(key) <= 0)
			{
				winnerFaction = war.GetEnemyKey(key);
				reason = EGameOverTypes.ENDREASON_SCORELIMIT;
				war.SetEndReasonText(CLW_Loc.W("#CLW-War_EndOutOfTickets", CLW_Loc.FactionName(key)));
				return true;
			}
		}

		int holdSec = Param("holdAllWinMin").GetInt() * 60;
		if (holdSec > 0 && !m_sHoldFaction.IsEmpty() && m_fHoldAll >= holdSec)
		{
			winnerFaction = m_sHoldFaction;
			reason = EGameOverTypes.ENDREASON_SCORELIMIT;
			war.SetEndReasonText(CLW_Loc.W("#CLW-War_EndHeldAll", CLW_Loc.FactionName(m_sHoldFaction), CLW_Loc.N(Math.Round(m_fHoldAll))));
			return true;
		}

		float limit = Param("timeLimitMin").GetInt() * 60;
		if (m_fElapsed >= limit && keys.Count() == 2)
		{
			int first = war.GetTickets(keys[0]);
			int second = war.GetTickets(keys[1]);
			if (first == second)
			{
				winnerFaction = "";
				reason = EGameOverTypes.ENDREASON_DRAW;
				war.SetEndReasonText(CLW_Loc.W("#CLW-War_EndTimeEqual"));
			}
			else
			{
				if (first > second)
					winnerFaction = keys[0];
				else
					winnerFaction = keys[1];
				reason = EGameOverTypes.ENDREASON_TIMELIMIT;
				war.SetEndReasonText(CLW_Loc.W("#CLW-War_EndTimeMore"));
			}
			return true;
		}

		return false;
	}

	override int GetIncome(CLW_WarState war, string factionKey)
	{
		CLW_EconomySetup economy = war.GetEconomy();
		float raw = economy.m_iIncomeBase + economy.m_iIncomePerValue * war.GetHeldValue(factionKey);
		return Math.Round(raw * Param("incomeMultiplier").m_fValue * IncomeScale());
	}

	override int GetAICap(string factionKey)
	{
		return CapFor(m_Config.m_aAiCaps);
	}

	override int GetActiveAgentCap(string factionKey)
	{
		return CapFor(m_Config.m_aActiveAgentCaps);
	}

	override int GetVehicleCap(string factionKey)
	{
		return CapFor(m_Config.m_aVehicleCaps);
	}

	override float GetTimeLeftSec()
	{
		return Math.Max(0, Param("timeLimitMin").GetInt() * 60 - m_fElapsed);
	}

	// The side holding fewer point value loses bleedPerValue * difference.
	protected void Bleed(CLW_WarState war, CLW_EconomySetup economy)
	{
		array<string> keys = {};
		war.GetFactionKeys(keys);
		if (keys.Count() != 2)
			return;

		int valueFirst = war.GetHeldValue(keys[0]);
		int valueSecond = war.GetHeldValue(keys[1]);
		if (valueFirst == valueSecond)
			return;

		string loser = keys[0];
		int diff = valueSecond - valueFirst;
		if (valueFirst > valueSecond)
		{
			loser = keys[1];
			diff = valueFirst - valueSecond;
		}

		war.LoseTickets(loser, economy.m_iBleedPerValue * diff, string.Format("bleed (points %1:%2 vs %3:%4)", keys[0], valueFirst, keys[1], valueSecond));
	}

	protected void TrackHoldAll(CLW_WarState war, float dt)
	{
		string holder = "";
		array<string> keys = {};
		war.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			if (war.HoldsAllPoints(key))
				holder = key;
		}

		if (holder != m_sHoldFaction)
		{
			if (!holder.IsEmpty() && Param("holdAllWinMin").GetInt() > 0)
				CLW_Debug.Announce(CLW_Loc.W("#CLW-War_HoldAll", CLW_Loc.FactionName(holder), CLW_Loc.N(Param("holdAllWinMin").GetInt())));
			else if (!m_sHoldFaction.IsEmpty())
				Print("CLW: war hold-all by " + m_sHoldFaction + " broken", LogLevel.NORMAL);
			m_sHoldFaction = holder;
			m_fHoldAll = 0;
		}

		if (!holder.IsEmpty())
			m_fHoldAll += dt;
	}

	protected int CapFor(array<int> caps)
	{
		if (!caps || caps.IsEmpty())
			return 0;

		int index = Param("battleSize").GetInt();
		if (index < 0 || index >= caps.Count())
			index = caps.Count() - 1;
		return caps[index];
	}

	// Income scale of the chosen battle size (1 when the scenario gives none).
	protected float IncomeScale()
	{
		array<float> scales = m_Config.m_aIncomeScales;
		if (!scales || scales.IsEmpty())
			return 1;

		int index = Param("battleSize").GetInt();
		if (index < 0 || index >= scales.Count())
			index = scales.Count() - 1;
		return scales[index];
	}
}

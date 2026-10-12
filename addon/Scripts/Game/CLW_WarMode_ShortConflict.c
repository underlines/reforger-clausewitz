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

		m_aParams.Insert(CLW_ModeParam.CreateNumber("timeLimitMin", "Time limit (min)", "The war ends after this time; more tickets wins.", CLW_ModeParam.TYPE_INT, 15, 180, 5, config.m_iTimeLimitMin));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("startTickets", "Start tickets", "War will of each side; at 0 the side loses.", CLW_ModeParam.TYPE_INT, 100, 2000, 50, config.m_iStartTickets));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("startSupplies", "Start supplies", "Purchasing power of each side at the start.", CLW_ModeParam.TYPE_INT, 0, 2000, 50, config.m_iStartSupplies));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("incomeMultiplier", "Income multiplier", "Scales the supplies income.", CLW_ModeParam.TYPE_FLOAT, 0.25, 3, 0.25, config.m_fIncomeMultiplier));
		m_aParams.Insert(CLW_ModeParam.CreateChoice("battleSize", "Battle size", "Caps for live AI and vehicles per side; bigger battles also get more income.", sizes, config.m_sBattleSize));
		m_aParams.Insert(CLW_ModeParam.CreateBool("ticketBleed", "Ticket bleed", "The side holding fewer points loses tickets over time.", config.m_bTicketBleed));
		m_aParams.Insert(CLW_ModeParam.CreateNumber("holdAllWinMin", "Hold-all win (min)", "Holding every point this long wins. 0 = off.", CLW_ModeParam.TYPE_INT, 0, 15, 1, config.m_iHoldAllWinMin));
	}

	override string GetId()
	{
		return "short";
	}

	override string GetName()
	{
		return "Short conflict";
	}

	override string GetDescription()
	{
		return "Tickets drain with losses and from holding fewer points; win by tickets, by holding all points, or by tickets at the time limit.";
	}

	override void OnWarStart(CLW_WarState war)
	{
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
				war.SetEndReasonText(key + " out of tickets");
				return true;
			}
		}

		int holdSec = Param("holdAllWinMin").GetInt() * 60;
		if (holdSec > 0 && !m_sHoldFaction.IsEmpty() && m_fHoldAll >= holdSec)
		{
			winnerFaction = m_sHoldFaction;
			reason = EGameOverTypes.ENDREASON_SCORELIMIT;
			war.SetEndReasonText(m_sHoldFaction + " held all points for " + Math.Round(m_fHoldAll) + " s");
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
				war.SetEndReasonText("time limit, equal tickets");
			}
			else
			{
				if (first > second)
					winnerFaction = keys[0];
				else
					winnerFaction = keys[1];
				reason = EGameOverTypes.ENDREASON_TIMELIMIT;
				war.SetEndReasonText("time limit, more tickets");
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
				CLW_Debug.Say(holder + " holds every point: wins in " + Param("holdAllWinMin").GetInt() + " min if kept");
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

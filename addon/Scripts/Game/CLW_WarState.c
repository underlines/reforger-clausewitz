enum CLW_EWarPhase
{
	SETUP,		// bases / points exist, nothing runs: waiting for StartWar
	RUNNING,
	ENDED
}

// War state (spec sections 2 and 2a): the MECHANISMS of the war. Ticket and supply ledgers, kill accounting, event queue,
// EndGameMode, respawn gate, lifecycle (SETUP -> RUNNING -> ENDED). Every RULE decision is delegated to the active
// CLW_WarMode (income, bleed, ticket costs, end conditions, caps, time limit). Server only.
class CLW_WarState
{
	protected ref map<string, int> m_mTickets = new map<string, int>();
	protected ref map<string, int> m_mSupplies = new map<string, int>();
	protected ref map<string, ref array<string>> m_mEvents = new map<string, ref array<string>>();
	protected ref array<string> m_aKeys = {};
	protected ref array<IEntity> m_aCounted = {};			// victims already charged (OnControllableDestroyed and OnPlayerKilled both fire for players)
	protected ref array<IEntity> m_aVehicleOwners = {};		// vehicle faction bookkeeping (RegisterVehicle), parallel arrays
	protected ref array<string> m_aVehicleFactions = {};
	protected ref array<ref CLW_WarMode> m_aModes = {};
	protected CLW_WarMode m_Mode;							// active mode (null in SETUP)
	protected CLW_ScenarioConfig m_Scenario;
	protected CLW_EconomySetup m_Economy;
	protected CLW_ControlPoints m_Points;
	protected CLW_EWarPhase m_ePhase = CLW_EWarPhase.SETUP;
	protected int m_iSetupOwner = -1;
	protected int m_iElapsedSec;
	protected int m_iSummaryAcc;
	protected string m_sEndReason;
	protected string m_sWinner;
	protected string m_sStartedMode;
	protected string m_sStartedParams;
	protected ref ScriptInvoker m_OnWarStarted;
	protected ref CLW_WarStatus m_Status;		// HUD broadcast
	protected ref CLW_SetupPreset m_Preset;		// overrides.json "setup" (null = none)

	void Init(notnull CLW_ScenarioConfig scenario, CLW_ControlPoints points)
	{
		if (!Replication.IsServer())
			return;

		m_Scenario = scenario;
		m_Economy = scenario.m_Economy;
		m_Points = points;
		scenario.GetFactionKeys(m_aKeys);
		CLW_Sides.PrepareSetup(scenario);		// default sides playable + hostile during SETUP
		foreach (string key : m_aKeys)
		{
			m_mTickets.Set(key, 0);
			m_mSupplies.Set(key, 0);
			m_mEvents.Set(key, new array<string>());
		}

		CLW_WarModeRegistry.CreateAll(scenario, m_aModes);
		foreach (CLW_WarMode mode : m_aModes)
		{
			Print(string.Format("CLW: war mode available id=%1 name='%2' defaults=%3", mode.GetId(), mode.GetName(), mode.ValuesText()), LogLevel.NORMAL);
		}

		if (m_Points)
			m_Points.GetOnOwnerChanged().Insert(OnPointOwnerChanged);

		// Server file preset (CLW_SetupPreset.c): pre-fills the menu and replaces the scenario's auto start delay.
		int autoStartSec = scenario.m_iAutoStartSec;
		m_Preset = CLW_SetupPreset.Load();
		if (m_Preset)
		{
			PrefillFromPreset();
			autoStartSec = m_Preset.m_iAutoStartSec;
		}

		Print(string.Format("CLW: war SETUP phase: %1 modes, default '%2', autoStartSec=%3", m_aModes.Count(), GetDefaultModeId(), autoStartSec), LogLevel.NORMAL);
		if (autoStartSec > 0)
			GetGame().GetCallqueue().CallLater(AutoStart, autoStartSec * 1000);

		m_Status = new CLW_WarStatus();
		m_Status.Init(this);
		CLW_SetupServer.Start(this, scenario);		// setup menu driver (CLW_SetupServer.c)
	}

	// ---- lifecycle ----

	CLW_EWarPhase GetPhase()
	{
		return m_ePhase;
	}

	bool IsRunning()
	{
		return m_ePhase == CLW_EWarPhase.RUNNING;
	}

	bool IsEnded()
	{
		return m_ePhase == CLW_EWarPhase.ENDED;
	}

	string GetPhaseName()
	{
		if (m_ePhase == CLW_EWarPhase.RUNNING)
			return "RUNNING";

		if (m_ePhase == CLW_EWarPhase.ENDED)
			return "ENDED";

		return "SETUP";
	}

	// Validates and switches SETUP -> RUNNING. paramsJson: flat object of parameter values ("" = defaults).
	// Returns false (and logs the reason) when not in SETUP, unknown mode or invalid values. byPlayerId -1 = system/auto.
	// factionA / factionB: the factions fighting on side A / B ("" = keep the scenario's current ones). They are validated
	// before anything changes, then CLW_Sides.Apply re-keys everything BEFORE the mode starts and before GetOnWarStarted fires.
	bool StartWar(string modeId, string paramsJson, int byPlayerId, string factionA = "", string factionB = "")
	{
		if (!Replication.IsServer())
			return false;

		if (m_ePhase != CLW_EWarPhase.SETUP)
		{
			Print("CLW: war StartWar refused: phase is " + GetPhaseName(), LogLevel.WARNING);
			return false;
		}

		if (modeId.IsEmpty() && m_Scenario)
			modeId = m_Scenario.m_sDefaultMode;

		CLW_WarMode mode = FindMode(modeId);
		if (!mode)
		{
			Print("CLW: war StartWar refused: unknown mode '" + modeId + "'", LogLevel.WARNING);
			return false;
		}

		string error;
		if (!mode.ApplyParamsJson(paramsJson, error))
		{
			Print("CLW: war StartWar refused: " + error, LogLevel.WARNING);
			return false;
		}

		if (m_Scenario)
		{
			if (factionA.IsEmpty())
				factionA = m_Scenario.GetFactionKey(0);
			if (factionB.IsEmpty())
				factionB = m_Scenario.GetFactionKey(1);

			string sidesError = m_Scenario.CheckSides(factionA, factionB);
			if (!sidesError.IsEmpty())
			{
				Print("CLW: war StartWar refused: " + sidesError, LogLevel.WARNING);
				return false;
			}

			// Sides become dynamic here: scenario keys, bases, points, ledgers, registry, playable/hostile, scene groups.
			if (!CLW_Sides.Apply(this, m_Scenario, factionA, factionB))
			{
				Print("CLW: war StartWar refused: could not apply the sides", LogLevel.ERROR);
				return false;
			}
		}

		m_Mode = mode;
		m_ePhase = CLW_EWarPhase.RUNNING;
		m_iElapsedSec = 0;
		m_iSummaryAcc = 0;
		m_sStartedMode = mode.GetId();
		m_sStartedParams = mode.ValuesText();
		mode.OnWarStart(this);
		LogEngineLimitHint();
		GetGame().GetCallqueue().CallLater(Tick, CLW_WarConfig.WAR_TICK_MS, true);
		CLW_Debug.Say(string.Format("WAR STARTED mode=%1 (%2) by player %3 values=%4", mode.GetId(), mode.GetName(), byPlayerId, m_sStartedParams));
		LogSummary();
		if (m_OnWarStarted)
			m_OnWarStarted.Invoke();
		NotifyStatus();
		return true;
	}

	// Hint only (caps are never changed silently): both AI caps together should fit under the engine's active-AI limit.
	protected void LogEngineLimitHint()
	{
		int active;
		int limit;
		if (!CLW_ForceSpawner.GetEngineAI(active, limit))
			return;

		int capSum;
		foreach (string key : m_aKeys)
		{
			capSum += GetAiCap(key);
		}

		int usable = limit - CLW_WarConfig.ENGINE_AI_MARGIN;
		if (capSum > usable)
			Print(string.Format("CLW: WARNING war aiCap sum %1 exceeds the engine active-AI limit %2 minus margin %3: purchases will be refused near the limit. Suggest lowering the battle size / aiCap, or raising the server -activeAILimit", capSum, limit, CLW_WarConfig.ENGINE_AI_MARGIN), LogLevel.WARNING);
		else
			Print(string.Format("CLW: war aiCap sum %1 fits the engine active-AI limit %2 (margin %3)", capSum, limit, CLW_WarConfig.ENGINE_AI_MARGIN), LogLevel.NORMAL);
	}

	// The scenario sides were assigned other factions (CLW_Sides.Apply, SETUP only): rebuild the keyed ledgers.
	// Tickets / supplies / events of the previous keys are dropped (nothing is running yet).
	void OnSidesChanged()
	{
		if (!Replication.IsServer() || !m_Scenario)
			return;

		m_aKeys.Clear();
		m_mTickets.Clear();
		m_mSupplies.Clear();
		m_mEvents.Clear();
		m_Scenario.GetFactionKeys(m_aKeys);
		foreach (string key : m_aKeys)
		{
			m_mTickets.Set(key, 0);
			m_mSupplies.Set(key, 0);
			m_mEvents.Set(key, new array<string>());
		}

		m_aVehicleOwners.Clear();
		m_aVehicleFactions.Clear();
		Print("CLW: war ledgers re-keyed: " + m_aKeys[0] + " vs " + m_aKeys[1], LogLevel.NORMAL);
	}

	// HUD: something visible changed, broadcast the status soon.
	void NotifyStatus()
	{
		if (m_Status)
			m_Status.Request();
	}

	// Fired once when the war switches SETUP -> RUNNING (ledgers are initialised). No arguments.
	ScriptInvoker GetOnWarStarted()
	{
		if (!m_OnWarStarted)
			m_OnWarStarted = new ScriptInvoker();

		return m_OnWarStarted;
	}

	protected void AutoStart()
	{
		if (m_ePhase != CLW_EWarPhase.SETUP)
			return;

		if (!m_Preset)
		{
			Print("CLW: war auto start (no setup happened)", LogLevel.NORMAL);
			StartWar("", "", -1);
			return;
		}

		Print("CLW: war auto start from the setup preset: " + m_Preset.m_sText, LogLevel.NORMAL);
		if (StartWar(GetDefaultModeId(), m_Preset.m_sValuesJson, -1, m_Preset.m_sFactionA, m_Preset.m_sFactionB))
			return;

		// never fall back to defaults silently: the owner asked for these settings; the setup menu stays open
		Print("CLW: ERROR setup preset REFUSED (see the line above), waiting for the setup menu", LogLevel.ERROR);
		CLW_Debug.Announce("server setup preset was refused, use the setup menu (/war setup)");
	}

	// The preset's values become the menu defaults (mode params) so a human sees and can adjust them before the auto start.
	protected void PrefillFromPreset()
	{
		CLW_WarMode mode = FindMode(GetDefaultModeId());
		if (!mode)
		{
			Print("CLW: ERROR setup preset: unknown mode '" + m_Preset.m_sMode + "'", LogLevel.ERROR);
			return;
		}

		string error;
		if (!mode.ApplyParamsJson(m_Preset.m_sValuesJson, error))
			Print("CLW: ERROR setup preset: " + error, LogLevel.ERROR);
	}

	// Mode the menu pre-selects and the auto start uses: preset mode, else the scenario default.
	string GetDefaultModeId()
	{
		if (m_Preset && !m_Preset.m_sMode.IsEmpty())
			return m_Preset.m_sMode;

		if (m_Scenario)
			return m_Scenario.m_sDefaultMode;

		return "";
	}

	// Server file preset (null = none).
	CLW_SetupPreset GetPreset()
	{
		return m_Preset;
	}

	// Called by the game mode for every spawned player: the first human during SETUP becomes the setup owner.
	void OnHumanPlayerSpawned(int playerId)
	{
		if (m_ePhase != CLW_EWarPhase.SETUP || m_iSetupOwner >= 0 || playerId <= 0)
			return;

		m_iSetupOwner = playerId;
		CLW_Debug.Say("player " + playerId + " is the war setup owner");
	}

	// Player allowed to configure the war (-1 = nobody yet / setup over).
	int GetSetupOwner()
	{
		return m_iSetupOwner;
	}

	// Setup menu driver: hand the owner role to another player (owner left).
	void SetSetupOwner(int playerId)
	{
		if (m_ePhase != CLW_EWarPhase.SETUP)
			return;

		m_iSetupOwner = playerId;
	}

	// All modes with their parameters (definitions + current values) as JSON, for the setup menu / dashboard.
	string GetModesJson()
	{
		string json = "{\"phase\":\"" + GetPhaseName() + "\",\"default\":\"";
		if (m_Scenario)
			json += CLW_ModeParam.Escape(m_Scenario.m_sDefaultMode);
		json += "\",\"setupOwner\":" + m_iSetupOwner + ",\"modes\":[";
		foreach (int i, CLW_WarMode mode : m_aModes)
		{
			if (i > 0)
				json += ",";
			json += mode.ToJson();
		}
		return json + "]}";
	}

	// Mode list access for the setup menu (CLW_SetupServer).
	int GetModeCount()
	{
		return m_aModes.Count();
	}

	CLW_WarMode GetModeAt(int index)
	{
		if (index < 0 || index >= m_aModes.Count())
			return null;

		return m_aModes[index];
	}

	CLW_WarMode GetMode()
	{
		return m_Mode;
	}

	protected CLW_WarMode FindMode(string id)
	{
		foreach (CLW_WarMode mode : m_aModes)
		{
			if (mode.GetId() == id)
				return mode;
		}

		return null;
	}

	// ---- scenario / points access for modes ----

	CLW_EconomySetup GetEconomy()
	{
		return m_Economy;
	}

	void GetFactionKeys(notnull array<string> keys)
	{
		keys.InsertAll(m_aKeys);
	}

	string GetEnemyKey(string factionKey)
	{
		if (!m_Scenario)
			return "";

		return m_Scenario.GetEnemyKey(factionKey);
	}

	int GetHeldValue(string factionKey)
	{
		if (!m_Points)
			return 0;

		return m_Points.HeldValue(factionKey);
	}

	int GetHeldCount(string factionKey)
	{
		if (!m_Points)
			return 0;

		return m_Points.HeldCount(factionKey);
	}

	bool HoldsAllPoints(string factionKey)
	{
		return m_Points && m_Points.HoldsAll(factionKey);
	}

	// Modes call this from CheckEnd to explain the result in the log.
	void SetEndReasonText(string text)
	{
		m_sEndReason = text;
	}

	// ---- getters (situation, dashboard) ----

	int GetTickets(string factionKey)
	{
		int v = 0;
		m_mTickets.Find(factionKey, v);
		return v;
	}

	int GetSupplies(string factionKey)
	{
		int v = 0;
		m_mSupplies.Find(factionKey, v);
		return v;
	}

	// Supplies per income interval at the current holdings (mode rule).
	int GetIncome(string factionKey)
	{
		if (!m_Mode)
			return 0;

		return m_Mode.GetIncome(this, factionKey);
	}

	int GetIncomeIntervalSec()
	{
		if (!m_Economy)
			return 60;

		return m_Economy.m_iIncomeIntervalSec;
	}

	int GetAiCap(string factionKey)
	{
		if (!m_Mode)
			return 0;

		return m_Mode.GetAICap(factionKey);
	}

	int GetActiveAgentCap(string factionKey)
	{
		if (m_Mode && m_Mode.GetActiveAgentCap(factionKey) > 0)
			return m_Mode.GetActiveAgentCap(factionKey);

		return CLW_WarConfig.ACTIVITY_DEFAULT_CAP;
	}

	int GetVehicleCap(string factionKey)
	{
		if (!m_Mode)
			return 0;

		return m_Mode.GetVehicleCap(factionKey);
	}

	// Seconds left on the time limit (-1 = no limit / not running).
	int GetTimeLeftSec()
	{
		if (!m_Mode || !IsRunning())
			return -1;

		float left = m_Mode.GetTimeLeftSec();
		if (left < 0)
			return -1;

		return Math.Round(left);
	}

	int GetElapsedSec()
	{
		return m_iElapsedSec;
	}

	string GetWinner()
	{
		return m_sWinner;
	}

	// Respawn gate (CanPlayerSpawn_S): players may always spawn in SETUP (they configure the war), never after the end,
	// while RUNNING the mode decides (default: tickets > 0).
	bool CanFactionSpawn(string factionKey)
	{
		if (m_ePhase == CLW_EWarPhase.ENDED)
			return false;

		if (m_ePhase == CLW_EWarPhase.SETUP || !m_Mode)
			return true;

		return m_Mode.AllowsPlayerSpawn(factionKey) && GetTickets(factionKey) > 0;
	}

	// ---- ledgers (server). Mutations only while RUNNING, except the mode's own OnWarStart setup. ----

	void SetTickets(string factionKey, int value)
	{
		if (!m_mTickets.Contains(factionKey))
			return;

		m_mTickets.Set(factionKey, Math.Max(0, value));
		NotifyStatus();
		Print(string.Format("CLW: war %1 tickets set %2", factionKey, value), LogLevel.NORMAL);
	}

	void SetSupplies(string factionKey, int value)
	{
		if (!m_mSupplies.Contains(factionKey))
			return;

		m_mSupplies.Set(factionKey, Math.Max(0, value));
		Print(string.Format("CLW: war %1 supplies set %2", factionKey, value), LogLevel.NORMAL);
	}

	// Ticket refill (endless modes) or reinforcement.
	void AddTickets(string factionKey, int amount, string reason)
	{
		if (!m_mTickets.Contains(factionKey) || amount == 0)
			return;

		int now = Math.Max(0, GetTickets(factionKey) + amount);
		m_mTickets.Set(factionKey, now);
		NotifyStatus();
		Print(string.Format("CLW: war %1 tickets %2 (%3) -> %4", factionKey, amount, reason, now), LogLevel.NORMAL);
	}

	void LoseTickets(string factionKey, int amount, string reason)
	{
		if (!IsRunning() || amount <= 0)
			return;

		AddTickets(factionKey, -amount, reason);
		CheckEndNow();
	}

	// Charge supplies. False (and nothing charged) when the war is not running or the faction cannot pay.
	bool TrySpend(string factionKey, int amount)
	{
		if (!IsRunning() || amount < 0 || !m_mSupplies.Contains(factionKey))
			return false;

		int have = GetSupplies(factionKey);
		if (have < amount)
		{
			Print(string.Format("CLW: war %1 spend %2 refused (supplies %3)", factionKey, amount, have), LogLevel.NORMAL);
			return false;
		}

		m_mSupplies.Set(factionKey, have - amount);
		Print(string.Format("CLW: war %1 spent %2 supplies -> %3", factionKey, amount, have - amount), LogLevel.NORMAL);
		return true;
	}

	void Refund(string factionKey, int amount)
	{
		AddSupplies(factionKey, amount);
	}

	void AddSupplies(string factionKey, int amount)
	{
		if (amount == 0 || !m_mSupplies.Contains(factionKey))
			return;

		int now = Math.Max(0, GetSupplies(factionKey) + amount);
		m_mSupplies.Set(factionKey, now);
		Print(string.Format("CLW: war %1 supplies %2 -> %3", factionKey, amount, now), LogLevel.NORMAL);
	}

	// ---- events for the situation builder ----

	void PushEvent(string factionKey, string text)
	{
		array<string> queue;
		if (!m_mEvents.Find(factionKey, queue) || !queue)
			return;

		queue.Insert(text);
		while (queue.Count() > CLW_WarConfig.EVENT_QUEUE_MAX)
		{
			queue.RemoveOrdered(0);
		}
	}

	// Appends and clears the faction's queued events (oldest first).
	void DrainEvents(string factionKey, notnull array<string> outEvents)
	{
		array<string> queue;
		if (!m_mEvents.Find(factionKey, queue) || !queue)
			return;

		outEvents.InsertAll(queue);
		queue.Clear();
	}

	// ---- kill accounting (called by CLW_GameMode) ----

	// Optional bookkeeping for vehicles whose affiliation is empty when destroyed (force spawner registers spawned vehicles).
	void RegisterVehicle(IEntity vehicle, string factionKey)
	{
		if (!vehicle)
			return;

		int i = m_aVehicleOwners.Find(vehicle);
		if (i >= 0)
		{
			m_aVehicleFactions[i] = factionKey;
			return;
		}

		m_aVehicleOwners.Insert(vehicle);
		m_aVehicleFactions.Insert(factionKey);
	}

	// A controllable (character or vehicle) was destroyed. playerId > 0 when a player controlled it.
	void OnEntityDestroyed(IEntity victim, int victimPlayerId)
	{
		if (!IsRunning() || !victim || !m_Mode)
			return;

		if (m_aCounted.Contains(victim))
			return;

		m_aCounted.Insert(victim);
		if (m_aCounted.Count() > 128)
			m_aCounted.RemoveOrdered(0);

		bool isPlayer = victimPlayerId > 0;
		if (!isPlayer)
			isPlayer = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(victim) > 0;

		string factionKey = GetEntityFactionKey(victim);
		if (!m_mTickets.Contains(factionKey))
		{
			Print(string.Format("CLW: war kill ignored (faction '%1' not in scenario) victim=%2", factionKey, victim), LogLevel.NORMAL);
			return;
		}

		bool isVehicle = Vehicle.Cast(victim) != null;
		bool isCharacter = ChimeraCharacter.Cast(victim) != null;
		if (!isVehicle && !isCharacter)
			return;

		bool heavy = isVehicle && IsHeavyVehicle(victim);
		if (isVehicle)
		{
			string kind = "light vehicle destroyed";
			if (heavy)
				kind = "heavy vehicle destroyed";
			PushEvent(factionKey, kind);
		}

		m_Mode.OnUnitKilled(this, factionKey, isPlayer, isVehicle, heavy);
	}

	// Faction of a character or vehicle: affiliation component, then own bookkeeping, then the prefab default.
	string GetEntityFactionKey(IEntity entity)
	{
		FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(entity.FindComponent(FactionAffiliationComponent));
		if (affiliation)
		{
			Faction faction = affiliation.GetAffiliatedFaction();
			if (faction)
				return faction.GetFactionKey();
		}

		int i = m_aVehicleOwners.Find(entity);
		if (i >= 0)
			return m_aVehicleFactions[i];

		if (affiliation)
			return affiliation.GetDefaultFactionKey();

		return "";
	}

	// Generic classification: armed (has a TURRET compartment) with enough compartments counts as heavy; no prefab names involved.
	protected bool IsHeavyVehicle(IEntity vehicle)
	{
		BaseCompartmentManagerComponent compartments = BaseCompartmentManagerComponent.Cast(vehicle.FindComponent(BaseCompartmentManagerComponent));
		if (!compartments)
			return false;

		array<BaseCompartmentSlot> slots = {};
		compartments.GetCompartments(slots);
		bool hasTurret;
		foreach (BaseCompartmentSlot slot : slots)
		{
			if (slot && slot.GetType() == ECompartmentType.TURRET)
				hasTurret = true;
		}

		return hasTurret && slots.Count() >= CLW_WarConfig.HEAVY_VEHICLE_MIN_COMPARTMENTS;
	}

	// ---- tick ----

	protected void OnPointOwnerChanged(string id, string oldOwner, string newOwner)
	{
		CLW_ControlPoint point;
		if (m_Points)
			point = m_Points.Find(id);

		string name = id;
		if (point)
			name = id + " " + point.m_sName;

		string text = name + " captured by " + newOwner;
		if (newOwner.IsEmpty())
			text = name + " neutralised";

		foreach (string key : m_aKeys)
		{
			PushEvent(key, text);
		}

		NotifyStatus();
		if (IsRunning() && m_Mode)
			m_Mode.OnObjectiveChanged(this, id, oldOwner, newOwner);
	}

	protected void Tick()
	{
		if (!IsRunning() || !m_Mode)
			return;

		int stepSec = CLW_WarConfig.WAR_TICK_MS / 1000;
		m_iElapsedSec += stepSec;
		m_iSummaryAcc += stepSec;
		m_Mode.OnTick(this, stepSec);
		if (CheckEndNow())
			return;

		if (m_iSummaryAcc >= CLW_WarConfig.SUMMARY_INTERVAL_SEC)
		{
			m_iSummaryAcc = 0;
			LogSummary();
		}
	}

	// Asks the mode whether the war is over; ends it if so. Returns true when the war ended.
	protected bool CheckEndNow()
	{
		if (!IsRunning() || !m_Mode)
			return false;

		string winner;
		EGameOverTypes reason;
		if (!m_Mode.CheckEnd(this, winner, reason))
			return false;

		Finish(winner, reason, m_sEndReason);
		return true;
	}

	// Ends the game once. winnerKey "" = draw. Public so modes/other systems can force an end.
	void Finish(string winnerKey, EGameOverTypes reason, string why)
	{
		if (m_ePhase == CLW_EWarPhase.ENDED)
			return;

		m_ePhase = CLW_EWarPhase.ENDED;
		m_sWinner = winnerKey;
		NotifyStatus();
		GetGame().GetCallqueue().Remove(Tick);
		CLW_Services.OnWarEnded();
		CLW_Debug.Announce(string.Format("WAR OVER: winner=%1 (%2)", winnerKey, why));
		LogSummary();

		int winnerIndex = -1;
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!winnerKey.IsEmpty() && factionManager)
		{
			Faction winner = factionManager.GetFactionByKey(winnerKey);
			if (winner)
				winnerIndex = factionManager.GetFactionIndex(winner);
		}

		SCR_BaseGameMode gameMode = SCR_BaseGameMode.Cast(GetGame().GetGameMode());
		if (!gameMode)
		{
			Print("CLW: war cannot end the game: no SCR_BaseGameMode", LogLevel.ERROR);
			return;
		}

		gameMode.EndGameMode(SCR_GameModeEndData.CreateSimple(reason, -1, winnerIndex));
		Print(string.Format("CLW: war EndGameMode called reason=%1 winnerFactionIndex=%2", reason, winnerIndex), LogLevel.NORMAL);
	}

	// "CLW: war [phase] US tickets=.. supplies=.. points=.. | USSR ..."
	protected void LogSummary()
	{
		string line = "CLW: war " + GetPhaseName();
		if (!m_sStartedMode.IsEmpty())
			line += " mode=" + m_sStartedMode;

		foreach (int i, string key : m_aKeys)
		{
			if (i > 0)
				line += " |";

			line += " " + key + " tickets=" + GetTickets(key) + " supplies=" + GetSupplies(key) + " points=" + GetHeldCount(key) + " (value " + GetHeldValue(key) + ") income=" + GetIncome(key);
		}

		line += " | t=" + m_iElapsedSec + "s left=" + GetTimeLeftSec();
		Print("" + line, LogLevel.NORMAL);	// Print(variable) would log "string line = ..."
	}
}

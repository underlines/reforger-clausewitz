// Scenario data (spec 0a): everything content-specific about one battlefield. Loaded from a .conf resource
// (Configs/CLW/Scenarios/<World>.conf) chosen by the CLW_GameMode prefab. Code never hard-wires factions or coordinates.

[BaseContainerProps()]
class CLW_FactionSetup
{
	[Attribute("", UIWidgets.EditBox, "Faction key (must exist in the FactionManager), e.g. US")]
	string m_sKey;

	[Attribute("", UIWidgets.EditBox, "Map label of the main base, e.g. 'US HQ'")]
	string m_sBaseLabel;

	[Attribute("BLUFOR", UIWidgets.EditBox, "Map symbol identity: BLUFOR, OPFOR or INDFOR")]
	string m_sIdentity;

	[Attribute("0 0 0", UIWidgets.EditBox, "Main base position (x y z, y is replaced by the surface height)")]
	vector m_vBasePos;

	[Attribute("0 0 0", UIWidgets.EditBox, "Fallback base position when the main one is water (0 0 0 = none)")]
	vector m_vBaseFallbackPos;

	[Attribute("", UIWidgets.ResourceNamePicker, "Spawn point prefab spawned at the base for this faction", "et")]
	ResourceName m_SpawnPointPrefab;

	[Attribute("30", UIWidgets.EditBox, "Spawn radius of the base spawn point (m)")]
	float m_fSpawnRadius;

	[Attribute("", UIWidgets.ResourceNamePicker, "Faction profile (force catalog), used by the force phase", "conf")]
	ResourceName m_ProfileConfig;

	[Attribute("", UIWidgets.EditBox, "Entity name of the scene-placed squad a joining player should lead (empty = first free squad)")]
	string m_sPreferredSquad;

	[Attribute()]
	ref array<string> m_aStartForces;	// force type ids (from the profile) spawned free at the base when the war starts

	EMilitarySymbolIdentity GetIdentity()
	{
		if (m_sIdentity == "BLUFOR")
			return EMilitarySymbolIdentity.BLUFOR;

		if (m_sIdentity == "OPFOR")
			return EMilitarySymbolIdentity.OPFOR;

		if (m_sIdentity == "INDFOR")
			return EMilitarySymbolIdentity.INDFOR;

		return EMilitarySymbolIdentity.UNKNOWN;
	}
}

[BaseContainerProps()]
class CLW_ObjectiveSetup
{
	[Attribute("", UIWidgets.EditBox, "Short id, e.g. C")]
	string m_sId;

	[Attribute("", UIWidgets.EditBox, "Display name")]
	string m_sName;

	[Attribute("0 0 0", UIWidgets.EditBox, "Position (x y z, y is replaced by the surface height)")]
	vector m_vPos;

	[Attribute("50", UIWidgets.EditBox, "Capture radius (m)")]
	float m_fRadius;

	[Attribute("1", UIWidgets.EditBox, "Point value (income and ticket bleed weight)")]
	int m_iValue;

	[Attribute("", UIWidgets.EditBox, "Faction key owning the point at start (empty = neutral)")]
	string m_sStartOwner;
}

[BaseContainerProps()]
class CLW_EconomySetup
{
	// Mechanism numbers shared by all war modes. Mode-specific values (tickets, time limit, caps...) are in the war mode configs.
	[Attribute("60", UIWidgets.EditBox, "Income interval (s)")]
	int m_iIncomeIntervalSec;

	[Attribute("20", UIWidgets.EditBox, "Income per interval, flat")]
	int m_iIncomeBase;

	[Attribute("10", UIWidgets.EditBox, "Income per interval per point value held")]
	int m_iIncomePerValue;

	[Attribute("30", UIWidgets.EditBox, "Ticket bleed interval (s)")]
	int m_iBleedIntervalSec;

	[Attribute("2", UIWidgets.EditBox, "Bleed: tickets lost per interval per point value of difference")]
	int m_iBleedPerValue;

	[Attribute("1", UIWidgets.EditBox, "Ticket cost: AI soldier killed")]
	int m_iCostAiKilled;

	[Attribute("1", UIWidgets.EditBox, "Ticket cost: player killed")]
	int m_iCostPlayerKilled;

	[Attribute("5", UIWidgets.EditBox, "Ticket cost: heavy vehicle destroyed (armed, many seats)")]
	int m_iCostVehicleHeavy;

	[Attribute("3", UIWidgets.EditBox, "Ticket cost: other vehicle destroyed")]
	int m_iCostVehicleLight;
}

[BaseContainerProps(configRoot: true)]
class CLW_ScenarioConfig
{
	[Attribute("", UIWidgets.EditBox, "Scenario name (log and dashboard)")]
	string m_sName;

	[Attribute()]
	ref array<ref CLW_FactionSetup> m_aFactions;

	[Attribute()]
	ref array<ref CLW_ObjectiveSetup> m_aObjectives;

	[Attribute()]
	ref CLW_EconomySetup m_Economy;

	[Attribute("", UIWidgets.ResourceNamePicker, "Faction profiles (CLW_FactionProfile) available in this scenario, looked up by the profile's faction key. A faction without one uses catalog auto-discovery", "conf")]
	ref array<ResourceName> m_aProfileConfigs;

	[Attribute("short", UIWidgets.EditBox, "Id of the war mode started by default / by auto start")]
	string m_sDefaultMode;

	[Attribute("0", UIWidgets.EditBox, "Seconds after game start to start the default mode with defaults (0 = wait for the setup). For headless / AI-vs-AI runs")]
	int m_iAutoStartSec;

	[Attribute()]
	ref array<ref CLW_WarModeConfig> m_aModes;

	[Attribute(desc: "Faction keys never offered in the setup menu (civilians, renegades, ...)")]
	ref array<string> m_aIgnoreFactions;

	protected static ref CLW_ScenarioConfig s_Active;
	protected bool m_bValid;

	// The loaded scenario (null before Load succeeded).
	static CLW_ScenarioConfig Get()
	{
		return s_Active;
	}

	// Load, validate and activate. Returns null (and leaves the previous one) on failure. Logs a summary.
	static CLW_ScenarioConfig Load(ResourceName path)
	{
		if (path.IsEmpty())
			return null;

		CLW_ScenarioConfig cfg = SCR_ConfigHelperT<CLW_ScenarioConfig>.GetConfigObject(path);
		if (!cfg)
		{
			Print("CLW: scenario config failed to load: " + path, LogLevel.ERROR);
			return null;
		}

		if (!cfg.Validate())
		{
			Print("CLW: scenario config INVALID: " + path, LogLevel.ERROR);
			return null;
		}

		s_Active = cfg;
		cfg.LogSummary(path);
		return cfg;
	}

	bool IsValid()
	{
		return m_bValid;
	}

	CLW_FactionSetup FindFaction(string key)
	{
		if (!m_aFactions)
			return null;

		foreach (CLW_FactionSetup setup : m_aFactions)
		{
			if (setup && setup.m_sKey == key)
				return setup;
		}

		return null;
	}

	bool IsFaction(string key)
	{
		return !key.IsEmpty() && FindFaction(key) != null;
	}

	// Static convenience for callers without a scenario reference: false when nothing is loaded.
	static bool IsClwFaction(string key)
	{
		if (!s_Active)
			return false;

		return s_Active.IsFaction(key);
	}

	// Key of faction number i (0 or 1), "" when out of range.
	string GetFactionKey(int index)
	{
		if (!m_aFactions || index < 0 || index >= m_aFactions.Count() || !m_aFactions[index])
			return "";

		return m_aFactions[index].m_sKey;
	}

	void GetFactionKeys(notnull array<string> keys)
	{
		if (!m_aFactions)
			return;

		foreach (CLW_FactionSetup setup : m_aFactions)
		{
			if (setup)
				keys.Insert(setup.m_sKey);
		}
	}

	// The other faction of a two-faction scenario ("" if unknown).
	string GetEnemyKey(string key)
	{
		if (!m_aFactions)
			return "";

		foreach (CLW_FactionSetup setup : m_aFactions)
		{
			if (setup && setup.m_sKey != key)
				return setup.m_sKey;
		}

		return "";
	}

	CLW_ObjectiveSetup FindObjective(string id)
	{
		if (!m_aObjectives)
			return null;

		foreach (CLW_ObjectiveSetup setup : m_aObjectives)
		{
			if (setup && setup.m_sId == id)
				return setup;
		}

		return null;
	}

	// Map symbol identity of a faction key (UNKNOWN for neutral / unknown keys).
	static EMilitarySymbolIdentity GetIdentity(string key)
	{
		if (!s_Active)
			return EMilitarySymbolIdentity.UNKNOWN;

		CLW_FactionSetup setup = s_Active.FindFaction(key);
		if (!setup)
			return EMilitarySymbolIdentity.UNKNOWN;

		return setup.GetIdentity();
	}

	// Checks the data; logs every problem. Fills defaults for a missing economy block.
	protected bool Validate()
	{
		m_bValid = false;
		bool ok = true;

		if (!m_Economy)
		{
			Print("CLW: scenario has no economy block, using defaults", LogLevel.WARNING);
			m_Economy = new CLW_EconomySetup();
			m_Economy.m_iIncomeIntervalSec = 60;
			m_Economy.m_iIncomeBase = 20;
			m_Economy.m_iIncomePerValue = 10;
			m_Economy.m_iBleedIntervalSec = 30;
			m_Economy.m_iBleedPerValue = 2;
			m_Economy.m_iCostAiKilled = 1;
			m_Economy.m_iCostPlayerKilled = 1;
			m_Economy.m_iCostVehicleHeavy = 5;
			m_Economy.m_iCostVehicleLight = 3;
		}

		if (m_Economy.m_iIncomeIntervalSec < 1 || m_Economy.m_iBleedIntervalSec < 1)
		{
			Print("CLW: scenario economy intervals must be >= 1 s", LogLevel.ERROR);
			ok = false;
		}

		if (!m_aFactions || m_aFactions.Count() != 2)
		{
			Print("CLW: scenario needs exactly 2 factions", LogLevel.ERROR);
			return false;
		}

		FactionManager factionManager = GetGame().GetFactionManager();
		array<string> seen = {};
		foreach (CLW_FactionSetup setup : m_aFactions)
		{
			if (!setup || setup.m_sKey.IsEmpty())
			{
				Print("CLW: scenario faction without key", LogLevel.ERROR);
				return false;
			}

			if (seen.Contains(setup.m_sKey))
			{
				Print("CLW: scenario duplicate faction " + setup.m_sKey, LogLevel.ERROR);
				ok = false;
			}
			seen.Insert(setup.m_sKey);

			if (factionManager && !factionManager.GetFactionByKey(setup.m_sKey))
			{
				Print("CLW: scenario faction not in FactionManager: " + setup.m_sKey, LogLevel.ERROR);
				ok = false;
			}

			if (setup.m_vBasePos == vector.Zero)
			{
				Print("CLW: scenario faction " + setup.m_sKey + " has no base position", LogLevel.ERROR);
				ok = false;
			}

			if (setup.m_SpawnPointPrefab.IsEmpty())
				Print("CLW: scenario faction " + setup.m_sKey + " has no spawn point prefab (players spawn at scene spawn points)", LogLevel.WARNING);
		}

		if (!m_aObjectives)
		{
			m_aObjectives = {};
		}

		array<string> ids = {};
		foreach (CLW_ObjectiveSetup obj : m_aObjectives)
		{
			if (!obj || obj.m_sId.IsEmpty())
			{
				Print("CLW: scenario objective without id", LogLevel.ERROR);
				ok = false;
				continue;
			}

			if (ids.Contains(obj.m_sId))
			{
				Print("CLW: scenario duplicate objective id " + obj.m_sId, LogLevel.ERROR);
				ok = false;
			}
			ids.Insert(obj.m_sId);

			if (obj.m_fRadius <= 0 || obj.m_iValue < 0)
			{
				Print("CLW: scenario objective " + obj.m_sId + " has bad radius/value", LogLevel.ERROR);
				ok = false;
			}

			if (!obj.m_sStartOwner.IsEmpty() && !seen.Contains(obj.m_sStartOwner))
			{
				Print("CLW: scenario objective " + obj.m_sId + " start owner unknown: " + obj.m_sStartOwner, LogLevel.ERROR);
				ok = false;
			}
		}

		if (!m_aModes || m_aModes.IsEmpty())
		{
			Print("CLW: scenario lists no war modes", LogLevel.ERROR);
			ok = false;
		}
		else
		{
			bool defaultFound;
			foreach (CLW_WarModeConfig modeConfig : m_aModes)
			{
				if (modeConfig && modeConfig.GetModeId() == m_sDefaultMode)
					defaultFound = true;
			}

			if (!defaultFound)
			{
				Print("CLW: scenario default war mode not listed: " + m_sDefaultMode, LogLevel.ERROR);
				ok = false;
			}
		}

		if (m_aObjectives.IsEmpty())
			Print("CLW: scenario has no objectives (war ends only by tickets/time)", LogLevel.WARNING);

		m_bValid = ok;
		return ok;
	}

	protected void LogSummary(string path)
	{
		Print(string.Format("CLW: scenario '%1' loaded from %2: %3 factions, %4 objectives", m_sName, path, m_aFactions.Count(), m_aObjectives.Count()), LogLevel.NORMAL);
		foreach (CLW_FactionSetup setup : m_aFactions)
		{
			Print(string.Format("CLW: scenario faction %1 identity=%2 base=%3 spawnPrefab=%4 profile=%5", setup.m_sKey, setup.m_sIdentity, setup.m_vBasePos, setup.m_SpawnPointPrefab, setup.m_ProfileConfig), LogLevel.NORMAL);
		}
		foreach (CLW_ObjectiveSetup obj : m_aObjectives)
		{
			Print(string.Format("CLW: scenario objective %1 '%2' pos=%3 r=%4 value=%5 owner='%6'", obj.m_sId, obj.m_sName, obj.m_vPos, obj.m_fRadius, obj.m_iValue, obj.m_sStartOwner), LogLevel.NORMAL);
		}
		Print(string.Format("CLW: scenario economy income=%1+%2/value per %3s bleed=%4/value per %5s costs ai=%6 player=%7 heavy=%8 light=%9", m_Economy.m_iIncomeBase, m_Economy.m_iIncomePerValue, m_Economy.m_iIncomeIntervalSec, m_Economy.m_iBleedPerValue, m_Economy.m_iBleedIntervalSec, m_Economy.m_iCostAiKilled, m_Economy.m_iCostPlayerKilled, m_Economy.m_iCostVehicleHeavy, m_Economy.m_iCostVehicleLight), LogLevel.NORMAL);
		Print(string.Format("CLW: scenario modes=%1 default=%2 autoStartSec=%3", m_aModes.Count(), m_sDefaultMode, m_iAutoStartSec), LogLevel.NORMAL);
	}
}

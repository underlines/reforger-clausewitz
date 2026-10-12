// War mode base (spec 2a): all RULE decisions of the war live in a mode object; CLW_WarState keeps the mechanisms
// (ledgers, kill accounting, events, EndGameMode, spawn gate). Hooks are generic so other modes (endless: ticket refill,
// "faction lost all units" victory, HQ respawning units, no time limit) fit without changing CLW_WarState.
class CLW_WarMode
{
	protected ref array<ref CLW_ModeParam> m_aParams = {};

	string GetId()
	{
		return "";
	}

	string GetName()
	{
		return "";
	}

	string GetDescription()
	{
		return "";
	}

	// ---- capabilities (see CLW_Capabilities) ----

	// Capability names this mode declares (services, fob, logistics, requests, reports, kitCost, ...). Append to caps.
	void GetCapabilities(notnull array<string> caps)
	{
	}

	// Declared AND not switched off by a bool mode param "cap_<name>" (only when the mode defines such a param).
	bool HasCapability(string name)
	{
		array<string> caps = {};
		GetCapabilities(caps);
		if (!caps.Contains(name))
			return false;

		CLW_ModeParam toggle = FindIn(m_aParams, "cap_" + name);
		if (toggle && toggle.m_eType == CLW_ModeParam.TYPE_BOOL && !toggle.GetBool())
			return false;

		return true;
	}

	// All capabilities that are on right now.
	void GetActiveCapabilities(notnull array<string> outNames)
	{
		array<string> caps = {};
		GetCapabilities(caps);
		foreach (string cap : caps)
		{
			if (HasCapability(cap))
				outNames.Insert(cap);
		}
	}

	// Live parameter list (definitions + current values).
	void GetParams(notnull array<ref CLW_ModeParam> outParams)
	{
		foreach (CLW_ModeParam one : m_aParams)
			outParams.Insert(one);
	}

	// Validates ALL values first; applies them only if every one is valid. Unknown ids are rejected.
	bool ApplyParams(notnull array<ref CLW_ModeParam> values, out string error)
	{
		array<ref CLW_ModeParam> staged = {};
		foreach (CLW_ModeParam param : m_aParams)
		{
			staged.Insert(param.Copy());
		}

		foreach (CLW_ModeParam value : values)
		{
			CLW_ModeParam target = FindIn(staged, value.m_sId);
			if (!target)
			{
				error = "unknown parameter " + value.m_sId;
				return false;
			}

			string text = value.ValueText();
			if (!target.SetFromText(text, error))
				return false;
		}

		foreach (int i, CLW_ModeParam applied : staged)
		{
			m_aParams[i].m_fValue = applied.m_fValue;
		}

		return true;
	}

	// Same from a flat JSON object {"id":value,...}; an empty string / "{}" keeps the defaults.
	bool ApplyParamsJson(string json, out string error)
	{
		json = json.Trim();
		if (json.IsEmpty())
			return true;

		map<string, string> parsed = new map<string, string>();
		if (!CLW_ModeParam.ParseFlat(json, parsed, error))
			return false;

		array<ref CLW_ModeParam> values = {};
		foreach (string key, string text : parsed)
		{
			CLW_ModeParam own = FindIn(m_aParams, key);
			if (!own)
			{
				error = "unknown parameter " + key;
				return false;
			}

			CLW_ModeParam candidate = own.Copy();
			if (!candidate.SetFromText(text, error))
				return false;
			values.Insert(candidate);
		}

		return ApplyParams(values, error);
	}

	// {"id":"short","name":"..","description":"..","params":[{...}]}
	string ToJson()
	{
		string json = "{\"id\":\"" + CLW_ModeParam.Escape(GetId()) + "\",\"name\":\"" + CLW_ModeParam.Escape(GetName()) + "\",\"description\":\"" + CLW_ModeParam.Escape(GetDescription()) + "\",\"params\":[";
		foreach (int i, CLW_ModeParam param : m_aParams)
		{
			if (i > 0)
				json += ",";
			json += param.ToJson();
		}
		return json + "]}";
	}

	string ValuesText()
	{
		return CLW_ModeParam.ValuesToJson(m_aParams);
	}

	// Called once when the war switches SETUP -> RUNNING: set ledgers via war.SetTickets / SetSupplies.
	void OnWarStart(CLW_WarState war)
	{
	}

	// Every war tick (dt seconds) while RUNNING: income, bleed, timers.
	void OnTick(CLW_WarState war, float dt)
	{
	}

	// A character or vehicle of factionKey was destroyed (RUNNING only).
	void OnUnitKilled(CLW_WarState war, string factionKey, bool isPlayer, bool isVehicle, bool isHeavyVehicle)
	{
	}

	void OnObjectiveChanged(CLW_WarState war, string id, string oldOwner, string newOwner)
	{
	}

	// Win/draw decision. Return true to end; winnerFaction "" = draw.
	bool CheckEnd(CLW_WarState war, out string winnerFaction, out EGameOverTypes reason)
	{
		return false;
	}

	// Supplies per income interval for a faction at the current holdings.
	int GetIncome(CLW_WarState war, string factionKey)
	{
		return 0;
	}

	int GetAICap(string factionKey)
	{
		return 0;
	}

	// Prevented-agent budget of the activity management per side (0 = the mode gives none, CLW_WarConfig.ACTIVITY_DEFAULT_CAP applies).
	int GetActiveAgentCap(string factionKey)
	{
		return 0;
	}

	int GetVehicleCap(string factionKey)
	{
		return 0;
	}

	// ---- battle layout (HQ / FOB separation, objective set) ----

	// Scale of the authored HQ separation for the chosen battle size (1 = as authored). FOB distances use the same scale.
	float GetDistanceScale()
	{
		return 1;
	}

	// Index of the chosen battle size in the mode's size list (-1 = the mode has no battle size), and its name.
	int GetBattleSizeIndex()
	{
		return -1;
	}

	string GetBattleSizeName()
	{
		return "";
	}

	float GetMinHqDistanceM()
	{
		return 0;
	}

	float GetHqObjectiveClearM()
	{
		return 0;
	}

	float GetObjectiveGapM()
	{
		return 0;
	}

	// Moves the HQs (scaled separation), re-spaces / filters the objectives. Call from OnWarStart BEFORE anything is spawned
	// at the bases (start forces spawn on GetOnWarStarted, which fires after OnWarStart).
	protected void ApplyBattleLayout()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (!scenario || !bases)
		{
			Print("CLW: bases layout skipped: no scenario / bases object", LogLevel.WARNING);
			return;
		}

		vector mid;
		float factor = 1;
		bases.ApplyLayout(scenario, GetBattleSizeName(), GetDistanceScale(), GetMinHqDistanceM(), mid, factor);
		if (points)
		{
			array<vector> hqs = {};
			array<string> keys = {};
			scenario.GetFactionKeys(keys);
			foreach (string key : keys)
			{
				hqs.Insert(bases.GetBasePos(key));
			}

			points.ApplyLayout(scenario, GetBattleSizeIndex(), GetBattleSizeName(), mid, factor, hqs, GetHqObjectiveClearM(), GetObjectiveGapM());
		}
	}

	// Seconds left (-1 = no time limit).
	float GetTimeLeftSec()
	{
		return -1;
	}

	// False for modes without a ticket ledger (the HUD shows "-").
	bool UsesTickets()
	{
		return true;
	}

	bool AllowsPlayerSpawn(string factionKey)
	{
		return true;
	}

	protected CLW_ModeParam FindIn(notnull array<ref CLW_ModeParam> list, string id)
	{
		foreach (CLW_ModeParam param : list)
		{
			if (param.m_sId == id)
				return param;
		}

		return null;
	}

	protected CLW_ModeParam Param(string id)
	{
		return FindIn(m_aParams, id);
	}
}

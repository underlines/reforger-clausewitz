// Action registry (M3 phase R, notes/decision-command-tools.md): ONE list of everything HQ (rules, LLM) and later players can ask
// the game to do. An entry is data (Configs/CLW/Actions/Actions.conf maps name -> executor class) or is registered from script by
// later phases. The strategic prompt's command list, the validator and the executors all come from here, so they cannot drift.
// Entries whose `requires` (war-mode capabilities, see CLW_Capabilities) are unmet drop out of the manifest and are refused.
// Server only. Singleton per war: CLW_ActionRegistry.Get(); the game mode calls Reset() once at start.

// One parameter of an action: validation schema + manifest/LLM documentation.
[BaseContainerProps()]
class CLW_ActionParam
{
	static const string TYPE_STRING = "string";
	static const string TYPE_INT = "int";
	static const string TYPE_ENUM = "enum";
	static const string TYPE_POSITION = "position";	// [x, z] world coordinates
	static const string TYPE_UNIT = "unit";			// squad / unit id from the situation
	static const string TYPE_BASE = "base";			// base or FOB id

	// No field initializers on [Attribute] members (they would override the .conf values), defaults live in the Attribute.
	[Attribute("", UIWidgets.EditBox, "Parameter name (JSON key of the command)")]
	string m_sName;

	[Attribute("string", UIWidgets.EditBox, "Type: string, int, enum, position, unit, base")]
	string m_sType;

	[Attribute("0", UIWidgets.CheckBox, "Required")]
	bool m_bRequired;

	[Attribute()]
	ref array<string> m_aEnum;

	[Attribute("", UIWidgets.EditBox, "Short hint for the LLM")]
	string m_sHint;

	static CLW_ActionParam Create(string name, string type, bool required, string hint)
	{
		CLW_ActionParam param = new CLW_ActionParam();
		param.m_sName = name;
		param.m_sType = type;
		param.m_bRequired = required;
		param.m_sHint = hint;
		param.m_aEnum = {};
		return param;
	}

	string ToJson()
	{
		string text = "{\"name\":\"" + CLW_Situation.Escape(m_sName) + "\",\"type\":\"" + CLW_Situation.Escape(m_sType) + "\",\"required\":" + CLW_Situation.BoolText(m_bRequired);
		if (m_aEnum && !m_aEnum.IsEmpty())
			text += ",\"enum\":" + CLW_ActionRegistry.JsonStrings(m_aEnum);
		if (!m_sHint.IsEmpty())
			text += ",\"hint\":\"" + CLW_Situation.Escape(m_sHint) + "\"";
		return text + "}";
	}
}

// Registry entry. Data from the .conf, or built in script (Create + AddParam).
[BaseContainerProps()]
class CLW_ActionEntry
{
	static const string LAYER_INTENT = "intent";
	static const string LAYER_PRIMITIVE = "primitive";
	static const string AUTH_HQ = "hq";
	static const string AUTH_PLAYER = "player";
	static const string AUTH_ADMIN = "admin";

	[Attribute("", UIWidgets.EditBox, "Unique action name; intents are upper case (ATTACK, RESUPPLY), primitives lower case (form_squad)")]
	string m_sName;

	[Attribute("intent", UIWidgets.EditBox, "Layer: intent (strategic tick, ~10-15 operational commands) or primitive (agent turn / rare actions)")]
	string m_sLayer;

	[Attribute("orders", UIWidgets.EditBox, "Key of the command JSON reply the sidecar files this action under: orders (one per squad), buy, missions")]
	string m_sChannel;

	[Attribute("hq", UIWidgets.EditBox, "Who may call it: hq (commander rules / LLM), player (a player request), admin")]
	string m_sAuthority;

	[Attribute("0", UIWidgets.EditBox, "Cost hint in supplies (0 = free or depends on the parameters)")]
	int m_iCost;

	[Attribute("", UIWidgets.EditBox, "Description shown to the LLM")]
	string m_sDescription;

	[Attribute()]
	ref array<string> m_aRequires;	// war-mode capability names that must be on

	[Attribute()]
	ref array<string> m_aTags;

	[Attribute()]
	ref array<ref CLW_ActionParam> m_aParams;

	[Attribute("", UIWidgets.EditBox, "Executor script class (derived from CLW_ActionExecutor), created with typename.Spawn()")]
	string m_sExecutor;

	[Attribute()]
	ref array<string> m_aSteps;	// macro: action names run in order with the same arguments (instead of an executor)

	// runtime
	ref CLW_ActionExecutor m_Executor;

	// Script-registered entry. Add params with AddParam, then CLW_ActionRegistry.Get().Register(entry).
	static CLW_ActionEntry Create(string name, string layer, string description, CLW_ActionExecutor executor)
	{
		CLW_ActionEntry entry = new CLW_ActionEntry();
		entry.m_sName = name;
		entry.m_sLayer = layer;
		entry.m_sChannel = "orders";
		entry.m_sAuthority = AUTH_HQ;
		entry.m_sDescription = description;
		entry.m_Executor = executor;
		entry.Normalize();
		return entry;
	}

	CLW_ActionEntry AddParam(string name, string type, bool required, string hint)
	{
		Normalize();
		m_aParams.Insert(CLW_ActionParam.Create(name, type, required, hint));
		return this;
	}

	// Arrays absent in the .conf are null; empty strings get the attribute defaults (script-built entries).
	void Normalize()
	{
		if (!m_aRequires)
			m_aRequires = {};
		if (!m_aTags)
			m_aTags = {};
		if (!m_aParams)
			m_aParams = {};
		if (!m_aSteps)
			m_aSteps = {};
		if (m_sLayer.IsEmpty())
			m_sLayer = LAYER_INTENT;
		if (m_sChannel.IsEmpty())
			m_sChannel = "orders";
		if (m_sAuthority.IsEmpty())
			m_sAuthority = AUTH_HQ;
	}

	string ToJson()
	{
		string text = "{\"name\":\"" + CLW_Situation.Escape(m_sName) + "\",\"layer\":\"" + CLW_Situation.Escape(m_sLayer) + "\",\"channel\":\"" + CLW_Situation.Escape(m_sChannel) + "\",\"authority\":\"" + CLW_Situation.Escape(m_sAuthority) + "\",\"cost\":" + m_iCost;
		text += ",\"tags\":" + CLW_ActionRegistry.JsonStrings(m_aTags) + ",\"requires\":" + CLW_ActionRegistry.JsonStrings(m_aRequires);
		if (!m_aSteps.IsEmpty())
			text += ",\"steps\":" + CLW_ActionRegistry.JsonStrings(m_aSteps);
		text += ",\"desc\":\"" + CLW_Situation.Escape(m_sDescription) + "\",\"params\":[";
		foreach (int i, CLW_ActionParam param : m_aParams)
		{
			if (i > 0)
				text += ",";
			text += param.ToJson();
		}

		return text + "]}";
	}
}

[BaseContainerProps(configRoot: true)]
class CLW_ActionRegistryConfig
{
	[Attribute()]
	ref array<ref CLW_ActionEntry> m_aEntries;
}

// One invocation of an action: who calls (faction, authority, source), the arguments and some context for the executor.
class CLW_ActionCall
{
	string m_sAction;
	string m_sFaction;
	string m_sAuthority = "hq";		// hq | player | admin
	string m_sSource = "rules";		// rules | llm | player (for logs)
	int m_iPlayerId = -1;
	string m_sReason;
	CLW_RuleCommander m_Rules;		// weak: the faction's rule commander (squad checks, orders), set by the HQ callers
	ref Managed m_Payload;			// structured extra input an executor understands (e.g. the mission offers of a squad)
	int m_iApplied;					// set by executors: how many items were done (buys, offers)
	string m_sResultWire;			// player-facing version of the result / refusal as a CLW_Loc wire ("" = only the English text); the registry and the executors set it
	int m_iDepth;					// macro nesting

	protected ref map<string, string> m_mArgs = new map<string, string>();
	protected ref map<string, float> m_mNums = new map<string, float>();	// positions: name#x, name#z

	void SetString(string name, string value)
	{
		m_mArgs.Set(name, value);
	}

	void SetInt(string name, int value)
	{
		m_mArgs.Set(name, value.ToString());
	}

	string GetString(string name)
	{
		string value;
		m_mArgs.Find(name, value);
		return value;
	}

	int GetInt(string name, int fallback = 0)
	{
		string value;
		if (!m_mArgs.Find(name, value) || !CLW_ModeParam.IsNumber(value))
			return fallback;

		return value.ToInt();
	}

	// A string/int/enum argument that is set and not empty.
	bool HasArg(string name)
	{
		string value;
		return m_mArgs.Find(name, value) && !value.IsEmpty();
	}

	void SetPos(string name, float x, float z)
	{
		m_mNums.Set(name + "#x", x);
		m_mNums.Set(name + "#z", z);
	}

	bool HasPos(string name)
	{
		return m_mNums.Contains(name + "#x") && m_mNums.Contains(name + "#z");
	}

	// World position (y = 0), vector.Zero when absent.
	vector GetPos(string name)
	{
		float x;
		float z;
		if (!m_mNums.Find(name + "#x", x) || !m_mNums.Find(name + "#z", z))
			return vector.Zero;

		return Vector(x, 0, z);
	}

	// What the player reads about the result: the wire when the registry / executor set one, else the English text as a flat param
	// (a deep reason from a service stays English). English text stays for logs, history, dashboard and the LLM.
	string PlayerWire(string english)
	{
		if (!m_sResultWire.IsEmpty())
			return m_sResultWire;

		return CLW_HqText.Param(english);
	}

	// Same call under another action name (macro steps).
	CLW_ActionCall CopyAs(string action)
	{
		CLW_ActionCall copy = new CLW_ActionCall();
		copy.m_sAction = action;
		copy.m_sFaction = m_sFaction;
		copy.m_sAuthority = m_sAuthority;
		copy.m_sSource = m_sSource;
		copy.m_iPlayerId = m_iPlayerId;
		copy.m_sReason = m_sReason;
		copy.m_Rules = m_Rules;
		copy.m_Payload = m_Payload;
		copy.m_iDepth = m_iDepth + 1;
		foreach (string key, string text : m_mArgs)
		{
			copy.m_mArgs.Set(key, text);
		}

		foreach (string numKey, float number : m_mNums)
		{
			copy.m_mNums.Set(numKey, number);
		}

		return copy;
	}
}

// Base class of the executors named in the .conf. One shared instance per entry: keep NO per-call state in members.
class CLW_ActionExecutor
{
	// Extra availability check on top of the entry's capabilities (e.g. "the faction has a depot"). False = drops out of the manifest.
	bool IsAvailable(string factionKey)
	{
		return true;
	}

	// Checks the call (the registry already checked the schema, capabilities and authority). May normalise arguments.
	bool Validate(CLW_ActionCall call, out string error)
	{
		return true;
	}

	// Does it. result = short text for logs / summaries. False when nothing was done (result then says why).
	bool Execute(CLW_ActionCall call, out string result)
	{
		result = "nothing to do";
		return false;
	}
}

// Manifest cache of one faction: the hash changes whenever the visible content changes.
class CLW_ManifestCache
{
	string m_sBody;
	string m_sJson;
	string m_sHash;
	int m_iSeq;
}

class CLW_ActionRegistry
{
	static const ResourceName CONFIG = "Configs/CLW/Actions/Actions.conf";
	static const int MAX_MACRO_DEPTH = 4;

	protected static ref CLW_ActionRegistry s_Instance;

	protected ref array<ref CLW_ActionEntry> m_aEntries = {};
	protected ref map<string, ref CLW_ManifestCache> m_mManifests = new map<string, ref CLW_ManifestCache>();
	protected int m_iEpoch;

	void CLW_ActionRegistry()
	{
		m_iEpoch = Math.RandomInt(1, 1000000);
	}

	static CLW_ActionRegistry Get()
	{
		if (!s_Instance)
		{
			s_Instance = new CLW_ActionRegistry();
			s_Instance.LoadConfig(CONFIG);
		}

		return s_Instance;
	}

	// New registry for a new war (data entries reloaded). Script entries must register after this.
	static void Reset()
	{
		s_Instance = null;
		Get();
	}

	// ---- registration ----

	bool LoadConfig(ResourceName path)
	{
		CLW_ActionRegistryConfig config = SCR_ConfigHelperT<CLW_ActionRegistryConfig>.GetConfigObject(path);
		if (!config || !config.m_aEntries)
		{
			Print("CLW: action registry config failed to load: " + path, LogLevel.ERROR);
			return false;
		}

		int added;
		foreach (CLW_ActionEntry entry : config.m_aEntries)
		{
			if (Register(entry))
				added++;
		}

		Print(string.Format("CLW: action registry: %1 of %2 entries from %3", added, config.m_aEntries.Count(), path), LogLevel.NORMAL);
		return added > 0;
	}

	// Adds (or replaces, same name) an entry. Creates the executor from m_sExecutor unless one is set. False (logged) when invalid.
	bool Register(CLW_ActionEntry entry)
	{
		if (!entry || entry.m_sName.IsEmpty())
		{
			Print("CLW: action registry: entry without a name ignored", LogLevel.WARNING);
			return false;
		}

		entry.Normalize();
		if (!entry.m_Executor && !entry.m_sExecutor.IsEmpty())
		{
			typename executorType = entry.m_sExecutor.ToType();
			if (executorType)
				entry.m_Executor = CLW_ActionExecutor.Cast(executorType.Spawn());

			if (!entry.m_Executor)
			{
				Print(string.Format("CLW: action registry: %1: executor class '%2' not found or not a CLW_ActionExecutor, entry ignored", entry.m_sName, entry.m_sExecutor), LogLevel.ERROR);
				return false;
			}
		}

		if (!entry.m_Executor && entry.m_aSteps.IsEmpty())
		{
			Print(string.Format("CLW: action registry: %1 has neither an executor nor steps, entry ignored", entry.m_sName), LogLevel.ERROR);
			return false;
		}

		for (int i = m_aEntries.Count() - 1; i >= 0; i--)
		{
			if (m_aEntries[i].m_sName == entry.m_sName)
			{
				Print("CLW: action registry: " + entry.m_sName + " replaced", LogLevel.NORMAL);
				m_aEntries.Remove(i);
			}
		}

		m_aEntries.Insert(entry);
		return true;
	}

	CLW_ActionEntry Find(string name)
	{
		foreach (CLW_ActionEntry entry : m_aEntries)
		{
			if (entry.m_sName == name)
				return entry;
		}

		return null;
	}

	int Count()
	{
		return m_aEntries.Count();
	}

	// ---- availability ----

	// All capabilities of the entry are on and its executor accepts the faction ("" = any).
	bool IsAvailable(CLW_ActionEntry entry, string factionKey = "")
	{
		foreach (string capability : entry.m_aRequires)
		{
			if (!CLW_Capabilities.Has(capability))
				return false;
		}

		if (entry.m_Executor && !entry.m_Executor.IsAvailable(factionKey))
			return false;

		return true;
	}

	// ---- validate / execute ----

	// Refusal of the registry itself: English text for log / history / LLM, wire for the player. Always returns false.
	protected bool Fail(CLW_ActionCall call, out string text, string english, string wire)
	{
		text = english;
		call.m_sResultWire = wire;
		return false;
	}

	bool Validate(CLW_ActionCall call, out string error)
	{
		CLW_ActionEntry entry = Find(call.m_sAction);
		if (!entry)
			return Fail(call, error, "unknown action " + call.m_sAction, CLW_Loc.W("#CLW-Act_Unknown", call.m_sAction));

		if (call.m_iDepth > MAX_MACRO_DEPTH)
			return Fail(call, error, "macro nested too deep at " + call.m_sAction, CLW_Loc.W("#CLW-Act_MacroDeep", call.m_sAction));

		foreach (string capability : entry.m_aRequires)
		{
			if (!CLW_Capabilities.Has(capability))
				return Fail(call, error, string.Format("%1 needs capability '%2', which is off in this war mode", entry.m_sName, capability), CLW_Loc.W("#CLW-Act_CapOff", entry.m_sName));
		}

		if (call.m_sAuthority != CLW_ActionEntry.AUTH_ADMIN && call.m_sAuthority != entry.m_sAuthority)
			return Fail(call, error, string.Format("%1 needs authority '%2', caller has '%3'", entry.m_sName, entry.m_sAuthority, call.m_sAuthority), CLW_Loc.W("#CLW-Act_Authority", entry.m_sName));

		if (!ValidateParams(entry, call, error))
			return false;

		if (entry.m_Executor)
		{
			if (!entry.m_Executor.IsAvailable(call.m_sFaction))
				return Fail(call, error, entry.m_sName + " is not available to " + call.m_sFaction, CLW_Loc.W("#CLW-Act_NotAvailable", entry.m_sName));

			return entry.m_Executor.Validate(call, error);
		}

		foreach (string step : entry.m_aSteps)
		{
			if (!Find(step))
				return Fail(call, error, string.Format("%1: unknown step %2", entry.m_sName, step), CLW_Loc.W("#CLW-Act_UnknownStep", entry.m_sName, step));
		}

		return true;
	}

	// Executes an already validated call (macros run their steps, each validated and executed in turn).
	bool Execute(CLW_ActionCall call, out string result)
	{
		CLW_ActionEntry entry = Find(call.m_sAction);
		if (!entry)
			return Fail(call, result, "unknown action " + call.m_sAction, CLW_Loc.W("#CLW-Act_Unknown", call.m_sAction));

		if (entry.m_Executor)
			return entry.m_Executor.Execute(call, result);

		string summary;
		string summaryWire;
		foreach (string step : entry.m_aSteps)
		{
			string stepResult;
			CLW_ActionCall stepCall = call.CopyAs(step);
			if (!Run(stepCall, stepResult))
			{
				result = string.Format("%1 stopped at %2: %3", entry.m_sName, step, stepResult);
				call.m_sResultWire = CLW_Loc.W("#CLW-Act_MacroStopped", entry.m_sName, step, stepCall.PlayerWire(stepResult));
				return false;
			}

			if (!summary.IsEmpty())
			{
				summary += "; ";
				summaryWire += "
";
			}

			summary += stepResult;
			summaryWire += stepCall.PlayerWire(stepResult);
		}

		result = summary;
		call.m_sResultWire = summaryWire;
		return true;
	}

	// Validate + Execute. On a failed validation result holds the error.
	bool Run(CLW_ActionCall call, out string result)
	{
		string error;
		if (!Validate(call, error))
		{
			result = error;
			return false;
		}

		return Execute(call, result);
	}

	protected bool ValidateParams(CLW_ActionEntry entry, CLW_ActionCall call, out string error)
	{
		foreach (CLW_ActionParam param : entry.m_aParams)
		{
			if (!param)
				continue;

			bool present;
			if (param.m_sType == CLW_ActionParam.TYPE_POSITION)
				present = call.HasPos(param.m_sName);
			else
				present = call.HasArg(param.m_sName);

			if (!present)
			{
				if (param.m_bRequired)
					return Fail(call, error, string.Format("%1: missing parameter %2", entry.m_sName, param.m_sName), CLW_Loc.W("#CLW-Act_MissingParam", entry.m_sName, param.m_sName));

				continue;
			}

			if (param.m_sType == CLW_ActionParam.TYPE_POSITION)
			{
				vector pos = call.GetPos(param.m_sName);
				for (int k = 0; k < 3; k += 2)
				{
					float coordinate = pos[k];
					if (coordinate != coordinate || coordinate > 100000 || coordinate < -100000)
						return Fail(call, error, string.Format("%1: %2 not finite", entry.m_sName, param.m_sName), CLW_Loc.W("#CLW-Act_PosInvalid", entry.m_sName, param.m_sName));
				}

				continue;
			}

			string value = call.GetString(param.m_sName);
			if (param.m_sType == CLW_ActionParam.TYPE_INT && !CLW_ModeParam.IsNumber(value))
				return Fail(call, error, string.Format("%1: %2 is not a number", entry.m_sName, param.m_sName), CLW_Loc.W("#CLW-Act_NotNumber", entry.m_sName, param.m_sName));

			if (param.m_sType == CLW_ActionParam.TYPE_ENUM && param.m_aEnum && !param.m_aEnum.IsEmpty() && !param.m_aEnum.Contains(value))
				return Fail(call, error, string.Format("%1: %2 '%3' is not one of the allowed values", entry.m_sName, param.m_sName, value), CLW_Loc.W("#CLW-Act_BadValue", entry.m_sName, param.m_sName, CLW_HqText.Param(value)));
		}

		return true;
	}

	// ---- manifest ----

	static string JsonStrings(array<string> list)
	{
		string text = "[";
		if (list)
		{
			foreach (int i, string item : list)
			{
				if (i > 0)
					text += ",";
				text += "\"" + CLW_Situation.Escape(item) + "\"";
			}
		}

		return text + "]";
	}

	// Manifest JSON of what factionKey's HQ may use right now (POSTed to the sidecar) and its hash. intents = layer intent with hq
	// authority (the only entries the strategic tick sees), primitives = layer primitive (agent turns), listed separately. Entries
	// with unmet capabilities are left out. The hash changes whenever the visible content changes (mode / capability switch,
	// new entries) and is unique per game session, so the sidecar can tell a stale or lost manifest.
	string GetManifest(string factionKey, out string hash)
	{
		string body = BuildManifestBody(factionKey);
		CLW_ManifestCache cache;
		if (!m_mManifests.Find(factionKey, cache))
		{
			cache = new CLW_ManifestCache();
			m_mManifests.Set(factionKey, cache);
		}

		if (cache.m_sHash.IsEmpty() || cache.m_sBody != body)
		{
			cache.m_iSeq++;
			cache.m_sBody = body;
			cache.m_sHash = m_iEpoch.ToString() + "-" + cache.m_iSeq;
			cache.m_sJson = "{\"v\":3,\"faction\":\"" + CLW_Situation.Escape(factionKey) + "\",\"hash\":\"" + cache.m_sHash + "\"," + body + "}";
		}

		hash = cache.m_sHash;
		return cache.m_sJson;
	}

	protected string BuildManifestBody(string factionKey)
	{
		string intents;
		string primitives;
		foreach (CLW_ActionEntry entry : m_aEntries)
		{
			if (!IsAvailable(entry, factionKey))
				continue;

			if (entry.m_sLayer == CLW_ActionEntry.LAYER_INTENT)
			{
				if (entry.m_sAuthority != CLW_ActionEntry.AUTH_HQ)
					continue;

				if (!intents.IsEmpty())
					intents += ",";
				intents += entry.ToJson();
			}
			else if (entry.m_sLayer == CLW_ActionEntry.LAYER_PRIMITIVE)
			{
				if (entry.m_sAuthority == CLW_ActionEntry.AUTH_ADMIN)
					continue;

				if (!primitives.IsEmpty())
					primitives += ",";
				primitives += entry.ToJson();
			}
		}

		string modeId;
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war && war.GetMode())
			modeId = war.GetMode().GetId();

		array<string> caps = {};
		CLW_Capabilities.GetAll(caps);
		string body = "\"mode\":\"" + CLW_Situation.Escape(modeId) + "\",\"capabilities\":" + JsonStrings(caps);
		body += ",\"intents\":[" + intents + "],\"primitives\":[" + primitives + "]";
		return body;
	}
}

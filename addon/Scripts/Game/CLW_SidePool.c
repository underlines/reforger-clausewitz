// Side pools (M4 WP3, VISION 10a): a side's buy catalog = its own faction entries, then the entries of its kin factions for the roles
// (kinds) the own faction lacks. Kin = the factions the faction data calls friendly (a pack faction lists the vanilla faction it is
// built on, the same rule as the arsenal item pool CLW_ArsenalPool.GetKin), plus explicit kin from data (CLW_FactionProfile.m_aKinFactions)
// and from the server file; never the enemy side, never a faction that is friendly to the enemy side (civilians), never an excluded one.
// Own entries always win; kin entries only fill an EMPTY role (unless kinFill all, see overrides.md), deduped by role + vehicle model. The catalog code lives in CLW_ForceCatalog
// (FillFromKin); this file holds the rules, the server-file readers (overrides.json keys "sides" and "props") and a tiny JSON reader.
//
// overrides.json (details: dev/notes/overrides.md):
//   "sides": { "<faction key or *>": { "ownOnly": true, "kin": ["US"], "excludeFactions": ["FIA"], "excludePathContains": ["Tank"] } }
//   "props": { "<faction key or *>": { "style": "<set>", "<role>": "<prefab path | vanilla | set name>" } }
// Log prefixes: "CLW: sidepool" (kin selection and imports), "CLW: overrides sides|props" (the file), "CLW: props" (CLW_PropResolver).

// ---- tiny JSON reader (the file is small; the engine's typed readers need fixed field names, we need free keys) ----
class CLW_JsonNode
{
	static const int T_NONE = 0;
	static const int T_STRING = 1;
	static const int T_SCALAR = 2;		// number, true, false, null (raw text)
	static const int T_OBJECT = 3;
	static const int T_ARRAY = 4;

	int m_iType;
	string m_sText;
	ref array<string> m_aKeys = {};					// object keys, parallel to m_aValues
	ref array<ref CLW_JsonNode> m_aValues = {};		// object values or array items

	bool IsObject()
	{
		return m_iType == T_OBJECT;
	}

	bool IsArray()
	{
		return m_iType == T_ARRAY;
	}

	bool IsString()
	{
		return m_iType == T_STRING;
	}

	CLW_JsonNode Field(string key)
	{
		if (m_iType != T_OBJECT)
			return null;

		int i = 0;
		while (i < m_aKeys.Count())
		{
			if (m_aKeys[i] == key)
				return m_aValues[i];

			i++;
		}

		return null;
	}

	// Text of a string or scalar field, "" when absent or a container.
	string FieldText(string key)
	{
		CLW_JsonNode node = Field(key);
		if (!node || (node.m_iType != T_STRING && node.m_iType != T_SCALAR))
			return "";

		return node.m_sText;
	}

	// true / false literal (or the strings "true" / "false"); def when absent or anything else.
	bool FieldBool(string key, bool def)
	{
		string text = FieldText(key);
		text.ToLower();
		if (text == "true" || text == "1")
			return true;
		if (text == "false" || text == "0")
			return false;

		return def;
	}

	// String items of an array field (other item types are skipped).
	void FieldStrings(string key, notnull array<string> outStrings)
	{
		CLW_JsonNode node = Field(key);
		if (!node || node.m_iType != T_ARRAY)
			return;

		foreach (CLW_JsonNode item : node.m_aValues)
		{
			if (item && item.m_iType == T_STRING && !item.m_sText.IsEmpty())
				outStrings.Insert(item.m_sText);
		}
	}
}

class CLW_JsonReader
{
	static const int MAX_DEPTH = 12;

	protected string m_sText;
	protected int m_iPos;
	protected int m_iLen;
	protected string m_sError;

	// Parses the first JSON object of the text. Null with an error text when it is malformed.
	static CLW_JsonNode Parse(string text, out string error)
	{
		CLW_JsonReader reader = new CLW_JsonReader();
		reader.m_sText = text;
		reader.m_iLen = text.Length();
		reader.m_iPos = text.IndexOf("{");
		if (reader.m_iPos < 0)
		{
			error = "no JSON object";
			return null;
		}

		CLW_JsonNode root = reader.ParseValue(0);
		if (!root)
			error = reader.m_sError;

		return root;
	}

	protected void SkipSpace()
	{
		while (m_iPos < m_iLen)
		{
			string c = m_sText.Get(m_iPos);
			if (c == " " || c == "\t" || c == "\r" || c == "\n")
				m_iPos++;
			else
				return;
		}
	}

	protected CLW_JsonNode Fail(string why)
	{
		if (m_sError.IsEmpty())
			m_sError = why + " at character " + m_iPos;

		return null;
	}

	protected CLW_JsonNode ParseValue(int depth)
	{
		if (depth > MAX_DEPTH)
			return Fail("nesting too deep");

		SkipSpace();
		if (m_iPos >= m_iLen)
			return Fail("unexpected end");

		string c = m_sText.Get(m_iPos);
		if (c == "{")
			return ParseObject(depth);
		if (c == "[")
			return ParseArray(depth);

		CLW_JsonNode node = new CLW_JsonNode();
		if (c == "\"")
		{
			node.m_iType = CLW_JsonNode.T_STRING;
			node.m_sText = ParseString();
			if (!m_sError.IsEmpty())
				return null;

			return node;
		}

		node.m_iType = CLW_JsonNode.T_SCALAR;
		while (m_iPos < m_iLen)
		{
			string d = m_sText.Get(m_iPos);
			if (d == "," || d == "}" || d == "]" || d == " " || d == "\t" || d == "\r" || d == "\n")
				break;

			node.m_sText += d;
			m_iPos++;
		}

		if (node.m_sText.IsEmpty())
			return Fail("unexpected character");

		return node;
	}

	// Position on the opening quote; leaves it behind the closing quote.
	protected string ParseString()
	{
		string value;
		m_iPos++;
		while (m_iPos < m_iLen)
		{
			string c = m_sText.Get(m_iPos);
			if (c == "\"")
			{
				m_iPos++;
				return value;
			}

			if (c == "\\")
			{
				m_iPos++;
				if (m_iPos >= m_iLen)
					break;

				string e = m_sText.Get(m_iPos);
				if (e == "n")
					value += "\n";
				else if (e == "t")
					value += "\t";
				else if (e == "r")
					value += "\r";
				else if (e == "u")
				{
					value += "?";	// \uXXXX is not needed for keys and paths
					m_iPos += 4;
				}
				else
					value += e;	// \" \\ \/ and the rest

				m_iPos++;
				continue;
			}

			value += c;
			m_iPos++;
		}

		Fail("unterminated string");
		return "";
	}

	protected CLW_JsonNode ParseObject(int depth)
	{
		CLW_JsonNode node = new CLW_JsonNode();
		node.m_iType = CLW_JsonNode.T_OBJECT;
		m_iPos++;
		while (true)
		{
			SkipSpace();
			if (m_iPos >= m_iLen)
				return Fail("unterminated object");

			string c = m_sText.Get(m_iPos);
			if (c == "}")
			{
				m_iPos++;
				return node;
			}

			if (c != "\"")
				return Fail("object key expected");

			string key = ParseString();
			if (!m_sError.IsEmpty())
				return null;

			SkipSpace();
			if (m_iPos >= m_iLen || m_sText.Get(m_iPos) != ":")
				return Fail("':' expected");

			m_iPos++;
			CLW_JsonNode value = ParseValue(depth + 1);
			if (!value)
				return null;

			node.m_aKeys.Insert(key);
			node.m_aValues.Insert(value);
			SkipSpace();
			if (m_iPos < m_iLen && m_sText.Get(m_iPos) == ",")
				m_iPos++;
		}

		return null;
	}

	protected CLW_JsonNode ParseArray(int depth)
	{
		CLW_JsonNode node = new CLW_JsonNode();
		node.m_iType = CLW_JsonNode.T_ARRAY;
		m_iPos++;
		while (true)
		{
			SkipSpace();
			if (m_iPos >= m_iLen)
				return Fail("unterminated array");

			if (m_sText.Get(m_iPos) == "]")
			{
				m_iPos++;
				return node;
			}

			CLW_JsonNode value = ParseValue(depth + 1);
			if (!value)
				return null;

			node.m_aValues.Insert(value);
			SkipSpace();
			if (m_iPos < m_iLen && m_sText.Get(m_iPos) == ",")
				m_iPos++;
		}

		return null;
	}
}

// ---- admin rules of one side (overrides.json "sides") ----
class CLW_SideRules
{
	bool m_bOwnOnly;
	bool m_bKinAllInfantry;						// kinFill all also imports foot entries for roles the side has
	string m_sKinFill;							// "" = not set (inherit), "missing" (default) | "all"
	float m_fKinWeight = -1;					// < 0 = not set (inherit), default 0.5 (CLW_SideRules.DEFAULT_KIN_WEIGHT)
	ref array<string> m_aKin = {};				// explicit kin faction keys (tried first)
	ref array<string> m_aExcludeFactions = {};	// never kin
	ref array<string> m_aExcludePaths = {};		// lower case; prefab path texts never used (own and kin entries)

	static const string KIN_MISSING = "missing";
	static const string KIN_ALL = "all";
	static const float DEFAULT_KIN_WEIGHT = 0.5;

	// kinFill all: kin entries are also imported for roles the own faction already has.
	bool KinAll()
	{
		return m_sKinFill == KIN_ALL;
	}

	float KinWeight()
	{
		if (m_fKinWeight < 0)
			return DEFAULT_KIN_WEIGHT;

		return m_fKinWeight;
	}

	// Reads kinFill / kinWeight of a rules object (a "sides" entry or "sidesDefault"); bad values are logged and ignored.
	void ReadKinFill(CLW_JsonNode body, string where)
	{
		string fill = body.FieldText("kinFill");
		fill.ToLower();
		if (fill == KIN_MISSING || fill == KIN_ALL)
			m_sKinFill = fill;
		else if (!fill.IsEmpty())
			Print("CLW: ERROR overrides " + where + ": kinFill must be \"missing\" or \"all\" (got '" + fill + "'), ignored", LogLevel.ERROR);

		if (body.FieldBool("kinAllInfantry", false))
			m_bKinAllInfantry = true;

		string weight = body.FieldText("kinWeight");
		if (!weight.IsEmpty())
		{
			float parsed = weight.ToFloat();
			if (parsed > 0)
				m_fKinWeight = parsed;
			else
				Print("CLW: ERROR overrides " + where + ": kinWeight must be a number > 0 (got '" + weight + "'), ignored", LogLevel.ERROR);
		}
	}

	bool ExcludesFaction(string key)
	{
		return m_aExcludeFactions.Contains(key);
	}

	// True when one of the entry's prefab paths contains an excluded text. 'which' tells the text (log).
	bool BlocksEntry(CLW_ForceEntry entry, out string which)
	{
		which = "";
		if (!entry || m_aExcludePaths.IsEmpty())
			return false;

		string vehicle = entry.m_VehiclePrefab;
		string group = entry.m_GroupPrefab;
		string cargo = entry.m_CargoGroupPrefab;
		vehicle.ToLower();
		group.ToLower();
		cargo.ToLower();
		foreach (string text : m_aExcludePaths)
		{
			if (text.IsEmpty())
				continue;

			if (vehicle.Contains(text) || group.Contains(text) || cargo.Contains(text))
			{
				which = text;
				return true;
			}
		}

		return false;
	}

	void Merge(CLW_SideRules other)
	{
		if (!other)
			return;

		if (other.m_bOwnOnly)
			m_bOwnOnly = true;

		if (other.m_bKinAllInfantry)
			m_bKinAllInfantry = true;
		if (!other.m_sKinFill.IsEmpty())
			m_sKinFill = other.m_sKinFill;
		if (other.m_fKinWeight >= 0)
			m_fKinWeight = other.m_fKinWeight;

		m_aKin.InsertAll(other.m_aKin);
		m_aExcludeFactions.InsertAll(other.m_aExcludeFactions);
		m_aExcludePaths.InsertAll(other.m_aExcludePaths);
	}
}

// ---- the admin keys of overrides.json: "sides" and "props" (read once per game start) ----
class CLW_AdminOverrides
{
	static const string ALL = "*";
	static const string KEY_STYLE = "style";

	protected static bool s_bLoaded;
	protected static ref map<string, ref CLW_SideRules> s_mSides = new map<string, ref CLW_SideRules>();
	protected static ref CLW_SideRules s_SidesDefault;	// "sidesDefault": kinFill / kinWeight for every side without its own value
	protected static ref map<string, ref map<string, string>> s_mProps = new map<string, ref map<string, string>>();

	// Game start: statics survive a Play restart, so the file is read again for the next session.
	static void Reset()
	{
		s_bLoaded = false;
		s_SidesDefault = null;
		s_mSides.Clear();
		s_mProps.Clear();
	}

	protected static void EnsureLoaded()
	{
		if (s_bLoaded)
			return;

		s_bLoaded = true;
		s_SidesDefault = null;
		s_mSides.Clear();
		s_mProps.Clear();
		if (!FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
			return;

		string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
		if (text.IsEmpty())
			return;

		string error;
		CLW_JsonNode root = CLW_JsonReader.Parse(text, error);
		if (!root)
		{
			Print("CLW: ERROR overrides sides/props not read, " + CLW_CatalogOverrides.FILE_PATH + " is not valid JSON (" + error + ")", LogLevel.ERROR);
			return;
		}

		CLW_JsonNode sidesDefault = root.Field("sidesDefault");
		if (sidesDefault)
		{
			if (sidesDefault.IsObject())
			{
				s_SidesDefault = new CLW_SideRules();
				s_SidesDefault.ReadKinFill(sidesDefault, "sidesDefault");
				Print(string.Format("CLW: overrides sidesDefault: kinFill=%1 kinWeight=%2", s_SidesDefault.m_sKinFill, s_SidesDefault.m_fKinWeight), LogLevel.NORMAL);
			}
			else
			{
				Print("CLW: ERROR overrides sidesDefault must be an object {\"kinFill\":\"all\"}, ignored", LogLevel.ERROR);
			}
		}

		LoadSides(root.Field("sides"));
		LoadProps(root.Field("props"));
	}

	protected static void LoadSides(CLW_JsonNode node)
	{
		if (!node)
			return;

		if (!node.IsObject())
		{
			Print("CLW: ERROR overrides sides must be an object {\"<faction key>\":{...}}, ignored", LogLevel.ERROR);
			return;
		}

		int i = 0;
		while (i < node.m_aKeys.Count())
		{
			string key = node.m_aKeys[i];
			CLW_JsonNode body = node.m_aValues[i];
			i++;
			if (!body || !body.IsObject())
			{
				Print("CLW: ERROR overrides sides '" + key + "' must be an object, ignored", LogLevel.ERROR);
				continue;
			}

			CLW_SideRules rules = new CLW_SideRules();
			rules.m_bOwnOnly = body.FieldBool("ownOnly", false);
			rules.ReadKinFill(body, "sides '" + key + "'");
			body.FieldStrings("kin", rules.m_aKin);
			body.FieldStrings("excludeFactions", rules.m_aExcludeFactions);
			body.FieldStrings("excludePathContains", rules.m_aExcludePaths);
			int j = 0;
			while (j < rules.m_aExcludePaths.Count())
			{
				string lower = rules.m_aExcludePaths[j];
				lower.ToLower();
				rules.m_aExcludePaths.Set(j, lower);
				j++;
			}

			j = 0;
			while (j < body.m_aKeys.Count())
			{
				string field = body.m_aKeys[j];
				if (field != "ownOnly" && field != "kin" && field != "excludeFactions" && field != "excludePathContains" && field != "kinFill" && field != "kinWeight" && field != "kinAllInfantry")
					Print("CLW: overrides sides '" + key + "': unknown key '" + field + "' ignored (known: ownOnly, kin, kinFill, kinWeight, kinAllInfantry, excludeFactions, excludePathContains)", LogLevel.WARNING);

				j++;
			}

			s_mSides.Set(key, rules);
			Print(string.Format("CLW: overrides sides '%1': ownOnly=%2 kin=%3 excludeFactions=%4 excludePathContains=%5 kinFill='%6' kinWeight=%7", key, rules.m_bOwnOnly, rules.m_aKin.Count(), rules.m_aExcludeFactions.Count(), rules.m_aExcludePaths.Count(), rules.m_sKinFill, rules.m_fKinWeight), LogLevel.NORMAL);
		}
	}

	protected static void LoadProps(CLW_JsonNode node)
	{
		if (!node)
			return;

		if (!node.IsObject())
		{
			Print("CLW: ERROR overrides props must be an object {\"<faction key>\":{\"<role>\":\"<prefab>\"}}, ignored", LogLevel.ERROR);
			return;
		}

		int i = 0;
		while (i < node.m_aKeys.Count())
		{
			string key = node.m_aKeys[i];
			CLW_JsonNode body = node.m_aValues[i];
			i++;
			if (!body || !body.IsObject())
			{
				Print("CLW: ERROR overrides props '" + key + "' must be an object, ignored", LogLevel.ERROR);
				continue;
			}

			map<string, string> roles = new map<string, string>();
			int j = 0;
			while (j < body.m_aKeys.Count())
			{
				CLW_JsonNode value = body.m_aValues[j];
				string role = body.m_aKeys[j];
				j++;
				if (!value || !value.IsString() || value.m_sText.IsEmpty())
				{
					Print("CLW: ERROR overrides props '" + key + "' role '" + role + "' must be a non-empty string, ignored", LogLevel.ERROR);
					continue;
				}

				roles.Set(role, value.m_sText);
				Print("CLW: overrides props '" + key + "' " + role + " -> " + value.m_sText, LogLevel.NORMAL);
			}

			s_mProps.Set(key, roles);
		}
	}

	// The rules of a side: the "*" entry merged with the entry of its faction key. Never null.
	static CLW_SideRules GetSideRules(string sideKey)
	{
		EnsureLoaded();
		CLW_SideRules merged = new CLW_SideRules();
		merged.Merge(s_SidesDefault);
		CLW_SideRules common;
		if (s_mSides.Find(ALL, common))
			merged.Merge(common);

		CLW_SideRules own;
		if (s_mSides.Find(sideKey, own))
			merged.Merge(own);

		return merged;
	}

	// Prop override of a role for a side: the side's entry, else the "*" entry. "" = none. Value: prefab path, "vanilla" or a set name.
	static string GetProp(string sideKey, string role)
	{
		EnsureLoaded();
		map<string, string> roles;
		string value;
		if (s_mProps.Find(sideKey, roles) && roles.Find(role, value))
			return value;

		if (s_mProps.Find(ALL, roles) && roles.Find(role, value))
			return value;

		return "";
	}

	// Style set name for all roles of a side that have no explicit override. "" = none.
	static string GetStyle(string sideKey)
	{
		return GetProp(sideKey, KEY_STYLE);
	}
}

// ---- side pool rules (pure helpers; CLW_ForceCatalog does the building) ----
class CLW_SidePool
{
	// The kinds a complete buy catalog covers. A side without an enabled entry of a kind has a role gap the kin may fill.
	static void AllKinds(notnull array<string> kinds)
	{
		kinds.Insert(CLW_ForceKind.INFANTRY);
		kinds.Insert(CLW_ForceKind.RECON);
		kinds.Insert(CLW_ForceKind.MOTORIZED);
		kinds.Insert(CLW_ForceKind.LIGHT);
		kinds.Insert(CLW_ForceKind.MECH);
		kinds.Insert(CLW_ForceKind.ARMOR);
		kinds.Insert(CLW_ForceKind.ARMORED_RECON);
		kinds.Insert(CLW_ForceKind.LOGISTICS);
	}

	// Kinds with no usable entry: no enabled entry; a logistics entry needs a vehicle and a cargo amount (CLW_Convoys.PickTruckEntry),
	// a vehicle kind must not be crewless.
	static void MissingKinds(notnull array<ref CLW_ForceEntry> entries, notnull array<string> missing)
	{
		array<string> kinds = {};
		AllKinds(kinds);
		foreach (string kind : kinds)
		{
			bool have = false;
			foreach (CLW_ForceEntry entry : entries)
			{
				if (!entry.m_bEnabled || entry.m_sKind != kind || entry.m_bCrewless)
					continue;

				if (kind == CLW_ForceKind.LOGISTICS && (!entry.HasVehicle() || entry.m_iCargo <= 0))
					continue;

				have = true;
				break;
			}

			if (!have)
				missing.Insert(kind);
		}
	}

	static string BaseOf(ResourceName prefab)
	{
		string text = CLW_ForceCatalog.PathOf(prefab);
		int slash = -1;
		int i = 0;
		while (i < text.Length())
		{
			if (text.Get(i) == "/")
				slash = i;

			i++;
		}

		text = text.Substring(slash + 1, text.Length() - slash - 1);
		int dot = text.IndexOf(".");
		if (dot > 0)
			text = text.Substring(0, dot);

		return text;
	}

	// Same role + same vehicle model (file name of the vehicle prefab) or same group prefab as an entry already in the list.
	static bool IsDuplicate(notnull array<ref CLW_ForceEntry> entries, CLW_ForceEntry candidate)
	{
		foreach (CLW_ForceEntry entry : entries)
		{
			if (entry.m_sKind != candidate.m_sKind)
				continue;

			if (candidate.HasVehicle() && entry.HasVehicle() && BaseOf(entry.m_VehiclePrefab) == BaseOf(candidate.m_VehiclePrefab))
				return true;

			if (!candidate.HasVehicle() && !entry.HasVehicle() && !candidate.m_GroupPrefab.IsEmpty() && CLW_ForceCatalog.PathOf(entry.m_GroupPrefab) == CLW_ForceCatalog.PathOf(candidate.m_GroupPrefab))
				return true;
		}

		return false;
	}

	// Kin faction keys of a side in priority order: admin kin (overrides.json), profile kin, then the friendly factions of the faction
	// data. Never the side itself, the enemy side, an excluded faction; the automatic ones also not a faction friendly to the enemy side.
	static void GetKinKeys(string sideKey, CLW_FactionProfile profile, notnull CLW_SideRules rules, notnull array<string> kinKeys)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (!manager || rules.m_bOwnOnly)
			return;

		string enemyKey;
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario && scenario.IsFaction(sideKey))
			enemyKey = scenario.GetEnemyKey(sideKey);

		SCR_Faction enemy;
		if (!enemyKey.IsEmpty())
			enemy = SCR_Faction.Cast(manager.GetFactionByKey(enemyKey));

		array<string> named = {};
		named.InsertAll(rules.m_aKin);
		if (profile && profile.m_aKinFactions)
			named.InsertAll(profile.m_aKinFactions);

		foreach (string key : named)
		{
			string why = RejectKin(manager, sideKey, enemyKey, key, rules, kinKeys);
			if (why.IsEmpty())
				kinKeys.Insert(key);
			else
				Print(string.Format("CLW: sidepool %1: explicit kin %2 not used (%3)", sideKey, key, why), LogLevel.NORMAL);
		}

		SCR_Faction faction = SCR_Faction.Cast(manager.GetFactionByKey(sideKey));
		if (!faction)
			return;

		array<SCR_Faction> friends = {};
		CLW_ArsenalPool.GetKin(faction, friends);
		foreach (SCR_Faction ally : friends)
		{
			string friendKey = ally.GetFactionKey();
			string reason = RejectKin(manager, sideKey, enemyKey, friendKey, rules, kinKeys);
			if (reason.IsEmpty() && enemy && (enemy.DoCheckIfFactionFriendly(ally) || ally.DoCheckIfFactionFriendly(enemy)))
				reason = "friendly to the enemy side " + enemyKey;

			if (reason.IsEmpty())
				kinKeys.Insert(friendKey);
			else
				Print(string.Format("CLW: sidepool %1: friendly faction %2 not used (%3)", sideKey, friendKey, reason), LogLevel.NORMAL);
		}
	}

	protected static string RejectKin(FactionManager manager, string sideKey, string enemyKey, string key, CLW_SideRules rules, notnull array<string> chosen)
	{
		if (key.IsEmpty() || key == sideKey)
			return "the side itself";
		if (key == enemyKey)
			return "the enemy side";
		if (chosen.Contains(key))
			return "listed twice";
		if (rules.ExcludesFaction(key))
			return "excluded in overrides.json";
		if (!manager.GetFactionByKey(key))
			return "no such faction";

		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario && scenario.IsIgnoredFaction(key))
			return "on the scenario's ignore list (civilians etc.)";

		return "";
	}
}

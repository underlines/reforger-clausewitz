// Catalog overrides: fixes to the classification / cost / availability of force entries WITHOUT editing packed .conf files.
// Two sources, applied after profile + discovery in this order (the later wins, so the server owner has the last word):
//   1. the scenario config: optional m_aOverrides (CLW_CatalogOverride), so compat addons can ship fixes;
//   2. the server file  $profile:Clausewitz/overrides.json  (read at every catalog build; an example file is written when missing).
//
// File schema (all fields except "match" are optional; a missing field leaves the entry unchanged):
//   {"entries":[{"match":"<text>","faction":"US","kind":"light","cost":90,"weight":10,"enabled":true,"name":"M1025 M2","antiArmor":false}]}
//   match    : case-insensitive substring of the vehicle / group / cargo-group prefab path, OR the exact type id,
//              OR a substring of the display name / vehicle name. Every matching entry is changed.
//   faction  : only entries of this faction key (empty / missing = all factions)
//   kind     : infantry, recon, motorized, light, mech, armor, armored_recon
//   cost     : supplies cost (>= 0)       weight : buy-mix weight (>= 0)
//   enabled  : false hides the entry from the buy mix      antiArmor : marks the entry as an anti-armour team
//   name     : display name (also the vehicle name for vehicle entries)
// Details: dev/notes/overrides.md

// One override in the scenario config. Tri-state numbers use -1 = unchanged; no field initializers on [Attribute] members.
[BaseContainerProps()]
class CLW_CatalogOverride
{
	[Attribute("", UIWidgets.EditBox, "Prefab path substring, exact type id or display name substring")]
	string m_sMatch;

	[Attribute("", UIWidgets.EditBox, "Only this faction key (empty = all)")]
	string m_sFaction;

	[Attribute("", UIWidgets.EditBox, "New kind (empty = unchanged): infantry, recon, motorized, light, mech, armor, armored_recon")]
	string m_sKind;

	[Attribute("-1", UIWidgets.EditBox, "New cost (-1 = unchanged)")]
	int m_iCost;

	[Attribute("-1", UIWidgets.EditBox, "New buy-mix weight (-1 = unchanged)")]
	int m_iWeight;

	[Attribute("-1", UIWidgets.EditBox, "Enabled: -1 unchanged, 0 disable, 1 enable")]
	int m_iEnabled;

	[Attribute("", UIWidgets.EditBox, "New display name (empty = unchanged)")]
	string m_sName;

	[Attribute("-1", UIWidgets.EditBox, "Anti-armour team: -1 unchanged, 0 no, 1 yes")]
	int m_iAntiArmor;

	[Attribute("", UIWidgets.EditBox, "Where the override came from (log only)")]
	string m_sSource;
}

// ---- JSON file DTOs. Read twice with different field defaults (A / B) to tell "absent" from "explicitly set". ----
class CLW_OverrideDtoA
{
	string match;
	string faction;
	string kind;
	string name;
	int cost;
	int weight;
	bool enabled;
	bool antiArmor;

	void CLW_OverrideDtoA()
	{
		cost = -1;
		weight = -1;
		enabled = true;
		antiArmor = true;
	}
}

class CLW_OverrideDtoB : CLW_OverrideDtoA
{
	void CLW_OverrideDtoB()
	{
		cost = -2;
		weight = -2;
		enabled = false;
		antiArmor = false;
	}
}

class CLW_OverridesFileA
{
	ref array<ref CLW_OverrideDtoA> entries = {};
}

class CLW_OverridesFileB
{
	ref array<ref CLW_OverrideDtoB> entries = {};
}

class CLW_CatalogOverrides
{
	static const string FILE_DIR = "$profile:Clausewitz";
	static const string FILE_PATH = "$profile:Clausewitz/overrides.json";

	protected static ref array<ref CLW_CatalogOverride> s_aFile;	// cached parse of the file (null = not read yet)

	// Apply scenario + file overrides to a freshly built catalog list. Returns the number of changed entries.
	static int Apply(string factionKey, notnull array<ref CLW_ForceEntry> entries)
	{
		int applied;
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario && scenario.m_aOverrides)
		{
			foreach (CLW_CatalogOverride fromScenario : scenario.m_aOverrides)
			{
				applied += ApplyOne(factionKey, entries, fromScenario, "scenario");
			}
		}

		array<ref CLW_CatalogOverride> fromFile = LoadFile();
		foreach (CLW_CatalogOverride fileOverride : fromFile)
		{
			applied += ApplyOne(factionKey, entries, fileOverride, "file");
		}

		if (applied > 0)
			Print(string.Format("CLW: overrides: %1 applied to catalog %2", applied, factionKey), LogLevel.NORMAL);

		return applied;
	}

	protected static string Lower(string text)
	{
		string copy = text;
		copy.ToLower();	// in place
		return copy;
	}

	protected static bool Matches(CLW_ForceEntry entry, string needleLower)
	{
		if (Lower(entry.m_sType) == needleLower)
			return true;

		string path = entry.m_VehiclePrefab;
		if (Lower(path).IndexOf(needleLower) >= 0)
			return true;

		path = entry.m_GroupPrefab;
		if (Lower(path).IndexOf(needleLower) >= 0)
			return true;

		path = entry.m_CargoGroupPrefab;
		if (Lower(path).IndexOf(needleLower) >= 0)
			return true;

		if (Lower(entry.m_sName).IndexOf(needleLower) >= 0)
			return true;

		return Lower(entry.m_sVehicleName).IndexOf(needleLower) >= 0;
	}

	protected static int ApplyOne(string factionKey, notnull array<ref CLW_ForceEntry> entries, CLW_CatalogOverride rule, string source)
	{
		if (!rule || rule.m_sMatch.IsEmpty())
			return 0;

		if (!rule.m_sFaction.IsEmpty() && rule.m_sFaction != factionKey)
			return 0;

		string needle = Lower(rule.m_sMatch);
		int changed;
		foreach (CLW_ForceEntry entry : entries)
		{
			if (!entry || !Matches(entry, needle))
				continue;

			string what = "";
			if (!rule.m_sKind.IsEmpty())
			{
				if (CLW_ForceKind.IsKnown(rule.m_sKind))
				{
					what += " kind " + entry.m_sKind + "->" + rule.m_sKind;
					entry.m_sKind = rule.m_sKind;
				}
				else
				{
					Print("CLW: overrides: unknown kind '" + rule.m_sKind + "' in override '" + rule.m_sMatch + "' ignored", LogLevel.WARNING);
				}
			}

			if (rule.m_iCost >= 0)
			{
				what += " cost " + entry.m_iCost.ToString() + "->" + rule.m_iCost.ToString();
				entry.m_iCost = rule.m_iCost;
				entry.m_iVehicleCost = 0;	// derive the vehicle share again
			}

			if (rule.m_iWeight >= 0)
			{
				what += " weight " + entry.m_iWeight.ToString() + "->" + rule.m_iWeight.ToString();
				entry.m_iWeight = rule.m_iWeight;
			}

			if (rule.m_iEnabled >= 0)
			{
				entry.m_bEnabled = rule.m_iEnabled > 0;
				what += string.Format(" enabled=%1", entry.m_bEnabled);
			}

			if (!rule.m_sName.IsEmpty())
			{
				what += " name '" + entry.m_sName + "'->'" + rule.m_sName + "'";
				entry.m_sName = rule.m_sName;
				if (entry.HasVehicle())
					entry.m_sVehicleName = rule.m_sName;
			}

			if (rule.m_iAntiArmor >= 0)
			{
				entry.m_bAntiArmor = rule.m_iAntiArmor > 0;
				what += string.Format(" antiArmor=%1", entry.m_bAntiArmor);
			}

			if (what.IsEmpty())
				continue;

			changed++;
			Print("CLW: overrides: [" + source + "] match='" + rule.m_sMatch + "' faction=" + factionKey + " entry " + entry.m_sType + ":" + what, LogLevel.NORMAL);
		}

		return changed;
	}

	// ---- the server file ----

	protected static array<ref CLW_CatalogOverride> LoadFile()
	{
		if (s_aFile)
			return s_aFile;

		s_aFile = {};
		if (!FileIO.FileExists(FILE_PATH))
		{
			WriteExample();
			return s_aFile;
		}

		string text = SCR_FileIOHelper.GetFileStringContent(FILE_PATH, false);
		if (text.IsEmpty())
		{
			Print("CLW: overrides: " + FILE_PATH + " is empty, ignored", LogLevel.WARNING);
			return s_aFile;
		}

		SCR_JsonLoadContext ctxA = new SCR_JsonLoadContext();
		SCR_JsonLoadContext ctxB = new SCR_JsonLoadContext();
		CLW_OverridesFileA fileA = new CLW_OverridesFileA();
		CLW_OverridesFileB fileB = new CLW_OverridesFileB();
		if (!ctxA.ImportFromString(text) || !ctxB.ImportFromString(text) || !ctxA.ReadValue("", fileA) || !ctxB.ReadValue("", fileB))	// [verify] ReadValue("", dto) fills ref arrays
		{
			Print("CLW: overrides: " + FILE_PATH + " is not valid JSON of the expected shape, ignored", LogLevel.ERROR);
			return s_aFile;
		}

		int count = Math.Min(fileA.entries.Count(), fileB.entries.Count());
		for (int i = 0; i < count; i++)
		{
			CLW_OverrideDtoA a = fileA.entries[i];
			CLW_OverrideDtoB b = fileB.entries[i];
			if (!a || !b)
				continue;

			CLW_CatalogOverride rule = new CLW_CatalogOverride();
			rule.m_sMatch = a.match;
			rule.m_sFaction = a.faction;
			rule.m_sKind = a.kind;
			rule.m_sName = a.name;
			rule.m_sSource = "file";
			rule.m_iCost = -1;
			rule.m_iWeight = -1;
			rule.m_iEnabled = -1;
			rule.m_iAntiArmor = -1;
			// same value under both defaults = explicitly present
			if (a.cost == b.cost)
				rule.m_iCost = a.cost;
			if (a.weight == b.weight)
				rule.m_iWeight = a.weight;
			if (a.enabled == b.enabled)
			{
				rule.m_iEnabled = 0;
				if (a.enabled)
					rule.m_iEnabled = 1;
			}

			if (a.antiArmor == b.antiArmor)
			{
				rule.m_iAntiArmor = 0;
				if (a.antiArmor)
					rule.m_iAntiArmor = 1;
			}

			s_aFile.Insert(rule);
		}

		Print(string.Format("CLW: overrides: read %1 entries from %2", s_aFile.Count(), FILE_PATH), LogLevel.NORMAL);
		return s_aFile;
	}

	protected static void WriteExample()
	{
		FileIO.MakeDirectory(FILE_DIR);
		FileHandle handle = FileIO.OpenFile(FILE_PATH, FileMode.WRITE);
		if (!handle)
		{
			Print("CLW: overrides: cannot create the example file " + FILE_PATH, LogLevel.WARNING);
			return;
		}

		handle.WriteLine("{\"entries\":[");
		handle.WriteLine("{\"match\":\"EXAMPLE_ONLY\",\"faction\":\"\",\"kind\":\"light\",\"cost\":90,\"weight\":10,\"enabled\":false,\"name\":\"M1025 M2\",\"antiArmor\":false}");
		handle.WriteLine("]}");
		handle.Close();
		Print("CLW: overrides: no file found, wrote an example (matches nothing) to " + FILE_PATH + " - edit it to fix the catalog, see dev/notes/overrides.md in the mod repo", LogLevel.NORMAL);
	}
}

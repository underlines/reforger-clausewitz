// Role picker (loadouts-arsenal-plan R2): per faction and ROLE_* label ONE official CHARACTER catalog prefab, so the 7 spawn slots (R3) and the
// AI squad members (R4, CLW_SquadRoles) can be "a rifleman / medic / AT soldier of this faction" without any configuration. Kin factions (friendly
// lists, CLW_ArsenalPool.GetKin) fill the roles the faction itself does not have (pack factions that only ship a few characters).
// Pick rules per role: skip ROLE_GUARD and vehicle / helicopter crew characters; prefer a character that carries only that one role, then SIZE_M,
// then the shorter prefab file name (the plain variant before its camo / Conflict copies), then catalog order. Only catalog data goes in, so the
// answer is the same on the server and on every client. Results are cached per faction (an empty result is not cached: catalogs may fill late).
// No faction keys, unit names or prefab GUIDs in code. Log: "CLW: squad roles picker <faction> <ROLE>=<prefab> ..." (once per faction and machine).

// Result for one faction.
class CLW_FactionRoles
{
	ref array<string> m_aRoles = {};						// roles that have a prefab, in the default slot order
	ref map<string, ResourceName> m_mPrefab = new map<string, ResourceName>();
}

class CLW_RolePicker
{
	protected static ref map<string, ref CLW_FactionRoles> s_mCache = new map<string, ref CLW_FactionRoles>();
	protected static ref array<string> s_aOrder;

	// Every role the picker knows, in the default slot order (leader, rifleman, machinegunner, antitank, grenadier, medic, sharpshooter, then the rest).
	// ROLE_GUARD is never offered.
	static void AllRoles(notnull array<string> roles)
	{
		if (!s_aOrder)
		{
			s_aOrder = {};
			s_aOrder.Insert("ROLE_LEADER");
			s_aOrder.Insert("ROLE_RIFLEMAN");
			s_aOrder.Insert("ROLE_MACHINEGUNNER");
			s_aOrder.Insert("ROLE_ANTITANK");
			s_aOrder.Insert("ROLE_GRENADIER");
			s_aOrder.Insert("ROLE_MEDIC");
			s_aOrder.Insert("ROLE_SHARPSHOOTER");
			s_aOrder.Insert("ROLE_RADIOOPERATOR");
			s_aOrder.Insert("ROLE_SCOUT");
			s_aOrder.Insert("ROLE_AMMOBEARER");
			s_aOrder.Insert("ROLE_SAPPER");
		}

		foreach (string role : s_aOrder)
		{
			roles.Insert(role);
		}
	}

	// Name of the ROLE_* label of a role string ("ROLE_MEDIC"), -1 when it is not a label.
	static int LabelOf(string role)
	{
		return typename.StringToEnum(EEditableEntityLabel, role);
	}

	// The first known ROLE_* label among the labels ("" = none). Order = the default slot order, so a character with several roles counts as the
	// first of them.
	static string RoleOfLabels(notnull array<EEditableEntityLabel> labels)
	{
		array<string> all = {};
		AllRoles(all);
		foreach (string role : all)
		{
			if (labels.Contains(LabelOf(role)))
				return role;
		}

		return "";
	}

	// ROLE_* label of a character prefab ("" = none). Works for any character prefab, catalogued or not.
	static string RoleOfPrefab(ResourceName prefab)
	{
		if (prefab.IsEmpty())
			return "";

		SCR_EditableEntityUIInfo info = SCR_EditableEntityUIInfo.ExtractEditableUIInfoFromPrefab(prefab);
		if (!info)
			return "";

		array<EEditableEntityLabel> labels = {};
		info.GetEntityLabels(labels);
		return RoleOfLabels(labels);
	}

	// One CHARACTER catalog prefab per ROLE_* for the faction (kin factions fill missing roles); "" if none.
	static ResourceName RolePrefab(string factionKey, string role)
	{
		CLW_FactionRoles data = Get(factionKey);
		if (!data)
			return "";

		ResourceName prefab;
		if (data.m_mPrefab.Find(role, prefab))
			return prefab;

		return "";
	}

	// Roles the faction can field, in the default slot order (leader, rifleman, machinegunner, antitank, grenadier, medic, sharpshooter, ...).
	static void Roles(string factionKey, notnull array<string> roles)
	{
		CLW_FactionRoles data = Get(factionKey);
		if (!data)
			return;

		foreach (string role : data.m_aRoles)
		{
			roles.Insert(role);
		}
	}

	// Display name of the role prefab (UI info) for spawn-slot names; the plain label text when the faction has no such prefab.
	static string RoleName(string factionKey, string role)
	{
		ResourceName prefab = RolePrefab(factionKey, role);
		if (!prefab.IsEmpty())
		{
			SCR_EditableEntityUIInfo info = SCR_EditableEntityUIInfo.ExtractEditableUIInfoFromPrefab(prefab);
			if (info)
			{
				string name = info.GetName();
				if (!name.IsEmpty())
				{
					string translated = WidgetManager.Translate(name);
					if (!translated.IsEmpty() && translated.IndexOf("#") != 0)
						return translated;
				}
			}
		}

		string plain = role;
		plain.Replace("ROLE_", "");
		plain.ToLower();
		return plain;
	}

	// Forget the cache (new war / faction change).
	static void Reset()
	{
		s_mCache.Clear();
	}

	// ---- build ----

	protected static CLW_FactionRoles Get(string factionKey)
	{
		CLW_FactionRoles cached;
		if (s_mCache.Find(factionKey, cached))
			return cached;

		FactionManager manager = GetGame().GetFactionManager();
		if (!manager || factionKey.IsEmpty())
			return null;

		SCR_Faction faction = SCR_Faction.Cast(manager.GetFactionByKey(factionKey));
		if (!faction)
			return null;

		CLW_FactionRoles data = new CLW_FactionRoles();
		map<string, int> scores = new map<string, int>();
		Scan(faction, data, scores, false);

		// kin factions fill the roles the faction does not have
		array<SCR_Faction> kin = {};
		CLW_ArsenalPool.GetKin(faction, kin);
		foreach (SCR_Faction kinFaction : kin)
		{
			Scan(kinFaction, data, scores, true);
		}

		array<string> all = {};
		AllRoles(all);
		string line;
		foreach (string role : all)
		{
			ResourceName picked;
			if (!data.m_mPrefab.Find(role, picked))
				continue;

			data.m_aRoles.Insert(role);
			line += " " + role + "=" + ShortName(picked);
		}

		if (data.m_aRoles.IsEmpty())
			return data;	// catalogs may not be filled yet: not cached, the next call scans again

		s_mCache.Set(factionKey, data);
		Print("CLW: squad roles picker " + factionKey + line, LogLevel.NORMAL);
		return data;
	}

	// Score of a candidate: single-role character first (+2), SIZE_M (+1).
	protected static int ScoreOf(notnull array<EEditableEntityLabel> labels, int roleCount)
	{
		int score;
		if (roleCount == 1)
			score += 2;

		if (labels.Contains(EEditableEntityLabel.SIZE_M))
			score += 1;

		return score;
	}

	// Walk the CHARACTER catalog of one faction. fillOnly = a kin faction: it only provides roles the result does not have yet.
	protected static void Scan(notnull SCR_Faction faction, notnull CLW_FactionRoles data, notnull map<string, int> scores, bool fillOnly)
	{
		SCR_EntityCatalog catalog = faction.GetFactionEntityCatalogOfType(EEntityCatalogType.CHARACTER, false);
		if (!catalog)
			return;

		array<SCR_EntityCatalogEntry> list = {};
		catalog.GetEntityList(list);
		array<string> all = {};
		AllRoles(all);

		// candidates of this faction only: a faction's own pick never loses to a kin faction's, whatever the scores
		map<string, int> local = new map<string, int>();
		foreach (SCR_EntityCatalogEntry entry : list)
		{
			if (!entry || !entry.IsEnabled())
				continue;

			ResourceName prefab = entry.GetPrefab();
			if (prefab.IsEmpty())
				continue;

			array<EEditableEntityLabel> labels = {};
			entry.GetEditableEntityLabels(labels);
			if (labels.Contains(EEditableEntityLabel.ROLE_GUARD) || labels.Contains(EEditableEntityLabel.TRAIT_GUARD))
				continue;

			if (labels.Contains(EEditableEntityLabel.TRAIT_VEHICLE_CREW) || labels.Contains(EEditableEntityLabel.TRAIT_HELI_CREW))
				continue;

			array<string> roles = {};
			foreach (string role : all)
			{
				if (labels.Contains(LabelOf(role)))
					roles.Insert(role);
			}

			int score = ScoreOf(labels, roles.Count());
			foreach (string candidateRole : roles)
			{
				if (fillOnly && data.m_mPrefab.Contains(candidateRole) && !local.Contains(candidateRole))
					continue;	// the faction itself (or an earlier kin faction) already has this role

				ResourceName current;
				int currentScore = -1;
				bool has = data.m_mPrefab.Find(candidateRole, current);
				if (has)
					scores.Find(candidateRole, currentScore);

				if (!has || IsBetter(score, prefab, currentScore, current))
				{
					data.m_mPrefab.Set(candidateRole, prefab);
					scores.Set(candidateRole, score);
					local.Set(candidateRole, 1);
				}
			}
		}
	}

	protected static bool IsBetter(int score, ResourceName prefab, int currentScore, ResourceName current)
	{
		if (score != currentScore)
			return score > currentScore;

		// plain variant before camo / Conflict copies: shorter file name wins; a full tie keeps the earlier catalog entry (deterministic)
		return ShortName(prefab).Length() < ShortName(current).Length();
	}

	// "{GUID}Prefabs/Characters/Factions/US/Character_US_Rifleman.et" -> "Character_US_Rifleman.et"
	static string ShortName(string prefab)
	{
		string text = prefab;
		int slash = -1;
		for (int i = 0; i < text.Length(); i++)
		{
			if (text.Get(i) == "/")
				slash = i;
		}

		if (slash < 0)
		{
			int close = text.IndexOf("}");
			if (close >= 0)
				return text.Substring(close + 1, text.Length() - close - 1);

			return text;
		}

		return text.Substring(slash + 1, text.Length() - slash - 1);
	}
}

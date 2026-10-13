// Role picker (loadouts-arsenal-plan R2): per faction and ROLE_* label ONE official CHARACTER catalog prefab, so the 7 spawn slots (R3) and the
// AI squad members (R4, CLW_SquadRoles) can be "a rifleman / medic / AT soldier of this faction" without any configuration. Kin factions (friendly
// lists, CLW_ArsenalPool.GetKin) fill the roles the faction itself does not have (pack factions that only ship a few characters).
// Pick rules: skip ROLE_GUARD and vehicle / helicopter crew characters; the leader first, then the specific roles, the rifleman last; per role prefer
// own faction over kin, a prefab not used for another role, the leader's unit folder, fewer roles on the character, SIZE_M, the shorter file name
// (the plain variant before camo / Conflict copies), then catalog order. Only catalog data goes in, so the
// answer is the same on the server and on every client. Results are cached per faction (an empty result is not cached: catalogs may fill late).
// No faction keys, unit names or prefab GUIDs in code. Log: "CLW: squad roles picker <faction> <ROLE>=<prefab> ..." (once per faction and machine).

// Result for one faction.
class CLW_FactionRoles
{
	ref array<string> m_aRoles = {};						// roles that have a prefab, in the default slot order
	ref map<string, ResourceName> m_mPrefab = new map<string, ResourceName>();
}

// One usable character of a catalog (picker input).
class CLW_RoleCandidate
{
	ResourceName m_Prefab;
	ref array<string> m_aRoles = {};
	int m_iRank;		// 0 = the faction itself, 1.. = kin factions in friendly-list order
	int m_iOrder;		// catalog order (deterministic ties)
	bool m_bSizeM;
	string m_sDir;		// unit folder of the prefab
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
		array<ref CLW_RoleCandidate> candidates = {};
		Collect(faction, 0, candidates);

		// kin factions fill the roles the faction does not have (rank 1+: they never beat an own candidate)
		array<SCR_Faction> kin = {};
		CLW_ArsenalPool.GetKin(faction, kin);
		int rank = 1;
		foreach (SCR_Faction kinFaction : kin)
		{
			Collect(kinFaction, rank, candidates);
			rank++;
		}

		Assign(candidates, data);

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

	// Every usable character of one faction's CHARACTER catalog (no guards, no vehicle / helicopter crew), with its roles.
	protected static void Collect(notnull SCR_Faction faction, int rank, notnull array<ref CLW_RoleCandidate> candidates)
	{
		SCR_EntityCatalog catalog = faction.GetFactionEntityCatalogOfType(EEntityCatalogType.CHARACTER, false);
		if (!catalog)
			return;

		array<SCR_EntityCatalogEntry> list = {};
		catalog.GetEntityList(list);
		array<string> all = {};
		AllRoles(all);
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

			CLW_RoleCandidate candidate = new CLW_RoleCandidate();
			foreach (string role : all)
			{
				if (labels.Contains(LabelOf(role)))
					candidate.m_aRoles.Insert(role);
			}

			if (candidate.m_aRoles.IsEmpty())
				continue;

			candidate.m_Prefab = prefab;
			candidate.m_iRank = rank;
			candidate.m_iOrder = candidates.Count();
			candidate.m_bSizeM = labels.Contains(EEditableEntityLabel.SIZE_M);
			candidate.m_sDir = DirOf(prefab);
			candidates.Insert(candidate);
		}
	}

	// Assignment order: the leader first (it sets the unit folder the others should come from), then the specific roles, the generic rifleman last
	// (many specialists also carry ROLE_RIFLEMAN). One prefab per role while another candidate exists (no "Grenadier" twice on the spawn screen).
	protected static void Assign(notnull array<ref CLW_RoleCandidate> candidates, notnull CLW_FactionRoles data)
	{
		array<string> all = {};
		AllRoles(all);
		array<string> order = {};
		order.Insert("ROLE_LEADER");
		foreach (string role : all)
		{
			if (role != "ROLE_LEADER" && role != "ROLE_RIFLEMAN")
				order.Insert(role);
		}
		order.Insert("ROLE_RIFLEMAN");

		map<string, bool> used = new map<string, bool>();
		string family;
		foreach (string wanted : order)
		{
			CLW_RoleCandidate best;
			int bestScore;
			foreach (CLW_RoleCandidate candidate : candidates)
			{
				if (!candidate.m_aRoles.Contains(wanted))
					continue;

				int score = ScoreOf(candidate, family, used.Contains(candidate.m_Prefab));
				if (!best || IsBetter(score, candidate, bestScore, best))
				{
					best = candidate;
					bestScore = score;
				}
			}

			if (!best)
				continue;

			data.m_mPrefab.Set(wanted, best.m_Prefab);
			used.Set(best.m_Prefab, true);
			if (wanted == "ROLE_LEADER")
				family = best.m_sDir;
		}
	}

	// Own faction before any kin faction (rank), then not used for another role, same unit folder as the leader, fewer roles on the character,
	// SIZE_M.
	protected static int ScoreOf(notnull CLW_RoleCandidate candidate, string family, bool used)
	{
		int score = -1000 * candidate.m_iRank;
		if (!used)
			score += 100;

		if (!family.IsEmpty() && candidate.m_sDir == family)
			score += 10;

		int roles = candidate.m_aRoles.Count();
		if (roles < 4)
			score += 2 * (4 - roles);

		if (candidate.m_bSizeM)
			score += 1;

		return score;
	}

	protected static bool IsBetter(int score, notnull CLW_RoleCandidate candidate, int bestScore, notnull CLW_RoleCandidate best)
	{
		if (score != bestScore)
			return score > bestScore;

		// plain variant before camo / Conflict copies: shorter file name wins; a full tie keeps the earlier catalog entry (deterministic)
		int length = ShortName(candidate.m_Prefab).Length();
		int bestLength = ShortName(best.m_Prefab).Length();
		if (length != bestLength)
			return length < bestLength;

		return candidate.m_iOrder < best.m_iOrder;
	}

	// "{GUID}Prefabs/Characters/Factions/US/Character_US_Rifleman.et" -> "{GUID}Prefabs/Characters/Factions/US" without the GUID: the unit folder.
	static string DirOf(string prefab)
	{
		string text = prefab;
		int close = text.IndexOf("}");
		int slash = -1;
		for (int i = 0; i < text.Length(); i++)
		{
			if (text.Get(i) == "/")
				slash = i;
		}

		if (slash <= close + 1)
			return "";

		return text.Substring(close + 1, slash - close - 1);
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

// Role picker (loadouts-arsenal-plan R2): per faction and ROLE_* label ONE official CHARACTER catalog prefab, so the 7 spawn slots (R3) and the
// AI squad members (R4, CLW_SquadRoles) can be "a rifleman / medic / AT soldier of this faction" without any configuration.
// OWN FACTION FIRST (dev/notes/own-faction-roles.md): a role the faction has no character for is SUBSTITUTED by the closest own role (stand-in chains in
// Configs/CLW/SquadCompositions.conf, CLW_SquadCompositionStore.StandIns), never filled with a kin (friendly vanilla) prefab. Kin factions
// (CLW_ArsenalPool.GetKin) are used only when the faction itself has fewer than MIN_OWN_ROLES roles ("tiny pack": cannot field a squad alone).
// Pick rules: skip ROLE_GUARD and vehicle / helicopter crew characters; the leader first, then the specific roles, the rifleman last; per role prefer
// own faction over kin, a prefab not used for another role, the leader's unit folder, fewer roles on the character, SIZE_M, (rifleman only) the
// character the faction's own groups field most often, the shorter file name (the plain variant before camo / Conflict copies), then catalog order.
// Only catalog data goes in, so the answer is the same on the server and on every client. Results are cached per faction (an empty result is not
// cached: catalogs may fill late). No faction keys, unit names or prefab GUIDs in code.
// Log: "CLW: squad roles picker <faction> <ROLE>=<own|sub(<ROLE>)|kin>:<prefab> ..." (once per faction and machine).

// Result for one faction.
class CLW_FactionRoles
{
	ref array<string> m_aRoles = {};						// roles with a character of their own (or kin in a tiny pack), in the default slot order: the spawn slots
	ref map<string, ResourceName> m_mPrefab = new map<string, ResourceName>();	// also the substituted roles (AI squads)
	ref map<string, string> m_mSource = new map<string, string>();				// role -> "own" | "kin" | "sub(<ROLE>)"
	bool m_bTiny;											// fewer own roles than MIN_OWN_ROLES: kin fills in
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
	int m_iFreq;		// how often the faction's own groups field this character (rank 0 only)
}

class CLW_RolePicker
{
	static const int MIN_OWN_ROLES = 2;		// fewer own roles than this = tiny pack, kin characters fill in
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

	// One CHARACTER catalog prefab per ROLE_* for the faction: its own, else the own stand-in (SourceOf tells which), kin only in a tiny pack; "" if none.
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

	// Where the role's prefab comes from: "own", "kin" (tiny pack), "sub(<ROLE>)" (own stand-in) or "" (no prefab).
	static string SourceOf(string factionKey, string role)
	{
		CLW_FactionRoles data = Get(factionKey);
		if (!data)
			return "";

		string source;
		if (data.m_mSource.Find(role, source))
			return source;

		return "";
	}

	// Roles the faction really has a character for (own; kin in a tiny pack), in the default slot order (leader, rifleman, machinegunner,
	// antitank, grenadier, medic, sharpshooter, ...). Substituted roles are NOT listed: RolePrefab still answers for them (AI squads).
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
		map<string, int> freq = new map<string, int>();
		GroupFrequency(faction, freq);
		array<ref CLW_RoleCandidate> candidates = {};
		Collect(faction, 0, freq, candidates);
		Assign(candidates, data);

		int ownRoles = data.m_mPrefab.Count();
		if (ownRoles < MIN_OWN_ROLES)
		{
			// tiny pack: the faction alone cannot field a squad, kin characters fill the missing roles (rank 1+: they never beat an own candidate)
			data = new CLW_FactionRoles();
			data.m_bTiny = true;
			array<SCR_Faction> kin = {};
			CLW_ArsenalPool.GetKin(faction, kin);
			map<string, int> noFreq = new map<string, int>();
			int rank = 1;
			foreach (SCR_Faction kinFaction : kin)
			{
				Collect(kinFaction, rank, noFreq, candidates);
				rank++;
			}

			Assign(candidates, data);
			Print(string.Format("CLW: squad roles picker %1 TINY pack: only %2 own role(s), kin characters fill in (%3 roles now)", factionKey, ownRoles, data.m_mPrefab.Count()), LogLevel.WARNING);
		}

		// a role without a character of its own is played by the closest own role (never by a kin prefab)
		array<string> all = {};
		AllRoles(all);
		foreach (string missingRole : all)
		{
			if (data.m_mPrefab.Contains(missingRole))
				continue;

			array<string> standIns = {};
			CLW_SquadCompositionStore.StandIns(missingRole, standIns);
			string chosen = "";
			foreach (string standIn : standIns)
			{
				if (HasRealPrefab(data, standIn))
				{
					chosen = standIn;
					break;
				}
			}

			if (chosen.IsEmpty())
			{
				foreach (string anyRole : all)
				{
					if (HasRealPrefab(data, anyRole))
					{
						chosen = anyRole;
						break;
					}
				}
			}

			if (chosen.IsEmpty())
				continue;

			ResourceName standPrefab;
			data.m_mPrefab.Find(chosen, standPrefab);
			data.m_mPrefab.Set(missingRole, standPrefab);
			data.m_mSource.Set(missingRole, "sub(" + chosen + ")");
		}

		string line;
		foreach (string role : all)
		{
			ResourceName picked;
			if (!data.m_mPrefab.Find(role, picked))
				continue;

			string source = "own";
			data.m_mSource.Find(role, source);
			if (HasRealPrefab(data, role))
				data.m_aRoles.Insert(role);

			line += " " + role + "=" + source + ":" + ShortName(picked);
		}

		if (data.m_aRoles.IsEmpty())
			return data;	// catalogs may not be filled yet: not cached, the next call scans again

		s_mCache.Set(factionKey, data);
		Print("CLW: squad roles picker " + factionKey + line, LogLevel.NORMAL);
		return data;
	}

	// The role has a character of its own (or a kin one in a tiny pack), not a stand-in.
	protected static bool HasRealPrefab(notnull CLW_FactionRoles data, string role)
	{
		string source;
		if (!data.m_mSource.Find(role, source))
			return false;

		return source == "own" || source == "kin";
	}

	// How often each character (lower-case path without GUID) appears in the faction's own GROUP catalog entries. The plain rifleman is the one
	// squads field most; a specialist that is only labelled ROLE_RIFLEMAN (a grenade launcher man) appears once.
	protected static void GroupFrequency(notnull SCR_Faction faction, notnull map<string, int> freq)
	{
		SCR_EntityCatalog catalog = faction.GetFactionEntityCatalogOfType(EEntityCatalogType.GROUP, false);
		if (!catalog)
			return;

		array<SCR_EntityCatalogEntry> list = {};
		catalog.GetEntityList(list);
		foreach (SCR_EntityCatalogEntry entry : list)
		{
			if (!entry || !entry.IsEnabled())
				continue;

			array<ResourceName> slots = {};
			if (!GroupSlots(entry.GetPrefab(), slots))
				continue;

			foreach (ResourceName slot : slots)
			{
				string key = CLW_ForceCatalog.PathOf(slot);
				int count = 0;
				freq.Find(key, count);
				freq.Set(key, count + 1);
			}
		}
	}

	// Member character prefabs of a group prefab (SCR_AIGroup.m_aUnitPrefabSlots). False when unreadable.
	static bool GroupSlots(ResourceName prefab, notnull array<ResourceName> slots)
	{
		if (prefab.IsEmpty())
			return false;

		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
			return false;

		BaseResourceObject object = resource.GetResource();
		if (!object)
			return false;

		BaseContainer container = object.ToBaseContainer();
		if (!container)
			return false;

		array<ResourceName> raw = {};
		if (!container.Get("m_aUnitPrefabSlots", raw))
			return false;

		foreach (ResourceName slot : raw)
		{
			if (!slot.IsEmpty())
				slots.Insert(slot);
		}

		return !slots.IsEmpty();
	}

	// ---- character ownership (also used by CLW_ForceCatalog to skip groups made of another faction's soldiers) ----

	// Lower-case paths (no GUID) of the enabled CHARACTER catalog entries of a faction.
	static void CharacterPaths(notnull SCR_Faction faction, notnull map<string, bool> paths)
	{
		SCR_EntityCatalog catalog = faction.GetFactionEntityCatalogOfType(EEntityCatalogType.CHARACTER, false);
		if (!catalog)
			return;

		array<SCR_EntityCatalogEntry> list = {};
		catalog.GetEntityList(list);
		foreach (SCR_EntityCatalogEntry entry : list)
		{
			if (entry && entry.IsEnabled() && !entry.GetPrefab().IsEmpty())
				paths.Set(CLW_ForceCatalog.PathOf(entry.GetPrefab()), true);
		}
	}

	// own = the faction's characters; other = the characters of every other faction (own wins when both list one).
	static void OwnershipSets(notnull SCR_Faction faction, notnull map<string, bool> own, notnull map<string, bool> other)
	{
		CharacterPaths(faction, own);
		FactionManager manager = GetGame().GetFactionManager();
		if (!manager)
			return;

		array<Faction> all = {};
		manager.GetFactionsList(all);
		foreach (Faction item : all)
		{
			SCR_Faction candidate = SCR_Faction.Cast(item);
			if (candidate && candidate != faction)
				CharacterPaths(candidate, other);
		}
	}

	// A group is foreign when none of its members is a character of the faction and at least one belongs to another faction (its soldiers are
	// that faction's, e.g. a vanilla rifle squad listed in a pack faction's catalog). Uncatalogued members count as neither.
	static bool IsForeignGroup(notnull array<ResourceName> slots, notnull map<string, bool> own, notnull map<string, bool> other)
	{
		int ownCount;
		int foreignCount;
		foreach (ResourceName slot : slots)
		{
			string key = CLW_ForceCatalog.PathOf(slot);
			if (own.Contains(key))
				ownCount++;
			else if (other.Contains(key))
				foreignCount++;
		}

		return ownCount == 0 && foreignCount > 0;
	}

	// Every usable character of one faction's CHARACTER catalog (no guards, no vehicle / helicopter crew), with its roles.
	protected static void Collect(notnull SCR_Faction faction, int rank, notnull map<string, int> freq, notnull array<ref CLW_RoleCandidate> candidates)
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
			int fielded = 0;
			freq.Find(CLW_ForceCatalog.PathOf(prefab), fielded);
			candidate.m_iFreq = fielded;
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

				int score = ScoreOf(candidate, family, used.Contains(candidate.m_Prefab), wanted);
				if (!best || IsBetter(score, candidate, bestScore, best))
				{
					best = candidate;
					bestScore = score;
				}
			}

			if (!best)
				continue;

			data.m_mPrefab.Set(wanted, best.m_Prefab);
			if (best.m_iRank == 0)
				data.m_mSource.Set(wanted, "own");
			else
				data.m_mSource.Set(wanted, "kin");

			used.Set(best.m_Prefab, true);
			if (wanted == "ROLE_LEADER")
				family = best.m_sDir;
		}
	}

	// Own faction before any kin faction (rank), then not used for another role, same unit folder as the leader, fewer roles on the character,
	// SIZE_M; for the rifleman also how often the faction's own groups field the character (a plain rifleman is the common member).
	protected static int ScoreOf(notnull CLW_RoleCandidate candidate, string family, bool used, string wanted)
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

		if (wanted == "ROLE_RIFLEMAN")
			score += 4 * Math.Min(candidate.m_iFreq, 6);

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

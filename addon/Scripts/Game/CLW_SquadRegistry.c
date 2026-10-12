// One adopted AI group. The group is a weak reference: it becomes null when the entity is deleted.
class CLW_SquadRecord
{
	string m_sId;
	string m_sFaction;
	SCR_AIGroup m_Group;	// weak
	int m_iSerial;			// unique per adoption (a released and re-adopted group gets a new serial)
	int m_iStartStrength;	// living AI at adoption
	int m_iEmptySweeps;		// consecutive sweeps without a living member
	string m_sKind;			// generic unit kind (CLW_ForceKind), "" = not resolved yet
	string m_sDisplayType;	// human-readable type from data
	string m_sTypeId;		// force type id from the faction profile, "" when unknown
	bool m_bHidden;			// linked cargo group of a vehicle unit: adopted and polled, but not commandable
	bool m_bTagged;			// kind/type were set explicitly (spawner), never overwritten by the lazy resolve
	string m_sRole;			// naming role that overrides the kind (CLW_SquadRegistry.ROLE_*), "" = named by kind
	string m_sName;			// readable display name ("1st Rifle Squad"), "" = not assigned yet
	string m_sNameKey;		// kind / role the name was built from (a changed key re-names the squad)
	string m_sNamePattern;	// pattern of the name (ordinals are unique per faction + pattern)
	int m_iOrdinal;			// ordinal in the name
	string m_sParentId;		// embarked cargo group of a vehicle unit: id of the unit (crew) group, "" = not a cargo group
	string m_sAppliedName;	// last name pushed to the vanilla group list (cargo names follow the parent and can change)
}

// Kind / type set by the force spawner for a group it created, keyed by the group's entity name. Applied at adoption.
class CLW_SquadTag
{
	string m_sKind;
	string m_sDisplayType;
	string m_sTypeId;
	bool m_bHidden;
}

// CLW_SquadRegistry: the single source of truth for "which AI groups does HQ command per faction". Server only,
// one instance owned by the game mode. Commanders, markers and the situation report query it by stable string id.
//
// Ids: the entity name when the group has one (scene-placed "CLW_Squad_US_1" keeps its id), else "<FACTION>_G<n>"
// (per-faction counter, never reused in a session).
// Adopt: group of a CLW faction, >= 1 living AI member, not a slave group, not opted out (name prefix CLW_IGNORE_),
//        and (ADOPT_RUNTIME_GROUPS or it has an entity name).
// Release: entity gone, or no living member for EMPTY_SWEEPS_TO_RELEASE consecutive sweeps. The commanders drop their
//          state for ids that are no longer listed; a lost squad is simply absent from the next situation report.
// Names: every commandable squad gets a readable display name once ("1st Rifle Squad", "Recon Team 1", "HQ Garrison"), see NamePattern
//        (the single kind -> pattern table). The ordinal is the lowest free one per faction and pattern; a living squad is never
//        renumbered. The id stays the internal / LLM key. The name feeds the vanilla group lists (CLW_SquadGroups.ApplyName).
// Enumeration: AIWorld.GetAIAgents (every spawned agent: scene-placed, Game Master and script-spawned) -> parent group.
class CLW_SquadRegistry
{
	static const bool ADOPT_RUNTIME_GROUPS = true;	// false: only groups with an entity name (placed in the scene) are adopted
	static const string IGNORE_PREFIX = "CLW_IGNORE_";
	static const int SWEEP_MS = 5000;
	static const int EMPTY_SWEEPS_TO_RELEASE = 2;
	static const string ROLE_GARRISON = "garrison";	// naming roles (override the kind, set by the owner of the role via SetRole)
	static const string ROLE_FOB_TEAM = "fob_team";
	static const string CARGO_SUFFIX = "_Inf";	// ForceSpawner names the embarked group of a vehicle unit "<unit id>_Inf"

	protected ref array<ref CLW_SquadRecord> m_aRecords = {};
	protected ref map<string, int> m_mCounters = new map<string, int>();	// generated id counter per faction
	protected int m_iVersion;	// bumped on every adoption/release: commanders resync when it changed
	protected int m_iSerial;
	protected int m_iSweeps;
	protected ref map<string, ref CLW_SquadTag> m_mTags = new map<string, ref CLW_SquadTag>();	// by entity name
	protected ref map<string, int> m_mGenericCount = new map<string, int>();	// "Infantry group (n)" counter per faction
	protected ref map<string, string> m_mJoinableLog = new map<string, string>();	// last logged joinable set per faction
	protected CLW_ForceCatalog m_Catalog;	// weak: owned by the game mode; resolves display types of scene-placed groups

	static bool IsClwFaction(string factionKey)
	{
		// the factions are the scenario's (data), nothing is adopted before it is loaded
		return CLW_ScenarioConfig.IsClwFaction(factionKey);
	}

	// Run an immediate sweep, then repeat. Server only.
	void Start()
	{
		if (!Replication.IsServer())
			return;

		Sweep();
		GetGame().GetCallqueue().CallLater(Sweep, SWEEP_MS, true);
	}

	// The scenario sides were assigned other factions (CLW_Sides.Apply): the faction filter is the scenario's active keys, so
	// release the squads of factions that are no longer a side, then re-sweep to adopt the new factions' scene groups.
	void Rekey()
	{
		if (!Replication.IsServer())
			return;

		int released;
		for (int i = m_aRecords.Count() - 1; i >= 0; i--)
		{
			if (!IsClwFaction(m_aRecords[i].m_sFaction))
			{
				Release(i, "faction left the war");
				released++;
			}
		}

		if (released > 0)
			m_iVersion++;

		Print(string.Format("CLW: registry re-keyed, released %1 squads of other factions, sweeping", released), LogLevel.NORMAL);
		Sweep();
	}

	void SetCatalog(CLW_ForceCatalog catalog)
	{
		m_Catalog = catalog;
	}

	// The spawner announces what a group it created is, BEFORE the registry can adopt it (key = entity name = future id).
	// Applied at adoption; if the group is already adopted, the record is updated.
	void Tag(string entityName, string kind, string displayType, string typeId, bool hidden = false)
	{
		if (entityName.IsEmpty())
			return;

		CLW_SquadTag tag = new CLW_SquadTag();
		tag.m_sKind = kind;
		tag.m_sDisplayType = displayType;
		tag.m_sTypeId = typeId;
		tag.m_bHidden = hidden;
		m_mTags.Set(entityName, tag);
		CLW_SquadRecord record = FindRecord(entityName);
		if (record)
		{
			ApplyTag(record, tag);
			EnsureName(record);
			m_iVersion++;
		}

		Print(string.Format("CLW: registry tag %1 kind=%2 type='%3' typeId=%4 hidden=%5", entityName, kind, displayType, typeId, hidden), LogLevel.NORMAL);
	}

	// Hide / show a squad for HQ (linked cargo groups are hidden while their vehicle unit exists).
	void SetHidden(string id, bool hidden)
	{
		CLW_SquadTag tag;
		if (m_mTags.Find(id, tag) && tag)
			tag.m_bHidden = hidden;

		CLW_SquadRecord record = FindRecord(id);
		if (record && record.m_bHidden != hidden)
		{
			record.m_bHidden = hidden;
			if (!hidden)
				EnsureName(record);
			m_iVersion++;
			Print(string.Format("CLW: registry %1 hidden=%2", id, hidden), LogLevel.NORMAL);
		}
	}

	// Change kind / display type of an adopted squad (a vehicle unit that dissolved: its crew is infantry now).
	void SetKind(string id, string kind, string displayType)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (!record)
			return;

		record.m_sKind = kind;
		record.m_sDisplayType = displayType;
		record.m_bTagged = true;
		EnsureName(record);
		m_iVersion++;
		Print(string.Format("CLW: registry %1 kind=%2 type='%3'", id, kind, displayType), LogLevel.NORMAL);
	}

	// ---- readable names ----

	// THE kind -> name pattern table (data of the naming, keyed by the CLW kind enum / naming role, never by faction text).
	// Tokens: {ord} = English ordinal (1st, 2nd, 3rd, 11th, 21st), {n} = number, {n2} = "" for 1 and " <n>" otherwise.
	static string NamePattern(string key)
	{
		if (key == ROLE_GARRISON)
			return "HQ Garrison{n2}";
		if (key == ROLE_FOB_TEAM)
			return "FOB Team {n}";
		if (key == CLW_ForceKind.INFANTRY)
			return "{ord} Rifle Squad";
		if (key == CLW_ForceKind.RECON)
			return "Recon Team {n}";
		if (key == CLW_ForceKind.MOTORIZED)
			return "Motorized Squad {n}";
		if (key == CLW_ForceKind.LIGHT)
			return "Light Vehicle {n}";
		if (key == CLW_ForceKind.MECH)
			return "Mechanized Squad {n}";
		if (key == CLW_ForceKind.ARMOR)
			return "Armor {n}";
		if (key == CLW_ForceKind.ARMORED_RECON)
			return "Armored Recon {n}";
		if (key == CLW_ForceKind.LOGISTICS)
			return "Supply Truck {n}";

		return "Squad {n}";
	}

	// English ordinal: 1st 2nd 3rd 4th ... 11th 12th 13th ... 21st.
	static string Ordinal(int n)
	{
		string suffix = "th";
		int mod100 = n % 100;
		if (mod100 < 11 || mod100 > 13)
		{
			int mod10 = n % 10;
			if (mod10 == 1)
				suffix = "st";
			else if (mod10 == 2)
				suffix = "nd";
			else if (mod10 == 3)
				suffix = "rd";
		}

		return n.ToString() + suffix;
	}

	static string FormatName(string pattern, int n)
	{
		string name = pattern;
		string n2 = "";
		if (n > 1)
			n2 = " " + n.ToString();
		name.Replace("{ord}", Ordinal(n));
		name.Replace("{n2}", n2);
		name.Replace("{n}", n.ToString());
		return name;
	}

	// Readable name of a squad ("" for an unknown id). Linked cargo groups (hidden, unnamed) fall back to their display type.
	string GetName(string id)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (!record)
			return "";

		EnsureName(record);
		if (!record.m_sParentId.IsEmpty())
			return record.m_sName;

		if (record.m_sName.IsEmpty())
		{
			Resolve(record);
			return record.m_sDisplayType;
		}

		return record.m_sName;
	}

	// Give the squad a naming role (garrison, FOB team): it is re-named from the role's pattern with the next free ordinal.
	// Role "" returns to the kind name. Server only.
	void SetRole(string id, string role)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (!record || record.m_sRole == role)
			return;

		record.m_sRole = role;
		EnsureName(record);
		m_iVersion++;
	}

	// Assign (or, when the kind / role changed, re-assign) the display name. Hidden cargo groups stay unnamed.
	protected void EnsureName(CLW_SquadRecord record)
	{
		if (!record.m_sParentId.IsEmpty())
		{
			record.m_sName = CargoName(record);	// follows the parent unit, no ordinal of its own
			return;
		}

		if (record.m_bHidden)
			return;

		Resolve(record);
		string key = record.m_sRole;
		if (key.IsEmpty())
			key = record.m_sKind;
		if (!record.m_sName.IsEmpty() && record.m_sNameKey == key)
			return;

		string pattern = NamePattern(key);
		int ordinal = FreeOrdinal(record, pattern);
		string old = record.m_sName;
		record.m_sNameKey = key;
		record.m_sNamePattern = pattern;
		record.m_iOrdinal = ordinal;
		record.m_sName = FormatName(pattern, ordinal);
		Print(string.Format("CLW: squad name %1 -> \"%2\" (side %3, kind %4)", record.m_sId, record.m_sName, record.m_sFaction, key), LogLevel.NORMAL);
		if (!old.IsEmpty() && old != record.m_sName)
			Print(string.Format("CLW: squad name %1 renamed from \"%2\" (kind/role changed)", record.m_sId, old), LogLevel.NORMAL);

		if (record.m_Group && CLW_SquadGroups.IsRegistered(record.m_Group))
			CLW_SquadGroups.ApplyName(record.m_Group);
	}

	// Name of an embarked cargo group: the parent unit's name ("Motorized Squad 1"); "<name> (dismounts)" when the unit's own
	// crew group is also in the vanilla lists (so the two stay distinguishable). No parent record: the entry's display type.
	protected string CargoName(CLW_SquadRecord record)
	{
		CLW_SquadRecord parent = FindRecord(record.m_sParentId);
		if (!parent)
			return record.m_sDisplayType;

		EnsureName(parent);
		string name = parent.m_sName;
		if (name.IsEmpty())
			name = parent.m_sDisplayType;
		if (name.IsEmpty())
			return record.m_sDisplayType;

		if (parent.m_Group && IsJoinableRecord(parent))
			name += " (dismounts)";

		return name;
	}

	// Lowest ordinal not used by another living squad of the same faction and pattern.
	protected int FreeOrdinal(CLW_SquadRecord self, string pattern)
	{
		int n = 1;
		while (n < 1000)
		{
			bool used = false;
			foreach (CLW_SquadRecord other : m_aRecords)
			{
				if (other != self && other.m_sFaction == self.m_sFaction && other.m_sNamePattern == pattern && other.m_iOrdinal == n && !other.m_sName.IsEmpty())
				{
					used = true;
					break;
				}
			}

			if (!used)
				return n;

			n++;
		}

		return n;
	}

	string GetKind(string id)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (!record)
			return "";

		Resolve(record);
		return record.m_sKind;
	}

	string GetDisplayType(string id)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (!record)
			return "";

		Resolve(record);
		return record.m_sDisplayType;
	}

	string GetTypeId(string id)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (!record)
			return "";

		Resolve(record);
		return record.m_sTypeId;
	}

	bool IsHidden(string id)
	{
		CLW_SquadRecord record = FindRecord(id);
		return record && record.m_bHidden;
	}

	// Squads HQ may order: GetIds() without the hidden (linked cargo) groups.
	void GetCommandableIds(string factionKey, notnull array<string> ids)
	{
		foreach (CLW_SquadRecord record : m_aRecords)
		{
			if (record.m_sFaction == factionKey && !record.m_bHidden)
				ids.Insert(record.m_sId);
		}
	}

	protected void ApplyTag(CLW_SquadRecord record, CLW_SquadTag tag)
	{
		record.m_sKind = tag.m_sKind;
		record.m_sDisplayType = tag.m_sDisplayType;
		record.m_sTypeId = tag.m_sTypeId;
		record.m_bHidden = tag.m_bHidden;
		record.m_bTagged = true;
		record.m_sParentId = "";
		int idLen = record.m_sId.Length();
		int suffixLen = CARGO_SUFFIX.Length();
		if (tag.m_bHidden && idLen > suffixLen && record.m_sId.Substring(idLen - suffixLen, suffixLen) == CARGO_SUFFIX)
			record.m_sParentId = record.m_sId.Substring(0, idLen - suffixLen);	// cargo group of the unit "<id>"
	}

	// Scene-placed / runtime groups nobody tagged: kind "infantry"; display type from the catalog entry with the same group
	// prefab if the catalog knows it, else "Infantry group (n)". Retried on every query until the catalog can answer.
	protected void Resolve(CLW_SquadRecord record)
	{
		if (record.m_bTagged)
			return;

		if (record.m_sKind.IsEmpty())
			record.m_sKind = CLW_ForceKind.INFANTRY;

		if (!record.m_sDisplayType.IsEmpty() && record.m_sDisplayType.IndexOf("Infantry group") != 0)
			return;

		if (m_Catalog && record.m_Group)
		{
			EntityPrefabData data = record.m_Group.GetPrefabData();
			if (data)
			{
				CLW_ForceEntry entry = m_Catalog.FindByGroupPrefab(record.m_sFaction, data.GetPrefabName());
				if (entry)
				{
					record.m_sKind = entry.m_sKind;
					record.m_sDisplayType = entry.m_sName;
					record.m_sTypeId = entry.m_sType;
					return;
				}
			}
		}

		if (record.m_sDisplayType.IsEmpty())
		{
			int n;
			m_mGenericCount.Find(record.m_sFaction, n);
			n++;
			m_mGenericCount.Set(record.m_sFaction, n);
			record.m_sDisplayType = "Infantry group (" + n + ")";
		}
	}

	// Changes whenever a squad was adopted or released.
	int GetVersion()
	{
		return m_iVersion;
	}

	// Appends the ids of the faction's adopted squads (adoption order).
	void GetIds(string factionKey, notnull array<string> ids)
	{
		foreach (CLW_SquadRecord record : m_aRecords)
		{
			if (record.m_sFaction == factionKey)
				ids.Insert(record.m_sId);
		}
	}

	SCR_AIGroup Find(string id)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (record)
			return record.m_Group;

		return null;
	}

	int GetStartStrength(string id)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (record)
			return record.m_iStartStrength;

		return 0;
	}

	// Unique per adoption; 0 for an unknown id.
	int GetSerial(string id)
	{
		CLW_SquadRecord record = FindRecord(id);
		if (record)
			return record.m_iSerial;

		return 0;
	}

	bool IsAdopted(SCR_AIGroup group)
	{
		return FindRecordOf(group) != null;
	}

	// Id of an adopted group, else "".
	string GetId(SCR_AIGroup group)
	{
		CLW_SquadRecord record = FindRecordOf(group);
		if (record)
			return record.m_sId;

		return "";
	}

	// Faction key of any group ("" when it has none).
	static string GetFactionKey(SCR_AIGroup group)
	{
		if (!group)
			return "";

		Faction faction = group.GetFaction();
		if (!faction)
			return "";

		return faction.GetFactionKey();
	}

	// Squad a joining player should lead: the preferred id if it is adopted, else a squad that already has a human leader
	// (further players join their side's player squad), else the first adopted squad without one.
	SCR_AIGroup FindPlayerSquad(string factionKey, string preferredId)
	{
		CLW_SquadRecord preferred = FindRecord(preferredId);
		if (preferred && preferred.m_sFaction == factionKey && preferred.m_Group)
			return preferred.m_Group;

		SCR_AIGroup firstFree;
		foreach (CLW_SquadRecord record : m_aRecords)
		{
			if (record.m_sFaction != factionKey || !record.m_Group)
				continue;

			if (CLW_SquadInfo.HumanLeaderEntity(record.m_Group))
				return record.m_Group;

			if (!firstFree)
				firstFree = record.m_Group;
		}

		return firstFree;
	}

	// Release vanished/emptied squads, then adopt new ones. Cheap enough to call every few seconds.
	void Sweep()
	{
		if (!Replication.IsServer())
			return;

		m_iSweeps++;
		bool changed = false;
		for (int i = m_aRecords.Count() - 1; i >= 0; i--)
		{
			CLW_SquadRecord record = m_aRecords[i];
			if (!record.m_Group)
			{
				Release(i, "entity gone");
				changed = true;
				continue;
			}

			if (CLW_SquadInfo.AliveCount(record.m_Group) > 0)
			{
				record.m_iEmptySweeps = 0;
				continue;
			}

			record.m_iEmptySweeps++;
			if (record.m_iEmptySweeps == 1)
				CLW_Waypoints.Clear(record.m_Group);	// empty group + leftover move = vanilla SCR_AIProcessFailedMovementResult exception (null leader)
			if (record.m_iEmptySweeps >= EMPTY_SWEEPS_TO_RELEASE)
			{
				Release(i, "no living members");
				changed = true;
			}
		}

		AIWorld aiWorld = GetGame().GetAIWorld();
		if (aiWorld)
		{
			array<AIAgent> agents = {};
			aiWorld.GetAIAgents(agents);
			array<SCR_AIGroup> seen = {};
			foreach (AIAgent agent : agents)
			{
				if (!agent)
					continue;

				// the world list may hold the group agents themselves or only their members: handle both
				SCR_AIGroup group = SCR_AIGroup.Cast(agent);
				if (!group)
					group = SCR_AIGroup.Cast(agent.GetParentGroup());
				if (group && group.IsSlave())
					group = group.GetMaster();
				if (!group || seen.Contains(group))
					continue;

				seen.Insert(group);
				if (TryAdopt(group))
					changed = true;
			}
		}

		SyncJoinable();
		if (changed)
		{
			m_iVersion++;
			LogSummary();
		}
	}

	// ---- joinable squads (vanilla group lists, see CLW_SquadGroups) ----

	// Register the squads a player may pick (infantry without a vehicle link, not hidden, alive, on a side, war running),
	// unregister the ones that stopped qualifying (when no human is in them), drop empty vanilla groups.
	protected void SyncJoinable()
	{
		foreach (CLW_SquadRecord record : m_aRecords)
		{
			if (!record.m_Group)
				continue;

			bool want = IsJoinableRecord(record);
			bool have = CLW_SquadGroups.IsRegistered(record.m_Group);
			if (want && !have)
			{
				EnsureName(record);
				CLW_SquadGroups.Register(record.m_Group);
				record.m_sAppliedName = record.m_sName;
			}
			else if (!want && have && record.m_Group.GetPlayerCount() == 0)
			{
				CLW_SquadGroups.Unregister(record.m_Group);
			}
			else if (have && !record.m_sParentId.IsEmpty())
			{
				EnsureName(record);
				if (record.m_sName != record.m_sAppliedName)
				{
					record.m_sAppliedName = record.m_sName;
					CLW_SquadGroups.ApplyName(record.m_Group);
				}
			}
		}

		CLW_SquadGroups.PruneVanillaGroups(this);
		LogJoinable();
	}

	// "CLW: squad groups joinable <side>: <n> [names]", once per change of a side's set.
	protected void LogJoinable()
	{
		array<string> keys = {};
		foreach (CLW_SquadRecord record : m_aRecords)
		{
			if (!keys.Contains(record.m_sFaction))
				keys.Insert(record.m_sFaction);
		}

		foreach (string key : keys)
		{
			int count = 0;
			string names = "";
			foreach (CLW_SquadRecord member : m_aRecords)
			{
				if (member.m_sFaction != key || !member.m_Group || !CLW_SquadGroups.IsRegistered(member.m_Group))
					continue;

				if (count > 0)
					names += ", ";
				names += member.m_sName;
				count++;
			}

			string last;
			string line = count.ToString() + " [" + names + "]";
			if (m_mJoinableLog.Find(key, last) && last == line)
				continue;

			m_mJoinableLog.Set(key, line);
			Print("CLW: squad groups joinable " + key + ": " + line, LogLevel.NORMAL);
		}
	}

	protected bool IsJoinableRecord(CLW_SquadRecord record)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return false;

		if ((record.m_bHidden && record.m_sParentId.IsEmpty()) || !IsClwFaction(record.m_sFaction))
			return false;

		Resolve(record);
		if (record.m_sParentId.IsEmpty())
		{
			// Not an embarked cargo group: it must not be a vehicle's crew. Dismounted kinds (rifle, recon) are joinable.
			CLW_ForceEntry entry;
			if (m_Catalog && !record.m_sTypeId.IsEmpty())
				entry = m_Catalog.Find(record.m_sFaction, record.m_sTypeId);

			if (entry)
			{
				if (entry.HasVehicle())
					return false;
			}
			else if (record.m_sKind != CLW_ForceKind.INFANTRY && record.m_sKind != CLW_ForceKind.RECON)
			{
				return false;
			}
		}

		return CLW_SquadInfo.AliveCount(record.m_Group) > 0;
	}

	// True when the squad is in the vanilla group lists (a player can pick it).
	bool IsJoinable(SCR_AIGroup group)
	{
		return IsAdopted(group) && CLW_SquadGroups.IsRegistered(group);
	}

	// Number of joinable squads of the faction ("" = all).
	int CountJoinable(string factionKey = "")
	{
		return CLW_SquadGroups.Count(factionKey);
	}

	protected bool TryAdopt(notnull SCR_AIGroup group)
	{
		if (FindRecordOf(group))
			return false;

		string factionKey = GetFactionKey(group);
		if (!IsClwFaction(factionKey))
			return false;

		string entityName = group.GetName();
		if (entityName.IndexOf(IGNORE_PREFIX) == 0)
			return false;
		if (!ADOPT_RUNTIME_GROUPS && entityName.IsEmpty())
			return false;

		int alive = CLW_SquadInfo.AliveCount(group);
		if (alive <= 0)
			return false;

		string id = entityName;
		if (id.IsEmpty() || FindRecord(id))
			id = NextId(factionKey);

		m_iSerial++;
		CLW_SquadRecord record = new CLW_SquadRecord();
		record.m_sId = id;
		record.m_sFaction = factionKey;
		record.m_Group = group;
		record.m_iSerial = m_iSerial;
		record.m_iStartStrength = alive;
		CLW_SquadTag tag;
		if (m_mTags.Find(id, tag) && tag)
			ApplyTag(record, tag);
		m_aRecords.Insert(record);
		EnsureName(record);

		string source = "runtime";
		if (!entityName.IsEmpty())
			source = "scene";
		Print(string.Format("CLW: registry adopted %1 faction=%2 alive=%3 source=%4 hidden=%5", id, factionKey, alive, source, record.m_bHidden), LogLevel.NORMAL);
		return true;
	}

	protected void Release(int index, string reason)
	{
		CLW_SquadRecord record = m_aRecords[index];
		Print(string.Format("CLW: registry released %1 reason=%2", record.m_sId, reason), LogLevel.NORMAL);
		m_mTags.Remove(record.m_sId);
		if (record.m_Group && CLW_SquadGroups.IsRegistered(record.m_Group))
			CLW_SquadJoin.OnSquadReleased(record.m_Group);
		m_aRecords.Remove(index);
	}

	protected string NextId(string factionKey)
	{
		while (true)
		{
			int n = 0;
			m_mCounters.Find(factionKey, n);
			n++;
			m_mCounters.Set(factionKey, n);
			string id = factionKey + "_G" + n;
			if (!FindRecord(id))
				return id;
		}

		return "";
	}

	protected CLW_SquadRecord FindRecord(string id)
	{
		foreach (CLW_SquadRecord record : m_aRecords)
		{
			if (record.m_sId == id)
				return record;
		}

		return null;
	}

	protected CLW_SquadRecord FindRecordOf(SCR_AIGroup group)
	{
		if (!group)
			return null;

		foreach (CLW_SquadRecord record : m_aRecords)
		{
			if (record.m_Group == group)
				return record;
		}

		return null;
	}

	// One line, only after a change: "CLW: registry US=[a,b] USSR=[c]".
	protected void LogSummary()
	{
		string line = "CLW: registry";
		array<string> keys = {};
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario)
			scenario.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			array<string> ids = {};
			GetIds(key, ids);
			line += " " + key + "=[";
			foreach (int k, string id : ids)
			{
				if (k > 0)
					line += ",";
				line += id;
			}
			line += "]";
		}

		Print("" + line, LogLevel.NORMAL);	// Print(variable) would log "string line = ..."
	}

	// Rematch (CLW_Rematch.c): stop the sweep, free the humans and delete every adopted squad (AI members, slave group, group). Returns the squads deleted.
	int RematchReset()
	{
		GetGame().GetCallqueue().Remove(Sweep);
		int deleted;
		for (int i = m_aRecords.Count() - 1; i >= 0; i--)
		{
			SCR_AIGroup group = m_aRecords[i].m_Group;
			if (group)
			{
				CLW_SquadJoin.OnSquadReleased(group);	// unregisters from the vanilla lists, frees the humans
				SCR_AIGroup slave = group.GetSlave();
				CLW_ForceSpawner.DeleteGroupAndMembers(group);
				if (slave)
					SCR_EntityHelper.DeleteEntityAndChildren(slave);
				deleted++;
			}

			m_aRecords.Remove(i);
		}

		m_mCounters.Clear();
		m_mTags.Clear();
		m_mGenericCount.Clear();
		m_iVersion++;
		m_iSweeps = 0;
		return deleted;
	}
}

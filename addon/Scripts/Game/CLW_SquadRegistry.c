// One adopted AI group. The group is a weak reference: it becomes null when the entity is deleted.
class CLW_SquadRecord
{
	string m_sId;
	string m_sFaction;
	SCR_AIGroup m_Group;	// weak
	int m_iSerial;			// unique per adoption (a released and re-adopted group gets a new serial)
	int m_iStartStrength;	// living AI at adoption
	int m_iEmptySweeps;		// consecutive sweeps without a living member
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
// Enumeration: AIWorld.GetAIAgents (every spawned agent: scene-placed, Game Master and script-spawned) -> parent group.
class CLW_SquadRegistry
{
	static const bool ADOPT_RUNTIME_GROUPS = true;	// false: only groups with an entity name (placed in the scene) are adopted
	static const string IGNORE_PREFIX = "CLW_IGNORE_";
	static const int SWEEP_MS = 5000;
	static const int EMPTY_SWEEPS_TO_RELEASE = 2;

	protected ref array<ref CLW_SquadRecord> m_aRecords = {};
	protected ref map<string, int> m_mCounters = new map<string, int>();	// generated id counter per faction
	protected int m_iVersion;	// bumped on every adoption/release: commanders resync when it changed
	protected int m_iSerial;
	protected int m_iSweeps;

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

		if (changed)
		{
			m_iVersion++;
			LogSummary();
		}
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
		m_aRecords.Insert(record);

		string source = "runtime";
		if (!entityName.IsEmpty())
			source = "scene";
		Print(string.Format("CLW: registry adopted %1 faction=%2 alive=%3 source=%4", id, factionKey, alive, source), LogLevel.NORMAL);
		return true;
	}

	protected void Release(int index, string reason)
	{
		CLW_SquadRecord record = m_aRecords[index];
		Print(string.Format("CLW: registry released %1 reason=%2", record.m_sId, reason), LogLevel.NORMAL);
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

		Print(line, LogLevel.NORMAL);
	}
}

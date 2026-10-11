// Rule-based faction commander v0: turns the faction's contact picture into canned orders
// (MOVE search / ATTACK / DEFEND waypoints) for AI-only squads. Server only, call Think() every ~10 s.
class CLW_RuleCommander
{
	static const ResourceName WAYPOINT_MOVE = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";
	static const ResourceName WAYPOINT_ATTACK = "{B3E7B8DC2BAB8ACC}Prefabs/AI/Waypoints/AIWaypoint_SearchAndDestroy.et";
	static const ResourceName WAYPOINT_DEFEND = "{93291E72AC23930F}Prefabs/AI/Waypoints/AIWaypoint_Defend.et";

	static const float REISSUE_DISTANCE = 50;	// m: target must move this far before the order is re-issued
	static const float DEFEND_TRIGGER = 150;	// m: contact this close to a defending squad keeps it defending
	static const float WEAK_STRENGTH = 0.5;		// agents / start agents below this -> defend

	static const int MODE_NONE = 0;
	static const int MODE_SEARCH = 1;
	static const int MODE_ATTACK = 2;
	static const int MODE_DEFEND = 3;

	protected string m_sFaction;
	protected CLW_ContactBook m_Book;
	protected ref array<string> m_aSquads = {};
	protected ref array<int> m_aStartCount = {};	// agents seen the first time the squad was alive
	protected ref array<int> m_aMode = {};
	protected ref array<int> m_aPatrolIndex = {};
	protected ref array<vector> m_aTarget = {};		// target of the last issued order
	protected ref array<vector> m_aPatrolPoints = {};

	void Init(string factionKey, CLW_ContactBook book, notnull array<string> squadNames)
	{
		m_sFaction = factionKey;
		m_Book = book;
		foreach (string name : squadNames)
		{
			m_aSquads.Insert(name);
			m_aStartCount.Insert(0);
			m_aMode.Insert(MODE_NONE);
			m_aPatrolIndex.Insert(0);
			m_aTarget.Insert(vector.Zero);
		}
	}

	void SetPatrolPoints(notnull array<vector> points)
	{
		m_aPatrolPoints.Copy(points);
	}

	void Think()
	{
		if (!m_Book || !Replication.IsServer())
			return;

		array<CLW_Contact> contacts = {};
		m_Book.GetContacts(contacts);

		for (int i = 0; i < m_aSquads.Count(); i++)
		{
			ThinkSquad(i, contacts);
		}
	}

	protected void ThinkSquad(int i, notnull array<CLW_Contact> contacts)
	{
		SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().GetWorld().FindEntityByName(m_aSquads[i]));
		// A human-led squad gets missions instead of waypoints (VISION 2a); a player who is just a member does not block orders to the AI leader.
		if (!group || HasHumanLeader(group))
			return;

		IEntity leader = group.GetLeaderEntity();
		int agents = group.GetAgentsCount();
		if (!leader || agents <= 0)
			return;	// dead, dormant or not spawned yet

		if (m_aStartCount[i] < agents)
			m_aStartCount[i] = agents;

		vector pos = leader.GetOrigin();
		bool idle = (group.GetCurrentWaypoint() == null);
		bool weak = agents < m_aStartCount[i] * WEAK_STRENGTH;

		CLW_Contact nearest;
		float nearestDist = 1000000;
		foreach (CLW_Contact contact : contacts)
		{
			float dist = vector.DistanceXZ(pos, contact.m_vPos);
			if (dist < nearestDist)
			{
				nearestDist = dist;
				nearest = contact;
			}
		}

		if (!nearest)
		{
			// No contacts: keep the current order, or search the next patrol point when idle.
			if (!idle || m_aPatrolPoints.IsEmpty())
				return;
			vector point = m_aPatrolPoints[m_aPatrolIndex[i] % m_aPatrolPoints.Count()];
			m_aPatrolIndex[i] = m_aPatrolIndex[i] + 1;
			Issue(i, group, MODE_SEARCH, point, "no contacts");
			return;
		}

		bool defending = (m_aMode[i] == MODE_DEFEND);
		if (weak || (defending && nearestDist < DEFEND_TRIGGER))
		{
			// Hold where we are; only (re-)issue when not already defending or the order is gone.
			if (!defending || idle)
				Issue(i, group, MODE_DEFEND, pos, string.Format("strength %1/%2 or contact at %3 m", agents, m_aStartCount[i], Math.Round(nearestDist)));
			return;
		}

		bool targetMoved = vector.DistanceXZ(m_aTarget[i], nearest.m_vPos) > REISSUE_DISTANCE;
		if (idle || m_aMode[i] != MODE_ATTACK || targetMoved)
			Issue(i, group, MODE_ATTACK, nearest.m_vPos, "contact " + nearest.Describe());
	}

	// Replace all waypoints of the group by exactly one of the given mode.
	// True only if one of the squad's players is its leader (GetFirstPlayerLeaderID proved unreliable).
	static bool HasHumanLeader(notnull SCR_AIGroup group)
	{
		array<int> playerIds = group.GetPlayerIDs();
		if (!playerIds)
			return false;

		foreach (int playerId : playerIds)
		{
			if (group.IsPlayerLeader(playerId))
				return true;
		}
		return false;
	}

	protected bool Issue(int i, SCR_AIGroup group, int mode, vector target, string reason)
	{
		ResourceName prefab = WAYPOINT_MOVE;
		string label = "MOVE";
		if (mode == MODE_ATTACK)
		{
			prefab = WAYPOINT_ATTACK;
			label = "ATTACK";
		}
		else if (mode == MODE_DEFEND)
		{
			prefab = WAYPOINT_DEFEND;
			label = "DEFEND";
		}

		ClearWaypoints(group);

		target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = target;

		AIWaypoint waypoint = AIWaypoint.Cast(GetGame().SpawnEntityPrefab(Resource.Load(prefab), GetGame().GetWorld(), params));
		if (!waypoint)
		{
			Print(string.Format("CLW: RuleCommander %1: failed to spawn %2 waypoint for %3", m_sFaction, label, m_aSquads[i]), LogLevel.ERROR);
			return false;
		}

		group.AddWaypoint(waypoint);
		m_aMode[i] = mode;
		m_aTarget[i] = target;
		Print(string.Format("CLW: RuleCommander %1: %2 %3 -> %4 (%5)", m_sFaction, label, m_aSquads[i], target.ToString(), reason), LogLevel.NORMAL);
		return true;
	}

	protected void ClearWaypoints(SCR_AIGroup group)
	{
		array<AIWaypoint> waypoints = {};
		group.GetWaypoints(waypoints);
		foreach (AIWaypoint old : waypoints)
		{
			group.RemoveWaypoint(old);
			SCR_EntityHelper.DeleteEntityAndChildren(old);
		}
	}
}

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
	protected ref CLW_MissionMarker m_Mission = new CLW_MissionMarker();	// the one mission marker of this faction (human squads)
	protected int m_iMissionSquad = -1;	// squad index the marker belongs to

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
		if (!group)
			return;

		bool human = HasHumanLeader(group);
		if (!human && m_Mission.IsShown() && m_iMissionSquad == i)
		{
			// leadership went back to the AI: drop the stale mission marker
			m_Mission.Clear();
			m_iMissionSquad = -1;
		}

		IEntity leader = group.GetLeaderEntity();
		int agents = group.GetAgentsCount();
		if (!leader || agents <= 0)
			return;	// dead, dormant or not spawned yet

		if (m_aStartCount[i] < agents)
			m_aStartCount[i] = agents;

		vector pos = leader.GetOrigin();
		// a human squad has no waypoints: it is "idle" until its first mission was delivered
		bool idle = (group.GetCurrentWaypoint() == null);
		if (human)
			idle = (m_aMode[i] == MODE_NONE);
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
			Deliver(human, i, group, MODE_SEARCH, point, "no contacts");
			return;
		}

		bool defending = (m_aMode[i] == MODE_DEFEND);
		if (weak || (defending && nearestDist < DEFEND_TRIGGER))
		{
			// Hold where we are; only (re-)issue when not already defending or the order is gone.
			if (!defending || idle)
				Deliver(human, i, group, MODE_DEFEND, pos, string.Format("strength %1/%2 or contact at %3 m", agents, m_aStartCount[i], Math.Round(nearestDist)));
			return;
		}

		bool targetMoved = vector.DistanceXZ(m_aTarget[i], nearest.m_vPos) > REISSUE_DISTANCE;
		if (idle || m_aMode[i] != MODE_ATTACK || targetMoved)
			Deliver(human, i, group, MODE_ATTACK, nearest.m_vPos, "contact " + nearest.Describe());
	}

	// Replace all waypoints of the group by exactly one of the given mode.
	// True only if the squad's leader soldier is actually controlled by a player. The group flags
	// (IsPlayerLeader, GetLeaderID, GetFirstPlayerLeaderID) say a joined player "leads" while an AI
	// soldier still holds the leader entity (verified 2026-10-11: "commanding only for group leaders").
	static bool HasHumanLeader(notnull SCR_AIGroup group)
	{
		IEntity leader = group.GetLeaderEntity();
		if (!leader)
			return false;

		return GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(leader) > 0;
	}

	// Same decision, two deliveries: waypoint for AI squads, message + map marker for human-led squads.
	protected void Deliver(bool human, int i, SCR_AIGroup group, int mode, vector target, string reason)
	{
		if (human)
			IssueMission(i, mode, target, reason);
		else
			Issue(i, group, mode, target, reason);
	}

	// Deliver the order to a human squad leader as a mission (chat/debug message + faction-only map marker).
	protected void IssueMission(int i, int mode, vector target, string reason)
	{
		string label = "Search";
		if (mode == MODE_ATTACK)
			label = "Attack";
		else if (mode == MODE_DEFEND)
			label = "Defend";

		target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
		m_aMode[i] = mode;
		m_aTarget[i] = target;

		m_Mission.Show(m_sFaction, "MISSION: " + label, target);
		m_iMissionSquad = i;

		string msg = string.Format("MISSION %1 (%2): %3 at %4 / %5 (%6)", m_aSquads[i], m_sFaction, label, Math.Round(target[0]), Math.Round(target[2]), reason);
		CLW_Debug.Say(msg);
		Print("CLW: RuleCommander " + msg, LogLevel.NORMAL);
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
		CLW_Debug.Say(string.Format("%1: %2 %3 -> %4 (%5)", m_sFaction, label, m_aSquads[i], target.ToString(), reason));
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

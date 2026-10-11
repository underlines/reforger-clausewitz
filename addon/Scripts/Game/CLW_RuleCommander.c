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
	protected ref array<string> m_aReason = {};		// reason of the last issued order
	protected ref array<vector> m_aLastPos = {};	// leader position at the previous BuildSituation (speed/heading)
	protected ref array<float> m_aLastTime = {};	// world time (s) of that sample, -1 = none
	protected ref array<vector> m_aPatrolPoints = {};
	static const ResourceName TASK_PREFAB = "{1D0F815858EE24AD}Prefabs/Tasks/BaseTask.et";	// vanilla generic SCR_Task
	protected static int s_iMissionCounter;	// unique task IDs (a deleted task's ID may linger in the task system for a moment)
	protected SCR_Task m_MissionTask;		// the one vanilla task (J menu + map) of this faction's human squad
	protected int m_iMissionSquad = -1;	// squad index the task belongs to

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
			m_aReason.Insert("");
			m_aLastPos.Insert(vector.Zero);
			m_aLastTime.Insert(-1);
		}
	}

	void SetPatrolPoints(notnull array<vector> points)
	{
		m_aPatrolPoints.Copy(points);
	}

	// Human-led squads that never got a mission (e.g. the LLM ignored them) get one from the rules, so they always have an objective.
	void ThinkHumanLed()
	{
		if (!m_Book || !Replication.IsServer())
			return;

		array<CLW_Contact> contacts = {};
		m_Book.GetContacts(contacts);
		for (int i = 0; i < m_aSquads.Count(); i++)
		{
			if (m_aMode[i] != MODE_NONE)
				continue;

			SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().GetWorld().FindEntityByName(m_aSquads[i]));
			if (group && HasHumanLeader(group))
				ThinkSquad(i, contacts);
		}
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

	// ---- Remote (LLM) commander interface -------------------------------------------------------

	static string ModeName(int mode)
	{
		if (mode == MODE_SEARCH)
			return "SEARCH";
		if (mode == MODE_ATTACK)
			return "ATTACK";
		if (mode == MODE_DEFEND)
			return "DEFEND";
		return "NONE";
	}

	// Escape a string for use inside a JSON string literal.
	static string JsonEscape(string text)
	{
		string r = text;
		r.Replace("\\", "\\\\");
		r.Replace("\"", "\\\"");
		r.Replace("\n", " ");
		r.Replace("\r", " ");
		r.Replace("\t", " ");
		return r;
	}

	// Serialise this faction's situation as the sidecar's POST /orders body. Returns the number of live squads in it.
	int BuildSituation(out string json)
	{
		json = "";
		if (!m_Book)
			return 0;

		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		array<CLW_Contact> contacts = {};
		m_Book.GetContacts(contacts);
		string squadsJson = "";
		int squadCount = 0;
		for (int i = 0; i < m_aSquads.Count(); i++)
		{
			SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().GetWorld().FindEntityByName(m_aSquads[i]));
			if (!group)
				continue;

			IEntity leader = group.GetLeaderEntity();
			int agents = CLW_SquadInfo.AliveCount(group);
			if (!leader || agents <= 0)
				continue;

			if (m_aStartCount[i] < agents)
				m_aStartCount[i] = agents;

			vector pos = leader.GetOrigin();
			string humanText = "false";
			if (HasHumanLeader(group))
				humanText = "true";

			// optional extras: speed/heading from the position delta since the previous report
			float speed = 0;
			float heading = leader.GetYawPitchRoll()[0];
			if (m_aLastTime[i] >= 0 && now > m_aLastTime[i])
			{
				vector delta = pos - m_aLastPos[i];
				float planar = Math.Sqrt(delta[0] * delta[0] + delta[2] * delta[2]);
				speed = planar / (now - m_aLastTime[i]);
				if (planar > 2)
					heading = Math.Atan2(delta[0], delta[2]) * Math.RAD2DEG;
			}
			m_aLastPos[i] = pos;
			m_aLastTime[i] = now;
			int headingDeg = Math.Round(heading);
			headingDeg = headingDeg % 360;
			if (headingDeg < 0)
				headingDeg += 360;

			int wounded;
			float avgHealth;
			CLW_SquadInfo.Health(group, wounded, avgHealth);

			bool engaged = false;
			foreach (CLW_Contact seen : contacts)
			{
				if (vector.DistanceXZ(pos, seen.m_vPos) < DEFEND_TRIGGER)
				{
					engaged = true;
					break;
				}
			}
			string stateText = "idle";
			if (engaged)
				stateText = "engaged";
			else if (m_aMode[i] == MODE_SEARCH)
				stateText = "moving";
			else if (m_aMode[i] == MODE_ATTACK)
				stateText = "attacking";
			else if (m_aMode[i] == MODE_DEFEND)
				stateText = "defending";

			string objectiveJson = "";
			if (m_aMode[i] != MODE_NONE)
			{
				string cmd = "MOVE";
				if (m_aMode[i] == MODE_ATTACK)
					cmd = "ATTACK";
				else if (m_aMode[i] == MODE_DEFEND)
					cmd = "DEFEND";
				objectiveJson = string.Format(",\"objective\":{\"command\":\"%1\",\"pos\":[%2,%3],\"reason\":\"%4\"}",
					cmd, Math.Round(m_aTarget[i][0]), Math.Round(m_aTarget[i][2]), JsonEscape(m_aReason[i]));
			}

			if (squadCount > 0)
				squadsJson += ",";
			squadsJson += string.Format("{\"id\":\"%1\",\"pos\":[%2,%3,%4],\"agents\":%5,\"startAgents\":%6,\"mode\":\"%7\",\"humanLed\":%8",
				JsonEscape(m_aSquads[i]), Math.Round(pos[0]), Math.Round(pos[1]), Math.Round(pos[2]), agents, m_aStartCount[i], ModeName(m_aMode[i]), humanText);
			squadsJson += string.Format(",\"type\":\"%1\",\"wounded\":%2,\"health\":%3,\"speed\":%4,\"heading\":%5,\"state\":\"%6\"",
				JsonEscape(CLW_SquadInfo.TypeName(group)), wounded, avgHealth.ToString(-1, 2), speed.ToString(-1, 1), headingDeg, stateText);
			squadsJson += objectiveJson + "}";
			squadCount++;
		}

		string contactsJson = "";
		int contactCount = 0;
		foreach (CLW_Contact contact : contacts)
		{
			if (contactCount > 0)
				contactsJson += ",";
			string desc = string.Format("%1x %2", contact.m_iCount, contact.m_sEnemyFaction);
			contactsJson += string.Format("{\"pos\":[%1,%2,%3],\"desc\":\"%4\",\"ageSec\":%5}",
				Math.Round(contact.m_vPos[0]), Math.Round(contact.m_vPos[1]), Math.Round(contact.m_vPos[2]), JsonEscape(desc), Math.Round(Math.Max(0, now - contact.m_fLastSeen)));
			contactCount++;
		}

		json = string.Format("{\"faction\":\"%1\",\"time\":%2,\"squads\":[%3],\"contacts\":[%4]}", JsonEscape(m_sFaction), Math.Round(now), squadsJson, contactsJson);
		return squadCount;
	}

	// Execute one external order. command: MOVE | ATTACK | DEFEND. False for unknown squad, dead group or bad command.
	bool ApplyOrder(string squadName, string command, vector target, string reason)
	{
		int mode = MODE_NONE;
		if (command == "MOVE")
			mode = MODE_SEARCH;
		else if (command == "ATTACK")
			mode = MODE_ATTACK;
		else if (command == "DEFEND")
			mode = MODE_DEFEND;
		else
			return false;

		int i = m_aSquads.Find(squadName);
		if (i < 0)
			return false;

		SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().GetWorld().FindEntityByName(squadName));
		if (!group || !group.GetLeaderEntity() || CLW_SquadInfo.AliveCount(group) <= 0)
			return false;

		if (m_aStartCount[i] < CLW_SquadInfo.AliveCount(group))
			m_aStartCount[i] = CLW_SquadInfo.AliveCount(group);

		bool human = HasHumanLeader(group);
		if (!human && m_MissionTask && m_iMissionSquad == i)
		{
			ClearMissionTask();
			m_iMissionSquad = -1;
		}

		Deliver(human, i, group, mode, target, reason);
		return true;
	}

	protected void ThinkSquad(int i, notnull array<CLW_Contact> contacts)
	{
		SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().GetWorld().FindEntityByName(m_aSquads[i]));
		// A human-led squad gets missions instead of waypoints (VISION 2a); a player who is just a member does not block orders to the AI leader.
		if (!group)
			return;

		bool human = HasHumanLeader(group);
		if (!human && m_MissionTask && m_iMissionSquad == i)
		{
			// leadership went back to the AI: drop the stale mission task
			ClearMissionTask();
			m_iMissionSquad = -1;
		}

		IEntity leader = group.GetLeaderEntity();
		int agents = CLW_SquadInfo.AliveCount(group);
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
			IssueMission(i, group, mode, target, reason);
		else
			Issue(i, group, mode, target, reason);
	}

	protected void ClearMissionTask()
	{
		if (!m_MissionTask)
			return;

		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (taskSystem)
			taskSystem.DeleteTask(m_MissionTask);

		m_MissionTask = null;
	}

	// Create (or update in place) the squad's vanilla task: shown in the J menu and on the map of that group only.
	protected void ShowMissionTask(int i, SCR_AIGroup group, string label, vector target, string reason)
	{
		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (!taskSystem || !group)
		{
			Print("CLW: RuleCommander: no SCR_TaskSystem / group, mission task skipped", LogLevel.WARNING);
			return;
		}

		string desc = string.Format("%1 (%2 / %3)", reason, Math.Round(target[0]), Math.Round(target[2]));
		if (m_MissionTask && m_iMissionSquad == i)
		{
			// same squad: keep the task (and its assignment), just change what it says and where it is
			taskSystem.MoveTask(m_MissionTask, target);
			m_MissionTask.SetTaskName(label);
			m_MissionTask.SetTaskDescription(desc);
			return;
		}

		ClearMissionTask();

		s_iMissionCounter++;
		string taskId = "CLW_Mission_" + m_sFaction + "_" + s_iMissionCounter;
		SCR_Task task = taskSystem.CreateTask(TASK_PREFAB, taskId, label, desc, target);
		if (!task)
		{
			Print("CLW: RuleCommander: CreateTask failed for " + taskId, LogLevel.WARNING);
			return;
		}

		// Address the task to the leading PLAYER, not the group: giving the squad a real GroupID (AssignGroupID) silences the commander voice and move marker.
		IEntity leader = group.GetLeaderEntity();
		int playerId = 0;
		if (leader)
			playerId = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(leader);
		taskSystem.SetTaskOwnership(task, SCR_ETaskOwnership.FACTION);
		taskSystem.SetTaskVisibility(task, SCR_ETaskVisibility.ASSIGNEES);
		taskSystem.AddTaskFaction(task, m_sFaction);

		SCR_TaskExecutor executor;
		if (playerId > 0)
			executor = SCR_TaskExecutor.FromPlayerID(playerId);
		if (executor)
			taskSystem.AssignTask(task, executor, true);

		m_MissionTask = task;
		m_iMissionSquad = i;
	}

	// Deliver the order to a human squad leader as a mission (chat/debug message + vanilla task for the squad).
	protected void IssueMission(int i, SCR_AIGroup group, int mode, vector target, string reason)
	{
		string label = "Search";
		if (mode == MODE_ATTACK)
			label = "Attack";
		else if (mode == MODE_DEFEND)
			label = "Defend";

		target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
		m_aMode[i] = mode;
		m_aTarget[i] = target;
		m_aReason[i] = reason;

		ShowMissionTask(i, group, label, target, reason);

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
		m_aReason[i] = reason;
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

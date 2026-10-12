// Per-squad commander state, keyed by the squad id from CLW_SquadRegistry.
class CLW_SquadState
{
	int m_iSerial;						// registry adoption serial: a re-adopted group starts with fresh state
	int m_iStartCount;					// agents seen the first time the squad was alive
	int m_iMode;
	int m_iPatrolIndex;
	vector m_vTarget;					// target of the last issued order
	string m_sReason;					// reason of the last issued order
	vector m_vLastPos;					// leader position at the previous BuildSituation (speed/heading)
	float m_fLastTime = -1;				// world time (s) of that sample, -1 = none
}

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
	protected CLW_SquadRegistry m_Registry;	// weak: owned by the game mode
	protected ref array<string> m_aSquads = {};	// ids of this faction's adopted squads (registry order), kept in sync by Sync()
	protected ref map<string, ref CLW_SquadState> m_mState = new map<string, ref CLW_SquadState>();
	protected int m_iSeenVersion = -1;
	protected ref array<vector> m_aPatrolPoints = {};
	static const ResourceName TASK_PREFAB = "{1D0F815858EE24AD}Prefabs/Tasks/BaseTask.et";	// vanilla generic SCR_Task
	protected static int s_iMissionCounter;	// unique task IDs (a deleted task's ID may linger in the task system for a moment)
	protected SCR_Task m_MissionTask;		// the one vanilla task (J menu + map) of this faction's human squad
	protected string m_sMissionSquad;	// squad id the task belongs to ("" = none)

	// Squads come from the registry (all adopted groups of this faction, including ones added at runtime).
	void Init(string factionKey, CLW_ContactBook book, CLW_SquadRegistry registry)
	{
		m_sFaction = factionKey;
		m_Book = book;
		m_Registry = registry;
		Sync();
	}

	// Follow additions/removals in the registry: new ids get fresh state (start strength = alive count at adoption),
	// ids that vanished lose their state (and the mission task if it was theirs).
	protected void Sync()
	{
		if (!m_Registry || m_iSeenVersion == m_Registry.GetVersion())
			return;

		m_iSeenVersion = m_Registry.GetVersion();
		array<string> ids = {};
		m_Registry.GetIds(m_sFaction, ids);

		for (int k = m_aSquads.Count() - 1; k >= 0; k--)
		{
			string known = m_aSquads[k];
			CLW_SquadState old = m_mState.Get(known);
			if (ids.Contains(known) && old && old.m_iSerial == m_Registry.GetSerial(known))
				continue;

			if (known == m_sMissionSquad)
			{
				ClearMissionTask();
				m_sMissionSquad = "";
			}
			m_mState.Remove(known);
			m_aSquads.Remove(k);
		}

		foreach (string id : ids)
		{
			if (m_mState.Contains(id))
				continue;

			CLW_SquadState fresh = new CLW_SquadState();
			fresh.m_iSerial = m_Registry.GetSerial(id);
			fresh.m_iStartCount = m_Registry.GetStartStrength(id);
			m_mState.Insert(id, fresh);
			m_aSquads.Insert(id);
		}
	}

	protected CLW_SquadState State(string id)
	{
		return m_mState.Get(id);
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

		Sync();
		array<CLW_Contact> contacts = {};
		m_Book.GetContacts(contacts);
		foreach (string id : m_aSquads)
		{
			if (State(id).m_iMode != MODE_NONE)
				continue;

			SCR_AIGroup group = m_Registry.Find(id);
			if (group && HasHumanLeader(group))
				ThinkSquad(id, contacts);
		}
	}

	void Think()
	{
		if (!m_Book || !Replication.IsServer())
			return;

		Sync();
		array<CLW_Contact> contacts = {};
		m_Book.GetContacts(contacts);

		foreach (string id : m_aSquads)
		{
			ThinkSquad(id, contacts);
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

		Sync();
		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		array<CLW_Contact> contacts = {};
		m_Book.GetContacts(contacts);
		string squadsJson = "";
		int squadCount = 0;
		foreach (string squadId : m_aSquads)
		{
			SCR_AIGroup group = m_Registry.Find(squadId);
			if (!group)
				continue;

			CLW_SquadState st = State(squadId);
			IEntity leader = CLW_SquadInfo.LeaderEntity(group);
			int agents = CLW_SquadInfo.AliveCount(group);
			if (!leader || agents <= 0)
				continue;

			if (st.m_iStartCount < agents)
				st.m_iStartCount = agents;

			vector pos = leader.GetOrigin();
			string humanText = "false";
			if (HasHumanLeader(group))
				humanText = "true";

			// optional extras: speed/heading from the position delta since the previous report
			float speed = 0;
			float heading = leader.GetYawPitchRoll()[0];
			if (st.m_fLastTime >= 0 && now > st.m_fLastTime)
			{
				vector delta = pos - st.m_vLastPos;
				float planar = Math.Sqrt(delta[0] * delta[0] + delta[2] * delta[2]);
				speed = planar / (now - st.m_fLastTime);
				if (planar > 2)
					heading = Math.Atan2(delta[0], delta[2]) * Math.RAD2DEG;
			}
			st.m_vLastPos = pos;
			st.m_fLastTime = now;
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
			else if (st.m_iMode == MODE_SEARCH)
				stateText = "moving";
			else if (st.m_iMode == MODE_ATTACK)
				stateText = "attacking";
			else if (st.m_iMode == MODE_DEFEND)
				stateText = "defending";

			string objectiveJson = "";
			if (st.m_iMode != MODE_NONE)
			{
				string cmd = "MOVE";
				if (st.m_iMode == MODE_ATTACK)
					cmd = "ATTACK";
				else if (st.m_iMode == MODE_DEFEND)
					cmd = "DEFEND";
				objectiveJson = string.Format(",\"objective\":{\"command\":\"%1\",\"pos\":[%2,%3],\"reason\":\"%4\"}",
					cmd, Math.Round(st.m_vTarget[0]), Math.Round(st.m_vTarget[2]), JsonEscape(st.m_sReason));
			}

			if (squadCount > 0)
				squadsJson += ",";
			squadsJson += string.Format("{\"id\":\"%1\",\"pos\":[%2,%3,%4],\"agents\":%5,\"startAgents\":%6,\"mode\":\"%7\",\"humanLed\":%8",
				JsonEscape(squadId), Math.Round(pos[0]), Math.Round(pos[1]), Math.Round(pos[2]), agents, st.m_iStartCount, ModeName(st.m_iMode), humanText);
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

		// concatenate: string.Format truncates its result at ~8 KB
		json = "{\"faction\":\"" + JsonEscape(m_sFaction) + "\",\"time\":" + Math.Round(now) + ",\"squads\":[" + squadsJson + "],\"contacts\":[" + contactsJson + "]}";
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

		Sync();
		CLW_SquadState st = State(squadName);
		if (!st)
			return false;

		SCR_AIGroup group = m_Registry.Find(squadName);
		if (!group || !CLW_SquadInfo.LeaderEntity(group) || CLW_SquadInfo.AliveCount(group) <= 0)
			return false;

		if (st.m_iStartCount < CLW_SquadInfo.AliveCount(group))
			st.m_iStartCount = CLW_SquadInfo.AliveCount(group);

		bool human = HasHumanLeader(group);
		if (!human && m_MissionTask && m_sMissionSquad == squadName)
		{
			ClearMissionTask();
			m_sMissionSquad = "";
		}

		Deliver(human, squadName, group, mode, target, reason);
		return true;
	}

	protected void ThinkSquad(string squadId, notnull array<CLW_Contact> contacts)
	{
		SCR_AIGroup group = m_Registry.Find(squadId);
		CLW_SquadState st = State(squadId);
		// A human-led squad gets missions instead of waypoints (VISION 2a); a player who is just a member does not block orders to the AI leader.
		if (!group || !st)
			return;

		bool human = HasHumanLeader(group);
		if (!human && m_MissionTask && m_sMissionSquad == squadId)
		{
			// leadership went back to the AI: drop the stale mission task
			ClearMissionTask();
			m_sMissionSquad = "";
		}

		IEntity leader = CLW_SquadInfo.LeaderEntity(group);
		int agents = CLW_SquadInfo.AliveCount(group);
		if (!leader || agents <= 0)
			return;	// dead, dormant or not spawned yet

		if (st.m_iStartCount < agents)
			st.m_iStartCount = agents;

		vector pos = leader.GetOrigin();
		// a human squad has no waypoints: it is "idle" until its first mission was delivered
		bool idle = (group.GetCurrentWaypoint() == null);
		if (human)
			idle = (st.m_iMode == MODE_NONE);
		bool weak = agents < st.m_iStartCount * WEAK_STRENGTH;

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
			vector point = m_aPatrolPoints[st.m_iPatrolIndex % m_aPatrolPoints.Count()];
			st.m_iPatrolIndex = st.m_iPatrolIndex + 1;
			Deliver(human, squadId, group, MODE_SEARCH, point, "no contacts");
			return;
		}

		bool defending = (st.m_iMode == MODE_DEFEND);
		if (weak || (defending && nearestDist < DEFEND_TRIGGER))
		{
			// Hold where we are; only (re-)issue when not already defending or the order is gone.
			if (!defending || idle)
				Deliver(human, squadId, group, MODE_DEFEND, pos, string.Format("strength %1/%2 or contact at %3 m", agents, st.m_iStartCount, Math.Round(nearestDist)));
			return;
		}

		bool targetMoved = vector.DistanceXZ(st.m_vTarget, nearest.m_vPos) > REISSUE_DISTANCE;
		if (idle || st.m_iMode != MODE_ATTACK || targetMoved)
			Deliver(human, squadId, group, MODE_ATTACK, nearest.m_vPos, "contact " + nearest.Describe());
	}

	// Replace all waypoints of the group by exactly one of the given mode.
	// True only if the squad's leader soldier is actually controlled by a player. The group flags
	// (IsPlayerLeader, GetLeaderID, GetFirstPlayerLeaderID) say a joined player "leads" while an AI
	// soldier still holds the leader entity (verified 2026-10-11: "commanding only for group leaders").
	static bool HasHumanLeader(notnull SCR_AIGroup group)
	{
		return CLW_SquadInfo.HumanLeaderEntity(group) != null;
	}

	// Same decision, two deliveries: waypoint for AI squads, message + map marker for human-led squads.
	protected void Deliver(bool human, string squadId, SCR_AIGroup group, int mode, vector target, string reason)
	{
		if (human)
			IssueMission(squadId, group, mode, target, reason);
		else
			Issue(squadId, group, mode, target, reason);
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
	protected void ShowMissionTask(string squadId, SCR_AIGroup group, string label, vector target, string reason)
	{
		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (!taskSystem || !group)
		{
			Print("CLW: RuleCommander: no SCR_TaskSystem / group, mission task skipped", LogLevel.WARNING);
			return;
		}

		string desc = string.Format("%1 (%2 / %3)", reason, Math.Round(target[0]), Math.Round(target[2]));
		if (m_MissionTask && m_sMissionSquad == squadId)
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
		IEntity leader = CLW_SquadInfo.HumanLeaderEntity(group);
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
		m_sMissionSquad = squadId;
	}

	// Deliver the order to a human squad leader as a mission (chat/debug message + vanilla task for the squad).
	protected void IssueMission(string squadId, SCR_AIGroup group, int mode, vector target, string reason)
	{
		string label = "Search";
		if (mode == MODE_ATTACK)
			label = "Attack";
		else if (mode == MODE_DEFEND)
			label = "Defend";

		target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
		CLW_SquadState st = State(squadId);
		st.m_iMode = mode;
		st.m_vTarget = target;
		st.m_sReason = reason;

		ShowMissionTask(squadId, group, label, target, reason);

		string msg = string.Format("MISSION %1 (%2): %3 at %4 / %5 (%6)", squadId, m_sFaction, label, Math.Round(target[0]), Math.Round(target[2]), reason);
		CLW_Debug.Say(msg);
		Print("CLW: RuleCommander " + msg, LogLevel.NORMAL);
	}

	protected bool Issue(string squadId, SCR_AIGroup group, int mode, vector target, string reason)
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
			Print(string.Format("CLW: RuleCommander %1: failed to spawn %2 waypoint for %3", m_sFaction, label, squadId), LogLevel.ERROR);
			return false;
		}

		group.AddWaypoint(waypoint);
		CLW_SquadState st = State(squadId);
		st.m_iMode = mode;
		st.m_vTarget = target;
		st.m_sReason = reason;
		Print(string.Format("CLW: RuleCommander %1: %2 %3 -> %4 (%5)", m_sFaction, label, squadId, target.ToString(), reason), LogLevel.NORMAL);
		CLW_Debug.Say(string.Format("%1: %2 %3 -> %4 (%5)", m_sFaction, label, squadId, target.ToString(), reason));
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

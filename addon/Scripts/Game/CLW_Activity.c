// Activity management (AI LOD). Far from every player the engine puts AI agents into the maximum LOD where they barely act
// (observed: enemy vehicles only moved when a player flew near). We do NOT keep everything at full detail (too expensive):
// every ACTIVITY_TICK_MS a commanded unit (registry adopted group, incl. vehicle crews, linked cargo and slave groups) is
// scored for RELEVANCE and the best ones, within a per-side agent budget (war mode: m_aActiveAgentCaps), get PreventMaxLOD.
// Everything else gets AllowMaxLOD after ACTIVITY_HOLD_SEC without relevance (the engine decides again).
//
// Relevant when any of: (a) a real enemy (adopted unit of the other side, or an enemy-faction player) within
// ACTIVITY_ENEMY_RANGE_M; (b) an active HQ order, or a vehicle unit that is driving / dismounting / remounting; (c) inside the
// radius (+ margin) of a contested objective; (d) led by a human player.
// Score (for the budget): human 100, enemy 0..80 (closer = higher), vehicle 60, contested 50, order 40, held-by-hysteresis 10.
// Switch: CLW_ScenarioConfig.m_bActivityManagement (off = no calls at all, plain engine behaviour). Base economy is script-only
// and unaffected. Server only.
class CLW_ActivityState
{
	float m_fLastRelevant = -100000;
	bool m_bPrevented;
	int m_iScore;
}

class CLW_ActivityUnit
{
	string m_sId;
	string m_sFaction;
	SCR_AIGroup m_Group;
	vector m_vPos;
	int m_iScore;
	int m_iAgents;
	bool m_bRelevant;
}

class CLW_Activity
{
	protected ref map<string, ref CLW_ActivityState> m_mStates = new map<string, ref CLW_ActivityState>();
	protected float m_fLastLog = -100000;
	protected bool m_bOffHandled;
	protected bool m_bRunning;
	// player positions per faction key, refreshed every tick
	protected ref map<string, ref array<vector>> m_mPlayers = new map<string, ref array<vector>>();

	void Start()
	{
		if (!Replication.IsServer() || m_bRunning)
			return;

		m_bRunning = true;
		GetGame().GetCallqueue().CallLater(Tick, CLW_WarConfig.ACTIVITY_TICK_MS, true);
		Print("CLW: activity management ready (relevance based AI LOD, tick " + CLW_WarConfig.ACTIVITY_TICK_MS.ToString() + " ms)", LogLevel.NORMAL);
	}

	// ---- helpers ----

	protected static void CollectAgents(SCR_AIGroup group, notnull array<AIAgent> agents)
	{
		if (!group)
			return;

		group.GetAgents(agents);
		SCR_AIGroup slave = group.GetSlave();
		if (slave)
		{
			array<AIAgent> slaveAgents = {};
			slave.GetAgents(slaveAgents);
			foreach (AIAgent slaveAgent : slaveAgents)
			{
				agents.Insert(slaveAgent);
			}
		}
	}

	protected static void SetPrevented(SCR_AIGroup group, bool prevent)
	{
		if (!group)
			return;

		array<AIAgent> agents = {};
		CollectAgents(group, agents);
		// PreventMaxLOD only stops an agent from ENTERING the max LOD: an agent already there must be lifted out first
		// (vanilla SCR_ResupplyTaskSolver: SetLOD(GetMaxLOD() - 1), then PreventMaxLOD). Seen 2026-10-12: without the lift
		// all far agents stayed at max LOD although "prevented".
		int liftLod = AIAgent.GetMaxLOD() - 1;
		Lift(group, prevent, liftLod);
		Lift(group.GetSlave(), prevent, liftLod);
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			if (prevent)
			{
				if (agent.GetLOD() == AIAgent.GetMaxLOD())
					agent.SetLOD(liftLod);
				agent.PreventMaxLOD(CLW_WarConfig.ACTIVITY_PREVENT_SEC);
			}
			else
			{
				agent.AllowMaxLOD();
			}
		}
	}

	// The group agent itself (and its slave group): same lift + prevent as the members.
	protected static void Lift(SCR_AIGroup group, bool prevent, int liftLod)
	{
		if (!group)
			return;

		if (!prevent)
		{
			group.AllowMaxLOD();
			return;
		}

		if (group.GetLOD() == AIAgent.GetMaxLOD())
			group.SetLOD(liftLod);
		group.PreventMaxLOD(CLW_WarConfig.ACTIVITY_PREVENT_SEC);
	}

	protected static vector UnitPosition(SCR_AIGroup group)
	{
		IEntity leader = CLW_SquadInfo.LeaderEntity(group);
		if (leader)
			return leader.GetOrigin();

		return group.GetOrigin();
	}

	// Faction key of a player's controlled character ("" when unknown).
	protected static string FactionKeyOf(IEntity entity)
	{
		FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(entity.FindComponent(FactionAffiliationComponent));
		if (!affiliation)
			return "";

		Faction faction = affiliation.GetAffiliatedFaction();
		if (!faction)
			return "";

		return faction.GetFactionKey();
	}

	protected void RefreshPlayers()
	{
		m_mPlayers.Clear();
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			IEntity entity = players.GetPlayerControlledEntity(playerId);
			if (!entity || !CLW_VehicleUnit.IsAlive(entity))
				continue;

			string key = FactionKeyOf(entity);
			if (key.IsEmpty())
				continue;

			array<vector> list;
			if (!m_mPlayers.Find(key, list) || !list)
			{
				list = {};
				m_mPlayers.Set(key, list);
			}

			list.Insert(entity.GetOrigin());
		}
	}

	// ---- tick ----

	protected void Tick()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!scenario || !registry)
			return;

		if (!scenario.m_bActivityManagement)
		{
			ReleaseAll(registry, scenario);
			return;
		}

		m_bOffHandled = false;
		float now = CLW_Orders.Now();
		RefreshPlayers();
		array<string> keys = {};
		scenario.GetFactionKeys(keys);

		// 1. units per faction
		map<string, ref array<ref CLW_ActivityUnit>> byFaction = new map<string, ref array<ref CLW_ActivityUnit>>();
		foreach (string key : keys)
		{
			array<ref CLW_ActivityUnit> list = {};
			byFaction.Set(key, list);
			array<string> ids = {};
			registry.GetIds(key, ids);
			foreach (string id : ids)
			{
				SCR_AIGroup group = registry.Find(id);
				if (!group || CLW_SquadInfo.AliveCount(group) <= 0)
					continue;

				CLW_ActivityUnit unit = new CLW_ActivityUnit();
				unit.m_sId = id;
				unit.m_sFaction = key;
				unit.m_Group = group;
				unit.m_vPos = UnitPosition(group);
				list.Insert(unit);
			}
		}

		// 2. relevance
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		map<string, bool> seen = new map<string, bool>();
		foreach (string key2 : keys)
		{
			array<ref CLW_ActivityUnit> mine = byFaction.Get(key2);
			foreach (CLW_ActivityUnit unit2 : mine)
			{
				unit2.m_iScore = ScoreOf(unit2, key2, keys, byFaction, points);
				CLW_ActivityState state;
				if (!m_mStates.Find(unit2.m_sId, state) || !state)
				{
					state = new CLW_ActivityState();
					m_mStates.Set(unit2.m_sId, state);
				}

				seen.Set(unit2.m_sId, true);
				if (unit2.m_iScore > 0)
				{
					state.m_fLastRelevant = now;
					unit2.m_bRelevant = true;
				}
				else if (now - state.m_fLastRelevant <= CLW_WarConfig.ACTIVITY_HOLD_SEC)
				{
					unit2.m_iScore = CLW_WarConfig.ACTIVITY_SCORE_HELD;
					unit2.m_bRelevant = true;
				}

				state.m_iScore = unit2.m_iScore;
				array<AIAgent> agents = {};
				CollectAgents(unit2.m_Group, agents);
				unit2.m_iAgents = agents.Count();
			}
		}

		// forget vanished units
		array<string> gone = {};
		foreach (string stateId, CLW_ActivityState oldState : m_mStates)
		{
			if (!seen.Contains(stateId))
				gone.Insert(stateId);
		}

		foreach (string goneId : gone)
		{
			m_mStates.Remove(goneId);
		}

		// 3. budget per faction, apply
		bool log = now - m_fLastLog >= CLW_WarConfig.ACTIVITY_LOG_SEC;
		if (log)
			m_fLastLog = now;

		CLW_WarState war = CLW_GameMode.GetWarState();
		foreach (string key3 : keys)
		{
			Apply(key3, byFaction.Get(key3), war, log);
		}
	}

	protected void Apply(string factionKey, notnull array<ref CLW_ActivityUnit> units, CLW_WarState war, bool log)
	{
		int cap = CLW_WarConfig.ACTIVITY_DEFAULT_CAP;
		if (war)
			cap = war.GetActiveAgentCap(factionKey);

		// best first (selection sort on score: the list is small)
		array<CLW_ActivityUnit> order = {};
		foreach (CLW_ActivityUnit unit : units)
		{
			if (unit.m_bRelevant)
				order.Insert(unit);
		}

		for (int i = 0; i < order.Count(); i++)
		{
			int best = i;
			for (int j = i + 1; j < order.Count(); j++)
			{
				if (order[j].m_iScore > order[best].m_iScore)
					best = j;
			}

			if (best != i)
			{
				CLW_ActivityUnit swap = order[i];
				order[i] = order[best];
				order[best] = swap;
			}
		}

		int budget = cap;
		int prevented;
		int relevant = order.Count();
		foreach (CLW_ActivityUnit chosen : order)
		{
			CLW_ActivityState state = m_mStates.Get(chosen.m_sId);
			if (chosen.m_iAgents <= budget)
			{
				budget -= chosen.m_iAgents;
				prevented += chosen.m_iAgents;
				SetPrevented(chosen.m_Group, true);
				if (state && !state.m_bPrevented)
				{
					state.m_bPrevented = true;
					Print(string.Format("CLW: activity %1 %2 ACTIVE (score %3, %4 agents)", factionKey, chosen.m_sId, chosen.m_iScore, chosen.m_iAgents), LogLevel.NORMAL);
				}
			}
			else
			{
				chosen.m_bRelevant = false;	// over budget
				if (state && state.m_bPrevented)
				{
					state.m_bPrevented = false;
					SetPrevented(chosen.m_Group, false);
					Print(string.Format("CLW: activity %1 %2 over budget (score %3, %4 agents, cap %5): released", factionKey, chosen.m_sId, chosen.m_iScore, chosen.m_iAgents, cap), LogLevel.NORMAL);
				}
			}
		}

		int atMax;
		int agentsTotal;
		foreach (CLW_ActivityUnit unit3 : units)
		{
			CLW_ActivityState state3 = m_mStates.Get(unit3.m_sId);
			if (!unit3.m_bRelevant && state3 && state3.m_bPrevented)
			{
				state3.m_bPrevented = false;
				SetPrevented(unit3.m_Group, false);
				Print(string.Format("CLW: activity %1 %2 idle for %3 s: engine LOD allowed", factionKey, unit3.m_sId, CLW_WarConfig.ACTIVITY_HOLD_SEC), LogLevel.NORMAL);
			}

			if (log)
			{
				array<AIAgent> agents = {};
				CollectAgents(unit3.m_Group, agents);
				foreach (AIAgent agent : agents)
				{
					if (!agent)
						continue;

					agentsTotal++;
					if (agent.GetLOD() == AIAgent.GetMaxLOD())
						atMax++;
				}
			}
		}

		if (log)
			Print(string.Format("CLW: activity %1: units relevant %2/%3, prevented agents %4, cap %5, agents %6, at max LOD %7", factionKey, relevant, units.Count(), prevented, cap, agentsTotal, atMax), LogLevel.NORMAL);
	}

	// 0 = not relevant, else the relevance score (max over the reasons).
	protected int ScoreOf(CLW_ActivityUnit unit, string factionKey, notnull array<string> keys, map<string, ref array<ref CLW_ActivityUnit>> byFaction, CLW_ControlPoints points)
	{
		int score;

		// (d) human-led
		if (CLW_SquadInfo.HumanLeaderEntity(unit.m_Group))
			score = CLW_WarConfig.ACTIVITY_SCORE_HUMAN;

		// (a) enemy near
		float nearest = EnemyDistance(unit.m_vPos, factionKey, keys, byFaction);
		if (nearest >= 0 && nearest <= CLW_WarConfig.ACTIVITY_ENEMY_RANGE_M)
		{
			int enemyScore = Math.Round(CLW_WarConfig.ACTIVITY_SCORE_ENEMY_MAX * (1 - nearest / CLW_WarConfig.ACTIVITY_ENEMY_RANGE_M));
			score = Math.Max(score, Math.Max(1, enemyScore));
		}

		// (b) vehicle unit moving, or an active order
		if (IsVehicleActive(unit.m_sId, factionKey))
			score = Math.Max(score, CLW_WarConfig.ACTIVITY_SCORE_VEHICLE);
		else if (HasActiveOrder(unit.m_sId, factionKey))
			score = Math.Max(score, CLW_WarConfig.ACTIVITY_SCORE_ORDER);

		// (c) contested objective
		if (points && IsAtContested(unit.m_vPos, points))
			score = Math.Max(score, CLW_WarConfig.ACTIVITY_SCORE_CONTESTED);

		return score;
	}

	// Distance to the nearest real enemy (adopted unit of another side or enemy player), -1 when none.
	protected float EnemyDistance(vector pos, string factionKey, notnull array<string> keys, map<string, ref array<ref CLW_ActivityUnit>> byFaction)
	{
		float best = -1;
		foreach (string other : keys)
		{
			if (other == factionKey)
				continue;

			array<ref CLW_ActivityUnit> list;
			if (byFaction.Find(other, list) && list)
			{
				foreach (CLW_ActivityUnit enemy : list)
				{
					float d = vector.DistanceXZ(pos, enemy.m_vPos);
					if (best < 0 || d < best)
						best = d;
				}
			}

			array<vector> players;
			if (m_mPlayers.Find(other, players) && players)
			{
				foreach (vector playerPos : players)
				{
					float dp = vector.DistanceXZ(pos, playerPos);
					if (best < 0 || dp < best)
						best = dp;
				}
			}
		}

		return best;
	}

	protected bool IsVehicleActive(string id, string factionKey)
	{
		CLW_VehicleManager vehicles = CLW_GameMode.GetVehicleManager();
		if (!vehicles)
			return false;

		CLW_VehicleUnit unit = vehicles.Find(id);
		if (!unit)
			unit = vehicles.FindByCargo(id);

		return unit && unit.IsActiveState();
	}

	protected bool HasActiveOrder(string id, string factionKey)
	{
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(factionKey);
		if (!commander)
			return false;

		CLW_RuleCommander rules = commander.GetRules();
		if (!rules)
			return false;

		CLW_Orders orders = rules.GetOrders();
		if (!orders)
			return false;

		CLW_UnitOrder order = orders.GetOrder(id);
		return order && order.m_sStatus == CLW_Orders.STATUS_ACTIVE;
	}

	protected bool IsAtContested(vector pos, notnull CLW_ControlPoints points)
	{
		for (int i = 0; i < points.Count(); i++)
		{
			CLW_ControlPoint point = points.Get(i);
			if (!point || !point.m_bContested)
				continue;

			if (vector.DistanceXZ(pos, point.m_vPos) <= point.m_fRadius + CLW_WarConfig.ACTIVITY_CONTESTED_MARGIN_M)
				return true;
		}

		return false;
	}

	// Switch off: give the engine back its LOD decisions once, then do nothing.
	protected void ReleaseAll(CLW_SquadRegistry registry, CLW_ScenarioConfig scenario)
	{
		if (m_bOffHandled)
			return;

		m_bOffHandled = true;
		int released;
		foreach (string id, CLW_ActivityState state : m_mStates)
		{
			if (!state.m_bPrevented)
				continue;

			SetPrevented(registry.Find(id), false);
			state.m_bPrevented = false;
			released++;
		}

		Print(string.Format("CLW: activity management is OFF in the scenario config (%1 units released, engine LOD behaviour)", released), LogLevel.NORMAL);
	}
}

// One thing the doctrine wants units on: defend a threatened own point, recon a point, attack / capture a point, garrison a point.
class CLW_Task
{
	CLW_ECommand m_eCommand;
	string m_sTarget;			// objective id
	string m_sName;				// objective display name
	vector m_vPos;
	int m_iDemand;				// units wanted
	int m_iCovered;				// units already serving it
	int m_iPriority;			// higher first
	string m_sReason;			// English, for orders / log / LLM
	string m_sBriefWire;		// player-facing brief of the mission offer (CLW_Loc wire, flat params)
	bool m_bReconOnly;			// only recon-capable kinds
	string m_sMissionKind;		// CAPTURE | DEFEND | RECON (for offers to human-led squads)
}

// Rule-based faction commander, doctrine v1 (spec section 9): buys toward a target mix, turns the objectives and the contact
// picture into tasks, assigns units greedily, keeps a reserve, pulls weak units back. Orders go through CLW_Orders (infantry
// waypoints, vehicle units). Human-led squads get mission tasks instead of waypoints (legacy path until the mission board
// of P4 takes over: CLW_MissionHook.OwnsHumanSquads). Server only; Think() every ~10 s. It is the default and the fallback
// of the LLM bridge (CLW_RemoteCommander). One instance per faction key; recreated at war start.
class CLW_RuleCommander
{
	// ---- doctrine knobs ----
	static const int MAX_BUYS_PER_THINK = 2;
	static const int BUY_MIN_PRIORITY = 200;			// only tasks at or above this get purchases (garrison tasks of "all objectives held" are 100 and below)
	static const int SUPPLY_RESERVE = 40;				// never spend below this
	static const int RESERVE_RICH_SUPPLIES = 300;		// above this no reserve is held back
	static const float BUY_PENDING_SEC = 90;			// a purchase counts toward the mix this long (it needs time to arrive)
	static const float THREAT_RADIUS_M = 400;			// enemy contact this close to an own point threatens it
	static const float IDLE_ATTACK_RADIUS_M = 300;		// an idle unit attacks a contact this close
	static const float VEHICLE_PREFER_DIST_M = 800;		// vehicle units preferred for targets farther than this
	static const float VEHICLE_PREFER_BONUS_M = 600;
	static const float RECON_ON_ATTACK_PENALTY_M = 500;
	static const int MAX_ATTACK_UNITS = 3;
	static const int MAX_DEFEND_UNITS = 2;
	static const float OBJECTIVE_VALUE_WEIGHT_M = 1500;	// attack order score: value * this - distance from our base
	static const float MISSION_REFRESH_SEC = 600;		// legacy human mission is replaced after this
	static const float SETUP_LOG_SEC = 60;

	// buy mix categories (share of units by count)
	static const int CAT_INFANTRY = 0;
	static const int CAT_MOTORIZED = 1;
	static const int CAT_MECH = 2;
	static const int CAT_LIGHT = 3;
	static const float MIX_INFANTRY = 0.45;
	static const float MIX_MOTORIZED = 0.20;
	static const float MIX_MECH = 0.20;
	static const float MIX_LIGHT = 0.15;

	static const ResourceName TASK_PREFAB = "{1D0F815858EE24AD}Prefabs/Tasks/BaseTask.et";	// vanilla generic SCR_Task (legacy human mission)

	protected string m_sFaction;
	protected CLW_ContactBook m_Book;
	protected CLW_SquadRegistry m_Registry;			// weak: owned by the game mode
	protected ref CLW_Orders m_Orders = new CLW_Orders();
	protected ref CLW_Situation m_Situation = new CLW_Situation();
	protected ref array<string> m_aSquads = {};		// commandable ids of this faction (registry order), kept in sync by Sync()
	protected int m_iSeenVersion = -1;
	protected string m_sBuyState;					// last logged purchase state (log once per change)
	protected ref array<int> m_aBuyCategory = {};	// recent purchases (not arrived yet)
	protected ref array<float> m_aBuyTime = {};
	protected float m_fLastSetupLog = -1000;
	protected float m_fLastNoCatalogLog = -1000;
	protected float m_fCapBlockTime = -1;			// time of the last AI-cap refusal (-1 = not blocked)
	protected int m_iCapBlockLive;					// live + pending AI at that refusal
	protected float m_fLastCapLog = -1000;
	static const float CAP_BLOCK_SEC = 60;
	protected static int s_iMissionCounter;			// unique task IDs (a deleted task's ID may linger in the task system for a moment)
	protected SCR_Task m_MissionTask;				// the one vanilla task (J menu + map) of this faction's human squad (legacy path)
	protected string m_sMissionSquad;				// squad id the task belongs to ("" = none)

	// Squads come from the registry (all adopted, commandable groups of this faction, including ones added at runtime).
	void Init(string factionKey, CLW_ContactBook book, CLW_SquadRegistry registry)
	{
		m_sFaction = factionKey;
		m_Book = book;
		m_Registry = registry;
		m_Orders.Init(factionKey, registry);
		m_Situation.Init(factionKey, m_Orders);
		Sync();
		Print("CLW: RuleCommander created for faction " + factionKey, LogLevel.NORMAL);
	}

	string GetFaction()
	{
		return m_sFaction;
	}

	CLW_ContactBook GetBook()
	{
		return m_Book;
	}

	CLW_Orders GetOrders()
	{
		return m_Orders;
	}

	// Follow additions/removals in the registry. Lost units leave a "lost" event for the situation.
	protected void Sync()
	{
		if (!m_Registry || m_iSeenVersion == m_Registry.GetVersion())
			return;

		m_iSeenVersion = m_Registry.GetVersion();
		array<string> ids = {};
		m_Registry.GetCommandableIds(m_sFaction, ids);

		array<string> removed = {};
		m_Orders.Prune(ids, removed);
		CLW_WarState war = CLW_GameMode.GetWarState();
		foreach (string gone : removed)
		{
			SCR_AIGroup group = m_Registry.Find(gone);
			if (group && CLW_SquadInfo.AliveCount(group) > 0)
				continue;	// only hidden / re-adopted, not lost

			if (CLW_BaseWatch.WasStoodDown(gone))
			{
				Print(string.Format("CLW: RuleCommander %1: unit %2 stood down (idle at the base, not a loss)", m_sFaction, gone), LogLevel.NORMAL);
				continue;
			}

			Print(string.Format("CLW: RuleCommander %1: unit %2 lost", m_sFaction, gone), LogLevel.NORMAL);
			if (war && war.IsRunning())
				war.PushEvent(m_sFaction, gone + " lost");
		}

		if (!m_sMissionSquad.IsEmpty() && !ids.Contains(m_sMissionSquad))
		{
			ClearMissionTask();
			m_sMissionSquad = "";
		}

		m_aSquads.Copy(ids);
	}

	// ---- queries ----

	// True only if the squad's leader soldier is actually controlled by a player. The group flags
	// (IsPlayerLeader, GetLeaderID, GetFirstPlayerLeaderID) say a joined player "leads" while an AI
	// soldier still holds the leader entity (verified 2026-10-11: "commanding only for group leaders").
	static bool HasHumanLeader(notnull SCR_AIGroup group)
	{
		return CLW_SquadInfo.HumanLeaderEntity(group) != null;
	}

	// Escape a string for use inside a JSON string literal.
	static string JsonEscape(string text)
	{
		return CLW_Situation.Escape(text);
	}

	bool IsCommandable(string squadId)
	{
		Sync();
		return m_aSquads.Contains(squadId);
	}

	// True when the (commandable) squad is led by a human: it only gets missions.
	bool IsHumanLed(string squadId)
	{
		if (!m_Registry)
			return false;

		SCR_AIGroup group = m_Registry.Find(squadId);
		return group && HasHumanLeader(group);
	}

	// Serialise this faction's situation (protocol v2). Returns the number of live squads in it. Drains the event queue.
	int BuildSituation(out string json)
	{
		json = "";
		if (!m_Book)
			return 0;

		Sync();
		m_Orders.Update();
		return m_Situation.Build(m_Book, json);
	}

	// ---- think ----

	void Think()
	{
		if (!m_Book || !Replication.IsServer())
			return;

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war)
			return;

		float now = CLW_Orders.Now();
		if (war.GetPhase() == CLW_EWarPhase.SETUP)
		{
			// SETUP: nothing is bought or ordered, the squads hold where they stand
			if (now - m_fLastSetupLog >= SETUP_LOG_SEC)
			{
				m_fLastSetupLog = now;
				Print(string.Format("CLW: RuleCommander %1: war in SETUP, units hold position", m_sFaction), LogLevel.NORMAL);
			}
			return;
		}

		if (!war.IsRunning())
			return;

		Sync();
		m_Orders.Update();
		array<CLW_Contact> contacts = {};
		m_Book.GetContacts(contacts);

		CLW_FobDoctrine.Think(this, m_sFaction, war);	// M3 phase F: before Spend, which would use up the supplies a FOB needs
		CLW_LogisticsDoctrine.Think(this, m_sFaction, war);	// M3 phase L: RESUPPLY a FOB that runs low
		array<ref CLW_Task> tasks = {};
		Plan(war, contacts, tasks);		// the units on hand take the tasks first ...
		Spend(war, contacts, tasks);	// ... and units are bought only for what is still unmet (each carries its order)
		ThinkHumanLed();
	}

	// ---- spending ----

	static int CategoryOf(string kind)
	{
		if (kind == CLW_ForceKind.MOTORIZED)
			return CAT_MOTORIZED;
		if (kind == CLW_ForceKind.MECH || kind == CLW_ForceKind.ARMOR)
			return CAT_MECH;
		if (kind == CLW_ForceKind.LIGHT || kind == CLW_ForceKind.ARMORED_RECON)
			return CAT_LIGHT;

		return CAT_INFANTRY;
	}

	protected float MixShare(int category)
	{
		if (category == CAT_MOTORIZED)
			return MIX_MOTORIZED;
		if (category == CAT_MECH)
			return MIX_MECH;
		if (category == CAT_LIGHT)
			return MIX_LIGHT;

		return MIX_INFANTRY;
	}

	// Buys for concrete needs only (owner playtest 2026-10-12: no idle AI waiting at the base). Plan has assigned the units on hand; every task
	// that is still short of units gets one purchase per missing unit (at most MAX_BUYS_PER_THINK per think), the category picked by the
	// mix deficit, and each purchase carries its first order (CLW_PendingOrders): the new unit goes straight to the task when it is ready.
	// No unmet task means no purchase. A kind that was stood down as idle a moment ago is not bought again (CLW_BaseWatch).
	protected void Spend(CLW_WarState war, notnull array<CLW_Contact> contacts, notnull array<ref CLW_Task> tasks)
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!spawner || !catalog)
			return;

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(m_sFaction, entries);
		float now = CLW_Orders.Now();
		if (entries.IsEmpty())
		{
			if (now - m_fLastNoCatalogLog >= 120)
			{
				m_fLastNoCatalogLog = now;
				Print(string.Format("CLW: RuleCommander %1: no buyable forces in the catalog", m_sFaction), LogLevel.WARNING);
			}
			return;
		}

		// the tasks the units on hand (and the purchases already on their way) cannot cover
		array<CLW_Task> needs = {};
		int missingTotal;
		foreach (CLW_Task need : tasks)
		{
			if (need.m_iPriority < BUY_MIN_PRIORITY || need.m_iCovered >= need.m_iDemand)
				continue;

			needs.Insert(need);
			missingTotal += need.m_iDemand - need.m_iCovered;
		}

		if (needs.IsEmpty())
		{
			LogBuyState("noneed", string.Format("CLW: RuleCommander %1: no purchase, no unmet task (units on hand cover every task); supplies %2", m_sFaction, war.GetSupplies(m_sFaction)));
			return;
		}

		LogBuyState("need", string.Format("CLW: RuleCommander %1: %2 unmet task(s), %3 unit(s) missing, first: %4", m_sFaction, needs.Count(), missingTotal, needs[0].m_sReason));

		// WP6: at the AI cap every purchase would be refused: do not plan or pay, log once per change (not every think)
		int capLive;
		int capMax;
		if (CLW_EconomyGuard.AiCapFull(m_sFaction, capLive, capMax))
		{
			if (CLW_EconomyGuard.LogChanged("rulecap", m_sFaction, "" + capLive + "/" + capMax))
				Print(string.Format("CLW: RuleCommander %1: no buy, AI cap %2/%3", m_sFaction, capLive, capMax), LogLevel.NORMAL);
			return;
		}

		CLW_EconomyGuard.LogChanged("rulecap", m_sFaction, "ok");

		// a buy was refused for the AI cap: wait until the live AI count drops or CAP_BLOCK_SEC passed
		if (m_fCapBlockTime >= 0)
		{
			int liveNow = spawner.GetLiveAI(m_sFaction) + spawner.GetPendingAI(m_sFaction);
			if (now - m_fCapBlockTime < CAP_BLOCK_SEC && liveNow >= m_iCapBlockLive)
				return;

			Print(string.Format("CLW: RuleCommander %1: AI cap block lifted (live %2 was %3, %4 s)", m_sFaction, liveNow, m_iCapBlockLive, Math.Round(now - m_fCapBlockTime)), LogLevel.NORMAL);
			m_fCapBlockTime = -1;
		}

		// the engine's active-AI limit has no room for a purchase: do not even pick (buys would be refused anyway)
		if (spawner.GetEngineHeadroom() <= 0)
		{
			if (now - m_fLastCapLog >= 60)
			{
				m_fLastCapLog = now;
				int engineActive;
				int engineLimit;
				CLW_ForceSpawner.GetEngineAI(engineActive, engineLimit);
				Print(string.Format("CLW: RuleCommander %1: no buys, engine AI limit (active %2/%3, margin %4)", m_sFaction, engineActive, engineLimit, CLW_WarConfig.ENGINE_AI_MARGIN), LogLevel.NORMAL);
			}
			return;
		}

		// composition: living units by category plus purchases still on their way
		array<int> counts = {0, 0, 0, 0};
		foreach (string id : m_aSquads)
		{
			string kind = m_Registry.GetKind(id);
			CLW_VehicleUnit unit = m_Orders.FindVehicleUnit(id);
			if (unit)
				kind = unit.GetKind();
			if (kind == CLW_ForceKind.LOGISTICS)
				continue;	// supply trucks are not part of the fighting mix (M3 phase L)

			int category = CategoryOf(kind);
			counts[category] = counts[category] + 1;
		}

		for (int p = m_aBuyTime.Count() - 1; p >= 0; p--)
		{
			if (now - m_aBuyTime[p] > BUY_PENDING_SEC)
			{
				m_aBuyTime.Remove(p);
				m_aBuyCategory.Remove(p);
				continue;
			}

			int pendingCat = m_aBuyCategory[p];
			counts[pendingCat] = counts[pendingCat] + 1;
		}

		bool enemyVehicles;
		foreach (CLW_Contact contact : contacts)
		{
			if (contact.IsVehicle())
				enemyVehicles = true;
		}

		array<string> excluded = {};
		int bought;
		foreach (CLW_Task goal : needs)
		{
			int missing = goal.m_iDemand - goal.m_iCovered;
			int tries;
			while (missing > 0 && bought < MAX_BUYS_PER_THINK && tries < MAX_BUYS_PER_THINK + 3)
			{
				tries++;
				// WP6: the HQ reserve (mode param hqReserve, default 100) replaces the old fixed floor when it is higher
				int floorSupplies = Math.Max(SUPPLY_RESERVE, CLW_EconomyGuard.HqReserve());
				int spendable = war.GetSupplies(m_sFaction) - floorSupplies;
				if (spendable <= 0)
				{
					LogBuyState("reserve", string.Format("CLW: RuleCommander %1: no buy, HQ reserve: stock %2, reserve %3", m_sFaction, war.GetSupplies(m_sFaction), floorSupplies));
					return;
				}

				int total = counts[0] + counts[1] + counts[2] + counts[3];
				// categories by deficit (largest first), only those with an affordable entry
				CLW_ForceEntry pick;
				array<int> order = {};
				array<float> deficits = {};
				for (int c = 0; c < 4; c++)
				{
					float share = 0;
					if (total > 0)
						share = counts[c] * 1.0 / total;
					float deficit = MixShare(c) - share;
					int at = order.Count();
					for (int k = 0; k < order.Count(); k++)
					{
						if (deficit > deficits[k])
						{
							at = k;
							break;
						}
					}
					order.InsertAt(c, at);
					deficits.InsertAt(deficit, at);
				}

				string why;
				pick = null;
				foreach (int cat : order)
				{
					pick = PickEntry(entries, cat, spendable, excluded, enemyVehicles, goal.m_bReconOnly, why);
					if (pick)
						break;
				}

				if (!pick)
				{
					LogBuyState("saving", string.Format("CLW: RuleCommander %1: saving (supplies %2, reserve %3, nothing affordable or allowed for %4 %5)", m_sFaction, war.GetSupplies(m_sFaction), floorSupplies, CLW_Command.ToName(goal.m_eCommand), goal.m_sTarget));
					break;
				}

				string reason = string.Format("%1: %2 (mix inf=%3 mot=%4 mech=%5 light=%6)", goal.m_sReason, why, counts[0], counts[1], counts[2], counts[3]);
				string tag = CLW_PendingOrders.Add(m_sFaction, CLW_Command.ToName(goal.m_eCommand), goal.m_sTarget, vector.Zero, goal.m_sReason, CLW_Orders.SOURCE_RULES);
				string error;
				if (spawner.BuyTagged(m_sFaction, pick.m_sType, reason, tag, error))
				{
					int pickedCat = CategoryOf(pick.m_sKind);
					counts[pickedCat] = counts[pickedCat] + 1;
					m_aBuyCategory.Insert(pickedCat);
					m_aBuyTime.Insert(now);
					bought++;
					missing--;
					goal.m_iCovered = goal.m_iCovered + 1;
					Print(string.Format("CLW: RuleCommander %1: BUY %2 (%3, cost %4, supplies left %5) for %6 %7: %8", m_sFaction, pick.m_sType, pick.m_sKind, pick.m_iCost, war.GetSupplies(m_sFaction), CLW_Command.ToName(goal.m_eCommand), goal.m_sTarget, reason), LogLevel.NORMAL);
				}
				else
				{
					CLW_PendingOrders.Remove(tag);
					if (error.IndexOf("engine AI limit") == 0)
					{
						Print(string.Format("CLW: RuleCommander %1: buy %2 refused: %3", m_sFaction, pick.m_sType, error), LogLevel.NORMAL);
						return;
					}

					if (error.IndexOf("AI cap") == 0)
					{
						m_fCapBlockTime = now;
						m_iCapBlockLive = spawner.GetLiveAI(m_sFaction) + spawner.GetPendingAI(m_sFaction);
						if (now - m_fLastCapLog >= 60)
						{
							m_fLastCapLog = now;
							Print(string.Format("CLW: RuleCommander %1: buy %2 refused: %3; no further buys until the AI count drops or %4 s", m_sFaction, pick.m_sType, error, CAP_BLOCK_SEC), LogLevel.NORMAL);
						}
						return;
					}

					Print(string.Format("CLW: RuleCommander %1: buy %2 refused: %3", m_sFaction, pick.m_sType, error), LogLevel.NORMAL);
					if (error.IndexOf("vehicle cap") == 0)
					{
						foreach (CLW_ForceEntry vehicleEntry : entries)
						{
							if (vehicleEntry.HasVehicle())
								excluded.Insert(vehicleEntry.m_sType);
						}
					}
					else
						excluded.Insert(pick.m_sType);
				}
			}
		}
	}

	// One log line per change of the buy state (not every think).
	protected void LogBuyState(string key, string line)
	{
		if (key == m_sBuyState)
			return;

		m_sBuyState = key;
		Print(line, LogLevel.NORMAL);
	}

	// An affordable, allowed entry of the category: weighted random by the entry weights; against enemy vehicles anti-armour
	// infantry first. Null when the category has none.
	protected CLW_ForceEntry PickEntry(notnull array<CLW_ForceEntry> entries, int category, int spendable, notnull array<string> excluded, bool enemyVehicles, bool reconOnly, out string why)
	{
		array<CLW_ForceEntry> pool = {};
		foreach (CLW_ForceEntry entry : entries)
		{
			if (entry.m_sKind == CLW_ForceKind.LOGISTICS)
				continue;	// trucks are bought for convoys (CLW_Convoys), never by the mix

			if (CategoryOf(entry.m_sKind) != category || entry.m_iCost > spendable || excluded.Contains(entry.m_sType))
				continue;

			if (reconOnly && !IsReconKind(entry.m_sKind))
				continue;

			if (CLW_BaseWatch.KindRecentlyDespawned(m_sFaction, entry.m_sKind, CLW_BaseWatch.DESPAWN_BUY_BLOCK_SEC))
				continue;	// stood down as idle a moment ago: do not buy the same kind again (no buy -> despawn loop)

			pool.Insert(entry);
		}

		if (pool.IsEmpty())
			return null;

		why = "category " + category + " is under its share";
		if (enemyVehicles && category == CAT_INFANTRY)
		{
			array<CLW_ForceEntry> antiArmor = {};
			foreach (CLW_ForceEntry candidate : pool)
			{
				if (candidate.m_bAntiArmor)
					antiArmor.Insert(candidate);
			}

			if (!antiArmor.IsEmpty())
			{
				pool.Copy(antiArmor);
				why = "enemy vehicles known, anti-armour infantry";
			}
		}

		int weightSum;
		foreach (CLW_ForceEntry weighted : pool)
		{
			weightSum += Math.Max(1, weighted.m_iWeight);
		}

		int roll = Math.RandomInt(0, weightSum);
		foreach (CLW_ForceEntry chosen : pool)
		{
			roll -= Math.Max(1, chosen.m_iWeight);
			if (roll < 0)
				return chosen;
		}

		return pool[pool.Count() - 1];
	}

	// ---- tasks ----

	protected CLW_Task NewTask(CLW_ECommand command, CLW_ControlPoint point, int demand, int priority, string reason, string missionKind, string briefWire)
	{
		CLW_Task task = new CLW_Task();
		task.m_eCommand = command;
		task.m_sTarget = point.m_sId;
		task.m_sName = point.m_sName;
		task.m_vPos = point.m_vPos;
		task.m_iDemand = demand;
		task.m_iPriority = priority;
		task.m_sReason = reason;
		task.m_sBriefWire = briefWire;
		task.m_sMissionKind = missionKind;
		return task;
	}

	// Insert keeping the list sorted by priority (highest first).
	protected void InsertTask(notnull array<ref CLW_Task> tasks, CLW_Task task)
	{
		int at = tasks.Count();
		for (int i = 0; i < tasks.Count(); i++)
		{
			if (task.m_iPriority > tasks[i].m_iPriority)
			{
				at = i;
				break;
			}
		}

		tasks.InsertAt(task, at);
	}

	// The doctrine's targets: defend threatened own points, recon the most valuable unknown point, attack / capture the others
	// by value and nearness to our base, garrison own points when nothing is left to take.
	protected void BuildTasks(notnull array<CLW_Contact> contacts, notnull array<ref CLW_Task> tasks)
	{
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!points || !bases || !war)
			return;

		vector ownBase = bases.GetBasePos(m_sFaction);
		vector enemyBase = bases.GetBasePos(war.GetEnemyKey(m_sFaction));

		array<CLW_ControlPoint> own = {};
		array<CLW_ControlPoint> foreign = {};
		array<float> foreignScore = {};
		for (int i = 0; i < points.Count(); i++)
		{
			CLW_ControlPoint point = points.Get(i);
			if (!point)
				continue;

			if (point.m_sOwner == m_sFaction)
			{
				own.Insert(point);
				continue;
			}

			float score = point.m_iValue * OBJECTIVE_VALUE_WEIGHT_M - vector.DistanceXZ(ownBase, point.m_vPos);
			int at = foreign.Count();
			for (int k = 0; k < foreign.Count(); k++)
			{
				if (score > foreignScore[k])
				{
					at = k;
					break;
				}
			}
			foreign.InsertAt(point, at);
			foreignScore.InsertAt(score, at);
		}

		// 1. own points with enemy contacts close (or contested)
		array<string> threatened = {};
		foreach (CLW_ControlPoint ownPoint : own)
		{
			int near;
			float nearest = 1000000;
			foreach (CLW_Contact contact : contacts)
			{
				float dist = vector.DistanceXZ(contact.m_vPos, ownPoint.m_vPos);
				if (dist <= THREAT_RADIUS_M)
				{
					near++;
					nearest = Math.Min(nearest, dist);
				}
			}

			if (near == 0 && !ownPoint.m_bContested)
				continue;

			string why = string.Format("own point %1 threatened: %2 contact(s)", ownPoint.m_sId, near);
			if (near > 0)
				why += ", nearest " + Math.Round(nearest) + " m";
			if (ownPoint.m_bContested)
				why += ", contested";
			string holdBrief;
			if (near == 0)
				holdBrief = CLW_Loc.W("#CLW-Msn_Brief_HoldContested", ownPoint.m_sName, ownPoint.m_sId);
			else if (ownPoint.m_bContested)
				holdBrief = CLW_Loc.W("#CLW-Msn_Brief_HoldThreatContested", ownPoint.m_sName, ownPoint.m_sId, CLW_Loc.N(near), CLW_Loc.N(Math.Round(nearest)));
			else
				holdBrief = CLW_Loc.W("#CLW-Msn_Brief_HoldThreat", ownPoint.m_sName, ownPoint.m_sId, CLW_Loc.N(near), CLW_Loc.N(Math.Round(nearest)));
			threatened.Insert(ownPoint.m_sId);
			InsertTask(tasks, NewTask(CLW_ECommand.DEFEND, ownPoint, MAX_DEFEND_UNITS, 1000 + ownPoint.m_iValue * 100, why, "DEFEND", holdBrief));
		}

		// 2. recon: the most valuable foreign point without contact information
		bool reconPlanned;
		foreach (CLW_ControlPoint candidate : foreign)
		{
			bool known;
			foreach (CLW_Contact seen : contacts)
			{
				if (vector.DistanceXZ(seen.m_vPos, candidate.m_vPos) <= THREAT_RADIUS_M)
					known = true;
			}

			if (known)
				continue;

			CLW_Task recon = NewTask(CLW_ECommand.RECON, candidate, 1, 600, "no contact information at " + candidate.m_sId, "RECON", CLW_Loc.W("#CLW-Msn_Brief_Recon", candidate.m_sName, candidate.m_sId));
			recon.m_bReconOnly = true;
			InsertTask(tasks, recon);
			reconPlanned = true;
			break;
		}

		// 3. attack / capture in score order
		foreach (int rank, CLW_ControlPoint target : foreign)
		{
			string attackWhy = string.Format("take %1 %2 (value %3, owner %4)", target.m_sId, target.m_sName, target.m_iValue, target.m_sOwner);
			InsertTask(tasks, NewTask(CLW_ECommand.ATTACK, target, MAX_ATTACK_UNITS, 500 - rank * 10, attackWhy, "CAPTURE", CLW_Loc.W("#CLW-Msn_Brief_Capture", target.m_sName, target.m_sId, CLW_Loc.N(target.m_iValue))));
		}

		// 4. nothing to take: garrison own points, the ones nearest to the enemy first
		if (foreign.IsEmpty())
		{
			array<CLW_ControlPoint> garrison = {};
			array<float> garrisonDist = {};
			foreach (CLW_ControlPoint holdPoint : own)
			{
				if (threatened.Contains(holdPoint.m_sId))
					continue;

				float d = vector.DistanceXZ(enemyBase, holdPoint.m_vPos);
				int pos = garrison.Count();
				for (int j = 0; j < garrison.Count(); j++)
				{
					if (d < garrisonDist[j])
					{
						pos = j;
						break;
					}
				}
				garrison.InsertAt(holdPoint, pos);
				garrisonDist.InsertAt(d, pos);
			}

			foreach (int garrisonRank, CLW_ControlPoint hold : garrison)
			{
				InsertTask(tasks, NewTask(CLW_ECommand.DEFEND, hold, 1, 100 - garrisonRank * 10, "garrison " + hold.m_sId + " (all objectives held)", "DEFEND", CLW_Loc.W("#CLW-Msn_Brief_HoldGarrison", hold.m_sName, hold.m_sId)));
			}
		}
	}

	// ---- planning (AI-only units) ----

	protected string PointName(string id)
	{
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (!points)
			return id;

		CLW_ControlPoint point = points.Find(id);
		if (!point)
			return id;

		return id + " " + point.m_sName;
	}

	protected static bool IsReconKind(string kind)
	{
		return kind == CLW_ForceKind.RECON || kind == CLW_ForceKind.LIGHT || kind == CLW_ForceKind.ARMORED_RECON;
	}

	protected string KindOf(string id)
	{
		CLW_VehicleUnit unit = m_Orders.FindVehicleUnit(id);
		if (unit)
			return unit.GetKind();

		return m_Registry.GetKind(id);
	}

	// Weak units retreat to the nearest own point (else the base) regardless of order stability.
	protected void FallBack(string id)
	{
		CLW_UnitOrder current = m_Orders.GetOrder(id);
		if (current && current.m_eCommand == CLW_ECommand.FALL_BACK && current.m_sStatus != CLW_Orders.STATUS_FAILED)
			return;	// already falling back (or arrived and recovering)

		vector pos;
		if (!m_Orders.UnitPos(id, pos))
			return;

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		string targetId;
		float best = 1000000;
		if (points)
		{
			for (int i = 0; i < points.Count(); i++)
			{
				CLW_ControlPoint point = points.Get(i);
				if (point && point.m_sOwner == m_sFaction)
				{
					float dist = vector.DistanceXZ(pos, point.m_vPos);
					if (dist < best)
					{
						best = dist;
						targetId = point.m_sId;
					}
				}
			}
		}

		string reason = string.Format("weak: %1/%2 alive", m_Orders.UnitAlive(id), m_Orders.StartStrength(id, 0));
		if (targetId.IsEmpty())
			reason += ", back to base";
		else
			reason += ", back to own point " + targetId;
		string error;
		if (!m_Orders.Issue(id, CLW_ECommand.FALL_BACK, targetId, vector.Zero, reason, CLW_Orders.SOURCE_RULES, error))
			Print(string.Format("CLW: RuleCommander %1: FALL_BACK %2 refused: %3", m_sFaction, id, error), LogLevel.WARNING);
	}

	protected void Plan(CLW_WarState war, notnull array<CLW_Contact> contacts, notnull array<ref CLW_Task> tasks)
	{
		// AI-only, living units
		array<string> candidates = {};
		foreach (string id : m_aSquads)
		{
			SCR_AIGroup group = m_Registry.Find(id);
			if (!group || HasHumanLeader(group) || m_Orders.UnitAlive(id) <= 0)
				continue;

			if (CLW_FobManager.IsReserved(id))
				continue;	// builder / escort of a FOB being established (CLW_FobManager gives its orders)

			if (CLW_BaseWatch.IsGarrison(id))
				continue;	// the HQ garrison keeps its standing DEFEND (CLW_BaseWatch)

			if (CLW_PendingOrders.IsPendingUnit(id))
				continue;	// a purchase whose first order is about to be applied

			if (KindOf(id) == CLW_ForceKind.LOGISTICS)
				continue;	// supply trucks only move on convoy orders (M3 phase L)

			if (m_Orders.IsWeak(id))
			{
				FallBack(id);
				continue;
			}

			candidates.Insert(id);
		}

		BuildTasks(contacts, tasks);

		// who serves which task already
		array<string> serving = {};
		foreach (string unitId : candidates)
		{
			CLW_UnitOrder order = m_Orders.GetOrder(unitId);
			if (!order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE)
				continue;

			bool counted;
			foreach (CLW_Task existing : tasks)
			{
				if (existing.m_eCommand == order.m_eCommand && existing.m_sTarget == order.m_sTarget && existing.m_iCovered < existing.m_iDemand)
				{
					existing.m_iCovered = existing.m_iCovered + 1;
					counted = true;
					break;
				}
			}

			float age = CLW_Orders.Now() - order.m_fIssuedAt;
			bool adHoc = order.m_eCommand == CLW_ECommand.ATTACK && order.m_sTarget.IsEmpty() && age < 120;
			bool retreating = order.m_eCommand == CLW_ECommand.FALL_BACK;
			if (counted || adHoc || retreating)
				serving.Insert(unitId);
		}

		// units bought for a task whose order is still waiting for the new unit count as coverage (no second purchase for it)
		foreach (CLW_Task pendingTask : tasks)
		{
			pendingTask.m_iCovered = pendingTask.m_iCovered + CLW_PendingOrders.CountFor(m_sFaction, CLW_Command.ToName(pendingTask.m_eCommand), pendingTask.m_sTarget);
		}

		// assignable: not serving and allowed to get a new order (no order / finished / old enough)
		array<string> assignable = {};
		foreach (string freeId : candidates)
		{
			if (!serving.Contains(freeId) && m_Orders.CanReorder(freeId))
				assignable.Insert(freeId);
		}

		int assigned;

		// idle units (no live order) with a contact close by attack it
		for (int a = assignable.Count() - 1; a >= 0; a--)
		{
			string idleId = assignable[a];
			CLW_UnitOrder idleOrder = m_Orders.GetOrder(idleId);
			if (idleOrder && idleOrder.m_sStatus == CLW_Orders.STATUS_ACTIVE)
				continue;

			vector idlePos;
			if (!m_Orders.UnitPos(idleId, idlePos))
				continue;

			CLW_Contact nearestContact;
			float nearestDist = IDLE_ATTACK_RADIUS_M;
			foreach (CLW_Contact contact : contacts)
			{
				float contactDist = vector.DistanceXZ(idlePos, contact.m_vPos);
				if (contactDist <= nearestDist)
				{
					nearestDist = contactDist;
					nearestContact = contact;
				}
			}

			if (!nearestContact)
				continue;

			string fightError;
			string fightReason = string.Format("idle, %1 contact %2 m away (%3)", nearestContact.m_sKind, Math.Round(nearestDist), nearestContact.Describe());
			if (m_Orders.Issue(idleId, CLW_ECommand.ATTACK, "", nearestContact.m_vPos, fightReason, CLW_Orders.SOURCE_RULES, fightError))
				assigned++;
			else
				Print(string.Format("CLW: RuleCommander %1: ATTACK contact for %2 refused: %3", m_sFaction, idleId, fightError), LogLevel.WARNING);
			assignable.Remove(a);
		}

		// greedy assignment, tasks by priority
		foreach (CLW_Task goal : tasks)
		{
			while (goal.m_iCovered < goal.m_iDemand && !assignable.IsEmpty())
			{
				int bestIndex = -1;
				float bestScore = 1000000000;
				foreach (int index, string candidateId : assignable)
				{
					string kind = KindOf(candidateId);
					if (goal.m_bReconOnly && !IsReconKind(kind))
						continue;

					vector unitPos;
					if (!m_Orders.UnitPos(candidateId, unitPos))
						continue;

					float distance = vector.DistanceXZ(unitPos, goal.m_vPos);
					float cost = distance;
					if (distance > VEHICLE_PREFER_DIST_M && CLW_ForceKind.IsVehicleKind(kind))
						cost -= VEHICLE_PREFER_BONUS_M;
					if (goal.m_eCommand == CLW_ECommand.ATTACK && IsReconKind(kind))
						cost += RECON_ON_ATTACK_PENALTY_M;
					if (cost < bestScore)
					{
						bestScore = cost;
						bestIndex = index;
					}
				}

				if (bestIndex < 0)
					break;

				string chosen = assignable[bestIndex];
				assignable.Remove(bestIndex);
				string error;
				if (m_Orders.Issue(chosen, goal.m_eCommand, goal.m_sTarget, vector.Zero, goal.m_sReason, CLW_Orders.SOURCE_RULES, error))
				{
					goal.m_iCovered = goal.m_iCovered + 1;
					assigned++;
				}
				else
				{
					Print(string.Format("CLW: RuleCommander %1: %2 %3 for %4 refused: %5", m_sFaction, CLW_Command.ToName(goal.m_eCommand), goal.m_sTarget, chosen, error), LogLevel.WARNING);
				}
			}
		}

		Print(string.Format("CLW: RuleCommander %1: think units=%2 tasks=%3 assigned=%4 unassigned=%5 garrison=%6 pending=%7 supplies=%8 contacts=%9", m_sFaction, candidates.Count(), tasks.Count(), assigned, assignable.Count(), CLW_BaseWatch.GarrisonCount(m_sFaction), CLW_PendingOrders.Count(m_sFaction), war.GetSupplies(m_sFaction), contacts.Count()), LogLevel.NORMAL);
	}

	// ---- external orders (LLM) ----

	// Execute one external order. command: MOVE | ATTACK | DEFEND | RECON | FALL_BACK. targetId = objective id or "" (then pos;
	// FALL_BACK may have neither = own base). False (why set) for unknown / human-led / dead squads, bad command or target,
	// or an order that is still stable.
	bool ApplyOrder(string squadId, string commandName, string targetId, vector pos, string reason, out string why)
	{
		return ApplyOrderAs(squadId, commandName, targetId, pos, reason, CLW_Orders.SOURCE_LLM, why);
	}

	// Same with an explicit source (the first order of a purchased unit, CLW_PendingOrders). Garrison squads are refused: they defend the HQ.
	bool ApplyOrderAs(string squadId, string commandName, string targetId, vector pos, string reason, string source, out string why)
	{
		CLW_ECommand command;
		if (!CLW_Command.FromName(commandName, command))
		{
			why = "unknown command " + commandName;
			return false;
		}

		Sync();
		if (!m_aSquads.Contains(squadId))
		{
			why = "unknown or not commandable squad " + squadId;
			return false;
		}

		if (IsHumanLed(squadId))
		{
			why = "human-led squad (missions only)";
			return false;
		}

		string reservedFor = CLW_FobManager.ReservedFor(squadId);
		if (!reservedFor.IsEmpty())
		{
			why = "reserved: " + reservedFor;
			return false;
		}

		if (CLW_BaseWatch.IsGarrison(squadId))
		{
			why = "garrison squad (defends the HQ, not available for orders)";
			return false;
		}

		return m_Orders.Issue(squadId, command, targetId, pos, reason, source, why);
	}

	// ---- human-led squads: missions ----

	// Offers for a human-led squad from the doctrine's own plan: a threatened own point (DEFEND), the best uncovered target
	// (CAPTURE), a recon target, the nearest contact (DESTROY). At most 3. Used by the mission board (P4) and the fallback below.
	int BuildOffers(string squadId, notnull array<ref CLW_MissionOffer> offers)
	{
		array<CLW_Contact> contacts = {};
		if (m_Book)
			m_Book.GetContacts(contacts);

		array<ref CLW_Task> tasks = {};
		BuildTasks(contacts, tasks);
		bool hasDefend;
		bool hasCapture;
		bool hasRecon;
		foreach (CLW_Task task : tasks)
		{
			if (offers.Count() >= 3)
				break;

			if (task.m_sMissionKind == "DEFEND" && !hasDefend && task.m_iPriority >= 1000)
			{
				offers.Insert(MakeOffer("DEFEND", task.m_sTarget, task.m_sBriefWire));
				hasDefend = true;
			}
			else if (task.m_sMissionKind == "CAPTURE" && !hasCapture)
			{
				offers.Insert(MakeOffer("CAPTURE", task.m_sTarget, task.m_sBriefWire));
				hasCapture = true;
			}
			else if (task.m_sMissionKind == "RECON" && !hasRecon)
			{
				offers.Insert(MakeOffer("RECON", task.m_sTarget, task.m_sBriefWire));
				hasRecon = true;
			}
		}

		vector squadPos;
		if (offers.Count() < 3 && m_Orders.UnitPos(squadId, squadPos))
		{
			CLW_Contact nearest;
			float nearestDist = 1000000;
			foreach (CLW_Contact contact : contacts)
			{
				float dist = vector.DistanceXZ(squadPos, contact.m_vPos);
				if (dist < nearestDist)
				{
					nearestDist = dist;
					nearest = contact;
				}
			}

			if (nearest && nearestDist < 1500)
			{
				CLW_MissionOffer destroy = new CLW_MissionOffer();
				destroy.m_sKind = "DESTROY";
				destroy.m_bHasPos = true;
				destroy.m_vPos = nearest.m_vPos;
				string destroyKey = "#CLW-Msn_Brief_DestroyUnknown";
				if (nearest.m_sKind == CLW_Contact.KIND_VEHICLE)
					destroyKey = "#CLW-Msn_Brief_DestroyVehicle";
				else if (nearest.m_sKind == CLW_Contact.KIND_INFANTRY)
					destroyKey = "#CLW-Msn_Brief_DestroyInfantry";
				destroy.m_sBrief = CLW_Loc.W(destroyKey, CLW_Loc.N(Math.Round(nearestDist)));
				destroy.m_sSource = "rules";
				offers.Insert(destroy);
			}
		}

		return offers.Count();
	}

	protected CLW_MissionOffer MakeOffer(string kind, string targetId, string brief)
	{
		CLW_MissionOffer offer = new CLW_MissionOffer();
		offer.m_sKind = kind;
		offer.m_sTarget = targetId;
		offer.m_sBrief = brief;
		offer.m_sSource = "rules";
		return offer;
	}

	// Human-led squads that have no live mission get one from the rules (legacy path: one vanilla task per faction, expressed
	// with the shared commands). Skipped when the mission board owns human squads (CLW_MissionHook.OwnsHumanSquads).
	void ThinkHumanLed()
	{
		if (!m_Book || !Replication.IsServer())
			return;

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;

		Sync();
		if (CLW_MissionHook.Get().OwnsHumanSquads())
			return;

		array<CLW_Contact> contacts = {};
		m_Book.GetContacts(contacts);
		array<ref CLW_Task> tasks = {};
		BuildTasks(contacts, tasks);
		float now = CLW_Orders.Now();
		foreach (string id : m_aSquads)
		{
			SCR_AIGroup group = m_Registry.Find(id);
			if (!group)
				continue;

			if (!HasHumanLeader(group))
			{
				if (m_sMissionSquad == id)
				{
					// leadership went back to the AI: drop the stale mission task
					ClearMissionTask();
					m_sMissionSquad = "";
				}
				continue;
			}

			CLW_UnitOrder order = m_Orders.GetOrder(id);
			if (order && order.m_sStatus == CLW_Orders.STATUS_ACTIVE && now - order.m_fIssuedAt < MISSION_REFRESH_SEC)
				continue;

			if (tasks.IsEmpty())
				continue;

			CLW_Task best = tasks[0];
			string label = MissionLabel(best.m_eCommand);
			string reason = best.m_sReason;
			m_Orders.Record(id, best.m_eCommand, best.m_sTarget, vector.Zero, reason);
			ShowMissionTask(id, group, MissionLabelWire(best.m_eCommand, best.m_sName), best.m_vPos, best.m_sBriefWire);
			string msg = string.Format("MISSION %1 (%2): %3 %4 / %5 (%6)", id, m_sFaction, label, best.m_sTarget, best.m_sName, reason);
			CLW_Debug.Say(msg);
			Print("CLW: RuleCommander " + msg, LogLevel.NORMAL);
		}
	}

	// Legacy mission task title as a wire (flat params): "Capture Timber Hub".
	protected string MissionLabelWire(CLW_ECommand command, string pointName)
	{
		switch (command)
		{
			case CLW_ECommand.ATTACK: return CLW_Loc.W("#CLW-Msn_Label_Attack", pointName);
			case CLW_ECommand.DEFEND: return CLW_Loc.W("#CLW-Msn_Label_Defend", pointName);
			case CLW_ECommand.RECON: return CLW_Loc.W("#CLW-Msn_Label_Recon", pointName);
			case CLW_ECommand.FALL_BACK: return CLW_Loc.W("#CLW-Msn_Label_FallBack", pointName);
		}

		return CLW_Loc.W("#CLW-Msn_Label_Move", pointName);
	}

	protected string MissionLabel(CLW_ECommand command)
	{
		switch (command)
		{
			case CLW_ECommand.ATTACK: return "Capture";
			case CLW_ECommand.DEFEND: return "Defend";
			case CLW_ECommand.RECON: return "Recon";
			case CLW_ECommand.FALL_BACK: return "Fall back to";
		}

		return "Move to";
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

		target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
		string desc = reason;	// label / reason: plain text or CLW_Loc wires (flat params)
		if (m_MissionTask && m_sMissionSquad == squadId)
		{
			// same squad: keep the task (and its assignment), just change what it says and where it is
			taskSystem.MoveTask(m_MissionTask, target);
			CLW_Loc.SetTaskName(m_MissionTask, label);
			CLW_Loc.SetTaskDescription(m_MissionTask, desc);
			return;
		}

		ClearMissionTask();

		s_iMissionCounter++;
		string taskId = "CLW_Mission_" + m_sFaction + "_" + s_iMissionCounter;
		SCR_Task task = taskSystem.CreateTask(TASK_PREFAB, taskId, "", "", target);
		if (!task)
		{
			Print("CLW: RuleCommander: CreateTask failed for " + taskId, LogLevel.WARNING);
			return;
		}

		CLW_Loc.SetTaskName(task, label);
		CLW_Loc.SetTaskDescription(task, desc);

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
}

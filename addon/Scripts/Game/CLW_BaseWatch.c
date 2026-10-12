// Base watch (owner playtest 2026-10-12: "10-20 units waiting at the base for minutes until HQ gives them a task"): keeps the AI at the
// bases to the units that have a job.
//   Garrison   each side keeps garrisonSquads infantry squads at its HQ with an explicit standing DEFEND order on the base. They are
//              picked from the first squads that stand free at the base, are never counted as idle, never get other orders from the
//              rule commander and are refused LLM orders (the situation marks them "garrison":true).
//   Idle net   a squad without an active order that stands at the base (not garrison, not a player squad, not reserved for a FOB / convoy,
//              not a supply truck) longer than idleDespawnMin is stood down: its group / vehicle unit is deleted and the purchase cost is
//              refunded to the side's supplies. The refund and a recently-despawned counter appear in the situation (baseRules) and the
//              rule commander does not buy the same kind within DESPAWN_BUY_BLOCK_SEC, so buy -> despawn -> buy loops cannot form.
//   Ledger     the spawner reports every finished unit with the supplies it cost (0 = free start force) via OnUnitReady.
// Called every ~10 s per faction from CLW_RemoteCommander.Think (so it runs for the LLM and the rule path alike). Server only.
// Params (war mode): garrisonSquads (default 1), idleDespawnMin (default 6, 0 = off). Log prefix: "CLW: garrison" / "CLW: idle squad".

class CLW_BaseWatch
{
	static const int DEFAULT_GARRISON = 1;
	static const int DEFAULT_IDLE_MIN = 6;
	static const float BASE_RADIUS_M = 200;				// "at the base": within this of the HQ position
	static const float THINK_MIN_SEC = 8;
	static const float GARRISON_REISSUE_SEC = 30;
	static const float DESPAWN_BUY_BLOCK_SEC = 120;		// no purchase of a despawned unit's kind this long
	static const float DESPAWN_RECENT_SEC = 600;		// recentlyDespawned counts despawns of this window
	static const float STOOD_DOWN_KEEP_SEC = 120;
	static const float IDLE_LOG_SEC = 60;				// "CLW: idle check" log interval per side

	protected static ref map<string, string> s_mGarrison = new map<string, string>();	// unit id -> faction
	protected static ref map<string, float> s_mNoOrderSince = new map<string, float>();	// unit id -> world time it was first seen without an active order
	protected static ref map<string, int> s_mCost = new map<string, int>();				// unit id -> supplies charged for it (0 = free)
	protected static ref map<string, float> s_mGarrisonOrderAt = new map<string, float>();
	protected static ref map<string, float> s_mLastThink = new map<string, float>();	// faction -> last run
	protected static ref map<string, float> s_mIdleLogAt = new map<string, float>();	// faction -> last "idle check" log
	protected static ref map<string, float> s_mStoodDown = new map<string, float>();	// unit id -> when it was stood down
	protected static ref array<float> s_aDespawnAt = {};
	protected static ref array<string> s_aDespawnFaction = {};
	protected static ref array<string> s_aDespawnKind = {};
	protected static ref array<string> s_aDespawnUnit = {};
	protected static ref array<int> s_aDespawnRefund = {};

	// ---- lifecycle / ledger ----

	// War start (RemoteCommander.Init): forget the faction's state of a previous session.
	static void Reset(string faction)
	{
		array<string> gone = {};
		foreach (string id, string owner : s_mGarrison)
		{
			if (owner == faction)
				gone.Insert(id);
		}

		foreach (string stale : gone)
		{
			s_mGarrison.Remove(stale);
			s_mNoOrderSince.Remove(stale);
			s_mCost.Remove(stale);
			s_mGarrisonOrderAt.Remove(stale);
		}

		s_mLastThink.Remove(faction);
		s_mIdleLogAt.Remove(faction);
		for (int i = s_aDespawnAt.Count() - 1; i >= 0; i--)
		{
			if (s_aDespawnFaction[i] == faction)
			{
				s_aDespawnAt.Remove(i);
				s_aDespawnFaction.Remove(i);
				s_aDespawnKind.Remove(i);
				s_aDespawnUnit.Remove(i);
				s_aDespawnRefund.Remove(i);
			}
		}

		CLW_PendingOrders.Clear(faction);
	}

	// The spawner finished a unit (registry id; the crew group for a vehicle). charged = supplies paid for it, 0 for a start force.
	static void OnUnitReady(string unitId, string faction, int charged)
	{
		if (unitId.IsEmpty())
			return;

		s_mCost.Set(unitId, charged);
	}

	// ---- params ----

	protected static CLW_WarMode Mode()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war)
			return null;

		return war.GetMode();
	}

	static int GarrisonWanted()
	{
		CLW_WarMode mode = Mode();
		if (!mode)
			return DEFAULT_GARRISON;

		return Math.Clamp(mode.GetIntParam("garrisonSquads", DEFAULT_GARRISON), 0, 4);
	}

	static int IdleDespawnMin()
	{
		CLW_WarMode mode = Mode();
		if (!mode)
			return DEFAULT_IDLE_MIN;

		return Math.Clamp(mode.GetIntParam("idleDespawnMin", DEFAULT_IDLE_MIN), 0, 120);
	}

	// ---- queries ----

	static bool IsGarrison(string unitId)
	{
		return s_mGarrison.Contains(unitId);
	}

	static int GarrisonCount(string faction)
	{
		int count;
		foreach (string id, string owner : s_mGarrison)
		{
			if (owner == faction)
				count++;
		}

		return count;
	}

	// Seconds the unit has been without an active order (0 = it has one, is garrison or is not tracked).
	static int IdleSec(string unitId)
	{
		float since;
		if (!s_mNoOrderSince.Find(unitId, since))
			return 0;

		return Math.Max(0, Math.Round(CLW_Orders.Now() - since));
	}

	// True for a unit this watch stood down a moment ago (the commanders do not report it as lost).
	static bool WasStoodDown(string unitId)
	{
		float at;
		if (!s_mStoodDown.Find(unitId, at))
			return false;

		return CLW_Orders.Now() - at < STOOD_DOWN_KEEP_SEC;
	}

	static int RecentlyDespawned(string faction)
	{
		float now = CLW_Orders.Now();
		int count;
		for (int i = 0; i < s_aDespawnAt.Count(); i++)
		{
			if (s_aDespawnFaction[i] == faction && now - s_aDespawnAt[i] <= DESPAWN_RECENT_SEC)
				count++;
		}

		return count;
	}

	// True when a unit of this kind was stood down within 'withinSec' (the rule commander then does not buy that kind).
	static bool KindRecentlyDespawned(string faction, string kind, float withinSec)
	{
		float now = CLW_Orders.Now();
		for (int i = 0; i < s_aDespawnAt.Count(); i++)
		{
			if (s_aDespawnFaction[i] == faction && s_aDespawnKind[i] == kind && now - s_aDespawnAt[i] <= withinSec)
				return true;
		}

		return false;
	}

	// baseRules block of the situation.
	static string RulesJson(string faction)
	{
		string text = "{\"garrisonSquads\":" + GarrisonWanted() + ",\"garrison\":" + GarrisonCount(faction) + ",\"idleDespawnMin\":" + IdleDespawnMin() + ",\"recentlyDespawned\":" + RecentlyDespawned(faction) + ",\"hqReserve\":" + CLW_EconomyGuard.HqReserve();
		int last = -1;
		for (int i = s_aDespawnAt.Count() - 1; i >= 0; i--)
		{
			if (s_aDespawnFaction[i] == faction)
			{
				last = i;
				break;
			}
		}

		if (last >= 0)
		{
			text += ",\"lastDespawn\":{\"squad\":\"" + CLW_Situation.Escape(s_aDespawnUnit[last]) + "\",\"kind\":\"" + CLW_Situation.Escape(s_aDespawnKind[last]) + "\",\"refund\":" + s_aDespawnRefund[last];
			text += ",\"agoSec\":" + Math.Round(CLW_Orders.Now() - s_aDespawnAt[last]) + "}";
		}

		return text + "}";
	}

	// ---- the watch ----

	static void Think(CLW_RuleCommander rules, string faction)
	{
		if (!rules || !Replication.IsServer())
			return;

		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (!war || !war.IsRunning() || !registry || !bases)
			return;

		float now = CLW_Orders.Now();
		float lastRun = -1000;
		s_mLastThink.Find(faction, lastRun);
		if (now < lastRun)
			lastRun = -1000;	// a new session restarted the clock

		if (now - lastRun < THINK_MIN_SEC)
			return;

		s_mLastThink.Set(faction, now);
		vector hq = bases.GetBasePos(faction);
		if (hq == vector.Zero)
			return;

		CLW_Orders orders = rules.GetOrders();
		array<string> ids = {};
		registry.GetCommandableIds(faction, ids);

		// forget garrison bookkeeping of units that are gone
		array<string> stale = {};
		foreach (string knownId, string owner : s_mGarrison)
		{
			if (owner == faction && !ids.Contains(knownId))
				stale.Insert(knownId);
		}

		foreach (string goneId : stale)
		{
			Print(string.Format("CLW: garrison %1: %2 is gone", faction, goneId), LogLevel.NORMAL);
			s_mGarrison.Remove(goneId);
			s_mGarrisonOrderAt.Remove(goneId);
		}

		ThinkGarrison(orders, registry, faction, ids, hq, now);
		ThinkIdle(orders, registry, war, faction, ids, hq, now);
	}

	// Candidates are free infantry squads at the base: the one nearest to the HQ first.
	protected static void ThinkGarrison(CLW_Orders orders, CLW_SquadRegistry registry, string faction, array<string> ids, vector hq, float now)
	{
		int wanted = GarrisonWanted();
		int have = GarrisonCount(faction);
		while (have < wanted)
		{
			string pick;
			float best = BASE_RADIUS_M;
			foreach (string id : ids)
			{
				if (IsGarrison(id) || !IsFreeAtBase(orders, registry, id, hq, true))
					continue;

				if (registry.GetKind(id) != CLW_ForceKind.INFANTRY || orders.FindVehicleUnit(id))
					continue;

				vector unitPos;
				if (!orders.UnitPos(id, unitPos))
					continue;

				float dist = vector.DistanceXZ(unitPos, hq);
				if (dist < best)
				{
					best = dist;
					pick = id;
				}
			}

			if (pick.IsEmpty())
				break;

			s_mGarrison.Set(pick, faction);
			NameGarrison(registry, pick);
			s_mNoOrderSince.Remove(pick);
			have++;
			Print(string.Format("CLW: garrison %1: %2 assigned to the HQ (%3 of %4, %5 m from it)", faction, pick, have, wanted, Math.Round(best)), LogLevel.NORMAL);
			CLW_Debug.Say(string.Format("%1 garrison: %2 defends the HQ", faction, pick));
		}

		// keep the standing order alive
		foreach (string garrisonId, string garrisonFaction : s_mGarrison)
		{
			if (garrisonFaction != faction)
				continue;

			CLW_UnitOrder order = orders.GetOrder(garrisonId);
			if (order && order.m_sStatus == CLW_Orders.STATUS_ACTIVE)
				continue;

			float orderedAt = -1000;
			s_mGarrisonOrderAt.Find(garrisonId, orderedAt);
			if (now - orderedAt < GARRISON_REISSUE_SEC)
				continue;

			s_mGarrisonOrderAt.Set(garrisonId, now);
			string why;
			if (!orders.Issue(garrisonId, CLW_ECommand.DEFEND, "", hq, "garrison: defend the HQ", CLW_Orders.SOURCE_RULES, why))
				Print(string.Format("CLW: garrison %1: DEFEND the HQ for %2 refused: %3", faction, garrisonId, why), LogLevel.WARNING);
		}
	}

	// WP1 (readable names): a squad picked as garrison is named "HQ Garrison" ("HQ Garrison 2" for a second one of the side).
	protected static void NameGarrison(CLW_SquadRegistry registry, string unitId)
	{
		registry.SetRole(unitId, CLW_SquadRegistry.ROLE_GARRISON);
		Print(string.Format("CLW: garrison %1 named \"%2\"", unitId, registry.GetName(unitId)), LogLevel.NORMAL);
	}

	// A unit that stands at the base without anything to do and that this watch may touch: no human, no active order, not reserved
	// for a FOB / convoy, not a supply truck, alive (and for the garrison not weak), within the base radius.
	protected static bool IsFreeAtBase(CLW_Orders orders, CLW_SquadRegistry registry, string id, vector hq, bool forGarrison)
	{
		SCR_AIGroup group = registry.Find(id);
		if (!group || CLW_RuleCommander.HasHumanLeader(group) || group.GetPlayerCount() > 0)
			return false;

		if (CLW_FobManager.IsReserved(id) || CLW_PendingOrders.IsPendingUnit(id) || orders.UnitAlive(id) <= 0)
			return false;

		CLW_VehicleUnit unit = orders.FindVehicleUnit(id);
		string kind = registry.GetKind(id);
		if (unit)
			kind = unit.GetKind();
		if (kind == CLW_ForceKind.LOGISTICS)
			return false;

		CLW_UnitOrder order = orders.GetOrder(id);
		if (order && order.m_sStatus == CLW_Orders.STATUS_ACTIVE)
			return false;

		if (forGarrison && orders.IsWeak(id))
			return false;

		vector pos;
		if (!orders.UnitPos(id, pos))
			return false;

		return vector.DistanceXZ(pos, hq) <= BASE_RADIUS_M;
	}

	protected static void ThinkIdle(CLW_Orders orders, CLW_SquadRegistry registry, CLW_WarState war, string faction, array<string> ids, vector hq, float now)
	{
		int limitMin = IdleDespawnMin();
		// P10 diagnostics (one line per minute per side): why nothing idles
		int total = ids.Count();
		int nGarrison, nOrdered, nReserved, nHuman, nTruck, nCandidates, nAtBase, nFar;
		float oldest;
		float farthest;
		foreach (string id : ids)
		{
			if (IsGarrison(id))
			{
				s_mNoOrderSince.Remove(id);
				nGarrison++;
				continue;
			}

			// "without an order" anywhere (the situation shows idleSec so HQ can act), stand down only at the base
			SCR_AIGroup group = registry.Find(id);
			CLW_UnitOrder order = orders.GetOrder(id);
			bool hasOrder = order && order.m_sStatus == CLW_Orders.STATUS_ACTIVE;
			bool human = !group || CLW_RuleCommander.HasHumanLeader(group) || group.GetPlayerCount() > 0;
			bool reserved = CLW_FobManager.IsReserved(id) || CLW_PendingOrders.IsPendingUnit(id);
			if (hasOrder || human || reserved)
			{
				s_mNoOrderSince.Remove(id);
				if (hasOrder)
					nOrdered++;
				else if (human)
					nHuman++;
				else
					nReserved++;
				continue;
			}

			float since;
			if (!s_mNoOrderSince.Find(id, since))
			{
				since = now;
				s_mNoOrderSince.Set(id, since);
			}

			nCandidates++;
			float idleFor = now - since;
			if (idleFor > oldest)
				oldest = idleFor;

			CLW_VehicleUnit truckUnit = orders.FindVehicleUnit(id);
			string idKind = registry.GetKind(id);
			if (truckUnit)
				idKind = truckUnit.GetKind();
			if (idKind == CLW_ForceKind.LOGISTICS)
				nTruck++;

			vector idlePos;
			bool known = orders.UnitPos(id, idlePos);
			if (known && vector.DistanceXZ(idlePos, hq) <= BASE_RADIUS_M)
			{
				nAtBase++;
			}
			else
			{
				nFar++;
				if (known)
					farthest = Math.Max(farthest, vector.DistanceXZ(idlePos, hq));
			}

			if (limitMin <= 0 || idleFor < limitMin * 60)
				continue;

			if (!IsFreeAtBase(orders, registry, id, hq, false))
				continue;

			StandDown(orders, registry, war, faction, id, Math.Round(idleFor / 60.0), now);
		}

		float lastLog = -1000;
		s_mIdleLogAt.Find(faction, lastLog);
		if (now < lastLog)
			lastLog = -1000;	// a new session restarted the clock

		if (now - lastLog >= IDLE_LOG_SEC)
		{
			s_mIdleLogAt.Set(faction, now);
			string idleHead = string.Format("CLW: idle check %1: candidates %2, oldest %3 s (squads at base %4, excluded: garrison %5, ordered %6, reserved %7, human %8, truck %9", faction, nCandidates, Math.Round(oldest), nAtBase, nGarrison, nOrdered, nReserved, nHuman, nTruck);
			Print(idleHead + string.Format("; of %1 squads, %2 idle away from base up to %3 m; limit %4 min, radius %5 m)", total, nFar, Math.Round(farthest), limitMin, Math.Round(BASE_RADIUS_M)), LogLevel.NORMAL);
		}
	}

	protected static void StandDown(CLW_Orders orders, CLW_SquadRegistry registry, CLW_WarState war, string faction, string id, int minutes, float now)
	{
		string kind = registry.GetKind(id);
		string displayType = registry.GetDisplayType(id);
		CLW_VehicleUnit unit = orders.FindVehicleUnit(id);
		if (unit)
			kind = unit.GetKind();

		int refund;
		s_mCost.Find(id, refund);
		s_mStoodDown.Set(id, now);
		if (unit)
		{
			SCR_AIGroup cargo = unit.GetCargoGroup();
			SCR_AIGroup crew = unit.GetCrewGroup();
			IEntity vehicle = unit.GetVehicle();
			unit.Dissolve("idle at the base, stood down", false);
			CLW_ForceSpawner.DeleteGroupAndMembers(cargo);
			CLW_ForceSpawner.DeleteGroupAndMembers(crew);
			if (vehicle)
				SCR_EntityHelper.DeleteEntityAndChildren(vehicle);
		}
		else
		{
			CLW_ForceSpawner.DeleteGroupAndMembers(registry.Find(id));
		}

		if (refund > 0)
			war.Refund(faction, refund);

		s_aDespawnAt.Insert(now);
		s_aDespawnFaction.Insert(faction);
		s_aDespawnKind.Insert(kind);
		s_aDespawnUnit.Insert(id);
		s_aDespawnRefund.Insert(refund);
		s_mNoOrderSince.Remove(id);
		s_mCost.Remove(id);
		war.PushEvent(faction, string.Format("%1 stood down (idle %2 min at the base), refund %3", id, minutes, refund));
		Print(string.Format("CLW: idle squad %1 despawned after %2 min at base, refund %3 (%4, %5)", id, minutes, refund, kind, displayType), LogLevel.NORMAL);
		CLW_Debug.Say(string.Format("%1 stood down idle %2 (%3 min), refund %4", faction, id, minutes, refund));
	}

	// Rematch (CLW_Rematch.c): forget the idle / garrison state of every faction (Reset(faction) only knows the factions of the new war).
	static void RematchReset()
	{
		s_mGarrison.Clear();
		s_mNoOrderSince.Clear();
		s_mCost.Clear();
		s_mGarrisonOrderAt.Clear();
		s_mLastThink.Clear();
		s_mStoodDown.Clear();
		s_aDespawnAt.Clear();
		s_aDespawnFaction.Clear();
		s_aDespawnKind.Clear();
		s_aDespawnUnit.Clear();
		s_aDespawnRefund.Clear();
	}
}

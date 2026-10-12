// Logistics (M3 phase L, design section 5): supply convoys between the HQ and the FOBs. Capability `logistics`.
// A convoy is one supply truck unit (catalog kind `logistics`, cargo amount = CLW_ForceEntry.m_iCargo, both DATA) that
//   PLANNED     finds a free truck (not reserved) or buys one with CLW_ForceSpawner.BuyTagged (catalog data, no type is named here)
//   TO_SOURCE   drives to the source base when it is not there (a bought truck spawns at the HQ)
//   EN_ROUTE    cargo = min(truck capacity, source stock) is taken from the source stock AT DEPARTURE, the truck drives (MOVE order of the
//               vehicle unit); a destroyed / captured truck loses the cargo (log + event for the LLM)
//   DELIVERED   within ARRIVE_M of the destination the cargo is added to the destination stock (CLW_SupplyStore); the truck drives home
// The stock model is in CLW_SupplyStore.c (side ledger = HQ stock, FOBs have their own). Trucks of a convoy are RESERVED (IsReserved):
// the rule commander and LLM orders skip them. Max convoys in flight per side: mode parameter convoyMax (default 2).
// M4 WP5: the truck comes from CLW_ForceCatalog.GetEntries(side) (own entries + kin entries of the side pool); a refused purchase is retried,
// load / unload accept a stopped truck within IDLE_ARRIVE_M, a stalled truck escalates (re-issue, road point, give up with the cargo returned),
// a kin truck is not "captured" (only the enemy key counts), every refusal to start a convoy is logged with its reason.
// Server only. Log prefix: "CLW: convoy ...", "CLW: logistics ...".

enum CLW_EConvoyState
{
	PLANNED,
	TO_SOURCE,
	EN_ROUTE,
	DELIVERED,
	LOST,
	FAILED
}

class CLW_Convoy
{
	string m_sId;				// CV1, CV2, ...
	string m_sSide;
	string m_sFrom;				// base ids: "HQ" or a FOB id
	string m_sTo;
	string m_sSource;			// rules | llm
	string m_sReason;
	CLW_EConvoyState m_eState;
	int m_iCargo;				// supplies on the truck (0 until loaded)
	int m_iGoneChecks;			// consecutive failed truck checks before loading (a fresh truck can fail one check right after spawn)
	string m_sTruckId;			// registry id of the truck unit
	string m_sTag;				// tagged purchase in flight
	bool m_bLoaded;
	int m_iOrderTries;
	float m_fCreatedAt;
	float m_fStateAt;
	float m_fEndedAt;
	float m_fLastLogAt;
	vector m_vLastPos;
	// ---- M4 WP5 ----
	float m_fNextBuyAt;			// PLANNED: next purchase attempt (a refused purchase is retried until PLANNED_TIMEOUT_SEC)
	int m_iBuyTries;
	string m_sLastBuyError;
	float m_fBestDist;			// driving: smallest distance to the current target that counted as progress (0 = not set)
	float m_fBestAt;
	int m_iStalls;
	bool m_bLeg;				// driving to an interim road point first (route re-plan)
	vector m_vLeg;
	float m_fLegAt;

	bool IsLive()
	{
		return m_eState == CLW_EConvoyState.PLANNED || m_eState == CLW_EConvoyState.TO_SOURCE || m_eState == CLW_EConvoyState.EN_ROUTE;
	}

	string StateName()
	{
		switch (m_eState)
		{
			case CLW_EConvoyState.PLANNED: return "planned";
			case CLW_EConvoyState.TO_SOURCE: return "loading";
			case CLW_EConvoyState.EN_ROUTE: return "en_route";
			case CLW_EConvoyState.DELIVERED: return "delivered";
			case CLW_EConvoyState.LOST: return "lost";
			case CLW_EConvoyState.FAILED: return "failed";
		}

		return "?";
	}
}

class CLW_Convoys
{
	static const int TICK_MS = 3000;
	static const string ORDER_SOURCE = "convoy";		// CLW_Orders source of the truck orders
	static const int GONE_CHECKS_MAX = 4;				// failed truck checks in a row before a planned convoy gives up
	static const float ARRIVE_M = 60;					// truck this close to a base = arrived (load / unload)
	static const int MIN_CARGO = 40;					// smaller loads are not worth a convoy
	static const int DEFAULT_MAX = 2;
	static const float PLANNED_TIMEOUT_SEC = 240;
	static const float ROUTE_TIMEOUT_SEC = 1200;
	static const float SHOW_SEC = 300;					// ended convoys stay in the situation this long
	static const float LOG_SEC = 60;
	static const int MAX_ORDER_TRIES = 4;
	// ---- M4 WP5: robust trucks ----
	static const float IDLE_ARRIVE_M = 150;				// truck stopped (order ended, vehicle idle) this close = arrived (same radius as CLW_Orders.VEHICLE_STOPPED_DONE_M)
	static const float STALL_SEC = 120;					// no progress this long = stalled
	static const float STALL_IMPROVE_M = 20;
	static const int MAX_STALLS = 4;					// stalls before the convoy gives up (a loaded cargo goes back to the source stock)
	static const float BUY_RETRY_SEC = 20;				// refused truck purchase (AI limit, supplies): retried this often until PLANNED_TIMEOUT_SEC
	static const float LEG_MIN_M = 40;
	static const float LEG_MAX_M = 400;
	static const float LEG_ARRIVE_M = 50;
	static const float LEG_MAX_SEC = 150;
	static const float REFUSAL_LOG_SEC = 60;			// the same refusal text is logged this often at most

	protected static ref array<ref CLW_Convoy> s_aConvoys = {};
	protected static int s_iNext;
	protected static bool s_bTicking;
	protected static ref map<string, float> s_mRefusalLog = new map<string, float>();

	// ---- lifecycle ----

	// Game start (server): statics survive a Play restart.
	static void Reset()
	{
		GetGame().GetCallqueue().Remove(Tick);
		s_bTicking = false;
		s_aConvoys.Clear();
		s_iNext = 0;
		s_mRefusalLog.Clear();
		CLW_SupplyStore.Reset();
		CLW_LogisticsDoctrine.Reset();
	}

	// War SETUP -> RUNNING (after CLW_FobManager.OnWarStart).
	static void OnWarStart(notnull CLW_WarState war)
	{
		if (!Replication.IsServer())
			return;

		Reset();
		if (!CLW_Capabilities.Has("logistics"))
		{
			Print("CLW: logistics capability is off: one supply pool per side, FOBs are fed abstractly", LogLevel.NORMAL);
			return;
		}

		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (spawner)
		{
			spawner.GetOnTagged().Remove(OnSpawnTagged);
			spawner.GetOnTagged().Insert(OnSpawnTagged);
		}

		CLW_FobManager.GetOnLost().Remove(OnFobLost);
		CLW_FobManager.GetOnLost().Insert(OnFobLost);
		s_bTicking = true;
		GetGame().GetCallqueue().CallLater(Tick, TICK_MS, true);
		Print(string.Format("CLW: logistics ready: per-base stock (HQ = side ledger), convoyMax %1, truck data: %2", GetMax(), TruckSummary(war)), LogLevel.NORMAL);
	}

	protected static string TruckSummary(notnull CLW_WarState war)
	{
		string text;
		array<string> keys = {};
		war.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			CLW_ForceEntry entry = PickTruckEntry(key);
			if (!text.IsEmpty())
				text += "; ";
			if (entry)
				text += string.Format("%1 %2 cargo %3 cost %4 (entry source %5)", key, entry.m_sType, entry.m_iCargo, entry.m_iCost, entry.m_sSource);
			else
				text += key + " NO supply truck in the catalog (no convoys): " + ExplainNoTruck(key);
		}

		return text;
	}

	// Why a side has no usable supply truck: what its catalog holds (for the logs; the answer to "why does the doctrine not send convoys").
	static string ExplainNoTruck(string side)
	{
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!catalog)
			return "no force catalog";

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(side, entries);
		int logistics;
		int noVehicle;
		int noCargo;
		int noCrew;
		foreach (CLW_ForceEntry entry : entries)
		{
			if (entry.m_sKind != CLW_ForceKind.LOGISTICS)
				continue;

			logistics++;
			if (!entry.HasVehicle())
				noVehicle++;
			else if (entry.m_iCargo <= 0)
				noCargo++;
			else if (entry.m_bCrewless)
				noCrew++;
		}

		return string.Format("%1 catalog entries, %2 of kind logistics (%3 without vehicle, %4 without cargo amount, %5 without a default crew)", entries.Count(), logistics, noVehicle, noCargo, noCrew);
	}

	static int GetMax()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.GetMode())
			return DEFAULT_MAX;

		return Math.Clamp(war.GetMode().GetIntParam("convoyMax", DEFAULT_MAX), 1, 4);
	}

	// ---- data ----

	// The best supply truck of the side's catalog (CLW_ForceCatalog.GetEntries(side): own entries, plus the kin entries WP3's side pool adds
	// for roles the side lacks): kind logistics with a vehicle, a cargo amount and not marked crewless (m_bCrewless: "vehicle has
	// no default crew to spawn"). Most cargo per supply cost wins (ties: bigger cargo). Null when the side has none.
	static CLW_ForceEntry PickTruckEntry(string side)
	{
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!catalog)
			return null;

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(side, entries);
		CLW_ForceEntry pick;
		foreach (CLW_ForceEntry entry : entries)
		{
			if (entry.m_sKind != CLW_ForceKind.LOGISTICS || !entry.HasVehicle() || entry.m_iCargo <= 0 || entry.m_bCrewless)
				continue;

			if (!pick)
			{
				pick = entry;
				continue;
			}

			// cargo / cost compared by cross-multiplication
			int better = entry.m_iCargo * Math.Max(1, pick.m_iCost) - pick.m_iCargo * Math.Max(1, entry.m_iCost);
			if (better > 0 || (better == 0 && entry.m_iCargo > pick.m_iCargo))
				pick = entry;
		}

		return pick;
	}

	static bool HasTruckData(string side)
	{
		return PickTruckEntry(side) != null;
	}

	// ---- queries ----

	static bool IsReserved(string unitId)
	{
		if (unitId.IsEmpty())
			return false;

		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			if (convoy.IsLive() && convoy.m_sTruckId == unitId)
				return true;
		}

		return false;
	}

	static string ReservedFor(string unitId)
	{
		if (unitId.IsEmpty())
			return "";

		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			if (convoy.IsLive() && convoy.m_sTruckId == unitId)
				return string.Format("convoy %1 %2 -> %3", convoy.m_sId, convoy.m_sFrom, convoy.m_sTo);
		}

		return "";
	}

	static int CountLive(string side)
	{
		int count;
		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			if (convoy.m_sSide == side && convoy.IsLive())
				count++;
		}

		return count;
	}

	// A live convoy of the side bound for this base ("" = any base).
	static bool HasLiveTo(string side, string baseId)
	{
		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			if (convoy.m_sSide == side && convoy.IsLive() && convoy.m_sTo == baseId)
				return true;
		}

		return false;
	}

	// Supplies on live convoys bound for the base (counts as stock on its way for the doctrine).
	static int InboundTo(string side, string baseId)
	{
		int total;
		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			if (convoy.m_sSide == side && convoy.IsLive() && convoy.m_sTo == baseId)
				total += convoy.m_iCargo;
		}

		return total;
	}

	// World position of a base of the side: the HQ anchor, or an ACTIVE FOB. False when the base does not exist (any more).
	static bool BasePos(string side, string baseId, out vector pos)
	{
		if (CLW_SupplyStore.IsHq(baseId))
		{
			CLW_Bases bases = CLW_GameMode.GetBases();
			if (!bases)
				return false;

			pos = bases.GetBasePos(side);
			return pos != vector.Zero;
		}

		CLW_Fob fob = CLW_FobManager.Find(baseId);
		if (!fob || fob.m_sSide != side || !fob.IsActive())
			return false;

		pos = fob.m_vPos;
		return true;
	}

	protected static string NormBase(string baseId)
	{
		if (baseId.IsEmpty())
			return CLW_Services.BASE_HQ;

		string upper = baseId;
		upper.ToUpper();
		if (upper == "HQ" || upper == "BASE")
			return CLW_Services.BASE_HQ;

		return baseId;
	}

	// ---- start ----

	// All checks of a convoy order (no side effects).
	static bool CanStart(string side, string fromId, string toId, out string why)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
		{
			why = "war not running";
			return false;
		}

		if (!CLW_Capabilities.Has("logistics"))
		{
			why = "logistics is off in this war mode";
			return false;
		}

		string fromBase = NormBase(fromId);
		string toBase = NormBase(toId);
		if (fromBase == toBase)
		{
			why = "origin and destination are the same base";
			return false;
		}

		vector fromPos;
		vector toPos;
		if (!BasePos(side, fromBase, fromPos))
		{
			why = "unknown or inactive base " + fromBase;
			return false;
		}

		if (!BasePos(side, toBase, toPos))
		{
			why = "unknown or inactive base " + toBase;
			return false;
		}

		if (!HasTruckData(side))
		{
			why = "the faction has no supply truck in its catalog (" + ExplainNoTruck(side) + ")";
			return false;
		}

		if (CountLive(side) >= GetMax())
		{
			why = string.Format("convoy limit reached (%1 of %2 in flight)", CountLive(side), GetMax());
			return false;
		}

		if (HasLiveTo(side, toBase))
		{
			why = "a convoy is already on its way to " + toBase;
			return false;
		}

		if (CLW_SupplyStore.Get(side, fromBase) < MIN_CARGO)
		{
			why = string.Format("%1 has too little stock to send (%2, minimum %3)", fromBase, CLW_SupplyStore.Get(side, fromBase), MIN_CARGO);
			return false;
		}

		return true;
	}

	// The doctrine wanted a convoy for the FOB but the side has no supply truck: one line per side every 2 minutes.
	static void LogNoTruck(string side, string fobId)
	{
		string key = side + "|notruck";
		float now = CLW_Orders.Now();
		float last = -1000;
		s_mRefusalLog.Find(key, last);
		if (now < last)
			last = -1000;

		if (now - last < 2 * REFUSAL_LOG_SEC)
			return;

		s_mRefusalLog.Set(key, now);
		Print(string.Format("CLW: convoy NOT started for %1: %2 needs supplies but the side has no usable supply truck: %3", side, fobId, ExplainNoTruck(side)), LogLevel.NORMAL);
	}

	// "CLW: convoy NOT started ..." for a real start attempt that CanStart refused; the same text per side is logged every REFUSAL_LOG_SEC at most.
	protected static void LogRefusal(string side, string fromId, string toId, string source, string why)
	{
		string key = side + "|" + why;
		float now = CLW_Orders.Now();
		float last = -1000;
		s_mRefusalLog.Find(key, last);
		if (now < last)
			last = -1000;	// a new session restarted the clock

		if (now - last < REFUSAL_LOG_SEC)
			return;

		s_mRefusalLog.Set(key, now);
		Print(string.Format("CLW: convoy NOT started for %1: %2 -> %3 (source %4): %5 | stock: %6, in flight %7 of %8", side, NormBase(fromId), NormBase(toId), source, why, CLW_SupplyStore.Describe(side), CountLive(side), GetMax()), LogLevel.NORMAL);
	}

	static bool Start(string side, string fromId, string toId, string source, string reason, out string why)
	{
		if (!CanStart(side, fromId, toId, why))
		{
			LogRefusal(side, fromId, toId, source, why);
			return false;
		}

		s_iNext++;
		CLW_Convoy convoy = new CLW_Convoy();
		convoy.m_sId = "CV" + s_iNext;
		convoy.m_sSide = side;
		convoy.m_sFrom = NormBase(fromId);
		convoy.m_sTo = NormBase(toId);
		convoy.m_sSource = source;
		convoy.m_sReason = reason;
		convoy.m_eState = CLW_EConvoyState.PLANNED;
		convoy.m_fCreatedAt = CLW_Orders.Now();
		convoy.m_fStateAt = convoy.m_fCreatedAt;
		s_aConvoys.Insert(convoy);
		Print(string.Format("CLW: convoy %1 planned for %2: %3 -> %4 (stock %5), source=%6, reason: %7", convoy.m_sId, side, convoy.m_sFrom, convoy.m_sTo, CLW_SupplyStore.Get(side, convoy.m_sFrom), source, reason), LogLevel.NORMAL);
		CLW_ForceEntry truck = PickTruckEntry(side);
		if (truck)
			Print(string.Format("CLW: convoy %1 truck plan: %2 cargo %3 cost %4 (own entry: %5, from kin '%6'); stock %7", convoy.m_sId, truck.m_sType, truck.m_iCargo, truck.m_iCost, truck.m_sKinFrom.IsEmpty(), truck.m_sKinFrom, CLW_SupplyStore.Describe(side)), LogLevel.NORMAL);
		return true;
	}

	// ---- tick ----

	protected static void Tick()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;

		float now = CLW_Orders.Now();
		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			switch (convoy.m_eState)
			{
				case CLW_EConvoyState.PLANNED: StepPlanned(convoy, now); break;
				case CLW_EConvoyState.TO_SOURCE: StepToSource(convoy, now); break;
				case CLW_EConvoyState.EN_ROUTE: StepEnRoute(convoy, now); break;
			}
		}
	}

	protected static void SetState(CLW_Convoy convoy, CLW_EConvoyState state, string note)
	{
		string before = convoy.StateName();
		convoy.m_eState = state;
		convoy.m_fStateAt = CLW_Orders.Now();
		convoy.m_iOrderTries = 0;
		if (!convoy.IsLive())
			convoy.m_fEndedAt = convoy.m_fStateAt;

		Print(string.Format("CLW: convoy %1 %2 -> %3: %4", convoy.m_sId, before, convoy.StateName(), note), LogLevel.NORMAL);
	}

	// ---- units ----

	protected static CLW_Orders OrdersOf(string side)
	{
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (!commander || !commander.GetRules())
			return null;

		return commander.GetRules().GetOrders();
	}

	protected static bool IssueMove(string side, string unitId, vector pos, string reason, out string why)
	{
		CLW_Orders orders = OrdersOf(side);
		if (!orders)
		{
			why = "no orders object";
			return false;
		}

		return orders.Issue(unitId, CLW_ECommand.MOVE, "", pos, reason, ORDER_SOURCE, why);
	}

	// No order of its own, or the order finished.
	protected static bool IsFreeUnit(CLW_Orders orders, string id)
	{
		CLW_UnitOrder order = orders.GetOrder(id);
		return !order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE;
	}

	// The free supply truck unit nearest to the position, "" when none.
	protected static string FindFreeTruck(string side, vector around)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		CLW_Orders orders = OrdersOf(side);
		if (!registry || !orders)
			return "";

		array<string> ids = {};
		registry.GetCommandableIds(side, ids);
		string best;
		float bestDist = 1000000000;
		foreach (string id : ids)
		{
			CLW_VehicleUnit unit = orders.FindVehicleUnit(id);
			if (!unit || unit.GetKind() != CLW_ForceKind.LOGISTICS)
				continue;

			if (!registry.Find(id) || CLW_FobManager.IsReserved(id) || orders.UnitAlive(id) <= 0 || orders.IsWeak(id) || !IsFreeUnit(orders, id))
				continue;

			vector pos;
			if (!orders.UnitPos(id, pos))
				continue;

			float dist = vector.DistanceXZ(pos, around);
			if (dist < bestDist)
			{
				bestDist = dist;
				best = id;
			}
		}

		return best;
	}

	// Cargo capacity of a truck unit (catalog data).
	protected static int CapacityOf(string side, string unitId)
	{
		CLW_Orders orders = OrdersOf(side);
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!orders || !catalog)
			return 0;

		CLW_VehicleUnit unit = orders.FindVehicleUnit(unitId);
		if (!unit)
			return 0;

		CLW_ForceEntry entry = catalog.Find(side, unit.GetTypeId(), true);
		if (entry && entry.m_iCargo > 0)
			return entry.m_iCargo;

		// the unit's type is not in the side's list (e.g. a pool refresh): the side's truck data is the best guess
		CLW_ForceEntry fallback = PickTruckEntry(side);
		if (!fallback)
			return 0;

		Print(string.Format("CLW: convoy truck %1 type '%2' has no cargo amount in the %3 catalog: using %4 (%5)", unitId, unit.GetTypeId(), side, fallback.m_sType, fallback.m_iCargo), LogLevel.WARNING);
		return fallback.m_iCargo;
	}

	protected static bool TruckAlive(CLW_Convoy convoy, CLW_Orders orders, out vector pos, out string why)
	{
		why = "";
		if (convoy.m_sTruckId.IsEmpty() || !orders)
		{
			why = "no truck id / orders";
			return false;
		}

		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!registry || !registry.Find(convoy.m_sTruckId))
		{
			why = "not in the squad registry";
			return false;
		}

		if (orders.UnitAlive(convoy.m_sTruckId) <= 0)
		{
			why = "no crew alive";
			return false;
		}

		if (!orders.UnitPos(convoy.m_sTruckId, pos))
		{
			why = "no position";
			return false;
		}

		// captured: the vehicle changed to the ENEMY side. (A kin truck from the side pool may carry the kin faction's key, not the side's own:
		// that is not a capture, so only the enemy key counts.)
		CLW_VehicleUnit unit = orders.FindVehicleUnit(convoy.m_sTruckId);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (unit && unit.GetVehicle() && war)
		{
			string owner = war.GetEntityFactionKey(unit.GetVehicle());
			if (!owner.IsEmpty() && owner == war.GetEnemyKey(convoy.m_sSide))
			{
				why = "captured by " + owner;
				return false;
			}
		}

		return true;
	}

	// ---- planned: truck ----

	protected static void StepPlanned(CLW_Convoy convoy, float now)
	{
		if (now - convoy.m_fStateAt > PLANNED_TIMEOUT_SEC)
		{
			string last = "";
			if (!convoy.m_sLastBuyError.IsEmpty())
				last = string.Format(" (%1 purchase attempts, last refusal: %2)", convoy.m_iBuyTries, convoy.m_sLastBuyError);

			End(convoy, CLW_EConvoyState.FAILED, "no supply truck after " + Math.Round(PLANNED_TIMEOUT_SEC) + " s" + last);
			return;
		}

		vector fromPos;
		if (!BasePos(convoy.m_sSide, convoy.m_sFrom, fromPos))
		{
			End(convoy, CLW_EConvoyState.FAILED, "the source base " + convoy.m_sFrom + " is gone");
			return;
		}

		if (!convoy.m_sTruckId.IsEmpty())
		{
			BeginLoad(convoy, fromPos);
			return;
		}

		if (!convoy.m_sTag.IsEmpty())
			return;	// purchase in flight (OnSpawnTagged)

		string found = FindFreeTruck(convoy.m_sSide, fromPos);
		if (!found.IsEmpty())
		{
			convoy.m_sTruckId = found;
			Print(string.Format("CLW: convoy %1 truck assigned: %2 (free unit)", convoy.m_sId, found), LogLevel.NORMAL);
			BeginLoad(convoy, fromPos);
			return;
		}

		if (now >= convoy.m_fNextBuyAt)
			BuyTruck(convoy, now);
	}

	// A refused purchase (engine AI limit, supplies, spawn spot) is retried every BUY_RETRY_SEC until PLANNED_TIMEOUT_SEC; a free truck is
	// looked for on every tick meanwhile. Without truck data or a spawner the convoy fails at once, with the catalog explanation.
	protected static void BuyTruck(CLW_Convoy convoy, float now)
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		CLW_ForceEntry pick = PickTruckEntry(convoy.m_sSide);
		if (!spawner || !pick)
		{
			string cause = "no spawner";
			if (spawner)
				cause = "no supply truck in the catalog (" + ExplainNoTruck(convoy.m_sSide) + ")";

			End(convoy, CLW_EConvoyState.FAILED, cause);
			return;
		}

		convoy.m_sTag = convoy.m_sId + "_truck";
		string error;
		if (!spawner.BuyTagged(convoy.m_sSide, pick.m_sType, "convoy " + convoy.m_sId, convoy.m_sTag, error))
		{
			convoy.m_sTag = "";
			convoy.m_iBuyTries++;
			convoy.m_sLastBuyError = error;
			convoy.m_fNextBuyAt = now + BUY_RETRY_SEC;
			Print(string.Format("CLW: convoy %1 truck purchase refused (try %2): %3; %4 (%5 cargo, cost %6) retried in %7 s, a free truck is looked for meanwhile", convoy.m_sId, convoy.m_iBuyTries, error, pick.m_sType, pick.m_iCargo, pick.m_iCost, Math.Round(BUY_RETRY_SEC)), LogLevel.NORMAL);
			return;
		}

		Print(string.Format("CLW: convoy %1 supply truck bought: %2 (cargo %3, cost %4, from kin '%5')", convoy.m_sId, pick.m_sType, pick.m_iCargo, pick.m_iCost, pick.m_sKinFrom), LogLevel.NORMAL);
	}

	// A tagged purchase of the spawner finished (unitId set) or failed (unitId empty).
	static void OnSpawnTagged(string tag, string unitId)
	{
		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			if (convoy.m_sTag != tag)
				continue;

			if (unitId.IsEmpty())
			{
				// the purchase was accepted but the spawn failed: retry like a refusal
				convoy.m_sTag = "";
				convoy.m_iBuyTries++;
				convoy.m_sLastBuyError = "the spawn failed";
				convoy.m_fNextBuyAt = CLW_Orders.Now() + BUY_RETRY_SEC;
				Print(string.Format("CLW: convoy %1 supply truck purchase failed after it was accepted (try %2): retried in %3 s until %4 s after planning", convoy.m_sId, convoy.m_iBuyTries, Math.Round(BUY_RETRY_SEC), Math.Round(PLANNED_TIMEOUT_SEC)), LogLevel.NORMAL);
			}
			else if (convoy.IsLive())
			{
				convoy.m_sTruckId = unitId;
				Print(string.Format("CLW: convoy %1 supply truck ready: %2", convoy.m_sId, unitId), LogLevel.NORMAL);
			}

			return;
		}
	}

	// ---- loading ----

	// The truck is stopped (its order ended, vehicle idle) within IDLE_ARRIVE_M of the base: as good as arrived. Trucks cannot always get
	// to the exact base anchor (buildings, props), the vehicle unit gives up there ("vehicle arrived (<= 150 m)").
	protected static bool Arrived(CLW_Convoy convoy, CLW_Orders orders, float dist)
	{
		if (dist <= ARRIVE_M)
			return true;

		if (dist > IDLE_ARRIVE_M || !orders)
			return false;

		CLW_UnitOrder order = orders.GetOrder(convoy.m_sTruckId);
		if (order && order.m_sStatus == CLW_Orders.STATUS_ACTIVE)
			return false;

		CLW_VehicleUnit unit = orders.FindVehicleUnit(convoy.m_sTruckId);
		return unit && unit.GetStateName() == "MOUNTED_IDLE";
	}

	// The truck is assigned: load when it is at the source, else drive there first.
	protected static void BeginLoad(CLW_Convoy convoy, vector fromPos)
	{
		CLW_Orders orders = OrdersOf(convoy.m_sSide);
		vector pos;
		string truckWhy;
		if (!TruckAlive(convoy, orders, pos, truckWhy))
		{
			convoy.m_iGoneChecks++;
			if (convoy.m_iGoneChecks < GONE_CHECKS_MAX)
			{
				Print(string.Format("CLW: convoy %1 truck %2 not usable yet (%3), check %4 of %5", convoy.m_sId, convoy.m_sTruckId, truckWhy, convoy.m_iGoneChecks, GONE_CHECKS_MAX), LogLevel.NORMAL);
				return;	// StepPlanned calls again next tick
			}

			End(convoy, CLW_EConvoyState.FAILED, "the supply truck is gone before loading (" + truckWhy + ")");
			return;
		}
		convoy.m_iGoneChecks = 0;

		float dist = vector.DistanceXZ(pos, fromPos);
		if (Arrived(convoy, orders, dist))
		{
			Load(convoy, fromPos);
			return;
		}

		string why;
		bool ordered = TakeMove(convoy, fromPos, "convoy " + convoy.m_sId + " to the source " + convoy.m_sFrom, why);
		convoy.m_fBestDist = 0;
		convoy.m_bLeg = false;
		SetState(convoy, CLW_EConvoyState.TO_SOURCE, string.Format("truck %1 drives to %2 (%3 m, ordered=%4 %5)", convoy.m_sTruckId, convoy.m_sFrom, Math.Round(dist), ordered, why));
	}

	protected static void StepToSource(CLW_Convoy convoy, float now)
	{
		if (now - convoy.m_fStateAt > ROUTE_TIMEOUT_SEC)
		{
			End(convoy, CLW_EConvoyState.FAILED, "the truck did not reach the source in time");
			return;
		}

		vector fromPos;
		if (!BasePos(convoy.m_sSide, convoy.m_sFrom, fromPos))
		{
			End(convoy, CLW_EConvoyState.FAILED, "the source base " + convoy.m_sFrom + " is gone");
			return;
		}

		CLW_Orders orders = OrdersOf(convoy.m_sSide);
		vector pos;
		string truckWhy;
		if (!TruckAlive(convoy, orders, pos, truckWhy))
		{
			End(convoy, CLW_EConvoyState.FAILED, "the supply truck was lost on the way to the source (" + truckWhy + ")");
			return;
		}

		float dist = vector.DistanceXZ(pos, fromPos);
		if (Arrived(convoy, orders, dist))
		{
			Load(convoy, fromPos);
			return;
		}

		Drive(convoy, orders, pos, fromPos, "to the source");
	}

	// Takes the cargo from the source stock (deducted at departure) and sends the truck off.
	protected static void Load(CLW_Convoy convoy, vector fromPos)
	{
		vector toPos;
		if (!BasePos(convoy.m_sSide, convoy.m_sTo, toPos))
		{
			End(convoy, CLW_EConvoyState.FAILED, "the destination " + convoy.m_sTo + " is gone");
			return;
		}

		int capacity = CapacityOf(convoy.m_sSide, convoy.m_sTruckId);
		int stock = CLW_SupplyStore.Get(convoy.m_sSide, convoy.m_sFrom);
		int cargo = Math.Min(capacity, stock);
		if (cargo < MIN_CARGO || !CLW_SupplyStore.TrySpend(convoy.m_sSide, convoy.m_sFrom, cargo))
		{
			End(convoy, CLW_EConvoyState.FAILED, string.Format("nothing to load at %1 (stock %2, capacity %3)", convoy.m_sFrom, stock, capacity));
			return;
		}

		convoy.m_iCargo = cargo;
		convoy.m_bLoaded = true;
		convoy.m_fBestDist = 0;
		convoy.m_bLeg = false;
		convoy.m_iStalls = 0;
		string why;
		bool ordered = TakeMove(convoy, toPos, "convoy " + convoy.m_sId + " to " + convoy.m_sTo, why);
		SetState(convoy, CLW_EConvoyState.EN_ROUTE, string.Format("loaded %1 supplies at %2 (stock now %3), truck %4 drives to %5 %6 m away (ordered=%7 %8)", cargo, convoy.m_sFrom, CLW_SupplyStore.Get(convoy.m_sSide, convoy.m_sFrom), convoy.m_sTruckId, convoy.m_sTo, Math.Round(vector.DistanceXZ(fromPos, toPos)), ordered, why));
		Print(string.Format("CLW: convoy %1 departs %2 -> %3 with %4 supplies", convoy.m_sId, convoy.m_sFrom, convoy.m_sTo, cargo), LogLevel.NORMAL);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.PushEvent(convoy.m_sSide, string.Format("convoy %1 left %2 for %3 with %4 supplies", convoy.m_sId, convoy.m_sFrom, convoy.m_sTo, cargo));
	}

	// ---- en route ----

	// The truck's move order: it takes over from its previous order (forgotten, so a re-issue of the same target is not swallowed as
	// "already executing" and a stable-order refusal cannot block it).
	protected static bool TakeMove(CLW_Convoy convoy, vector pos, string reason, out string why)
	{
		CLW_Orders orders = OrdersOf(convoy.m_sSide);
		if (!orders)
		{
			why = "no orders object";
			return false;
		}

		orders.ForgetOrder(convoy.m_sTruckId);
		return orders.Issue(convoy.m_sTruckId, CLW_ECommand.MOVE, "", pos, reason, ORDER_SOURCE, why);
	}

	// Drives the truck toward a base position: progress watch (STALL_SEC without STALL_IMPROVE_M closer = stall), stall escalation
	// (re-issue; from the 2nd stall an interim road point first; MAX_STALLS = give up, a loaded cargo goes back to the source stock),
	// re-issue of an ended order. False when the convoy ended.
	protected static bool Drive(CLW_Convoy convoy, CLW_Orders orders, vector pos, vector baseTarget, string label)
	{
		float now = CLW_Orders.Now();
		string why;
		vector target = baseTarget;
		string targetText = label;
		if (convoy.m_bLeg)
		{
			float legDist = vector.DistanceXZ(pos, convoy.m_vLeg);
			CLW_UnitOrder legOrder = orders.GetOrder(convoy.m_sTruckId);
			bool legIdle = !legOrder || legOrder.m_sStatus != CLW_Orders.STATUS_ACTIVE;
			if (legDist <= LEG_ARRIVE_M || legIdle || now - convoy.m_fLegAt > LEG_MAX_SEC)
			{
				convoy.m_bLeg = false;
				convoy.m_fBestDist = 0;
				bool resumed = TakeMove(convoy, baseTarget, "convoy " + convoy.m_sId + " " + label, why);
				Print(string.Format("CLW: convoy %1 road point done (%2 m from it, orderEnded=%3): truck %4 drives on %5, %6 m to go (ordered=%7 %8)", convoy.m_sId, Math.Round(legDist), legIdle, convoy.m_sTruckId, label, Math.Round(vector.DistanceXZ(pos, baseTarget)), resumed, why), LogLevel.NORMAL);
				return true;
			}

			target = convoy.m_vLeg;
			targetText = "road point (" + label + ")";
		}

		float dist = vector.DistanceXZ(pos, baseTarget);
		float tdist = vector.DistanceXZ(pos, target);
		if (convoy.m_fBestDist <= 0 || tdist < convoy.m_fBestDist - STALL_IMPROVE_M)
		{
			convoy.m_fBestDist = Math.Max(tdist, 1);
			convoy.m_fBestAt = now;
			convoy.m_iOrderTries = 0;
		}
		else if (now - convoy.m_fBestAt >= STALL_SEC)
		{
			convoy.m_iStalls++;
			convoy.m_fBestDist = 0;
			convoy.m_iOrderTries = 0;
			Print(string.Format("CLW: convoy %1 STALL %2 of %3: truck %4 made no progress for %5 s, %6 m from the target %7", convoy.m_sId, convoy.m_iStalls, MAX_STALLS, convoy.m_sTruckId, Math.Round(now - convoy.m_fBestAt), Math.Round(dist), label), LogLevel.NORMAL);
			if (convoy.m_iStalls >= MAX_STALLS)
			{
				GiveUp(convoy, string.Format("the truck stalled %1 times, %2 m short of the target %3", convoy.m_iStalls, Math.Round(dist), label));
				return false;
			}

			if (convoy.m_iStalls >= 2 && !convoy.m_bLeg)
			{
				vector leg;
				if (CLW_RouteLeg.Find(pos, baseTarget, LEG_MIN_M, LEG_MAX_M, leg))
				{
					convoy.m_bLeg = true;
					convoy.m_vLeg = leg;
					convoy.m_fLegAt = now;
					bool legOrdered = TakeMove(convoy, leg, "convoy " + convoy.m_sId + " via a road point", why);
					Print(string.Format("CLW: convoy %1 route re-planned: truck %2 goes to the road point %3 (%4 m away, grid %5) first (ordered=%6 %7)", convoy.m_sId, convoy.m_sTruckId, leg, Math.Round(vector.DistanceXZ(pos, leg)), CLW_Places.Grid(leg), legOrdered, why), LogLevel.NORMAL);
					return true;
				}
			}

			bool again = TakeMove(convoy, baseTarget, "convoy " + convoy.m_sId + " " + label, why);
			Print(string.Format("CLW: convoy %1 order taken over again, move re-issued (ordered=%2 %3)", convoy.m_sId, again, why), LogLevel.NORMAL);
			return true;
		}

		CLW_UnitOrder order = orders.GetOrder(convoy.m_sTruckId);
		if (!order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE)
		{
			// the order ended short of the target: re-issue a few times, then wait for the stall rule (it escalates after STALL_SEC)
			if (convoy.m_iOrderTries < MAX_ORDER_TRIES)
			{
				convoy.m_iOrderTries++;
				bool ordered = TakeMove(convoy, target, "convoy " + convoy.m_sId + " " + label, why);
				Print(string.Format("CLW: convoy %1 move re-issued (%2 of %3, %4 m to go, ordered=%5 %6)", convoy.m_sId, convoy.m_iOrderTries, MAX_ORDER_TRIES, Math.Round(tdist), ordered, why), LogLevel.NORMAL);
			}
		}

		if (now - convoy.m_fLastLogAt >= LOG_SEC)
		{
			convoy.m_fLastLogAt = now;
			string orderText = "none";
			if (order)
				orderText = CLW_Command.ToName(order.m_eCommand) + "/" + order.m_sStatus;

			Print(string.Format("CLW: convoy %1 truck %2 en route %3, %4 m to go (%5 m to the %6), best %7 m, order=%8", convoy.m_sId, convoy.m_sTruckId, label, Math.Round(dist), Math.Round(tdist), targetText, Math.Round(convoy.m_fBestDist), orderText), LogLevel.NORMAL);
		}

		return true;
	}

	// The convoy cannot go on but the truck is alive: a loaded cargo returns to the source stock, the truck drives home.
	protected static void GiveUp(CLW_Convoy convoy, string why)
	{
		if (convoy.m_bLoaded && convoy.m_iCargo > 0)
		{
			TurnBack(convoy, why);
			return;
		}

		End(convoy, CLW_EConvoyState.FAILED, why);
		SendHome(convoy);
	}

	protected static void StepEnRoute(CLW_Convoy convoy, float now)
	{
		CLW_Orders orders = OrdersOf(convoy.m_sSide);
		vector pos;
		string truckWhy;
		if (!TruckAlive(convoy, orders, pos, truckWhy))
		{
			End(convoy, CLW_EConvoyState.LOST, "the truck was destroyed or captured (" + truckWhy + ")");
			return;
		}

		convoy.m_vLastPos = pos;
		vector toPos;
		if (!BasePos(convoy.m_sSide, convoy.m_sTo, toPos))
		{
			TurnBack(convoy, "the destination " + convoy.m_sTo + " is gone");
			return;
		}

		float dist = vector.DistanceXZ(pos, toPos);
		if (Arrived(convoy, orders, dist))
		{
			Deliver(convoy);
			return;
		}

		if (now - convoy.m_fStateAt > ROUTE_TIMEOUT_SEC)
		{
			GiveUp(convoy, "the truck did not arrive in time (" + Math.Round(dist) + " m short)");
			return;
		}

		Drive(convoy, orders, pos, toPos, "to " + convoy.m_sTo);
	}

	protected static void Deliver(CLW_Convoy convoy)
	{
		CLW_SupplyStore.Add(convoy.m_sSide, convoy.m_sTo, convoy.m_iCargo);
		int stock = CLW_SupplyStore.Get(convoy.m_sSide, convoy.m_sTo);
		Print(string.Format("CLW: convoy %1 delivered %2 supplies to %3 (stock now %4)", convoy.m_sId, convoy.m_iCargo, convoy.m_sTo, stock), LogLevel.NORMAL);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.PushEvent(convoy.m_sSide, string.Format("convoy %1 delivered %2 supplies to %3", convoy.m_sId, convoy.m_iCargo, convoy.m_sTo));

		CLW_Debug.Say(string.Format("%1 convoy %2 delivered %3 to %4", convoy.m_sSide, convoy.m_sId, convoy.m_iCargo, convoy.m_sTo));
		convoy.m_iCargo = 0;
		SetState(convoy, CLW_EConvoyState.DELIVERED, "unloaded");
		SendHome(convoy);
	}

	// The truck is free again: drive it to the HQ (it waits there for the next convoy).
	protected static void SendHome(CLW_Convoy convoy)
	{
		vector home;
		if (convoy.m_sTruckId.IsEmpty() || !BasePos(convoy.m_sSide, CLW_Services.BASE_HQ, home))
			return;

		string why;
		IssueMove(convoy.m_sSide, convoy.m_sTruckId, home, "convoy " + convoy.m_sId + " truck returns to the HQ", why);
	}

	// Destination lost / vanished with the cargo on board: the load goes back to the source stock, the truck returns.
	protected static void TurnBack(CLW_Convoy convoy, string why)
	{
		ReturnCargo(convoy, why);
		End(convoy, CLW_EConvoyState.FAILED, why);
		SendHome(convoy);
	}

	// The cargo on board goes back into the source stock (HQ when the source base is gone). No-op when nothing is loaded.
	protected static void ReturnCargo(CLW_Convoy convoy, string why)
	{
		if (!convoy.m_bLoaded || convoy.m_iCargo <= 0)
			return;

		vector fromPos;
		string returnTo = convoy.m_sFrom;
		if (!BasePos(convoy.m_sSide, returnTo, fromPos))
			returnTo = CLW_Services.BASE_HQ;

		CLW_SupplyStore.Add(convoy.m_sSide, returnTo, convoy.m_iCargo);
		Print(string.Format("CLW: convoy %1 turned back: %2; %3 supplies returned to %4 (stock: %5)", convoy.m_sId, why, convoy.m_iCargo, returnTo, CLW_SupplyStore.Describe(convoy.m_sSide)), LogLevel.NORMAL);
		convoy.m_iCargo = 0;
	}

	// Ends the convoy. LOST = the cargo (if loaded) is gone: log + event for the LLM. FAILED = nothing was lost.
	protected static void End(CLW_Convoy convoy, CLW_EConvoyState state, string why)
	{
		if (!convoy.IsLive())
			return;

		int lostCargo;
		if (state == CLW_EConvoyState.LOST)
			lostCargo = convoy.m_iCargo;
		else
			ReturnCargo(convoy, why);	// FAILED with cargo on board (e.g. a stuck truck): nothing is lost silently, the load goes back

		SetState(convoy, state, why);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (state == CLW_EConvoyState.LOST)
		{
			Print(string.Format("CLW: convoy %1 LOST near grid %2: %3, %4 supplies lost (%5 -> %6)", convoy.m_sId, CLW_Places.Grid(convoy.m_vLastPos), why, lostCargo, convoy.m_sFrom, convoy.m_sTo), LogLevel.WARNING);
			if (war)
				war.PushEvent(convoy.m_sSide, string.Format("convoy %1 to %2 LOST near %3 with %4 supplies: %5", convoy.m_sId, convoy.m_sTo, CLW_Places.Grid(convoy.m_vLastPos), lostCargo, why));

			CLW_Debug.Announce(string.Format("%1 supply convoy %2 lost with %3 supplies", convoy.m_sSide, convoy.m_sId, lostCargo));
			convoy.m_iCargo = 0;
		}
		else if (war)
		{
			war.PushEvent(convoy.m_sSide, string.Format("convoy %1 %2 -> %3 cancelled: %4", convoy.m_sId, convoy.m_sFrom, convoy.m_sTo, why));
		}
	}

	// CLW_FobManager.GetOnLost() (fobId, side): the FOB's stock is gone, convoys to / from it are cancelled.
	static void OnFobLost(string fobId, string side)
	{
		int lost = CLW_SupplyStore.Clear(side, fobId);
		if (lost > 0)
		{
			CLW_WarState war = CLW_GameMode.GetWarState();
			if (war)
				war.PushEvent(side, string.Format("%1 lost with %2 supplies in stock", fobId, lost));
		}

		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			if (convoy.m_sSide != side || !convoy.IsLive())
				continue;

			if (convoy.m_sTo == fobId)
				TurnBack(convoy, fobId + " was lost");
			else if (convoy.m_sFrom == fobId && !convoy.m_bLoaded)
				End(convoy, CLW_EConvoyState.FAILED, fobId + " was lost before loading");
		}
	}

	// ---- situation JSON ----

	// Convoys of the side (live ones and ones ended in the last SHOW_SEC): [{id, from, to, cargo, state, pos, grid}].
	static string ConvoysJson(string side)
	{
		string text = "";
		float now = CLW_Orders.Now();
		CLW_Orders orders = OrdersOf(side);
		foreach (CLW_Convoy convoy : s_aConvoys)
		{
			if (convoy.m_sSide != side)
				continue;

			if (!convoy.IsLive() && now - convoy.m_fEndedAt > SHOW_SEC)
				continue;

			vector pos = convoy.m_vLastPos;
			if (convoy.IsLive() && orders && !convoy.m_sTruckId.IsEmpty())
				orders.UnitPos(convoy.m_sTruckId, pos);

			if (pos == vector.Zero)
				BasePos(side, convoy.m_sFrom, pos);

			if (!text.IsEmpty())
				text += ",";
			text += "{\"id\":\"" + convoy.m_sId + "\",\"from\":\"" + CLW_Situation.Escape(convoy.m_sFrom) + "\",\"to\":\"" + CLW_Situation.Escape(convoy.m_sTo) + "\",\"cargo\":" + convoy.m_iCargo;
			text += ",\"state\":\"" + convoy.StateName() + "\",\"pos\":[" + Math.Round(pos[0]) + "," + Math.Round(pos[2]) + "],\"grid\":\"" + CLW_Places.Grid(pos) + "\"";
			if (!convoy.m_sTruckId.IsEmpty() && convoy.IsLive())
				text += ",\"truck\":\"" + CLW_Situation.Escape(convoy.m_sTruckId) + "\"";
			text += "}";
		}

		return text;
	}

	// {"convoyMax":2,"inFlight":1,"minCargo":40,"lowStock":120,"truckCargo":400}
	static string RulesJson(string side)
	{
		int cargo;
		CLW_ForceEntry truck = PickTruckEntry(side);
		if (truck)
			cargo = truck.m_iCargo;

		return "{\"convoyMax\":" + GetMax() + ",\"inFlight\":" + CountLive(side) + ",\"minCargo\":" + MIN_CARGO + ",\"lowStock\":" + CLW_LogisticsDoctrine.LOW_STOCK + ",\"truckCargo\":" + cargo + "}";
	}
}

// Rule commander doctrine (called from CLW_RuleCommander.Think): an active FOB whose stock (plus what is already on its way) is below
// LOW_STOCK gets a RESUPPLY from the HQ when the HQ stock allows, one convoy at a time per FOB. Same registry entries as the LLM.
class CLW_LogisticsDoctrine
{
	static const int LOW_STOCK = 120;				// FOB stock below this asks for a convoy
	static const float START_DELAY_SEC = 60;
	static const float INTERVAL_SEC = 20;			// per side

	protected static ref map<string, float> s_mLast = new map<string, float>();

	static void Reset()
	{
		s_mLast.Clear();
	}

	static void Think(CLW_RuleCommander rules, string side, CLW_WarState war)
	{
		if (!rules || !war || !CLW_Capabilities.Has("logistics") || !CLW_Capabilities.Has("fob"))
			return;

		if (war.GetElapsedSec() < START_DELAY_SEC)
			return;

		float now = CLW_Orders.Now();
		float last = -1000;
		s_mLast.Find(side, last);
		if (now < last)
			last = -1000;	// a new session restarted the clock

		if (now - last < INTERVAL_SEC)
			return;

		s_mLast.Set(side, now);
		array<CLW_Fob> own = {};
		CLW_FobManager.GetFobs(side, own, true);
		foreach (CLW_Fob fob : own)
		{
			if (CLW_Convoys.HasLiveTo(side, fob.m_sId))
				continue;

			if (CLW_SupplyStore.Get(side, fob.m_sId) >= LOW_STOCK)
				continue;

			if (!CLW_Convoys.HasTruckData(side))
			{
				// no supply truck for this side (own catalog + side pool): say so once in a while instead of a refused RESUPPLY every think
				CLW_Convoys.LogNoTruck(side, fob.m_sId);
				return;
			}

			if (war.GetSupplies(side) - CLW_RuleCommander.SUPPLY_RESERVE < CLW_Convoys.MIN_CARGO)
				return;	// the HQ cannot spare a load

			CLW_ActionCall call = new CLW_ActionCall();
			call.m_sAction = "RESUPPLY";
			call.m_sFaction = side;
			call.m_sAuthority = CLW_ActionEntry.AUTH_HQ;
			call.m_sSource = "rules";
			call.m_Rules = rules;
			call.SetString("fob", fob.m_sId);
			call.m_sReason = string.Format("doctrine: %1 stock %2 below %3", fob.m_sId, CLW_SupplyStore.Get(side, fob.m_sId), LOW_STOCK);
			string result;
			if (CLW_ActionRegistry.Get().Run(call, result))
				Print(string.Format("CLW: logistics doctrine %1: RESUPPLY %2 (%3)", side, fob.m_sId, call.m_sReason), LogLevel.NORMAL);
			else
				Print(string.Format("CLW: logistics doctrine %1: RESUPPLY %2 refused: %3", side, fob.m_sId, result), LogLevel.NORMAL);

			return;	// one per think
		}
	}
}

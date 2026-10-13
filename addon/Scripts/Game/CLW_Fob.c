// FOBs (M3 phase F, design section 4): forward operating bases a side establishes with the ESTABLISH_FOB intent (CLW_ActionsFob.c).
// A FOB goes PLANNED -> MOVING -> BUILDING -> ACTIVE (or FAILED before it stands, LOST afterwards):
//   PLANNED   the cost is paid (reserved); a builder is found or bought (CLW_ForceSpawner, tagged purchase). Without the `logistics`
//             capability there is no builder: the FOB is abstract (short delay, then the build step).
//   MOVING    the builder (a transport vehicle unit, else an infantry group) drives / walks to the site; an escort goes along (the
//             embarked group of the vehicle, else a free infantry group, else a bought one)
//   BUILDING  CLW_BuildExecutor: `instant` = done on arrival, `timed` = progress over fobBuildSec while the builder is within
//             ARRIVE_M of the site (paused otherwise)
//   ACTIVE    spawn point (respawn at ticket cost like the base), services tier 1 (command post, supply, arsenal), tier 2 on UPGRADE_FOB
// Loss: an enemy in the zone with no friendly for HOLD_LOST_SEC, or the command post destroyed -> LOST: spawn point and services
// removed, tickets lost, both sides told.
// Builder and escort are RESERVED while the FOB is in progress (IsReserved): the rule commander and LLM orders skip them.
// M4 WP5 (builders): a WALKING builder is ordered with a forced move (ignores combat), takes over from its previous order and waypoints,
// and may switch to a closer pool site when it starts far away. A stalled builder escalates: build from where it stands (close by) ->
// relocate to valid ground near it -> nearer pool site -> swap the builder -> interim road point (route re-plan) -> fail (refund).
// Server only. Log prefix: "CLW: fob".
//
// API for other phases (logistics L):
//   CLW_FobManager.GetFobs(side, out, activeOnly)   CLW_FobManager.Find(id)   CLW_Fob.GetSupplyLink()  (per-FOB stock link)
//   CLW_FobManager.GetOnBuilt() / GetOnLost()       ScriptInvoker, args (string fobId, string side)
//   CLW_FobManager.IsReserved(unitId)               a unit tied up by a FOB (trucks reserved by convoys should ask this too)
//   Service base id of a FOB = its id ("FOB1"): CLW_Services.FindAt(side, fob.m_sId, kind), CLW_SupplyLink.Find(side, fob.m_sId)

enum CLW_EFobState
{
	PLANNED,
	MOVING,
	BUILDING,
	ACTIVE,
	LOST,
	FAILED
}

class CLW_Fob
{
	string m_sId;				// FOB1, FOB2, ... (also the base id of its services and supply link)
	string m_sSide;				// faction key
	string m_sSiteId;
	string m_sName;
	vector m_vPos;
	CLW_EFobState m_eState;
	string m_sSource;			// rules | llm | player
	string m_sReason;
	int m_iCost;				// paid at the start, refunded when the FOB fails before it stands
	float m_fBuild;				// 0..100
	int m_iLoggedStep;			// last build percentage step that was logged
	int m_iTier;				// 0 = none yet, 1 = command post + arsenal, 2 = + depot
	float m_fCreatedAt;
	float m_fStateAt;
	float m_fEndedAt;
	bool m_bAbstract;			// no builder (logistics off)
	string m_sBuilderId;
	string m_sEscortId;
	string m_sTagBuilder;		// tagged purchase in flight
	string m_sTagEscort;
	int m_iBuilderTries;
	int m_iOrderTries;
	float m_fLastMoveLogAt;
	float m_fBestDist;			// MOVING: smallest builder distance to the site that counted as progress (0 = not set yet)
	float m_fBestAt;			// when m_fBestDist last improved by FOB_STALL_IMPROVE_M
	int m_iStalls;
	float m_fLastPauseLogAt;
	float m_fEnemyHoldSec;
	bool m_bUnderAttack;
	bool m_bHadPost;			// the command post existed once (its loss then means the FOB is lost)
	SCR_SpawnPoint m_SpawnPoint;
	// ---- M4 WP5: robust builders ----
	ref array<string> m_aBanned = {};	// units that stalled as builder of this FOB (not picked again)
	int m_iSwaps;				// builder swaps used (stall escalation)
	float m_fPresentM;			// build presence radius around the site, 0 = CLW_FobManager.ARRIVE_M (a stalled builder close by builds from there)
	bool m_bLeg;				// MOVING: the builder drives / walks to an interim road point first (route re-plan)
	vector m_vLeg;
	float m_fLegAt;
	float m_fLastDriftAt;
	float m_fLastStallAt;
	float m_fAwayAt;			// BUILDING: since when the builder is off the site (0 = on site)
	int m_iAwayReturns;
	float m_fBuilderDist;		// last builder distance to the site (for the logs)

	bool IsLive()
	{
		return m_eState != CLW_EFobState.LOST && m_eState != CLW_EFobState.FAILED;
	}

	bool IsInProgress()
	{
		return m_eState == CLW_EFobState.PLANNED || m_eState == CLW_EFobState.MOVING || m_eState == CLW_EFobState.BUILDING;
	}

	bool IsActive()
	{
		return m_eState == CLW_EFobState.ACTIVE;
	}

	string StateName()
	{
		switch (m_eState)
		{
			case CLW_EFobState.PLANNED: return "planned";
			case CLW_EFobState.MOVING: return "moving";
			case CLW_EFobState.BUILDING: return "building";
			case CLW_EFobState.ACTIVE: return "active";
			case CLW_EFobState.LOST: return "lost";
			case CLW_EFobState.FAILED: return "failed";
		}

		return "?";
	}

	// The per-FOB supply stock link (phase L: per-base stock). Null until the services are linked.
	CLW_SupplyLink GetSupplyLink()
	{
		return CLW_SupplyLink.Find(m_sSide, m_sId);
	}
}

// Characters in the FOB zone, per faction (QueryEntitiesBySphere callback holder).
class CLW_FobPresence
{
	string m_sSide;
	string m_sEnemy;
	int m_iFriendly;
	int m_iEnemy;

	bool OnEntity(IEntity entity)
	{
		SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(entity);
		if (!character)
			return true;

		CharacterControllerComponent controller = character.GetCharacterController();
		if (!controller || controller.GetLifeState() == ECharacterLifeState.DEAD)
			return true;

		string key = character.GetFactionKey();
		if (key == m_sSide)
			m_iFriendly++;
		else if (key == m_sEnemy)
			m_iEnemy++;

		return true;
	}
}

// Route re-plan helper (builders and convoy trucks): an interim point ON THE ROAD NETWORK toward a target, used when the direct order stalled.
class CLW_RouteLeg
{
	static const float CYL_RADIUS_M = 2.5;
	static const float CYL_HEIGHT_M = 3;

	// A free road point 'minDist'..'maxDist' from 'from' that lies closer to 'toward' (CLW_Placement.FindSpotToward). Roads only: open-ground
	// answers are ignored (the point of the re-plan is to get onto the road network). False when there is none.
	static bool Find(vector from, vector toward, float minDist, float maxDist, out vector leg)
	{
		vector spot;
		float yaw;
		string via;
		if (!CLW_Placement.FindSpotToward(from, toward, minDist, maxDist, CYL_RADIUS_M, CYL_HEIGHT_M, spot, yaw, via))
			return false;

		if (via != "road")
			return false;

		leg = spot;
		return true;
	}
}

class CLW_FobManager
{
	static const int TICK_MS = 3000;
	static const string ORDER_SOURCE = "fob";			// CLW_Orders source of the builder / escort orders
	static const float FOB_BUILDER_CONTACT_M = 300;		// a busy group this close to an enemy contact is fighting: not taken as walking builder
	static const float ARRIVE_M = 60;					// builder this close to the site = arrived / present
	static const float ZONE_M = 80;						// FOB zone for the presence test
	static const float HOLD_LOST_SEC = 45;				// enemy alone in the zone this long = FOB lost
	static const float ABSTRACT_DELAY_SEC = 20;			// logistics off: set-up time before the build step
	static const float PLANNED_TIMEOUT_SEC = 240;		// no builder after this long = failed
	static const float MOVE_TIMEOUT_SEC = 900;
	static const float MOVE_LOG_SEC = 60;
	static const float PAUSE_LOG_SEC = 30;
	static const int MAX_ORDER_TRIES = 4;
	static const int MAX_BUILDER_TRIES = 2;
	static const float SPACING_M = 350;					// authored distance between two live FOBs (scaled with the battle size)
	static const float SPACING_FLOOR_M = 200;
	static const float ENEMY_CONTACT_M = 300;			// a site this close to a known enemy contact is not offered
	static const int LOST_TICKETS = 5;					// tickets a side loses with a FOB
	static const float LOST_SHOW_SEC = 300;				// a lost FOB stays in the situation this long
	static const int DEFAULT_MAX = 2;
	static const int DEFAULT_COST = 300;
	static const int DEFAULT_BUILD_SEC = 90;
	static const int DEFAULT_EARLIEST_MIN = 6;			// FOB timing (owner playtest 2026-10-12): no FOB before this many minutes of war time (0 = old behaviour)
	static const float FRONT_MIN_M = 500;				// rule doctrine: the front objective is at least this far (authored, scaled) from the HQ
	static const int OWNER_CHANGE_GRACE_SEC = 20;		// objective owner changes during the layout / start owner resolution are not "lost objectives"
	// ---- M4 WP5: builder robustness (own constants; CLW_WarConfig.FOB_STALL_* still define the stall detection) ----
	static const int STALL_MAX = 4;						// stalls (per builder) before the FOB fails (CLW_WarConfig.FOB_MAX_STALLS was 3)
	static const int SWAP_AT_STALL = 3;					// from this stall on a stalled builder is swapped for another unit
	static const int MAX_SWAPS = 2;
	static const float ACCEPT_NEAR_M = 110;				// stalled this close: the builder builds from where it stands
	static const float ACCEPT_PRESENT_M = 130;			// build presence radius used then
	static const float RELOC_MAX_M = 600;				// stalled builder this close to the site: look for ground near the builder
	static const float RELOC_SEARCH_MAX_M = 450;
	static const float RELOC_MIN_GAIN_M = 40;			// a relocation must bring the site this much closer to the builder
	static const float RELOC_OBJ_RELAX_M = 120;			// second pass: clearance from objectives (instead of CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M)
	static const float SITE_SWITCH_GAIN_M = 80;			// stall: another pool site that is this much closer to the builder is taken
	static const float WALK_FAR_M = 450;				// a walking builder starts further than this from the site: closer pool site
	static const float WALK_SWITCH_GAIN_M = 150;
	static const float DRIFT_M = 80;					// builder this far FURTHER than its best distance: order lost / pulled away
	static const float DRIFT_REISSUE_SEC = 40;
	static const float LEG_MIN_M = 40;					// interim road point (route re-plan) distances from the builder
	static const float LEG_MAX_M = 350;
	static const float LEG_ARRIVE_M = 50;
	static const float LEG_MAX_SEC = 150;
	static const float AWAY_RETURN_SEC = 90;			// BUILDING: builder off the site this long = back to MOVING
	static const int MAX_AWAY_RETURNS = 3;
	static const float ORDER_STALL_GAP_SEC = 60;		// a builder whose order keeps ending counts as stalled at most this often
	static const float TOTAL_TIMEOUT_SEC = 1800;		// a FOB in progress this long after planning is cancelled

	protected static ref array<ref CLW_Fob> s_aFobs = {};
	protected static ref ScriptInvoker s_OnBuilt;
	protected static ref ScriptInvoker s_OnLost;
	protected static int s_iNext;
	protected static ref map<string, bool> s_mLostPoint = new map<string, bool>();		// side -> one of its objectives was taken by the enemy
	protected static ref map<string, string> s_mGateState = new map<string, string>();	// side -> last logged FOB gate state key (log once per change)
	protected static bool s_bTicking;
	protected static float s_fLastTick;

	// ---- lifecycle ----

	// Game start (server): statics survive a Play restart, so forget the previous session.
	static void Reset()
	{
		GetGame().GetCallqueue().Remove(Tick);
		s_bTicking = false;
		s_aFobs.Clear();
		s_mLostPoint.Clear();
		s_mGateState.Clear();
		s_iNext = 0;
		s_fLastTick = 0;
		CLW_FobSites.Reset();
	}

	// War SETUP -> RUNNING (CLW_GameMode.OnWarStarted, after the battle layout and the base services).
	static void OnWarStart(notnull CLW_WarState war)
	{
		if (!Replication.IsServer())
			return;

		Reset();
		if (!CLW_Capabilities.Has("fob"))
		{
			Print("CLW: fob capability is off: no FOBs this war", LogLevel.NORMAL);
			return;
		}

		CLW_FobSites.Build();
		CLW_ControlPoints gatePoints = CLW_GameMode.GetControlPoints();
		if (gatePoints)
		{
			gatePoints.GetOnOwnerChanged().Remove(OnPointOwner);
			gatePoints.GetOnOwnerChanged().Insert(OnPointOwner);
		}

		GetOnLost().Remove(CLW_RequestTerminals.OnFobLost);	// M3 phase L fix B: a lost FOB loses its request terminals
		GetOnLost().Insert(CLW_RequestTerminals.OnFobLost);
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (spawner)
		{
			spawner.GetOnTagged().Remove(OnSpawnTagged);
			spawner.GetOnTagged().Insert(OnSpawnTagged);
		}

		s_fLastTick = CLW_Orders.Now();
		s_bTicking = true;
		GetGame().GetCallqueue().CallLater(Tick, TICK_MS, true);
		Print(string.Format("CLW: fob ready: max %1 per side, build %2, cost %3, build time %4 s, %5 sites, logistics=%6, earliest %7 s", GetMax(), BuildModeName(), GetCost(), GetBuildSec(), CLW_FobSites.Count(), CLW_Capabilities.Has("logistics"), GetEarliestSec()), LogLevel.NORMAL);
	}

	// ---- timing gate (owner playtest 2026-10-12): FOBs follow the dynamics of the battle, not the war start ----

	// Minimum war time (s) before ESTABLISH_FOB is accepted. 0 = no gate (old behaviour).
	static int GetEarliestSec()
	{
		CLW_WarMode mode = ActiveMode();
		if (!mode)
			return 0;

		return Math.Max(0, mode.GetIntParam("fobEarliestMin", DEFAULT_EARLIEST_MIN)) * 60;
	}

	// One of the side's objectives changed hands (ControlPoints invoker): remembered as "front knowledge" of the old owner.
	static void OnPointOwner(string id, string oldOwner, string newOwner)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning() || war.GetElapsedSec() < OWNER_CHANGE_GRACE_SEC)
			return;

		if (oldOwner.IsEmpty() || oldOwner == newOwner)
			return;

		s_mLostPoint.Set(oldOwner, true);
		Print(string.Format("CLW: fob gate: %1 lost objective %2 (front knowledge)", oldOwner, id), LogLevel.NORMAL);
	}

	// What tells the side where the front is: a known enemy contact (any engagement of an own unit ends up in the contact book),
	// a contested objective, or an objective it lost. "" = nothing yet.
	protected static string FrontKnowledge(string side)
	{
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (commander && commander.GetRules() && commander.GetRules().GetBook())
		{
			array<CLW_Contact> contacts = {};
			commander.GetRules().GetBook().GetContacts(contacts);
			if (!contacts.IsEmpty())
				return string.Format("%1 known enemy contact(s)", contacts.Count());
		}

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (points)
		{
			for (int i = 0; i < points.Count(); i++)
			{
				CLW_ControlPoint point = points.Get(i);
				if (point && point.m_bContested)
					return "objective " + point.m_sId + " is contested";
			}
		}

		if (s_mLostPoint.Contains(side))
			return "an own objective was lost";

		return "";
	}

	// True when the side may establish a FOB now: fobEarliestMin of war time passed AND the side has front knowledge. why = the reason
	// when not (on true: the front knowledge). Logs once per state change per side ("CLW: fob gate ...").
	static bool IsReady(string side, out string why)
	{
		int earliest = GetEarliestSec();
		if (earliest <= 0)
		{
			why = "no timing gate (fobEarliestMin 0)";
			return true;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		int elapsed;
		if (war)
			elapsed = war.GetElapsedSec();

		if (elapsed < earliest)
		{
			why = string.Format("opening phase: %1 of %2 s of war time elapsed", elapsed, earliest);
			LogGate(side, "opening", why);
			return false;
		}

		string front = FrontKnowledge(side);
		if (front.IsEmpty())
		{
			why = "no front knowledge yet (no known enemy contact, no contested or lost objective)";
			LogGate(side, "nofront", why);
			return false;
		}

		why = front;
		LogGate(side, "ready", "FOB allowed: " + front);
		return true;
	}

	protected static void LogGate(string side, string key, string text)
	{
		string last;
		if (s_mGateState.Find(side, last) && last == key)
			return;

		s_mGateState.Set(side, key);
		Print(string.Format("CLW: fob gate %1: %2 (%3)", side, key, text), LogLevel.NORMAL);
	}

	// ---- mode parameters ----

	static int GetMax()
	{
		if (!CLW_Capabilities.Has("fob"))
			return 0;

		CLW_WarMode mode = ActiveMode();
		if (!mode)
			return 0;

		return Math.Clamp(mode.GetIntParam("fobMax", DEFAULT_MAX), 0, 4);
	}

	static int GetCost()
	{
		CLW_WarMode mode = ActiveMode();
		if (!mode)
			return DEFAULT_COST;

		return Math.Max(0, mode.GetIntParam("fobCost", DEFAULT_COST));
	}

	static int GetBuildSec()
	{
		CLW_WarMode mode = ActiveMode();
		if (!mode)
			return DEFAULT_BUILD_SEC;

		return Math.Max(5, mode.GetIntParam("fobBuildSec", DEFAULT_BUILD_SEC));
	}

	static string BuildModeName()
	{
		CLW_WarMode mode = ActiveMode();
		if (!mode)
			return "timed";

		string name = mode.GetChoiceParam("fobBuild");
		if (name == "instant")
			return "instant";

		return "timed";
	}

	static bool IsInstant()
	{
		return BuildModeName() == "instant";
	}

	// Tier 2 (vehicle depot) costs half of the FOB.
	static int GetUpgradeCost()
	{
		return Math.Max(25, GetCost() / 2);
	}

	protected static CLW_WarMode ActiveMode()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war)
			return null;

		return war.GetMode();
	}

	// ---- queries / API ----

	static ScriptInvoker GetOnBuilt()
	{
		if (!s_OnBuilt)
			s_OnBuilt = new ScriptInvoker();

		return s_OnBuilt;
	}

	static ScriptInvoker GetOnLost()
	{
		if (!s_OnLost)
			s_OnLost = new ScriptInvoker();

		return s_OnLost;
	}

	// Appends the side's FOBs (live ones only unless includeEnded). activeOnly: only state ACTIVE. Returns the count appended.
	static int GetFobs(string side, notnull array<CLW_Fob> outFobs, bool activeOnly = false, bool includeEnded = false)
	{
		int count;
		foreach (CLW_Fob fob : s_aFobs)
		{
			if (fob.m_sSide != side)
				continue;

			if (activeOnly && !fob.IsActive())
				continue;

			if (!includeEnded && !fob.IsLive())
				continue;

			outFobs.Insert(fob);
			count++;
		}

		return count;
	}

	static CLW_Fob Find(string id)
	{
		foreach (CLW_Fob fob : s_aFobs)
		{
			if (fob.m_sId == id)
				return fob;
		}

		return null;
	}

	static int CountLive(string side)
	{
		int count;
		foreach (CLW_Fob fob : s_aFobs)
		{
			if (fob.m_sSide == side && fob.IsLive())
				count++;
		}

		return count;
	}

	static int CountInProgress(string side)
	{
		int count;
		foreach (CLW_Fob fob : s_aFobs)
		{
			if (fob.m_sSide == side && fob.IsInProgress())
				count++;
		}

		return count;
	}

	// A unit tied up as builder / escort of a FOB that is still being established.
	static bool IsReserved(string unitId)
	{
		if (unitId.IsEmpty())
			return false;

		if (CLW_Convoys.IsReserved(unitId))
			return true;

		foreach (CLW_Fob fob : s_aFobs)
		{
			if (!fob.IsInProgress())
				continue;

			if (fob.m_sBuilderId == unitId || fob.m_sEscortId == unitId)
				return true;
		}

		return false;
	}

	// Short text of what a reserved unit does, "" when it is not reserved.
	static string ReservedFor(string unitId)
	{
		if (unitId.IsEmpty())
			return "";

		string convoyText = CLW_Convoys.ReservedFor(unitId);
		if (!convoyText.IsEmpty())
			return convoyText;

		foreach (CLW_Fob fob : s_aFobs)
		{
			if (!fob.IsInProgress())
				continue;

			if (fob.m_sBuilderId == unitId)
				return "builder of " + fob.m_sId;

			if (fob.m_sEscortId == unitId)
				return "escort of " + fob.m_sId;
		}

		return "";
	}

	// ---- establishing ----

	// Dynamic validity of a site for a side right now (no side effects): own half of the map, away from live FOBs, no enemy contact near.
	// ignoreFobId: a FOB (its own, being re-sited) that does not count for the spacing rule.
	static bool CheckSite(string side, CLW_FobSite site, out string why, string ignoreFobId = "")
	{
		if (!site)
		{
			why = "unknown site";
			return false;
		}

		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!scenario || !bases || !war)
		{
			why = "war systems missing";
			return false;
		}

		int sideIndex = scenario.GetSideIndex(side);
		if (site.m_iSide >= 0 && site.m_iSide != sideIndex)
		{
			why = "site belongs to the other side";
			return false;
		}

		vector own = bases.GetBasePos(side);
		vector enemy = bases.GetBasePos(war.GetEnemyKey(side));
		if (own != vector.Zero && enemy != vector.Zero && vector.DistanceXZ(site.m_vPos, enemy) < vector.DistanceXZ(site.m_vPos, own))
		{
			why = "site is on the enemy half of the map";
			return false;
		}

		float spacing = Math.Max(SPACING_FLOOR_M, bases.ScaleDistance(SPACING_M));
		foreach (CLW_Fob fob : s_aFobs)
		{
			if (!fob.IsLive() || fob.m_sId == ignoreFobId)
				continue;

			if (fob.m_sSiteId == site.m_sId || vector.DistanceXZ(fob.m_vPos, site.m_vPos) < spacing)
			{
				why = "too close to " + fob.m_sId;
				return false;
			}
		}

		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (commander && commander.GetRules() && commander.GetRules().GetBook())
		{
			array<CLW_Contact> contacts = {};
			commander.GetRules().GetBook().GetContacts(contacts);
			foreach (CLW_Contact contact : contacts)
			{
				if (vector.DistanceXZ(contact.m_vPos, site.m_vPos) < ENEMY_CONTACT_M)
				{
					why = "enemy contact near the site";
					return false;
				}
			}
		}

		return true;
	}

	// All checks of an ESTABLISH_FOB (no side effects): capability, cap, one at a time, supplies, site.
	static bool CanEstablish(string side, CLW_FobSite site, out string why)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
		{
			why = "war not running";
			return false;
		}

		if (!CLW_Capabilities.Has("fob") || GetMax() <= 0)
		{
			why = "FOBs are off in this war mode";
			return false;
		}

		if (CountLive(side) >= GetMax())
		{
			why = string.Format("FOB limit reached (%1 of %2)", CountLive(side), GetMax());
			return false;
		}

		if (CountInProgress(side) > 0)
		{
			why = "another FOB is still being established";
			return false;
		}

		string gateWhy;
		if (!IsReady(side, gateWhy))
		{
			why = "too early for a FOB: " + gateWhy;
			return false;
		}

		if (war.GetSupplies(side) < GetCost())
		{
			why = string.Format("not enough supplies (cost %1, have %2)", GetCost(), war.GetSupplies(side));
			return false;
		}

		return CheckSite(side, site, why);
	}

	// Pays the cost and creates the FOB (state PLANNED). The tick finds / buys the builder. False with why when refused.
	static bool Establish(string side, string siteId, string source, string reason, out string why)
	{
		CLW_FobSite site = CLW_FobSites.FindSite(siteId);
		if (!site)
		{
			why = "unknown FOB site " + siteId;
			return false;
		}

		if (!CanEstablish(side, site, why))
			return false;

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war.TrySpend(side, GetCost()))
		{
			why = "could not pay " + GetCost();
			return false;
		}

		s_iNext++;
		CLW_Fob fob = new CLW_Fob();
		fob.m_sId = "FOB" + s_iNext;
		fob.m_sSide = side;
		fob.m_sSiteId = site.m_sId;
		fob.m_sName = site.m_sName;
		fob.m_vPos = site.m_vPos;
		fob.m_eState = CLW_EFobState.PLANNED;
		fob.m_sSource = source;
		fob.m_sReason = reason;
		fob.m_iCost = GetCost();
		fob.m_fCreatedAt = CLW_Orders.Now();
		fob.m_fStateAt = fob.m_fCreatedAt;
		s_aFobs.Insert(fob);
		Print(string.Format("CLW: fob %1 planned for %2 at site %3 '%4' %5 grid %6: cost %7 paid", fob.m_sId, side, site.m_sId, site.m_sName, site.m_vPos, CLW_Places.Grid(site.m_vPos), fob.m_iCost), LogLevel.NORMAL);
		Print(string.Format("CLW: fob %1 build=%2 (%3 s), source=%4, reason: %5", fob.m_sId, BuildModeName(), GetBuildSec(), source, reason), LogLevel.NORMAL);
		war.PushEvent(side, fob.m_sId + " planned at " + site.m_sName);
		return true;
	}

	// Tier 2 services (vehicle depot) for an active FOB at tier 1.
	static bool Upgrade(string side, string fobId, string source, out string why)
	{
		CLW_Fob fob = Find(fobId);
		if (!fob || fob.m_sSide != side)
		{
			why = "unknown FOB " + fobId;
			return false;
		}

		if (!fob.IsActive())
		{
			why = fob.m_sId + " is not active (" + fob.StateName() + ")";
			return false;
		}

		if (fob.m_iTier >= 2)
		{
			why = fob.m_sId + " is already at tier 2";
			return false;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !CLW_SupplyStore.TrySpend(side, fobId, GetUpgradeCost()))	// M3 phase L: drawn from the FOB's own stock (logistics on)
		{
			why = string.Format("not enough supplies for the upgrade (cost %1, %2 has %3)", GetUpgradeCost(), fobId, CLW_SupplyStore.Get(side, fobId));
			return false;
		}

		CLW_BuildExecutor.Upgrade(fob);
		return true;
	}

	// Validate-only variant of Upgrade for the action validator.
	static bool CanUpgrade(string side, string fobId, out string why)
	{
		CLW_Fob fob = Find(fobId);
		if (!fob || fob.m_sSide != side)
		{
			why = "unknown FOB " + fobId;
			return false;
		}

		if (!fob.IsActive())
		{
			why = fob.m_sId + " is not active (" + fob.StateName() + ")";
			return false;
		}

		if (fob.m_iTier >= 2)
		{
			why = fob.m_sId + " is already at tier 2";
			return false;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || CLW_SupplyStore.Get(side, fobId) < GetUpgradeCost())
		{
			why = string.Format("not enough supplies for the upgrade (cost %1, %2 has %3)", GetUpgradeCost(), fobId, CLW_SupplyStore.Get(side, fobId));
			return false;
		}

		return true;
	}

	// ---- tick ----

	protected static void Tick()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;

		float now = CLW_Orders.Now();
		float dt = now - s_fLastTick;
		if (dt < 0 || dt > 30)
			dt = TICK_MS / 1000.0;
		s_fLastTick = now;

		foreach (CLW_Fob fob : s_aFobs)
		{
			if (fob.IsInProgress() && now - fob.m_fCreatedAt > TOTAL_TIMEOUT_SEC)
			{
				Fail(fob, "not finished " + Math.Round(TOTAL_TIMEOUT_SEC) + " s after planning");
				continue;
			}

			switch (fob.m_eState)
			{
				case CLW_EFobState.PLANNED: StepPlanned(fob, now); break;
				case CLW_EFobState.MOVING: StepMoving(fob, now); break;
				case CLW_EFobState.BUILDING: StepBuilding(fob, now, dt); break;
				case CLW_EFobState.ACTIVE: StepActive(fob, now, dt); break;
			}
		}
	}

	static void SetState(CLW_Fob fob, CLW_EFobState state, string note)
	{
		string before = fob.StateName();
		fob.m_eState = state;
		fob.m_fStateAt = CLW_Orders.Now();
		Print(string.Format("CLW: fob %1 %2 -> %3: %4", fob.m_sId, before, fob.StateName(), note), LogLevel.NORMAL);
	}

	// ---- planned: builder ----

	protected static void StepPlanned(CLW_Fob fob, float now)
	{
		if (now - fob.m_fStateAt > PLANNED_TIMEOUT_SEC)
		{
			Fail(fob, "no builder after " + Math.Round(PLANNED_TIMEOUT_SEC) + " s");
			return;
		}

		if (!CLW_Capabilities.Has("logistics"))
		{
			fob.m_bAbstract = true;
			SetState(fob, CLW_EFobState.MOVING, "logistics is off: abstract set-up, no truck");
			return;
		}

		if (!fob.m_sBuilderId.IsEmpty())
		{
			BeginMove(fob);
			return;
		}

		if (!fob.m_sTagBuilder.IsEmpty())
			return;	// purchase in flight (OnSpawnTagged)

		string found = FindFreeTransport(fob.m_sSide, fob.m_vPos, fob);
		if (!found.IsEmpty())
		{
			fob.m_sBuilderId = found;
			SetTeamRole(found, true);
			Print(string.Format("CLW: fob %1 truck assigned: %2 (free unit)", fob.m_sId, found), LogLevel.NORMAL);
			BeginMove(fob);
			return;
		}

		BuyBuilder(fob);
	}

	protected static void BuyBuilder(CLW_Fob fob)
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!spawner || !catalog)
		{
			Fail(fob, "no spawner / catalog");
			return;
		}

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(fob.m_sSide, entries);
		CLW_ForceEntry pick;
		string pickNote;
		foreach (CLW_ForceEntry transport : entries)
		{
			// m_bCrewless: probed / purchase showed that the vehicle has no default crew (GetEntries leaves such entries out already)
			if (transport.m_sKind == CLW_ForceKind.MOTORIZED && transport.HasVehicle() && !transport.m_bCrewless && (!pick || transport.m_iCost < pick.m_iCost))
			{
				pick = transport;
				pickNote = "motorized transport";
			}
		}

		if (!pick)
		{
			foreach (CLW_ForceEntry carrier : entries)
			{
				if (carrier.HasVehicle() && carrier.m_iCargoSeats != 0 && !carrier.m_bCrewless && carrier.m_sKind != CLW_ForceKind.LOGISTICS && (!pick || carrier.m_iCost < pick.m_iCost))
				{
					pick = carrier;
					pickNote = "vehicle with cargo seats";
				}
			}
		}

		if (!pick)
		{
			foreach (CLW_ForceEntry walker : entries)
			{
				if (!walker.HasVehicle() && (!pick || walker.m_iCost < pick.m_iCost))
				{
					pick = walker;
					pickNote = "no vehicle in the catalog, infantry walks";
				}
			}
		}

		if (!pick)
		{
			Fail(fob, "nothing in the catalog can carry the build");
			return;
		}

		fob.m_sTagBuilder = fob.m_sId + "_builder";
		string error;
		bool canBuy = CLW_EconomyGuard.CanAiSpend(fob.m_sSide, pick.m_iCost, error);	// HQ reserve floor (error = why)
		if (canBuy)
			canBuy = spawner.BuyTagged(fob.m_sSide, pick.m_sType, "build " + fob.m_sId, fob.m_sTagBuilder, error);

		if (!canBuy)
		{
			fob.m_sTagBuilder = "";
			if (fob.m_eState != CLW_EFobState.PLANNED)
				return;

			// no purchase possible (supplies, caps, engine AI limit): a free infantry group walks the build there
			// (DS run 2026-10-12: at 120/128 active AI the FOB failed although infantry stood idle)
			string walker = FindFreeInfantry(fob.m_sSide, fob.m_vPos, true, fob);	// an HQ build order outranks the group's current order
			if (walker.IsEmpty())
			{
				Fail(fob, "cannot buy a builder (" + error + ") and no free infantry");
				return;
			}

			fob.m_sBuilderId = walker;
			SetTeamRole(walker, true);
			Print(string.Format("CLW: fob %1 builder: %2 walks (purchase refused: %3)", fob.m_sId, walker, error), LogLevel.NORMAL);
			BeginMove(fob);
			return;
		}

		Print(string.Format("CLW: fob %1 truck bought: %2 (%3, cost %4)", fob.m_sId, pick.m_sType, pickNote, pick.m_iCost), LogLevel.NORMAL);
	}

	// A tagged purchase of the spawner finished (unitId set) or failed (unitId empty).
	static void OnSpawnTagged(string tag, string unitId)
	{
		foreach (CLW_Fob fob : s_aFobs)
		{
			if (fob.m_sTagBuilder == tag)
			{
				if (unitId.IsEmpty())
				{
					if (fob.m_eState == CLW_EFobState.PLANNED)
						Fail(fob, "the builder purchase failed");
				}
				else if (fob.IsInProgress())
				{
					fob.m_sBuilderId = unitId;
					SetTeamRole(unitId, true);
					Print(string.Format("CLW: fob %1 truck ready: %2", fob.m_sId, unitId), LogLevel.NORMAL);
				}

				return;
			}

			if (fob.m_sTagEscort == tag)
			{
				if (!unitId.IsEmpty() && fob.IsInProgress())
				{
					fob.m_sEscortId = unitId;
					string why;
					IssueMove(fob.m_sSide, unitId, fob.m_vPos, "escort " + fob.m_sId, why);
					Print(string.Format("CLW: fob %1 escort ready: %2 (%3)", fob.m_sId, unitId, why), LogLevel.NORMAL);
				}

				return;
			}
		}
	}

	protected static void BeginMove(CLW_Fob fob)
	{
		fob.m_iOrderTries = 0;
		fob.m_fBestDist = 0;
		fob.m_bLeg = false;
		fob.m_fPresentM = 0;
		fob.m_fAwayAt = 0;
		ConsiderCloserSite(fob);
		CLW_Orders orders = OrdersOf(fob.m_sSide);
		bool walker = orders && !orders.FindVehicleUnit(fob.m_sBuilderId);
		SetState(fob, CLW_EFobState.MOVING, string.Format("builder %1 (%2) heads for %3", fob.m_sBuilderId, BuilderKindText(walker), fob.m_vPos));
		string why;
		if (!IssueBuildMove(fob, fob.m_vPos, "build " + fob.m_sId, why))
			Print(string.Format("CLW: fob %1 move order refused: %2 (retried by the tick)", fob.m_sId, why), LogLevel.WARNING);

		EnsureEscort(fob);
	}

	protected static string BuilderKindText(bool walker)
	{
		if (walker)
			return "on foot, forced move";

		return "vehicle";
	}

	// A WALKING builder that starts far from the site takes a closer pool site (a walk of 700 m through contested ground failed in the DS run
	// 2026-10-12). Vehicles keep the chosen site.
	protected static void ConsiderCloserSite(CLW_Fob fob)
	{
		CLW_Orders orders = OrdersOf(fob.m_sSide);
		if (!orders || orders.FindVehicleUnit(fob.m_sBuilderId))
			return;

		vector pos;
		if (!orders.UnitPos(fob.m_sBuilderId, pos))
			return;

		float dist = vector.DistanceXZ(pos, fob.m_vPos);
		if (dist <= WALK_FAR_M)
			return;

		if (!TrySwitchSite(fob, pos, dist, WALK_SWITCH_GAIN_M, "walking builder starts " + Math.Round(dist) + " m from the site"))
			Print(string.Format("CLW: fob %1 walking builder %2 starts %3 m from the site and no closer pool site is valid: walks the whole way", fob.m_sId, fob.m_sBuilderId, Math.Round(dist)), LogLevel.NORMAL);
	}

	// ---- unit helpers ----

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

	// The BUILDER's move. The unit takes over from whatever it did before: its old order is forgotten (so CLW_Orders.Issue neither refuses it as
	// "stable" nor swallows a re-issue of the same target as "already executing") and CLW_Orders clears the group's old waypoints
	// (ExecuteInfantry -> CLW_Waypoints.Clear: previous objective / attack / defend waypoints). A walking builder gets FALL_BACK = forced
	// move (ignores combat) + hold, a vehicle unit a plain MOVE.
	protected static bool IssueBuildMove(CLW_Fob fob, vector pos, string reason, out string why)
	{
		CLW_Orders orders = OrdersOf(fob.m_sSide);
		if (!orders)
		{
			why = "no orders object";
			return false;
		}

		orders.ForgetOrder(fob.m_sBuilderId);
		CLW_ECommand command = CLW_ECommand.MOVE;
		if (!orders.FindVehicleUnit(fob.m_sBuilderId))
			command = CLW_ECommand.FALL_BACK;

		return orders.Issue(fob.m_sBuilderId, command, "", pos, reason, ORDER_SOURCE, why);
	}

	// True when a known enemy contact is within radius of pos (the group there is in a fight: AI in contact ignores plain move waypoints).
	static bool Engaged(string side, vector pos, float radius)
	{
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (!commander || !commander.GetRules() || !commander.GetRules().GetBook())
			return false;

		array<CLW_Contact> contacts = {};
		commander.GetRules().GetBook().GetContacts(contacts);
		foreach (CLW_Contact contact : contacts)
		{
			if (contact && vector.DistanceXZ(contact.m_vPos, pos) < radius)
				return true;
		}

		return false;
	}

	// pos is on the side's half of the map (closer to its own base than to the enemy's). True when the bases are unknown.
	static bool OnOwnHalf(string side, vector pos)
	{
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!bases || !war)
			return true;

		vector own = bases.GetBasePos(side);
		vector enemy = bases.GetBasePos(war.GetEnemyKey(side));
		if (own == vector.Zero || enemy == vector.Zero)
			return true;

		return vector.DistanceXZ(pos, own) <= vector.DistanceXZ(pos, enemy);
	}

	// No order of its own (or only the rule commander's standing reserve at the base).
	protected static bool IsFreeUnit(CLW_Orders orders, string id)
	{
		CLW_UnitOrder order = orders.GetOrder(id);
		if (!order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE)
			return true;

		return order.m_eCommand == CLW_ECommand.DEFEND && order.m_sTarget.IsEmpty();
	}

	// The free vehicle unit of kind motorized nearest to the site, "" when none.
	protected static string FindFreeTransport(string side, vector site, CLW_Fob fob = null)
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
			if (!unit || unit.GetKind() != CLW_ForceKind.MOTORIZED)
				continue;

			if (fob && fob.m_aBanned.Contains(id))
				continue;

			SCR_AIGroup group = registry.Find(id);
			if (!group || CLW_RuleCommander.HasHumanLeader(group) || IsReserved(id))
				continue;

			if (orders.UnitAlive(id) <= 0 || orders.IsWeak(id) || !IsFreeUnit(orders, id))
				continue;

			vector pos;
			if (!orders.UnitPos(id, pos))
				continue;

			float dist = vector.DistanceXZ(pos, site);
			if (dist < bestDist)
			{
				bestDist = dist;
				best = id;
			}
		}

		return best;
	}

	protected static string FindFreeInfantry(string side, vector site, bool busyOk = false, CLW_Fob fob = null)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		CLW_Orders orders = OrdersOf(side);
		if (!registry || !orders)
			return "";

		array<string> ids = {};
		registry.GetCommandableIds(side, ids);

		// busy groups only when they are not in a fight: AI in contact ignores move waypoints
		// (DS run 2026-10-12: two walking builders held their objective for 6 min and the FOBs failed)
		array<CLW_Contact> contacts = {};
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (busyOk && commander && commander.GetRules() && commander.GetRules().GetBook())
			commander.GetRules().GetBook().GetContacts(contacts);

		string best;
		float bestDist = 1000000000;
		foreach (string id : ids)
		{
			if (orders.FindVehicleUnit(id) || registry.GetKind(id) != CLW_ForceKind.INFANTRY)
				continue;

			if (fob && fob.m_aBanned.Contains(id))
				continue;

			if (busyOk && !contacts.IsEmpty())
			{
				vector unitPos;
				if (!orders.UnitPos(id, unitPos))
					continue;

				bool engaged = false;
				foreach (CLW_Contact contact : contacts)
				{
					if (contact && vector.DistanceXZ(contact.m_vPos, unitPos) < FOB_BUILDER_CONTACT_M)
					{
						engaged = true;
						break;
					}
				}

				if (engaged)
					continue;
			}

			SCR_AIGroup group = registry.Find(id);
			if (!group || CLW_RuleCommander.HasHumanLeader(group) || IsReserved(id))
				continue;

			if (orders.UnitAlive(id) <= 0 || orders.IsWeak(id) || (!busyOk && !IsFreeUnit(orders, id)))
				continue;

			vector pos;
			if (!orders.UnitPos(id, pos))
				continue;

			float dist = vector.DistanceXZ(pos, site);
			if (dist < bestDist)
			{
				bestDist = dist;
				best = id;
			}
		}

		return best;
	}

	// The vehicle's embarked group goes along by itself; otherwise a free infantry group, otherwise a bought one (not waited for).
	protected static void EnsureEscort(CLW_Fob fob)
	{
		CLW_Orders orders = OrdersOf(fob.m_sSide);
		if (!orders)
			return;

		CLW_VehicleUnit unit = orders.FindVehicleUnit(fob.m_sBuilderId);
		if (unit && unit.GetCargoGroup() && unit.GetCargoAlive() > 0)
		{
			Print(string.Format("CLW: fob %1 escort: %2 embarked soldiers ride with the truck", fob.m_sId, unit.GetCargoAlive()), LogLevel.NORMAL);
			return;
		}

		string escort = FindFreeInfantry(fob.m_sSide, fob.m_vPos, false, fob);
		if (!escort.IsEmpty())
		{
			fob.m_sEscortId = escort;
			string why;
			bool ordered = IssueMove(fob.m_sSide, escort, fob.m_vPos, "escort " + fob.m_sId, why);
			Print(string.Format("CLW: fob %1 escort: free infantry %2 (ordered=%3 %4)", fob.m_sId, escort, ordered, why), LogLevel.NORMAL);
			return;
		}

		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!spawner || !catalog)
			return;

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(fob.m_sSide, entries);
		CLW_ForceEntry cheapest;
		foreach (CLW_ForceEntry entry : entries)
		{
			if (entry.m_sKind == CLW_ForceKind.INFANTRY && !entry.HasVehicle() && (!cheapest || entry.m_iCost < cheapest.m_iCost))
				cheapest = entry;
		}

		if (!cheapest)
		{
			Print(string.Format("CLW: fob %1 escort: none available, the builder goes alone", fob.m_sId), LogLevel.NORMAL);
			return;
		}

		fob.m_sTagEscort = fob.m_sId + "_escort";
		string error;
		bool escortOk = CLW_EconomyGuard.CanAiSpend(fob.m_sSide, cheapest.m_iCost, error);	// HQ reserve floor (error = why)
		if (escortOk)
			escortOk = spawner.BuyTagged(fob.m_sSide, cheapest.m_sType, "escort " + fob.m_sId, fob.m_sTagEscort, error);

		if (escortOk)
			Print(string.Format("CLW: fob %1 escort: bought %2", fob.m_sId, cheapest.m_sType), LogLevel.NORMAL);
		else
			Print(string.Format("CLW: fob %1 escort: none (%2), the builder goes alone", fob.m_sId, error), LogLevel.NORMAL);
	}

	// ---- moving / building ----

	// WP1 naming: the builder squad shows as "FOB Team N" while it is reserved; "" gives back its kind name. Escorts keep their kind name.
	protected static void SetTeamRole(string unitId, bool on)
	{
		if (unitId.IsEmpty())
			return;

		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!registry)
			return;

		if (on)
			registry.SetRole(unitId, CLW_SquadRegistry.ROLE_FOB_TEAM);
		else
			registry.SetRole(unitId, "");
	}

	protected static bool BuilderAlive(CLW_Fob fob, CLW_Orders orders)
	{
		if (fob.m_sBuilderId.IsEmpty())
			return false;

		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!registry || !registry.Find(fob.m_sBuilderId))
			return false;

		return orders.UnitAlive(fob.m_sBuilderId) > 0;
	}

	protected static void BuilderLost(CLW_Fob fob)
	{
		fob.m_iBuilderTries++;
		Print(string.Format("CLW: fob %1 builder %2 lost (try %3 of %4)", fob.m_sId, fob.m_sBuilderId, fob.m_iBuilderTries, MAX_BUILDER_TRIES), LogLevel.WARNING);
		if (fob.m_iBuilderTries >= MAX_BUILDER_TRIES)
		{
			Fail(fob, "the builder was lost");
			return;
		}

		SetTeamRole(fob.m_sBuilderId, false);
		fob.m_sBuilderId = "";
		fob.m_sTagBuilder = "";
		SetState(fob, CLW_EFobState.PLANNED, "looking for a new builder (progress " + Math.Round(fob.m_fBuild) + " % kept)");
	}

	protected static void StepMoving(CLW_Fob fob, float now)
	{
		if (fob.m_bAbstract)
		{
			if (now - fob.m_fStateAt >= ABSTRACT_DELAY_SEC)
			{
				SetState(fob, CLW_EFobState.BUILDING, "abstract set-up done at " + fob.m_vPos);
				CLW_BuildExecutor.Step(fob, 0, true);
			}

			return;
		}

		CLW_Orders orders = OrdersOf(fob.m_sSide);
		if (!orders)
			return;

		if (!BuilderAlive(fob, orders))
		{
			BuilderLost(fob);
			return;
		}

		vector pos;
		if (!orders.UnitPos(fob.m_sBuilderId, pos))
		{
			BuilderLost(fob);
			return;
		}

		float dist = vector.DistanceXZ(pos, fob.m_vPos);
		fob.m_fBuilderDist = dist;
		if (dist <= ARRIVE_M)
		{
			Print(string.Format("CLW: fob %1 builder %2 arrived (%3 m from the site)", fob.m_sId, fob.m_sBuilderId, Math.Round(dist)), LogLevel.NORMAL);
			SetState(fob, CLW_EFobState.BUILDING, "builder on site");
			CLW_BuildExecutor.Step(fob, 0, true);
			return;
		}

		if (now - fob.m_fStateAt > MOVE_TIMEOUT_SEC)
		{
			Fail(fob, "the builder did not arrive in time");
			return;
		}

		string why;
		vector target = fob.m_vPos;
		string targetText = "site";

		// route re-plan: an interim road point first; reaching it (or its order ending, or too long) sends the builder on to the site
		if (fob.m_bLeg)
		{
			float legDist = vector.DistanceXZ(pos, fob.m_vLeg);
			CLW_UnitOrder legOrder = orders.GetOrder(fob.m_sBuilderId);
			bool legIdle = !legOrder || legOrder.m_sStatus != CLW_Orders.STATUS_ACTIVE;
			if (legDist <= LEG_ARRIVE_M || legIdle || now - fob.m_fLegAt > LEG_MAX_SEC)
			{
				fob.m_bLeg = false;
				fob.m_fBestDist = 0;
				bool resumed = IssueBuildMove(fob, fob.m_vPos, "build " + fob.m_sId, why);
				Print(string.Format("CLW: fob %1 road point done (%2 m from it, orderEnded=%3, %4 s): builder %5 heads for the site, %6 m (ordered=%7 %8)", fob.m_sId, Math.Round(legDist), legIdle, Math.Round(now - fob.m_fLegAt), fob.m_sBuilderId, Math.Round(dist), resumed, why), LogLevel.NORMAL);
				return;
			}

			target = fob.m_vLeg;
			targetText = "road point";
		}

		float tdist = vector.DistanceXZ(pos, target);

		// stall rule: no progress (FOB_STALL_IMPROVE_M closer) for FOB_STALL_SEC; drift rule: clearly further away than ever (order lost / pulled away)
		if (fob.m_fBestDist <= 0 || tdist < fob.m_fBestDist - CLW_WarConfig.FOB_STALL_IMPROVE_M)
		{
			fob.m_fBestDist = Math.Max(tdist, 1);
			fob.m_fBestAt = now;
			fob.m_iOrderTries = 0;
		}
		else if (now - fob.m_fBestAt >= CLW_WarConfig.FOB_STALL_SEC)
		{
			HandleStall(fob, now, pos, dist, "no progress for " + Math.Round(now - fob.m_fBestAt) + " s");
			return;
		}
		else if (tdist > fob.m_fBestDist + DRIFT_M && now - fob.m_fLastDriftAt >= DRIFT_REISSUE_SEC)
		{
			fob.m_fLastDriftAt = now;
			bool again = IssueBuildMove(fob, target, "build " + fob.m_sId, why);
			Print(string.Format("CLW: fob %1 builder %2 drifts away: %3 m from the %4, best was %5 m, engaged=%6, order re-taken (ordered=%7 %8)", fob.m_sId, fob.m_sBuilderId, Math.Round(tdist), targetText, Math.Round(fob.m_fBestDist), Engaged(fob.m_sSide, pos, FOB_BUILDER_CONTACT_M), again, why), LogLevel.NORMAL);
		}

		CLW_UnitOrder order = orders.GetOrder(fob.m_sBuilderId);
		if (!order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE)
		{
			if (fob.m_iOrderTries >= MAX_ORDER_TRIES)
			{
				// the order keeps ending without arriving: a stall, but not faster than one per ORDER_STALL_GAP_SEC
				if (now - fob.m_fLastStallAt >= ORDER_STALL_GAP_SEC)
					HandleStall(fob, now, pos, dist, "the move order ended " + MAX_ORDER_TRIES + " times short of the target");

				return;
			}

			fob.m_iOrderTries++;
			bool ordered = IssueBuildMove(fob, target, "build " + fob.m_sId, why);
			Print(string.Format("CLW: fob %1 move re-issued (%2 of %3, %4 m short of the %5, ordered=%6 %7)", fob.m_sId, fob.m_iOrderTries, MAX_ORDER_TRIES, Math.Round(tdist), targetText, ordered, why), LogLevel.NORMAL);
		}

		if (now - fob.m_fLastMoveLogAt >= MOVE_LOG_SEC)
		{
			fob.m_fLastMoveLogAt = now;
			string orderText = "none";
			if (order)
				orderText = CLW_Command.ToName(order.m_eCommand) + "/" + order.m_sStatus + "/" + order.m_sSource;

			Print(string.Format("CLW: fob %1 builder %2 en route, %3 m to go (%4 m to the %5), best %6 m, order=%7, engaged=%8", fob.m_sId, fob.m_sBuilderId, Math.Round(dist), Math.Round(tdist), targetText, Math.Round(fob.m_fBestDist), orderText, Engaged(fob.m_sSide, pos, FOB_BUILDER_CONTACT_M)), LogLevel.NORMAL);
		}
	}

	// The builder made no progress for FOB_STALL_SEC (or its order keeps ending short of the target). Escalation, first applicable step wins:
	//   1 builder within ACCEPT_NEAR_M     -> builds from where it stands (presence radius ACCEPT_PRESENT_M)
	//   2 valid ground near the builder     -> the FOB moves there (wider search than CLW_FobSites.FindNear, then relaxed objective clearance)
	//   3 another pool site nearer          -> the FOB takes that site
	//   4 stalls >= STALL_MAX               -> FAILED (refund)
	//   5 stalls >= SWAP_AT_STALL           -> another unit becomes the builder (this one is banned for this FOB)
	//   6 stalls >= 2, no road point yet    -> interim road point toward the site (route re-plan via the road network)
	//   7 otherwise                         -> the order is taken over again (old order + waypoints cleared) and re-issued
	// True when the state changed (building / failed / planned), the caller returns.
	protected static bool HandleStall(CLW_Fob fob, float now, vector builderPos, float dist, string cause)
	{
		fob.m_iStalls++;
		fob.m_fLastStallAt = now;
		fob.m_fBestDist = 0;
		fob.m_iOrderTries = 0;
		CLW_Orders orders = OrdersOf(fob.m_sSide);
		bool walker = orders && !orders.FindVehicleUnit(fob.m_sBuilderId);
		string orderText = "none";
		if (orders)
		{
			CLW_UnitOrder current = orders.GetOrder(fob.m_sBuilderId);
			if (current)
				orderText = CLW_Command.ToName(current.m_eCommand) + "/" + current.m_sStatus + "/" + current.m_sSource;
		}

		Print(string.Format("CLW: fob %1 STALL %2 of %3 (%4): builder %5 (%6) is %7 m from the site, engaged=%8, order=%9", fob.m_sId, fob.m_iStalls, STALL_MAX, cause, fob.m_sBuilderId, BuilderKindText(walker), Math.Round(dist), Engaged(fob.m_sSide, builderPos, FOB_BUILDER_CONTACT_M), orderText), LogLevel.NORMAL);

		string why;
		if (dist <= ACCEPT_NEAR_M)
		{
			fob.m_fPresentM = ACCEPT_PRESENT_M;
			fob.m_fAwayAt = 0;
			SetState(fob, CLW_EFobState.BUILDING, string.Format("builder stalled %1 m from the site: builds from where it stands (presence %2 m)", Math.Round(dist), ACCEPT_PRESENT_M));
			CLW_BuildExecutor.Step(fob, 0, true);
			return true;
		}

		if (dist <= RELOC_MAX_M && TryRelocate(fob, builderPos, dist))
			return fob.m_eState != CLW_EFobState.MOVING;

		if (TrySwitchSite(fob, builderPos, dist, SITE_SWITCH_GAIN_M, "stall: " + cause))
		{
			fob.m_bLeg = false;
			bool switched = IssueBuildMove(fob, fob.m_vPos, "build " + fob.m_sId, why);
			if (!fob.m_sEscortId.IsEmpty())
				IssueMove(fob.m_sSide, fob.m_sEscortId, fob.m_vPos, "escort " + fob.m_sId, why);

			Print(string.Format("CLW: fob %1 builder re-ordered to the new site (ordered=%2)", fob.m_sId, switched), LogLevel.NORMAL);
			return false;
		}

		if (fob.m_iStalls >= STALL_MAX)
		{
			Fail(fob, string.Format("the builder stalled %1 times (%2 m short, swaps used %3)", fob.m_iStalls, Math.Round(dist), fob.m_iSwaps));
			return true;
		}

		if (fob.m_iStalls >= SWAP_AT_STALL && fob.m_iSwaps < MAX_SWAPS)
		{
			SwapBuilder(fob, dist, cause);
			return true;
		}

		if (!fob.m_bLeg && fob.m_iStalls >= 2)
		{
			vector leg;
			if (CLW_RouteLeg.Find(builderPos, fob.m_vPos, LEG_MIN_M, LEG_MAX_M, leg))
			{
				fob.m_bLeg = true;
				fob.m_vLeg = leg;
				fob.m_fLegAt = now;
				bool legOrdered = IssueBuildMove(fob, leg, "build " + fob.m_sId + " via a road point", why);
				Print(string.Format("CLW: fob %1 route re-planned: builder %2 goes to the road point %3 (%4 m away, grid %5) first (ordered=%6 %7)", fob.m_sId, fob.m_sBuilderId, leg, Math.Round(vector.DistanceXZ(builderPos, leg)), CLW_Places.Grid(leg), legOrdered, why), LogLevel.NORMAL);
				return false;
			}

			Print(string.Format("CLW: fob %1 route re-plan: no free road point toward the site within %2 m of the builder", fob.m_sId, Math.Round(LEG_MAX_M)), LogLevel.NORMAL);
		}

		bool ordered = IssueBuildMove(fob, fob.m_vPos, "build " + fob.m_sId, why);
		Print(string.Format("CLW: fob %1 STALL %2 of %3: order taken over again, move re-issued (ordered=%4 %5)", fob.m_sId, fob.m_iStalls, STALL_MAX, ordered, why), LogLevel.NORMAL);
		return false;
	}

	// 2: the FOB moves to valid ground near the stalled builder. Search radius = min(RELOC_SEARCH_MAX_M, dist - RELOC_MIN_GAIN_M), so the new
	// site is always closer to the builder than the old one; first with the normal objective clearance, then with RELOC_OBJ_RELAX_M.
	protected static bool TryRelocate(CLW_Fob fob, vector builderPos, float dist)
	{
		float radius = Math.Min(RELOC_SEARCH_MAX_M, dist - RELOC_MIN_GAIN_M);
		if (radius < 60)
			return false;

		array<vector> extra = {};
		array<float> extraDist = {};
		float spacing = SPACING_FLOOR_M;
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (bases)
			spacing = Math.Max(SPACING_FLOOR_M, bases.ScaleDistance(SPACING_M));

		foreach (CLW_Fob other : s_aFobs)
		{
			if (other == fob || !other.IsLive())
				continue;

			extra.Insert(other.m_vPos);
			extraDist.Insert(spacing);
		}

		vector site;
		string pass = "normal clearance";
		if (!FindRelocSite(builderPos, radius, CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M, extra, extraDist, site))
		{
			pass = "relaxed objective clearance";
			if (!FindRelocSite(builderPos, radius, RELOC_OBJ_RELAX_M, extra, extraDist, site))
			{
				Print(string.Format("CLW: fob %1 no valid ground within %2 m of the builder (normal + relaxed objective clearance tried)", fob.m_sId, Math.Round(radius)), LogLevel.NORMAL);
				return false;
			}
		}

		float newDist = vector.DistanceXZ(builderPos, site);
		if (!OnOwnHalf(fob.m_sSide, site) || newDist > dist - RELOC_MIN_GAIN_M)
		{
			Print(string.Format("CLW: fob %1 relocation candidate %2 rejected (%3 m from the builder, was %4 m; own half=%5)", fob.m_sId, site, Math.Round(newDist), Math.Round(dist), OnOwnHalf(fob.m_sSide, site)), LogLevel.NORMAL);
			return false;
		}

		vector old = fob.m_vPos;
		fob.m_vPos = site;
		fob.m_bLeg = false;
		Print(string.Format("CLW: fob %1 relocated (%2): %3 -> %4 grid %5, %6 m from the builder (was %7 m)", fob.m_sId, pass, old, site, CLW_Places.Grid(site), Math.Round(newDist), Math.Round(dist)), LogLevel.NORMAL);
		string why;
		IssueBuildMove(fob, site, "build " + fob.m_sId, why);
		if (!fob.m_sEscortId.IsEmpty())
			IssueMove(fob.m_sSide, fob.m_sEscortId, site, "escort " + fob.m_sId, why);

		if (newDist <= ARRIVE_M)
		{
			SetState(fob, CLW_EFobState.BUILDING, "relocated onto the builder");
			CLW_BuildExecutor.Step(fob, 0, true);
		}

		return true;
	}

	// Like CLW_FobSites.FindNear, with the objective clearance as a parameter and no road needed. Dry, flat, clear of the HQs, the objectives
	// (radius + objClear), the extra positions, and room for the FOB cluster (CLW_FreeSpot).
	protected static bool FindRelocSite(vector around, float radius, float objClear, notnull array<vector> extraAvoid, notnull array<float> extraDist, out vector site)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		array<vector> avoid = {};
		array<float> avoidDist = {};
		if (scenario && bases && scenario.m_aFactions && scenario.m_aFactions.Count() == 2)
		{
			float baseClear = Math.Max(CLW_FobSites.BASE_CLEAR_FLOOR_M, bases.ScaleDistance(CLW_FobSites.BASE_CLEAR_M));
			for (int b = 0; b < 2; b++)
			{
				vector hq = bases.GetBasePos(scenario.GetFactionKey(b));
				if (hq != vector.Zero)
				{
					avoid.Insert(hq);
					avoidDist.Insert(baseClear);
				}
			}
		}

		if (points)
		{
			for (int pi = 0; pi < points.Count(); pi++)
			{
				CLW_ControlPoint point = points.Get(pi);
				if (!point)
					continue;

				avoid.Insert(point.m_vPos);
				avoidDist.Insert(point.m_fRadius + objClear);
			}
		}

		for (int xi = 0; xi < extraAvoid.Count(); xi++)
		{
			avoid.Insert(extraAvoid[xi]);
			avoidDist.Insert(extraDist[xi]);
		}

		float roadDist;
		float score;
		if (!CLW_Placement.FindSiteEx(around, radius, avoid, avoidDist, site, roadDist, score))
			return false;

		return CLW_FreeSpot.RefineSite("fob relocation", site, avoid, avoidDist);
	}

	// 3: the pool site (CLW_FobSites) that is valid for the side now (CheckSite, this FOB not counting for the spacing) and at least minGain
	// metres closer to the builder than the current one. Changes the FOB's site; the caller re-orders the builder. False when none.
	protected static bool TrySwitchSite(CLW_Fob fob, vector builderPos, float dist, float minGain, string cause)
	{
		array<CLW_FobSite> sites = {};
		CLW_FobSites.GetSites(sites);
		CLW_FobSite best;
		float bestDist = dist - minGain;
		foreach (CLW_FobSite site : sites)
		{
			if (site.m_sId == fob.m_sSiteId)
				continue;

			string why;
			if (!CheckSite(fob.m_sSide, site, why, fob.m_sId))
				continue;

			float d = vector.DistanceXZ(site.m_vPos, builderPos);
			if (d < bestDist)
			{
				bestDist = d;
				best = site;
			}
		}

		if (!best)
			return false;

		Print(string.Format("CLW: fob %1 takes the nearer site %2 '%3' %4 grid %5 instead of %6 (builder %7 m away instead of %8 m): %9", fob.m_sId, best.m_sId, best.m_sName, best.m_vPos, CLW_Places.Grid(best.m_vPos), fob.m_sSiteId, Math.Round(bestDist), Math.Round(dist), cause), LogLevel.NORMAL);
		fob.m_sSiteId = best.m_sId;
		fob.m_sName = best.m_sName;
		fob.m_vPos = best.m_vPos;
		return true;
	}

	// 5: another unit becomes the builder. The stalled one is banned for this FOB and forgotten by the FOB (its order is dropped, so the
	// commander can re-use it); the FOB goes back to PLANNED, progress is kept, the stall counter starts again.
	protected static void SwapBuilder(CLW_Fob fob, float dist, string cause)
	{
		string old = fob.m_sBuilderId;
		fob.m_aBanned.Insert(old);
		CLW_Orders orders = OrdersOf(fob.m_sSide);
		if (orders)
			orders.ForgetOrder(old);

		fob.m_iSwaps++;
		fob.m_iStalls = 0;
		SetTeamRole(old, false);
		fob.m_sBuilderId = "";
		fob.m_sTagBuilder = "";
		fob.m_bLeg = false;
		fob.m_fPresentM = 0;
		SetState(fob, CLW_EFobState.PLANNED, string.Format("builder %1 swapped out (swap %2 of %3, %4 m short, %5): looking for another unit", old, fob.m_iSwaps, MAX_SWAPS, Math.Round(dist), cause));
	}

	// BUILDING, the builder has been off the site for AWAY_RETURN_SEC (the build is paused meanwhile): back to MOVING.
	protected static void ReturnToMoving(CLW_Fob fob, float now, float dist)
	{
		fob.m_iAwayReturns++;
		if (fob.m_iAwayReturns > MAX_AWAY_RETURNS)
		{
			Fail(fob, string.Format("the builder keeps leaving the site (%1 m away, %2 pct built)", Math.Round(dist), Math.Round(fob.m_fBuild)));
			return;
		}

		fob.m_fAwayAt = 0;
		fob.m_fBestDist = 0;
		fob.m_iOrderTries = 0;
		fob.m_bLeg = false;
		SetState(fob, CLW_EFobState.MOVING, string.Format("builder %1 is %2 m off the site (build paused at %3 pct, return %4 of %5)", fob.m_sBuilderId, Math.Round(dist), Math.Round(fob.m_fBuild), fob.m_iAwayReturns, MAX_AWAY_RETURNS));
		string why;
		bool ordered = IssueBuildMove(fob, fob.m_vPos, "build " + fob.m_sId, why);
		Print(string.Format("CLW: fob %1 builder sent back to the site (ordered=%2 %3)", fob.m_sId, ordered, why), LogLevel.NORMAL);
	}

	protected static void StepBuilding(CLW_Fob fob, float now, float dt)
	{
		bool present = true;
		if (!fob.m_bAbstract)
		{
			CLW_Orders orders = OrdersOf(fob.m_sSide);
			if (!orders)
				return;

			if (!BuilderAlive(fob, orders))
			{
				BuilderLost(fob);
				return;
			}

			vector pos;
			float presentM = ARRIVE_M;
			if (fob.m_fPresentM > presentM)
				presentM = fob.m_fPresentM;

			float dist = 1000000;
			if (orders.UnitPos(fob.m_sBuilderId, pos))
				dist = vector.DistanceXZ(pos, fob.m_vPos);

			fob.m_fBuilderDist = dist;
			present = dist <= presentM;
			if (present)
				fob.m_fAwayAt = 0;
			else if (fob.m_fAwayAt <= 0)
				fob.m_fAwayAt = now;
			else if (now - fob.m_fAwayAt >= AWAY_RETURN_SEC)
			{
				ReturnToMoving(fob, now, dist);
				return;
			}

			// the builder holds the site while it builds: once its MOVE is done it has no waypoint and drifts off
			// (DS run 2026-10-12: build paused at 30 % for minutes after "MOVE -> done: arrived")
			CLW_UnitOrder order = orders.GetOrder(fob.m_sBuilderId);
			if (!order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE)
			{
				string why;
				if (orders.Issue(fob.m_sBuilderId, CLW_ECommand.DEFEND, "", fob.m_vPos, "hold the build site " + fob.m_sId, ORDER_SOURCE, why))
					Print(string.Format("CLW: fob %1 builder %2 holds the site while building", fob.m_sId, fob.m_sBuilderId), LogLevel.NORMAL);
			}
		}

		CLW_BuildExecutor.Step(fob, dt, present);
	}

	// ---- active: loss ----

	protected static void StepActive(CLW_Fob fob, float now, float dt)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war)
			return;

		CLW_FobPresence presence = new CLW_FobPresence();
		presence.m_sSide = fob.m_sSide;
		presence.m_sEnemy = war.GetEnemyKey(fob.m_sSide);
		GetGame().GetWorld().QueryEntitiesBySphere(fob.m_vPos, ZONE_M, presence.OnEntity, null, EQueryEntitiesFlags.DYNAMIC);

		bool wasAttacked = fob.m_bUnderAttack;
		fob.m_bUnderAttack = presence.m_iEnemy > 0;
		if (fob.m_bUnderAttack && !wasAttacked)
		{
			Print(string.Format("CLW: fob %1 under attack: %2 enemy, %3 friendly in the zone", fob.m_sId, presence.m_iEnemy, presence.m_iFriendly), LogLevel.NORMAL);
			war.PushEvent(fob.m_sSide, fob.m_sId + " is under attack");
		}
		else if (!fob.m_bUnderAttack && wasAttacked)
			Print(string.Format("CLW: fob %1 attack over", fob.m_sId), LogLevel.NORMAL);

		if (presence.m_iEnemy > 0 && presence.m_iFriendly == 0)
			fob.m_fEnemyHoldSec += dt;
		else if (presence.m_iEnemy == 0)
			fob.m_fEnemyHoldSec = 0;

		if (fob.m_fEnemyHoldSec >= HOLD_LOST_SEC)
		{
			Lose(fob, string.Format("the enemy held the zone for %1 s", Math.Round(fob.m_fEnemyHoldSec)), CLW_Loc.W("#CLW-Fob_LostHeld", CLW_Loc.N(Math.Round(fob.m_fEnemyHoldSec))));
			return;
		}

		if (fob.m_bHadPost && CLW_BuildExecutor.PostDestroyed(fob))
			Lose(fob, "the command post was destroyed", CLW_Loc.W("#CLW-Fob_LostPost"));
	}

	// ---- end states ----

	// Before the FOB stood: refund, free the units.
	protected static void Fail(CLW_Fob fob, string why)
	{
		if (!fob.IsLive() || fob.IsActive())
			return;

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war && fob.m_iCost > 0)
			war.Refund(fob.m_sSide, fob.m_iCost);

		SetState(fob, CLW_EFobState.FAILED, why + " (cost " + fob.m_iCost + " refunded)");
		fob.m_fEndedAt = CLW_Orders.Now();
		if (war)
			war.PushEvent(fob.m_sSide, fob.m_sId + " cancelled: " + why);

		CLW_Debug.Say(string.Format("%1 %2 cancelled: %3", fob.m_sSide, fob.m_sId, why));
		Release(fob, false);
	}

	// An active FOB is gone.
	protected static void Lose(CLW_Fob fob, string why, string whyWire)
	{
		if (!fob.IsActive())
			return;

		SetState(fob, CLW_EFobState.LOST, why);
		fob.m_fEndedAt = CLW_Orders.Now();
		CLW_BuildExecutor.Remove(fob);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
		{
			war.PushEvent(fob.m_sSide, fob.m_sId + " lost");
			war.PushEvent(war.GetEnemyKey(fob.m_sSide), "enemy " + fob.m_sName + " FOB taken out");
			war.LoseTickets(fob.m_sSide, LOST_TICKETS, fob.m_sId + " lost");
		}

		CLW_Debug.Announce(CLW_Loc.W("#CLW-Fob_Lost", CLW_Loc.FactionName(fob.m_sSide), fob.m_sId, fob.m_sName, whyWire));
		Print(string.Format("CLW: fob %1 lost (%2): spawn point and services removed, -%3 tickets", fob.m_sId, why, LOST_TICKETS), LogLevel.NORMAL);
		if (s_OnLost)
			s_OnLost.Invoke(fob.m_sId, fob.m_sSide);
	}

	// Called by CLW_BuildExecutor when the FOB stands.
	static void NotifyBuilt(CLW_Fob fob)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.PushEvent(fob.m_sSide, fob.m_sId + " established at " + fob.m_sName);

		Release(fob, true);
		if (s_OnBuilt)
			s_OnBuilt.Invoke(fob.m_sId, fob.m_sSide);
	}

	// The builder and escort are free again (reservation ends with the state). The escort garrisons a finished FOB.
	protected static void Release(CLW_Fob fob, bool garrison)
	{
		if (garrison && !fob.m_sEscortId.IsEmpty())
		{
			string why;
			IssueDefend(fob.m_sSide, fob.m_sEscortId, fob.m_vPos, "garrison " + fob.m_sId, why);
		}

		// a FOB that did not stand: builder and escort drop their "build / escort" orders (else they walk on to a refunded site) and
		// the commander can use them at once
		if (!garrison)
		{
			CLW_Orders orders = OrdersOf(fob.m_sSide);
			if (orders)
			{
				if (!fob.m_sBuilderId.IsEmpty())
					orders.ForgetOrder(fob.m_sBuilderId);
				if (!fob.m_sEscortId.IsEmpty())
					orders.ForgetOrder(fob.m_sEscortId);
			}
		}

		SetTeamRole(fob.m_sBuilderId, false);
		Print(string.Format("CLW: fob %1 released builder='%2' escort='%3' (garrison=%4)", fob.m_sId, fob.m_sBuilderId, fob.m_sEscortId, garrison), LogLevel.NORMAL);
	}

	protected static bool IssueDefend(string side, string unitId, vector pos, string reason, out string why)
	{
		CLW_Orders orders = OrdersOf(side);
		if (!orders)
		{
			why = "no orders object";
			return false;
		}

		return orders.Issue(unitId, CLW_ECommand.DEFEND, "", pos, reason, ORDER_SOURCE, why);
	}

	// ---- situation JSON (CLW_Situation) ----

	// The side's FOBs (own, fog: enemy FOBs are not listed), live ones and ones lost in the last LOST_SHOW_SEC.
	static string FobsJson(string side)
	{
		string text = "";
		float now = CLW_Orders.Now();
		foreach (CLW_Fob fob : s_aFobs)
		{
			if (fob.m_sSide != side)
				continue;

			if (fob.m_eState == CLW_EFobState.FAILED)
				continue;

			if (fob.m_eState == CLW_EFobState.LOST && now - fob.m_fEndedAt > LOST_SHOW_SEC)
				continue;

			string services = "";
			array<CLW_ServiceInstance> insts = {};
			CLW_Services.GetInstances(side, fob.m_sId, insts);
			foreach (CLW_ServiceInstance inst : insts)
			{
				if (!services.IsEmpty())
					services += ",";
				services += "\"" + CLW_Situation.Escape(inst.m_sKind) + "\"";
			}

			if (!text.IsEmpty())
				text += ",";
			text += "{\"id\":\"" + fob.m_sId + "\",\"site\":\"" + CLW_Situation.Escape(fob.m_sSiteId) + "\",\"name\":\"" + CLW_Situation.Escape(fob.m_sName) + "\",\"pos\":[" + Math.Round(fob.m_vPos[0]) + "," + Math.Round(fob.m_vPos[2]) + "],\"grid\":\"" + CLW_Places.Grid(fob.m_vPos) + "\"";
			text += ",\"state\":\"" + fob.StateName() + "\",\"build\":" + Math.Round(fob.m_fBuild) + ",\"tier\":" + fob.m_iTier + ",\"services\":[" + services + "],\"underAttack\":" + CLW_Situation.BoolText(fob.m_bUnderAttack);
			if (CLW_Capabilities.Has("logistics"))
				text += ",\"stock\":" + CLW_SupplyStore.Get(side, fob.m_sId);
			if (!fob.m_sBuilderId.IsEmpty() && fob.IsInProgress())
				text += ",\"builder\":\"" + CLW_Situation.Escape(fob.m_sBuilderId) + "\"";
			text += "}";
		}

		return text;
	}

	// Candidate sites the side could use now (at most 4, frontline first).
	static string SitesJson(string side)
	{
		string text = "";
		array<CLW_FobSite> sites = {};
		CLW_FobSites.PickSites(side, 4, sites);
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		vector hq;
		if (bases)
			hq = bases.GetBasePos(side);
		CLW_ControlPoint front = CLW_FobSites.FrontObjective(side);

		foreach (CLW_FobSite site : sites)
		{
			if (!text.IsEmpty())
				text += ",";
			text += "{\"id\":\"" + CLW_Situation.Escape(site.m_sId) + "\",\"name\":\"" + CLW_Situation.Escape(site.m_sName) + "\",\"pos\":[" + Math.Round(site.m_vPos[0]) + "," + Math.Round(site.m_vPos[2]) + "],\"grid\":\"" + CLW_Places.Grid(site.m_vPos) + "\"";
			text += ",\"place\":\"" + CLW_Situation.Escape(CLW_Places.PlaceLabel(site.m_vPos, points)) + "\",\"distFromHq\":" + Math.Round(vector.DistanceXZ(site.m_vPos, hq));
			if (front)
				text += ",\"front\":\"" + CLW_Situation.Escape(front.m_sId) + "\",\"toFront\":" + Math.Round(vector.DistanceXZ(site.m_vPos, front.m_vPos));
			text += "}";
		}

		return text;
	}

	static string RulesJson(string side)
	{
		string text = "{\"max\":" + GetMax() + ",\"live\":" + CountLive(side) + ",\"inProgress\":" + CountInProgress(side) + ",\"cost\":" + GetCost() + ",\"upgradeCost\":" + GetUpgradeCost();
		text += ",\"build\":\"" + BuildModeName() + "\",\"buildSec\":" + GetBuildSec() + ",\"logistics\":" + CLW_Situation.BoolText(CLW_Capabilities.Has("logistics"));
		string readyWhy;
		bool ready = IsReady(side, readyWhy);
		int elapsedSec;
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			elapsedSec = war.GetElapsedSec();
		text += ",\"earliestSec\":" + GetEarliestSec() + ",\"elapsedSec\":" + elapsedSec + ",\"ready\":" + CLW_Situation.BoolText(ready) + ",\"readyReason\":\"" + CLW_Situation.Escape(readyWhy) + "\"}";
		return text;
	}
}

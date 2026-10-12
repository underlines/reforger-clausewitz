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
	int m_iTier;				// 0 = none yet, 1 = command post + arsenal, 2 = + depot, medical
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
	float m_fLastPauseLogAt;
	float m_fEnemyHoldSec;
	bool m_bUnderAttack;
	bool m_bHadPost;			// the command post existed once (its loss then means the FOB is lost)
	SCR_SpawnPoint m_SpawnPoint;

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

class CLW_FobManager
{
	static const int TICK_MS = 3000;
	static const string ORDER_SOURCE = "fob";			// CLW_Orders source of the builder / escort orders
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

	protected static ref array<ref CLW_Fob> s_aFobs = {};
	protected static ref ScriptInvoker s_OnBuilt;
	protected static ref ScriptInvoker s_OnLost;
	protected static int s_iNext;
	protected static bool s_bTicking;
	protected static float s_fLastTick;

	// ---- lifecycle ----

	// Game start (server): statics survive a Play restart, so forget the previous session.
	static void Reset()
	{
		GetGame().GetCallqueue().Remove(Tick);
		s_bTicking = false;
		s_aFobs.Clear();
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
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (spawner)
		{
			spawner.GetOnTagged().Remove(OnSpawnTagged);
			spawner.GetOnTagged().Insert(OnSpawnTagged);
		}

		s_fLastTick = CLW_Orders.Now();
		s_bTicking = true;
		GetGame().GetCallqueue().CallLater(Tick, TICK_MS, true);
		Print(string.Format("CLW: fob ready: max %1 per side, build %2, cost %3, build time %4 s, %5 sites, logistics=%6", GetMax(), BuildModeName(), GetCost(), GetBuildSec(), CLW_FobSites.Count(), CLW_Capabilities.Has("logistics")), LogLevel.NORMAL);
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

	// Tier 2 (vehicle depot, medical) costs half of the FOB.
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
	static bool CheckSite(string side, CLW_FobSite site, out string why)
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
			if (!fob.IsLive())
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

	// Tier 2 services (vehicle depot, medical) for an active FOB at tier 1.
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
		if (!war || !war.TrySpend(side, GetUpgradeCost()))
		{
			why = string.Format("not enough supplies for the upgrade (cost %1)", GetUpgradeCost());
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
		if (!war || war.GetSupplies(side) < GetUpgradeCost())
		{
			why = string.Format("not enough supplies for the upgrade (cost %1)", GetUpgradeCost());
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

		string found = FindFreeTransport(fob.m_sSide, fob.m_vPos);
		if (!found.IsEmpty())
		{
			fob.m_sBuilderId = found;
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
			if (transport.m_sKind == CLW_ForceKind.MOTORIZED && transport.HasVehicle() && (!pick || transport.m_iCost < pick.m_iCost))
			{
				pick = transport;
				pickNote = "motorized transport";
			}
		}

		if (!pick)
		{
			foreach (CLW_ForceEntry carrier : entries)
			{
				if (carrier.HasVehicle() && carrier.m_iCargoSeats != 0 && (!pick || carrier.m_iCost < pick.m_iCost))
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
		if (!spawner.BuyTagged(fob.m_sSide, pick.m_sType, "build " + fob.m_sId, fob.m_sTagBuilder, error))
		{
			if (fob.m_eState == CLW_EFobState.PLANNED)
				Fail(fob, "cannot buy a builder: " + error);

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
		SetState(fob, CLW_EFobState.MOVING, "builder " + fob.m_sBuilderId + " heads for " + fob.m_vPos);
		string why;
		if (!IssueMove(fob.m_sSide, fob.m_sBuilderId, fob.m_vPos, "build " + fob.m_sId, why))
			Print(string.Format("CLW: fob %1 move order refused: %2 (retried by the tick)", fob.m_sId, why), LogLevel.WARNING);

		EnsureEscort(fob);
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

	// No order of its own (or only the rule commander's standing reserve at the base).
	protected static bool IsFreeUnit(CLW_Orders orders, string id)
	{
		CLW_UnitOrder order = orders.GetOrder(id);
		if (!order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE)
			return true;

		return order.m_eCommand == CLW_ECommand.DEFEND && order.m_sTarget.IsEmpty();
	}

	// The free vehicle unit of kind motorized nearest to the site, "" when none.
	protected static string FindFreeTransport(string side, vector site)
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

	protected static string FindFreeInfantry(string side, vector site)
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
			if (orders.FindVehicleUnit(id) || registry.GetKind(id) != CLW_ForceKind.INFANTRY)
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

		string escort = FindFreeInfantry(fob.m_sSide, fob.m_vPos);
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
		if (spawner.BuyTagged(fob.m_sSide, cheapest.m_sType, "escort " + fob.m_sId, fob.m_sTagEscort, error))
			Print(string.Format("CLW: fob %1 escort: bought %2", fob.m_sId, cheapest.m_sType), LogLevel.NORMAL);
		else
			Print(string.Format("CLW: fob %1 escort: none (%2), the builder goes alone", fob.m_sId, error), LogLevel.NORMAL);
	}

	// ---- moving / building ----

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

		CLW_UnitOrder order = orders.GetOrder(fob.m_sBuilderId);
		if (!order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE)
		{
			if (fob.m_iOrderTries >= MAX_ORDER_TRIES)
			{
				Fail(fob, "the builder cannot reach the site");
				return;
			}

			fob.m_iOrderTries++;
			string why;
			bool ordered = IssueMove(fob.m_sSide, fob.m_sBuilderId, fob.m_vPos, "build " + fob.m_sId, why);
			Print(string.Format("CLW: fob %1 move re-issued (%2 of %3, %4 m short, ordered=%5 %6)", fob.m_sId, fob.m_iOrderTries, MAX_ORDER_TRIES, Math.Round(dist), ordered, why), LogLevel.NORMAL);
		}

		if (now - fob.m_fLastMoveLogAt >= MOVE_LOG_SEC)
		{
			fob.m_fLastMoveLogAt = now;
			Print(string.Format("CLW: fob %1 builder %2 en route, %3 m to go", fob.m_sId, fob.m_sBuilderId, Math.Round(dist)), LogLevel.NORMAL);
		}
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
			present = orders.UnitPos(fob.m_sBuilderId, pos) && vector.DistanceXZ(pos, fob.m_vPos) <= ARRIVE_M;
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
			Lose(fob, string.Format("the enemy held the zone for %1 s", Math.Round(fob.m_fEnemyHoldSec)));
			return;
		}

		if (fob.m_bHadPost && CLW_BuildExecutor.PostDestroyed(fob))
			Lose(fob, "the command post was destroyed");
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
	protected static void Lose(CLW_Fob fob, string why)
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

		CLW_Debug.Announce(string.Format("%1 FOB %2 near %3 LOST: %4", fob.m_sSide, fob.m_sId, fob.m_sName, why));
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

		foreach (CLW_FobSite site : sites)
		{
			if (!text.IsEmpty())
				text += ",";
			text += "{\"id\":\"" + CLW_Situation.Escape(site.m_sId) + "\",\"name\":\"" + CLW_Situation.Escape(site.m_sName) + "\",\"pos\":[" + Math.Round(site.m_vPos[0]) + "," + Math.Round(site.m_vPos[2]) + "],\"grid\":\"" + CLW_Places.Grid(site.m_vPos) + "\"";
			text += ",\"place\":\"" + CLW_Situation.Escape(CLW_Places.PlaceLabel(site.m_vPos, points)) + "\",\"distFromHq\":" + Math.Round(vector.DistanceXZ(site.m_vPos, hq)) + "}";
		}

		return text;
	}

	static string RulesJson(string side)
	{
		string text = "{\"max\":" + GetMax() + ",\"live\":" + CountLive(side) + ",\"inProgress\":" + CountInProgress(side) + ",\"cost\":" + GetCost() + ",\"upgradeCost\":" + GetUpgradeCost();
		text += ",\"build\":\"" + BuildModeName() + "\",\"buildSec\":" + GetBuildSec() + ",\"logistics\":" + CLW_Situation.BoolText(CLW_Capabilities.Has("logistics")) + "}";
		return text;
	}
}

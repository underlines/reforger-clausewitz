enum CLW_ESpawnState
{
	GROUP_WAIT,		// infantry: waiting for the members of the spawned group
	CREW_WAIT,		// vehicle: waiting for SpawnDefaultOccupants (crew group)
	CARGO_WAIT,		// vehicle: embarked infantry group spawned, waiting for its members
	VERIFY,			// embarked: one tick later, check who really sits in the vehicle
	DONE,
	FAILED
}

// One purchase / start force in progress. Everything async has a timeout (CLW_WarConfig.SPAWN_*).
class CLW_SpawnJob
{
	string m_sFaction;
	ref CLW_ForceEntry m_Entry;
	string m_sReason;
	bool m_bFree;
	bool m_bCharged;
	vector m_vPos;
	float m_fYaw;				// heading of a vehicle (degrees), from the placement
	string m_sVia;				// how the spot was found: slot | road | navmesh | fallback
	float m_fAge;
	float m_fStateAge;
	CLW_ESpawnState m_eState;
	string m_sUnitId;			// registry id of the commandable unit (infantry group, or the crew group)
	string m_sCargoId;
	SCR_AIGroup m_Group;		// infantry group, or the crew group of a vehicle
	SCR_AIGroup m_Cargo;		// embarked infantry group
	IEntity m_Vehicle;
	bool m_bCrewDone;			// callback fired
	bool m_bCrewCanceled;
	ref array<IEntity> m_aCrew;	// occupants reported by the callback (weak entity pointers)
	string m_sFailure;
	string m_sTag;				// owner tag of a tagged purchase (CLW_ForceSpawner.BuyTagged), "" = none
	int m_iMounted;
	int m_iVerifyPass;			// 0: first check (then retry with explicit slots), 1: final check
	int m_iRetried;
	int m_iMemberRetries;		// spawn re-requests for a group that stayed empty (active-AI limit dropped the request)
	int m_iRequester = -1;		// player id whose request (CLW_RequestService) bought this infantry group, -1 = none
}

// Spawns forces at the base: purchases (supplies) and start forces (free). Server only. Vehicles get a crew from
// SpawnDefaultOccupants({PILOT, TURRET}); embarked infantry is a separate group moved into the real cargo seats.
// The finished vehicle unit is handed to CLW_VehicleManager. Every step is logged ("CLW: spawn ...").
class CLW_ForceSpawner
{
	protected ref array<ref CLW_SpawnJob> m_aJobs = {};
	protected ref map<string, int> m_mCounters = new map<string, int>();	// per faction + kind tag
	protected CLW_ForceCatalog m_Catalog;		// weak: owned by the game mode
	protected CLW_SquadRegistry m_Registry;		// weak
	protected CLW_VehicleManager m_Vehicles;	// weak
	protected bool m_bTicking;
	protected string m_sNextTag;				// tag handed to the job Start() creates (BuyTagged)
	protected ref ScriptInvoker m_OnTagged;		// (string tag, string unitId): a tagged purchase finished (unitId = registry id) or failed (unitId empty)
	protected int m_iNextRequester = -1;		// set by BuyFor for the job Start creates
	protected vector m_vNextAnchor;				// set by BuyFor: infantry forms near this position (a depot) instead of the HQ spawn zone

	void Init(CLW_ForceCatalog catalog, CLW_SquadRegistry registry, CLW_VehicleManager vehicles)
	{
		m_Catalog = catalog;
		m_Registry = registry;
		m_Vehicles = vehicles;
		if (!Replication.IsServer())
			return;

		GetGame().GetCallqueue().CallLater(Tick, CLW_WarConfig.SPAWNER_TICK_MS, true);
		Print("CLW: spawn spawner ready", LogLevel.NORMAL);
	}

	// ---- public API ----

	// Purchase: validates (war running, known type, AI cap, vehicle cap, supplies), charges, spawns at the base.
	// False with a reason in error; nothing is charged then. The unit is complete later ("arrived" event).
	bool Buy(string factionKey, string typeId, string reason, out string error)
	{
		return Start(factionKey, typeId, reason, false, error);
	}

	// Purchase for a player request (CLW_RequestService): like Buy, and when the infantry group is complete it is handed to
	// CLW_RequestService.OnSquadReady (the AI join the requester's squad). Infantry types only.
	bool BuyFor(string factionKey, string typeId, string reason, int requesterId, out string error, vector anchor = vector.Zero)
	{
		m_iNextRequester = requesterId;
		m_vNextAnchor = anchor;
		bool started = Start(factionKey, typeId, reason, false, error);
		m_iNextRequester = -1;
		m_vNextAnchor = vector.Zero;
		return started;
	}

	// Purchase for an owner system (FOB builders): same as Buy, and GetOnTagged() fires with (tag, unit id) when the unit is READY
	// (id = the registry id of the unit, the crew group for a vehicle) or (tag, "") when the purchase failed (also when this returns false).
	bool BuyTagged(string factionKey, string typeId, string reason, string tag, out string error)
	{
		m_sNextTag = tag;
		bool started = Start(factionKey, typeId, reason, false, error);
		m_sNextTag = "";
		return started;
	}

	ScriptInvoker GetOnTagged()
	{
		if (!m_OnTagged)
			m_OnTagged = new ScriptInvoker();

		return m_OnTagged;
	}

	// Same without charging supplies and without the AI/vehicle caps (start forces are part of the scenario).
	bool SpawnFree(string factionKey, string typeId, string reason, out string error)
	{
		return Start(factionKey, typeId, reason, true, error);
	}

	// Spawn the start forces listed in the scenario faction setups, staggered. Call when the war has started.
	void SpawnStartForces()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario || !scenario.m_aFactions)
			return;

		int slot;
		foreach (CLW_FactionSetup setup : scenario.m_aFactions)
		{
			if (!setup || !setup.m_aStartForces)
				continue;

			foreach (string typeId : setup.m_aStartForces)
			{
				int delayMs = (1 + slot) * CLW_WarConfig.SPAWN_STAGGER_SEC * 1000;
				slot++;
				Print(string.Format("CLW: spawn start force %1 %2 scheduled in %3 ms", setup.m_sKey, typeId, delayMs), LogLevel.NORMAL);
				GetGame().GetCallqueue().CallLater(StartForceLater, delayMs, false, setup.m_sKey, typeId);
			}
		}
	}

	protected static bool IsBetterStartEntry(CLW_ForceEntry a, CLW_ForceEntry b)
	{
		if (a.m_iWeight != b.m_iWeight)
			return a.m_iWeight > b.m_iWeight;

		int sizeA = a.m_iGroupMembers + a.m_iCargoMembers;
		int sizeB = b.m_iGroupMembers + b.m_iCargoMembers;
		if (sizeA != sizeB)
			return sizeA > sizeB;

		return a.m_iCost < b.m_iCost;
	}

	protected void StartForceLater(string factionKey, string typeId)
	{
		// The faction on a side may not be the one the type id was written for (faction choice, P6): when the catalog has
		// no such type id, read it as a generic kind and take the most representative entry of that kind: highest buy
		// weight (the profile's main unit), then the most soldiers (a full squad, not a 2-man patrol), then the cheapest.
		if (m_Catalog && !m_Catalog.Find(factionKey, typeId))
		{
			array<CLW_ForceEntry> candidates = {};
			m_Catalog.GetEntries(factionKey, candidates);
			CLW_ForceEntry cheapest;	// = the chosen entry
			foreach (CLW_ForceEntry candidate : candidates)
			{
				if (candidate.m_sKind != typeId || !candidate.m_bEnabled)
					continue;
				if (!cheapest || IsBetterStartEntry(candidate, cheapest))
					cheapest = candidate;
			}

			if (!cheapest)
			{
				Print(string.Format("CLW: spawn start force %1 %2 skipped: not a type id or kind of this faction's catalog", factionKey, typeId), LogLevel.WARNING);
				return;
			}

			Print(string.Format("CLW: spawn start force %1 %2 -> kind match %3", factionKey, typeId, cheapest.m_sType), LogLevel.NORMAL);
			typeId = cheapest.m_sType;
		}

		string error;
		if (!SpawnFree(factionKey, typeId, "start force", error))
			Print(string.Format("CLW: spawn start force %1 %2 FAILED: %3", factionKey, typeId, error), LogLevel.ERROR);
	}

	// Live AI soldiers of a faction (adopted squads, incl. hidden cargo groups) plus the expected AI of purchases in progress.
	int GetLiveAI(string factionKey)
	{
		int total;
		array<string> ids = {};
		if (m_Registry)
			m_Registry.GetIds(factionKey, ids);
		foreach (string id : ids)
		{
			SCR_AIGroup group = m_Registry.Find(id);
			if (group)
				total += CLW_SquadInfo.AliveCount(group);
		}

		return total;
	}

	int GetPendingAI(string factionKey)
	{
		int total;
		foreach (CLW_SpawnJob job : m_aJobs)
		{
			if (job.m_sFaction == factionKey)
				total += job.m_Entry.ExpectedAI();
		}

		return total;
	}

	// Vehicles of a faction: linked vehicle units plus purchases in progress.
	int GetLiveVehicles(string factionKey)
	{
		int total;
		if (m_Vehicles)
			total = m_Vehicles.CountVehicles(factionKey);
		foreach (CLW_SpawnJob job : m_aJobs)
		{
			if (job.m_sFaction == factionKey && job.m_Entry.HasVehicle())
				total++;
		}

		return total;
	}

	// Engine-wide active AI count and limit (false when there is no AI world).
	static bool GetEngineAI(out int active, out int limit)
	{
		ChimeraAIWorld aiWorld = ChimeraAIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld)
			return false;

		active = aiWorld.GetCurrentNumOfActiveAIs();
		limit = aiWorld.GetLimitOfActiveAIs();
		return limit > 0;
	}

	// AI the engine can still activate for new purchases: limit - active - margin - AI of purchases still spawning (all sides).
	// A large number when the limit is unknown.
	int GetEngineHeadroom()
	{
		int active;
		int limit;
		if (!GetEngineAI(active, limit))
			return 100000;

		int pending;
		foreach (CLW_SpawnJob job : m_aJobs)
		{
			pending += job.m_Entry.ExpectedAI();
		}

		return limit - active - CLW_WarConfig.ENGINE_AI_MARGIN - pending;
	}

	int GetPendingCount()
	{
		return m_aJobs.Count();
	}

	// ---- start ----

	protected bool Start(string factionKey, string typeId, string reason, bool free, out string error)
	{
		string label = "buy";
		if (free)
			label = "free";

		if (!Replication.IsServer())
		{
			error = "not server";
			return false;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
		{
			error = "war not running";
			Print(string.Format("CLW: spawn %1 %2 %3 refused: %4", label, factionKey, typeId, error), LogLevel.WARNING);
			return false;
		}

		if (!CLW_ScenarioConfig.IsClwFaction(factionKey))
		{
			error = "unknown faction " + factionKey;
			Print(string.Format("CLW: spawn %1 refused: %2", label, error), LogLevel.WARNING);
			return false;
		}

		CLW_ForceEntry source = m_Catalog.Find(factionKey, typeId);
		if (!source)
		{
			error = "unknown force type " + typeId;
			Print(string.Format("CLW: spawn %1 %2 refused: %3", label, factionKey, error), LogLevel.WARNING);
			return false;
		}

		int expected = source.ExpectedAI();
		int cap = war.GetAiCap(factionKey);
		int live = GetLiveAI(factionKey) + GetPendingAI(factionKey);
		if (!free && cap > 0 && live + expected > cap)
		{
			error = string.Format("AI cap (%1 live + %2 new > %3)", live, expected, cap);
			Print(string.Format("CLW: spawn %1 %2 %3 refused: %4", label, factionKey, typeId, error), LogLevel.NORMAL);
			return false;
		}

		if (!free && GetEngineHeadroom() < expected)
		{
			int engineActive;
			int engineLimit;
			GetEngineAI(engineActive, engineLimit);
			error = string.Format("engine AI limit (active %1/%2, needs %3, margin %4)", engineActive, engineLimit, expected, CLW_WarConfig.ENGINE_AI_MARGIN);
			Print(string.Format("CLW: spawn %1 %2 %3 refused: %4", label, factionKey, typeId, error), LogLevel.NORMAL);
			return false;
		}

		if (!free && source.HasVehicle())
		{
			int vehicleCap = war.GetVehicleCap(factionKey);
			int vehicles = GetLiveVehicles(factionKey);
			if (vehicleCap > 0 && vehicles + 1 > vehicleCap)
			{
				error = string.Format("vehicle cap (%1 + 1 > %2)", vehicles, vehicleCap);
				Print(string.Format("CLW: spawn %1 %2 %3 refused: %4", label, factionKey, typeId, error), LogLevel.NORMAL);
				return false;
			}
		}

		CLW_Bases bases = CLW_GameMode.GetBases();
		vector spot;
		float spotYaw;
		string via;
		bool placed;
		string kindLabel = "infantry";
		if (bases)
		{
			if (source.HasVehicle())
			{
				kindLabel = "vehicle";
				placed = bases.FindVehicleSpot(factionKey, CLW_WarConfig.SPAWN_RADIUS_M, CLW_WarConfig.SPAWN_VEHICLE_CYLINDER_R, CLW_WarConfig.SPAWN_VEHICLE_CYLINDER_H, spot, spotYaw, via);
			}
			else
			{
				// player squad request: form beside the vehicle depot (clear ground, CLW_FreeSpot), else the HQ spawn zone
				if (m_vNextAnchor != vector.Zero)
				{
					placed = CLW_FreeSpot.FindSmall("squad " + factionKey, m_vNextAnchor, 6, 35, 4, 2.2, spot, spotYaw);
					if (placed)
						via = "depot";
				}

				if (!placed)
					placed = bases.FindInfantrySpot(factionKey, CLW_WarConfig.SPAWN_RADIUS_M, spot, via);
			}
		}
		if (!placed)
		{
			error = "no spawn spot at the base";
			Print(string.Format("CLW: spawn %1 %2 %3 refused: %4", label, factionKey, typeId, error), LogLevel.ERROR);
			return false;
		}
		Print(string.Format("CLW: spawn place %1 at %2 yaw=%3 via %4", kindLabel, spot, Math.Round(spotYaw), via), LogLevel.NORMAL);

		CLW_SpawnJob job = new CLW_SpawnJob();
		job.m_sFaction = factionKey;
		job.m_Entry = source.CloneEntry();
		job.m_sReason = reason;
		job.m_sTag = m_sNextTag;
		job.m_bFree = free;
		if (!source.HasVehicle())
			job.m_iRequester = m_iNextRequester;
		job.m_vPos = spot;
		job.m_fYaw = spotYaw;
		job.m_sVia = via;
		if (!free)
		{
			if (!war.TrySpend(factionKey, source.m_iCost))
			{
				error = string.Format("not enough supplies (cost %1, have %2)", source.m_iCost, war.GetSupplies(factionKey));
				Print(string.Format("CLW: spawn buy %1 %2 refused: %3", factionKey, typeId, error), LogLevel.NORMAL);
				return false;
			}
			job.m_bCharged = true;
		}

		Print(string.Format("CLW: spawn %1 %2 %3 (%4) accepted at %5, cost %6, expected AI %7, reason: %8", label, factionKey, typeId, source.m_sName, spot, source.m_iCost, expected, reason), LogLevel.NORMAL);
		string verb = "bought";
		if (free)
			verb = "fielded";
		war.PushEvent(factionKey, factionKey + " " + verb + " " + source.m_sName);

		bool started;
		if (job.m_Entry.HasVehicle())
			started = BeginVehicle(job);
		else
			started = BeginInfantry(job);

		if (!started)
		{
			error = job.m_sFailure;
			Fail(job, job.m_sFailure);
			return false;
		}

		m_aJobs.Insert(job);
		return true;
	}

	// ---- naming ----

	protected string NextName(string factionKey, string kind)
	{
		string tag = CLW_ForceKind.Tag(kind);
		string key = factionKey + "_" + tag;
		while (true)
		{
			int n;
			m_mCounters.Find(key, n);
			n++;
			m_mCounters.Set(key, n);
			string name = key + n;
			if (!GetGame().GetWorld().FindEntityByName(name) && (!m_Registry || !m_Registry.Find(name)))
				return name;
		}

		return "";
	}

	// ---- spawning primitives ----

	protected SCR_AIGroup SpawnGroup(ResourceName prefab, vector pos)
	{
		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
		{
			Print("CLW: spawn group prefab does not load: " + prefab, LogLevel.ERROR);
			return null;
		}

		pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = pos;
		SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params));
		if (!group)
		{
			Print("CLW: spawn prefab is not an SCR_AIGroup: " + prefab, LogLevel.ERROR);
			return null;
		}

		group.SetDeleteWhenEmpty(true);
		// The engine's spawn queue admits a group only while the active AI count is below a share of the global limit that
		// depends on the group importance (LOW 50 %, NORMAL 70 %, HIGH 90 %, CRITICAL 100 %); a refused request is DROPPED and
		// the group stays empty. HQ purchases must not lose against ambient AI, so they get the full limit.
		group.SetImportance(SCR_EAISpawnImportance.CRITICAL);
		return group;
	}

	// "active AI 90/128" of the engine's global limit, for the spawn logs.
	protected static string ActiveAINote()
	{
		ChimeraAIWorld aiWorld = ChimeraAIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld)
			return "active AI n/a";

		return string.Format("active AI %1/%2", aiWorld.GetCurrentNumOfActiveAIs(), aiWorld.GetLimitOfActiveAIs());
	}

	// Group still empty after the timeout: ask the spawn queue again. True = re-requested (keep waiting).
	protected bool RetryMembers(CLW_SpawnJob job, SCR_AIGroup group, int slots)
	{
		if (job.m_iMemberRetries >= CLW_WarConfig.SPAWN_MEMBERS_RETRIES)
			return false;

		job.m_iMemberRetries++;
		job.m_fStateAge = 0;
		Print(string.Format("CLW: spawn %1 group %2 still empty after %3 s (%4): re-requesting members (retry %5/%6, dormant alive %7)", job.m_sFaction, job.m_sUnitId, CLW_WarConfig.SPAWN_MEMBERS_TIMEOUT_SEC, ActiveAINote(), job.m_iMemberRetries, CLW_WarConfig.SPAWN_MEMBERS_RETRIES, group.GetDormantAliveCount()), LogLevel.WARNING);
		group.RequestSpawn(slots);
		return true;
	}

	// Name a group (so its registry id is the name) and tell the registry what it is. If the registry already adopted
	// it under a generated id (rare race), that id is kept and tagged instead. Returns the id in effect.
	protected string NameAndTag(SCR_AIGroup group, string wantedName, string kind, string displayType, string typeId, bool hidden)
	{
		string id;
		if (m_Registry)
			id = m_Registry.GetId(group);

		if (id.IsEmpty())
		{
			group.SetName(wantedName);
			id = wantedName;
		}
		else if (id != wantedName)
		{
			Print(string.Format("CLW: spawn group already adopted as %1 (wanted %2), keeping the registry id", id, wantedName), LogLevel.WARNING);
		}

		if (m_Registry)
			m_Registry.Tag(id, kind, displayType, typeId, hidden);

		return id;
	}

	// ---- infantry ----

	protected bool BeginInfantry(CLW_SpawnJob job)
	{
		CLW_ForceEntry entry = job.m_Entry;
		SCR_AIGroup group = SpawnGroup(entry.m_GroupPrefab, job.m_vPos);
		if (!group)
		{
			job.m_sFailure = "group spawn failed";
			return false;
		}

		job.m_Group = group;
		job.m_sUnitId = NameAndTag(group, NextName(job.m_sFaction, entry.m_sKind), entry.m_sKind, entry.m_sName, entry.m_sType, false);
		job.m_eState = CLW_ESpawnState.GROUP_WAIT;
		Print(string.Format("CLW: spawn %1 infantry group spawned as %2 (%3), waiting for %4 members", job.m_sFaction, job.m_sUnitId, entry.m_sType, entry.m_iGroupMembers), LogLevel.NORMAL);
		return true;
	}

	protected void TickInfantry(CLW_SpawnJob job)
	{
		if (!job.m_Group)
		{
			Fail(job, "group vanished while waiting for members");
			return;
		}

		int members = job.m_Group.GetAgentsCount();
		bool complete = members >= job.m_Entry.m_iGroupMembers;
		if (!complete && job.m_fStateAge >= CLW_WarConfig.SPAWN_MEMBERS_TIMEOUT_SEC && members > 0)
		{
			Print(string.Format("CLW: spawn %1 members timeout: %2/%3 present, continuing", job.m_sUnitId, members, job.m_Entry.m_iGroupMembers), LogLevel.WARNING);
			complete = true;
		}

		if (!complete)
		{
			if (job.m_fStateAge >= CLW_WarConfig.SPAWN_MEMBERS_TIMEOUT_SEC && !RetryMembers(job, job.m_Group, job.m_Entry.m_iGroupMembers))
				Fail(job, "no members arrived (" + ActiveAINote() + ")");

			return;
		}

		Finish(job, string.Format("%1 members (%2)", members, ActiveAINote()));
	}

	// ---- vehicles ----

	protected bool BeginVehicle(CLW_SpawnJob job)
	{
		CLW_ForceEntry entry = job.m_Entry;
		Resource resource = Resource.Load(entry.m_VehiclePrefab);
		if (!resource || !resource.IsValid())
		{
			job.m_sFailure = "vehicle prefab does not load";
			return false;
		}

		vector pos = job.m_vPos;
		pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		vector mat[4];
		Math3D.AnglesToMatrix(Vector(job.m_fYaw, 0, 0), mat); // yaw, pitch, roll in degrees
		params.Transform[0] = mat[0];
		params.Transform[1] = mat[1];
		params.Transform[2] = mat[2];
		params.Transform[3] = pos;
		IEntity vehicle = GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params);
		if (!vehicle)
		{
			job.m_sFailure = "vehicle spawn failed";
			return false;
		}

		job.m_Vehicle = vehicle;
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.RegisterVehicle(vehicle, job.m_sFaction);

		SCR_BaseCompartmentManagerComponent compartments = SCR_BaseCompartmentManagerComponent.Cast(vehicle.FindComponent(SCR_BaseCompartmentManagerComponent));
		if (!compartments)
		{
			job.m_sFailure = "vehicle has no SCR_BaseCompartmentManagerComponent";
			return false;
		}

		compartments.GetOnDoneSpawningDefaultOccupants().Insert(OnOccupantsDone);
		array<ECompartmentType> crewTypes = {ECompartmentType.PILOT, ECompartmentType.TURRET};
		if (!compartments.SpawnDefaultOccupants(crewTypes))
		{
			compartments.GetOnDoneSpawningDefaultOccupants().Remove(OnOccupantsDone);
			job.m_sFailure = "vehicle has no default crew to spawn";
			return false;
		}

		job.m_eState = CLW_ESpawnState.CREW_WAIT;
		Print(string.Format("CLW: spawn %1 vehicle %2 spawned at %3, crew requested", job.m_sFaction, entry.m_sType, pos), LogLevel.NORMAL);
		return true;
	}

	// ScriptInvoker callback of the compartment manager: (manager, spawned characters, wasCanceled). Only records the result;
	// the job advances in Tick.
	protected void OnOccupantsDone(SCR_BaseCompartmentManagerComponent manager, array<IEntity> occupants, bool wasCanceled)
	{
		if (!manager)
			return;

		manager.GetOnDoneSpawningDefaultOccupants().Remove(OnOccupantsDone);
		IEntity owner = manager.GetOwner();
		foreach (CLW_SpawnJob job : m_aJobs)
		{
			if (job.m_Vehicle != owner)
				continue;

			job.m_bCrewDone = true;
			job.m_bCrewCanceled = wasCanceled;
			int count;
			if (occupants)
			{
				job.m_aCrew = {};
				job.m_aCrew.Copy(occupants);
				count = occupants.Count();
			}

			Print(string.Format("CLW: spawn %1 crew callback: occupants=%2 canceled=%3", job.m_sFaction, count, wasCanceled), LogLevel.NORMAL);
			return;
		}

		Print("CLW: spawn crew callback for a vehicle without job (job already failed?)", LogLevel.WARNING);
	}

	protected SCR_AIGroup GroupOfCharacter(IEntity character)
	{
		if (!character)
			return null;

		AIControlComponent control = AIControlComponent.Cast(character.FindComponent(AIControlComponent));
		if (!control)
			return null;

		AIAgent agent = control.GetControlAIAgent();
		if (!agent)
			return null;

		return SCR_AIGroup.Cast(agent.GetParentGroup());
	}

	// Default occupants of a pack vehicle can be vanilla US/USSR crew: move the crew group, its characters and the vehicle to the
	// buying faction so the registry adopts the group and the AI treats the crew as friendly.
	protected void ReaffiliateCrew(CLW_SpawnJob job, SCR_AIGroup crew)
	{
		FactionManager factions = GetGame().GetFactionManager();
		if (!factions)
			return;

		Faction target = factions.GetFactionByKey(job.m_sFaction);
		if (!target)
			return;

		string oldKey = CLW_SquadRegistry.GetFactionKey(crew);
		if (oldKey == job.m_sFaction)
			return;

		if (!crew.SetFaction(target))
			Print(string.Format("CLW: spawn %1 crew group SetFaction refused (group faction stays %2)", job.m_sUnitId, oldKey), LogLevel.WARNING);

		int changed;
		foreach (IEntity member : job.m_aCrew)
		{
			if (!member)
				continue;

			FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(member.FindComponent(FactionAffiliationComponent));
			if (!affiliation)
				continue;

			affiliation.SetAffiliatedFaction(target);
			changed++;
		}

		if (job.m_Vehicle)
		{
			FactionAffiliationComponent vehicleAffiliation = FactionAffiliationComponent.Cast(job.m_Vehicle.FindComponent(FactionAffiliationComponent));
			if (vehicleAffiliation)
				vehicleAffiliation.SetAffiliatedFaction(target);
		}

		Print(string.Format("CLW: spawn %1 crew re-affiliated %2 -> %3 (%4 characters, group now %5)", job.m_sUnitId, oldKey, job.m_sFaction, changed, CLW_SquadRegistry.GetFactionKey(crew)), LogLevel.NORMAL);
	}

	protected void TickCrew(CLW_SpawnJob job)
	{
		if (!job.m_Vehicle)
		{
			Fail(job, "vehicle vanished while spawning the crew");
			return;
		}

		if (!job.m_bCrewDone)
		{
			if (job.m_fStateAge >= CLW_WarConfig.SPAWN_CREW_TIMEOUT_SEC)
				Fail(job, "crew spawn timeout");

			return;
		}

		if (job.m_bCrewCanceled || !job.m_aCrew || job.m_aCrew.IsEmpty())
		{
			Fail(job, "crew spawn canceled or empty");
			return;
		}

		SCR_AIGroup crew;
		foreach (IEntity occupant : job.m_aCrew)
		{
			crew = GroupOfCharacter(occupant);
			if (crew)
				break;
		}

		if (!crew)
		{
			Fail(job, "crew group not found");
			return;
		}

		CLW_ForceEntry entry = job.m_Entry;
		job.m_Group = crew;
		crew.SetDeleteWhenEmpty(true);
		crew.SetImportance(SCR_EAISpawnImportance.CRITICAL);
		ReaffiliateCrew(job, crew);
		job.m_sUnitId = NameAndTag(crew, NextName(job.m_sFaction, entry.m_sKind), entry.m_sKind, entry.m_sName, entry.m_sType, false);
		Print(string.Format("CLW: spawn %1 crew group %2 has %3 agents (%4 occupants)", job.m_sFaction, job.m_sUnitId, crew.GetAgentsCount(), job.m_aCrew.Count()), LogLevel.NORMAL);

		AddUsable(crew, job.m_Vehicle);
		if (entry.m_CargoGroupPrefab.IsEmpty())
		{
			Finish(job, "crewed vehicle");
			return;
		}

		// beside the vehicle, on free navmesh ground (not on the road lane in front of it)
		vector offset;
		string cargoVia;
		CLW_Placement.FindGround(job.m_Vehicle.GetOrigin(), CLW_WarConfig.PLACE_CARGO_MIN_M, CLW_WarConfig.PLACE_CARGO_MAX_M, 0.5, 2, CLW_WarConfig.PLACE_CHAR_CLEAR_M, offset, cargoVia);
		Print(string.Format("CLW: spawn place cargo at %1 yaw=0 via %2", offset, cargoVia), LogLevel.NORMAL);
		SCR_AIGroup cargo = SpawnGroup(entry.m_CargoGroupPrefab, offset);
		if (!cargo)
		{
			Print(string.Format("CLW: spawn %1 embarked group failed to spawn, vehicle goes alone", job.m_sUnitId), LogLevel.ERROR);
			Finish(job, "crewed vehicle (cargo failed)");
			return;
		}

		job.m_Cargo = cargo;
		job.m_sCargoId = NameAndTag(cargo, job.m_sUnitId + "_Inf", CLW_ForceKind.INFANTRY, entry.m_sName + " (infantry)", entry.m_sType, true);
		job.m_eState = CLW_ESpawnState.CARGO_WAIT;
		job.m_fStateAge = 0;
		Print(string.Format("CLW: spawn %1 embarked group %2 spawned, waiting for %3 members", job.m_sUnitId, job.m_sCargoId, entry.m_iCargoMembers), LogLevel.NORMAL);
	}

	protected void TickCargo(CLW_SpawnJob job)
	{
		if (!job.m_Vehicle || !job.m_Group)
		{
			Fail(job, "vehicle or crew vanished while the embarked group spawned");
			return;
		}

		if (!job.m_Cargo)
		{
			Print(string.Format("CLW: spawn %1 embarked group vanished, vehicle goes alone", job.m_sUnitId), LogLevel.WARNING);
			Finish(job, "crewed vehicle (cargo vanished)");
			return;
		}

		int members = job.m_Cargo.GetAgentsCount();
		bool complete = members >= job.m_Entry.m_iCargoMembers;
		if (!complete && members <= 0 && job.m_fStateAge >= CLW_WarConfig.SPAWN_MEMBERS_TIMEOUT_SEC && RetryMembers(job, job.m_Cargo, job.m_Entry.m_iCargoMembers))
			return;

		if (!complete && job.m_fStateAge >= CLW_WarConfig.SPAWN_MEMBERS_TIMEOUT_SEC)
		{
			Print(string.Format("CLW: spawn %1 embarked members timeout: %2/%3 present, continuing", job.m_sUnitId, members, job.m_Entry.m_iCargoMembers), LogLevel.WARNING);
			complete = true;
		}

		if (!complete)
			return;

		if (members <= 0)
		{
			Print(string.Format("CLW: spawn %1 embarked group has no members, vehicle goes alone", job.m_sUnitId), LogLevel.WARNING);
			Finish(job, "crewed vehicle (empty cargo)");
			return;
		}

		// put the members into the real cargo seats; the ones that do not fit stay on foot with the group
		array<IEntity> people = CLW_SquadInfo.Members(job.m_Cargo);
		int seated;
		int refused;
		foreach (IEntity person : people)
		{
			if (!person || !CLW_VehicleUnit.IsAlive(person))
				continue;

			SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(person.FindComponent(SCR_CompartmentAccessComponent));
			if (!access)
				continue;

			if (access.MoveInVehicle(job.m_Vehicle, ECompartmentType.CARGO, false))
				seated++;
			else
				refused++;
		}

		AddUsable(job.m_Cargo, job.m_Vehicle);
		Print(string.Format("CLW: spawn %1 embarking: %2 seated, %3 without a free cargo seat (group has %4)", job.m_sUnitId, seated, refused, members), LogLevel.NORMAL);
		job.m_eState = CLW_ESpawnState.VERIFY;
		job.m_fStateAge = 0;
	}

	// Cargo slots of the vehicle that have no occupant (and are not taken by an entity already reserved for them).
	protected void FreeCargoSlots(IEntity vehicle, out array<BaseCompartmentSlot> slots)
	{
		slots = {};
		BaseCompartmentManagerComponent manager = BaseCompartmentManagerComponent.Cast(vehicle.FindComponent(BaseCompartmentManagerComponent));
		if (!manager)
			return;

		array<BaseCompartmentSlot> all = {};
		manager.GetCompartments(all);
		foreach (BaseCompartmentSlot slot : all)
		{
			if (!CargoCompartmentSlot.Cast(slot))
				continue;

			if (slot.GetOccupant())
				continue;

			slots.Insert(slot);
		}
	}

	// First pass: whoever is not inside gets an explicit free cargo slot (teleport). Second pass: final count; the ones still
	// outside get a Get-In waypoint and are treated as walking (the vehicle unit's remount logic).
	protected void TickVerify(CLW_SpawnJob job)
	{
		array<IEntity> outside = {};
		int inside;
		if (job.m_Cargo && job.m_Vehicle)
		{
			array<IEntity> people = CLW_SquadInfo.Members(job.m_Cargo);
			foreach (IEntity person : people)
			{
				if (!person || !CLW_VehicleUnit.IsAlive(person))
					continue;

				if (CLW_VehicleUnit.IsInVehicle(person, job.m_Vehicle))
					inside++;
				else
					outside.Insert(person);
			}
		}

		if (!outside.IsEmpty() && job.m_iVerifyPass == 0)
		{
			array<BaseCompartmentSlot> slots;
			FreeCargoSlots(job.m_Vehicle, slots);
			int tried;
			foreach (IEntity stray : outside)
			{
				if (slots.IsEmpty())
					break;

				SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(stray.FindComponent(SCR_CompartmentAccessComponent));
				if (!access)
					continue;

				BaseCompartmentSlot slot = slots[0];
				slots.Remove(0);
				access.GetInVehicle(job.m_Vehicle, slot, true, -1, ECloseDoorAfterActions.INVALID, false);
				tried++;
			}

			job.m_iVerifyPass = 1;
			job.m_iRetried = tried;
			job.m_fStateAge = 0;
			Print(string.Format("CLW: spawn %1 verify: %2 seated, %3 outside -> retry with explicit cargo slots for %4", job.m_sUnitId, inside, outside.Count(), tried), LogLevel.NORMAL);
			return;
		}

		if (!outside.IsEmpty() && job.m_Cargo && job.m_Vehicle)
		{
			CLW_Waypoints.AddGetIn(job.m_Cargo, job.m_Vehicle);
			Print(string.Format("CLW: spawn %1 verify: %2 still outside after retry, cargo group gets a Get-In waypoint", job.m_sUnitId, outside.Count()), LogLevel.WARNING);
		}

		job.m_iMounted = inside;
		Finish(job, string.Format("vehicle with %1 embarked", inside));
	}

	// The vehicle becomes a usable vehicle of the group (so Get-In/Get-Out waypoints and AI driving work).
	protected void AddUsable(SCR_AIGroup group, IEntity vehicle)
	{
		CLW_Waypoints.AddUsableVehicle(group, vehicle);
	}

	// ---- tick / completion ----

	protected void Tick()
	{
		float dt = CLW_WarConfig.SPAWNER_TICK_MS / 1000.0;
		for (int i = m_aJobs.Count() - 1; i >= 0; i--)
		{
			CLW_SpawnJob job = m_aJobs[i];
			job.m_fAge += dt;
			job.m_fStateAge += dt;
			if (job.m_eState == CLW_ESpawnState.DONE || job.m_eState == CLW_ESpawnState.FAILED)
			{
				m_aJobs.Remove(i);
				continue;
			}

			if (job.m_fAge >= CLW_WarConfig.SPAWN_JOB_TIMEOUT_SEC)
			{
				Fail(job, "overall timeout");
				continue;
			}

			CLW_ESpawnState before = job.m_eState;
			switch (job.m_eState)
			{
				case CLW_ESpawnState.GROUP_WAIT: TickInfantry(job); break;
				case CLW_ESpawnState.CREW_WAIT: TickCrew(job); break;
				case CLW_ESpawnState.CARGO_WAIT: TickCargo(job); break;
				case CLW_ESpawnState.VERIFY: TickVerify(job); break;
			}

			if (job.m_eState != before)
				job.m_fStateAge = 0;
		}
	}

	protected void Finish(CLW_SpawnJob job, string detail)
	{
		job.m_eState = CLW_ESpawnState.DONE;
		CLW_ForceEntry entry = job.m_Entry;
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (entry.HasVehicle() && m_Vehicles && job.m_Vehicle && job.m_Group)
			m_Vehicles.Link(job.m_sUnitId, job.m_sFaction, entry, job.m_Vehicle, job.m_Group, job.m_Cargo, job.m_sCargoId);

		Print(string.Format("CLW: spawn %1 %2 (%3) READY as %4: %5", job.m_sFaction, entry.m_sType, entry.m_sName, job.m_sUnitId, detail), LogLevel.NORMAL);
		if (war)
			war.PushEvent(job.m_sFaction, job.m_sUnitId + " arrived: " + entry.m_sName);

		if (!job.m_sTag.IsEmpty() && m_OnTagged)
			m_OnTagged.Invoke(job.m_sTag, job.m_sUnitId);

		if (job.m_iRequester >= 0 && job.m_Group)
			CLW_RequestService.OnSquadReady(job.m_iRequester, job.m_Group, job.m_sUnitId, entry.m_sName);
	}

	// Clean up everything the job spawned, refund, tell the war.
	protected void Fail(CLW_SpawnJob job, string why)
	{
		if (job.m_eState == CLW_ESpawnState.FAILED)
			return;

		job.m_eState = CLW_ESpawnState.FAILED;
		job.m_sFailure = why;
		Print(string.Format("CLW: spawn %1 %2 FAILED: %3 -> cleaning up%4", job.m_sFaction, job.m_Entry.m_sType, why, RefundNote(job)), LogLevel.ERROR);
		DeleteGroupAndMembers(job.m_Cargo);
		DeleteGroupAndMembers(job.m_Group);
		if (job.m_Vehicle)
			SCR_EntityHelper.DeleteEntityAndChildren(job.m_Vehicle);

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
		{
			if (job.m_bCharged)
				war.Refund(job.m_sFaction, job.m_Entry.m_iCost);

			war.PushEvent(job.m_sFaction, job.m_sFaction + " purchase failed: " + job.m_Entry.m_sName);
		}

		if (!job.m_sTag.IsEmpty() && m_OnTagged)
			m_OnTagged.Invoke(job.m_sTag, "");
	}

	protected string RefundNote(CLW_SpawnJob job)
	{
		if (job.m_bCharged)
			return ", refunding " + job.m_Entry.m_iCost;

		return "";
	}

	static void DeleteGroupAndMembers(SCR_AIGroup group)
	{
		if (!group)
			return;

		array<IEntity> members = CLW_SquadInfo.Members(group);
		foreach (IEntity member : members)
		{
			if (member)
				SCR_EntityHelper.DeleteEntityAndChildren(member);
		}

		SCR_EntityHelper.DeleteEntityAndChildren(group);
	}
}

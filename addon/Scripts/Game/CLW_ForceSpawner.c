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
	int m_iMounted;
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

	// Same without charging supplies (start forces). Caps still apply.
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

	protected void StartForceLater(string factionKey, string typeId)
	{
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
		if (cap > 0 && live + expected > cap)
		{
			error = string.Format("AI cap (%1 live + %2 new > %3)", live, expected, cap);
			Print(string.Format("CLW: spawn %1 %2 %3 refused: %4", label, factionKey, typeId, error), LogLevel.NORMAL);
			return false;
		}

		if (source.HasVehicle())
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
		float cylinderR = 0.5;
		float cylinderH = 2;
		if (source.HasVehicle())
		{
			cylinderR = CLW_WarConfig.SPAWN_VEHICLE_CYLINDER_R;
			cylinderH = CLW_WarConfig.SPAWN_VEHICLE_CYLINDER_H;
		}
		if (!bases || !bases.FindSpawnSpot(factionKey, CLW_WarConfig.SPAWN_RADIUS_M, spot, cylinderR, cylinderH))
		{
			error = "no spawn spot at the base";
			Print(string.Format("CLW: spawn %1 %2 %3 refused: %4", label, factionKey, typeId, error), LogLevel.ERROR);
			return false;
		}

		CLW_SpawnJob job = new CLW_SpawnJob();
		job.m_sFaction = factionKey;
		job.m_Entry = source.CloneEntry();
		job.m_sReason = reason;
		job.m_bFree = free;
		job.m_vPos = spot;
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
		return group;
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
			if (job.m_fStateAge >= CLW_WarConfig.SPAWN_MEMBERS_TIMEOUT_SEC)
				Fail(job, "no members arrived");

			return;
		}

		Finish(job, string.Format("%1 members", members));
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
		job.m_sUnitId = NameAndTag(crew, NextName(job.m_sFaction, entry.m_sKind), entry.m_sKind, entry.m_sName, entry.m_sType, false);
		Print(string.Format("CLW: spawn %1 crew group %2 has %3 agents (%4 occupants)", job.m_sFaction, job.m_sUnitId, crew.GetAgentsCount(), job.m_aCrew.Count()), LogLevel.NORMAL);

		AddUsable(crew, job.m_Vehicle);
		if (entry.m_CargoGroupPrefab.IsEmpty())
		{
			Finish(job, "crewed vehicle");
			return;
		}

		vector offset = job.m_Vehicle.GetOrigin() + Vector(CLW_WarConfig.SPAWN_CARGO_OFFSET_M, 0, 0);
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

	protected void TickVerify(CLW_SpawnJob job)
	{
		int inside;
		if (job.m_Cargo && job.m_Vehicle)
		{
			array<IEntity> people = CLW_SquadInfo.Members(job.m_Cargo);
			foreach (IEntity person : people)
			{
				if (person && CLW_VehicleUnit.IsInVehicle(person, job.m_Vehicle))
					inside++;
			}
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

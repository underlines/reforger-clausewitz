// Real seats of a vehicle prefab, read from a short-lived probe spawn (spawned and deleted in the same call).
class CLW_VehicleProbe
{
	bool m_bOk;
	int m_iCargo;
	int m_iPilot;
	int m_iTurret;
	int m_iCrew;		// pilot + turret seats that have a default occupant
}

// Per faction force catalog (what can be bought). Built from the faction profile (CLW_FactionProfile) and, when the
// profile asks for it or there is none, from auto-discovery over the faction's SCR_EntityCatalog (VEHICLE / GROUP) with
// a generic role classifier (spec 0a, research/entity-catalog-classification.md). No faction, prefab or vehicle name is
// known to this code. Build() may be called at any time for any faction key (war start, setup menu choice).
class CLW_ForceCatalog
{
	protected ref map<string, ref array<ref CLW_ForceEntry>> m_mEntries = new map<string, ref array<ref CLW_ForceEntry>>();
	protected ref map<string, ref CLW_FactionProfile> m_mProfiles = new map<string, ref CLW_FactionProfile>();	// by resource path
	protected ref map<string, ref CLW_VehicleProbe> m_mProbes = new map<string, ref CLW_VehicleProbe>();		// by prefab path
	protected int m_iAutoCounter;
	protected int m_iProbeSerial;
	static const int PROBE_LIFETIME_MS = 3000;	// probe vehicles live this long so their delayed inits ran before deletion

	// ---- queries ----

	bool Has(string factionKey)
	{
		return m_mEntries.Contains(factionKey);
	}

	// Enabled entries of the faction (builds the catalog on first use).
	void GetEntries(string factionKey, notnull array<CLW_ForceEntry> entries)
	{
		array<ref CLW_ForceEntry> list = GetList(factionKey);
		foreach (CLW_ForceEntry entry : list)
		{
			if (entry.m_bEnabled)
				entries.Insert(entry);
		}
	}

	CLW_ForceEntry Find(string factionKey, string typeId)
	{
		array<ref CLW_ForceEntry> list = GetList(factionKey);
		foreach (CLW_ForceEntry entry : list)
		{
			if (entry.m_bEnabled && entry.m_sType == typeId)
				return entry;
		}

		return null;
	}

	// The infantry entry (no vehicle) whose group prefab is this one (GUID prefix ignored). Null when unknown or not built.
	CLW_ForceEntry FindByGroupPrefab(string factionKey, ResourceName groupPrefab)
	{
		if (groupPrefab.IsEmpty() || !m_mEntries.Contains(factionKey))
			return null;

		string wanted = PathOf(groupPrefab);
		array<ref CLW_ForceEntry> list = m_mEntries.Get(factionKey);
		foreach (CLW_ForceEntry entry : list)
		{
			if (!entry.HasVehicle() && !entry.m_GroupPrefab.IsEmpty() && PathOf(entry.m_GroupPrefab) == wanted)
				return entry;
		}

		return null;
	}

	string GetKind(string factionKey, string typeId)
	{
		CLW_ForceEntry entry = Find(factionKey, typeId);
		if (entry)
			return entry.m_sKind;

		return "";
	}

	int GetCost(string factionKey, string typeId)
	{
		CLW_ForceEntry entry = Find(factionKey, typeId);
		if (entry)
			return entry.m_iCost;

		return -1;
	}

	string GetDescription(string factionKey, string typeId)
	{
		CLW_ForceEntry entry = Find(factionKey, typeId);
		if (entry)
			return entry.Describe();

		return "";
	}

	// ---- profile lookup ----

	// Profile of a faction key: the scenario faction setup's own profile if it is for this key (or names none), else the
	// scenario's profile list entry with this key. Null = none (pure discovery).
	CLW_FactionProfile FindProfile(string factionKey)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario)
			return null;

		CLW_FactionSetup setup = scenario.FindFaction(factionKey);
		if (setup && !setup.m_ProfileConfig.IsEmpty())
		{
			CLW_FactionProfile own = LoadProfile(setup.m_ProfileConfig);
			if (own && (own.m_sFactionKey.IsEmpty() || own.m_sFactionKey == factionKey))
				return own;
		}

		if (scenario.m_aProfileConfigs)
		{
			foreach (ResourceName path : scenario.m_aProfileConfigs)
			{
				CLW_FactionProfile candidate = LoadProfile(path);
				if (candidate && candidate.m_sFactionKey == factionKey)
					return candidate;
			}
		}

		return null;
	}

	protected CLW_FactionProfile LoadProfile(ResourceName path)
	{
		if (path.IsEmpty())
			return null;

		string cacheKey = path;
		CLW_FactionProfile cached;
		if (m_mProfiles.Find(cacheKey, cached))
			return cached;

		CLW_FactionProfile loaded = CLW_FactionProfile.Load(path);
		if (loaded)
			m_mProfiles.Set(cacheKey, loaded);

		return loaded;
	}

	// ---- building ----

	// Build (or rebuild) the catalogs of every scenario faction.
	void BuildAll()
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario)
		{
			Print("CLW: catalog BuildAll without a scenario", LogLevel.ERROR);
			return;
		}

		array<string> keys = {};
		scenario.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			if (m_mEntries.Contains(key))
			{
				Print("CLW: catalog " + key + " already built, not rebuilding", LogLevel.NORMAL);
				continue;
			}

			Build(key);
		}
	}

	protected array<ref CLW_ForceEntry> GetList(string factionKey)
	{
		array<ref CLW_ForceEntry> list;
		if (m_mEntries.Find(factionKey, list) && list)
			return list;

		Build(factionKey);
		if (m_mEntries.Find(factionKey, list) && list)
			return list;

		return new array<ref CLW_ForceEntry>();
	}

	// (Re)build one faction's catalog. Returns the number of enabled entries.
	int Build(string factionKey)
	{
		array<ref CLW_ForceEntry> entries = {};
		array<string> signatures = {};
		CLW_FactionProfile profile = FindProfile(factionKey);
		int fromProfile;
		if (profile)
		{
			foreach (CLW_ForceEntry source : profile.m_aEntries)
			{
				CLW_ForceEntry accepted = AcceptProfileEntry(factionKey, source, entries);
				if (!accepted)
					continue;

				entries.Insert(accepted);
				signatures.Insert(Signature(accepted));
				fromProfile++;
			}
		}
		else
		{
			Print("CLW: catalog " + factionKey + " has no profile, using auto-discovery only", LogLevel.NORMAL);
		}

		int discovered;
		if (!profile || profile.m_bIncludeDiscovered)
			discovered = Discover(factionKey, entries, signatures);

		m_mEntries.Set(factionKey, entries);
		Print(string.Format("CLW: catalog %1 built: %2 entries (%3 from profile, %4 discovered)", factionKey, entries.Count(), fromProfile, discovered), LogLevel.NORMAL);
		foreach (CLW_ForceEntry entry : entries)
		{
			LogEntry(factionKey, entry);
		}

		return entries.Count();
	}

	protected void LogEntry(string factionKey, CLW_ForceEntry entry)
	{
		Print(string.Format("CLW: catalog %1 [%2] type=%3 kind=%4 cost=%5 weight=%6 enabled=%7 name='%8'", factionKey, entry.m_sSource, entry.m_sType, entry.m_sKind, entry.m_iCost, entry.m_iWeight, entry.m_bEnabled, entry.m_sName), LogLevel.NORMAL);
		Print(string.Format("CLW: catalog %1   %2 vehicle=%3 cargoGroup=%4 group=%5 | crew=%6 cargoSeats=%7 armed=%8 members=%9", factionKey, entry.m_sType, entry.m_VehiclePrefab, entry.m_CargoGroupPrefab, entry.m_GroupPrefab, entry.m_iCrewSeats, entry.m_iCargoSeats, entry.m_bArmed, entry.m_iGroupMembers + entry.m_iCargoMembers), LogLevel.NORMAL);
	}

	// Validate a profile entry (data errors are logged and the entry skipped), probe its prefabs, return a runtime copy.
	protected CLW_ForceEntry AcceptProfileEntry(string factionKey, CLW_ForceEntry source, notnull array<ref CLW_ForceEntry> existing)
	{
		if (!source || source.m_sType.IsEmpty())
		{
			Print("CLW: catalog " + factionKey + " profile entry without type id skipped", LogLevel.ERROR);
			return null;
		}

		if (!source.m_bEnabled)
		{
			Print("CLW: catalog " + factionKey + " profile entry " + source.m_sType + " disabled", LogLevel.NORMAL);
			return null;
		}

		foreach (CLW_ForceEntry other : existing)
		{
			if (other.m_sType == source.m_sType)
			{
				Print("CLW: catalog " + factionKey + " duplicate type id " + source.m_sType + " skipped", LogLevel.ERROR);
				return null;
			}
		}

		if (!CLW_ForceKind.IsKnown(source.m_sKind))
		{
			Print("CLW: catalog " + factionKey + " entry " + source.m_sType + " unknown kind '" + source.m_sKind + "' skipped", LogLevel.ERROR);
			return null;
		}

		bool vehicleKind = CLW_ForceKind.IsVehicleKind(source.m_sKind);
		if (vehicleKind && source.m_VehiclePrefab.IsEmpty())
		{
			Print("CLW: catalog " + factionKey + " entry " + source.m_sType + " kind " + source.m_sKind + " needs a vehicle prefab, skipped", LogLevel.ERROR);
			return null;
		}

		if (!vehicleKind && source.m_GroupPrefab.IsEmpty())
		{
			Print("CLW: catalog " + factionKey + " entry " + source.m_sType + " needs a group prefab, skipped", LogLevel.ERROR);
			return null;
		}

		if (!vehicleKind && !source.m_VehiclePrefab.IsEmpty())
			Print("CLW: catalog " + factionKey + " entry " + source.m_sType + " has a vehicle but kind " + source.m_sKind + ": the vehicle is used (treated as a vehicle unit)", LogLevel.WARNING);

		if (!PrefabLoads(source.m_VehiclePrefab) || !PrefabLoads(source.m_GroupPrefab) || !PrefabLoads(source.m_CargoGroupPrefab))
		{
			Print("CLW: catalog " + factionKey + " entry " + source.m_sType + " has a prefab that does not load, skipped", LogLevel.ERROR);
			return null;
		}

		CLW_ForceEntry entry = source.CloneEntry();
		entry.m_sSource = "profile";
		if (entry.m_sName.IsEmpty())
			entry.m_sName = entry.m_sType;
		ProbeEntry(factionKey, entry);
		return entry;
	}

	protected bool PrefabLoads(ResourceName prefab)
	{
		if (prefab.IsEmpty())
			return true;

		Resource resource = Resource.Load(prefab);
		return resource && resource.IsValid();
	}

	// Fill the runtime facts: vehicle seats (probe spawn) and group sizes.
	protected void ProbeEntry(string factionKey, CLW_ForceEntry entry)
	{
		if (entry.HasVehicle())
		{
			CLW_VehicleProbe probe = ProbeVehicle(factionKey, entry.m_VehiclePrefab);
			if (probe && probe.m_bOk)
			{
				entry.m_iCargoSeats = probe.m_iCargo;
				entry.m_iCrewSeats = probe.m_iCrew;
				entry.m_bArmed = probe.m_iTurret > 0;
			}
		}

		if (!entry.m_GroupPrefab.IsEmpty())
			entry.m_iGroupMembers = CountGroupMembers(entry.m_GroupPrefab);

		if (!entry.m_CargoGroupPrefab.IsEmpty())
		{
			entry.m_iCargoMembers = CountGroupMembers(entry.m_CargoGroupPrefab);
			if (entry.m_iCargoSeats >= 0 && entry.m_iCargoMembers > entry.m_iCargoSeats)
				Print(string.Format("CLW: catalog %1 entry %2: embarked group (%3) is larger than the vehicle's %4 cargo seats, the rest will walk", factionKey, entry.m_sType, entry.m_iCargoMembers, entry.m_iCargoSeats), LogLevel.WARNING);
		}

		entry.m_bChecked = true;
	}

	protected string Signature(CLW_ForceEntry entry)
	{
		if (!entry.HasVehicle())
			return "g|" + entry.m_sKind + "|" + entry.m_iGroupMembers;

		string armed = "0";
		if (entry.m_bArmed)
			armed = "1";

		return "v|" + entry.m_sKind + "|" + entry.m_iCargoSeats + "|" + armed;
	}

	static string PathOf(ResourceName name)
	{
		string text = name;
		int close = text.IndexOf("}");
		if (close >= 0)
			text = text.Substring(close + 1, text.Length() - close - 1);

		text.ToLower();
		return text;
	}

	protected string BaseName(ResourceName name)
	{
		string text = PathOf(name);
		int slash = -1;
		for (int i = 0; i < text.Length(); i++)
		{
			if (text.Get(i) == "/")
				slash = i;
		}

		text = text.Substring(slash + 1, text.Length() - slash - 1);
		int dot = text.IndexOf(".");
		if (dot > 0)
			text = text.Substring(0, dot);

		return text;
	}

	// ---- probing ----

	// Seats of a vehicle prefab: spawn, read the compartment manager, delete. Cached per prefab.
	protected CLW_VehicleProbe ProbeVehicle(string factionKey, ResourceName prefab)
	{
		string cacheKey = PathOf(prefab);
		CLW_VehicleProbe cached;
		if (m_mProbes.Find(cacheKey, cached))
			return cached;

		CLW_VehicleProbe probe = new CLW_VehicleProbe();
		m_mProbes.Set(cacheKey, probe);

		// no spawn needed when the editable-entity UI info carries the default crew / passenger counts
		if (ReadSeatsFromInfo(prefab, probe))
		{
			Print(string.Format("CLW: catalog probe %1: from UI info (no spawn) pilot+turret crew=%2 cargo=%3 armed=%4", prefab, probe.m_iCrew, probe.m_iCargo, probe.m_iTurret > 0), LogLevel.NORMAL);
			return probe;
		}

		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
		{
			Print("CLW: catalog probe: prefab does not load: " + prefab, LogLevel.WARNING);
			return probe;
		}

		// far above the base, inactive, and kept alive until its delayed inits ran (see DeleteProbeVehicle)
		m_iProbeSerial++;
		vector pos = Vector(m_iProbeSerial * 25, 600, 0);
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (bases)
		{
			pos = bases.GetBasePos(factionKey);
			pos[0] = pos[0] + m_iProbeSerial * 25;
			pos[1] = pos[1] + 600;
		}

		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = pos;
		IEntity vehicle = GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params);
		if (!vehicle)
		{
			Print("CLW: catalog probe: spawn failed for " + prefab, LogLevel.WARNING);
			return probe;
		}

		BaseCompartmentManagerComponent compartments = BaseCompartmentManagerComponent.Cast(vehicle.FindComponent(BaseCompartmentManagerComponent));
		if (compartments)
		{
			array<BaseCompartmentSlot> slots = {};
			compartments.GetCompartments(slots);
			foreach (BaseCompartmentSlot slot : slots)
			{
				if (!slot)
					continue;

				ECompartmentType type = slot.GetType();
				if (type == ECompartmentType.CARGO)
				{
					probe.m_iCargo++;
				}
				else if (type == ECompartmentType.PILOT || type == ECompartmentType.TURRET)
				{
					if (type == ECompartmentType.PILOT)
						probe.m_iPilot++;
					else
						probe.m_iTurret++;

					if (!slot.GetDefaultOccupantPrefab().IsEmpty())
						probe.m_iCrew++;
				}
			}

			probe.m_bOk = true;
		}
		else
		{
			Print("CLW: catalog probe: no compartment manager on " + prefab, LogLevel.WARNING);
		}

		Physics physics = vehicle.GetPhysics();
		if (physics)
			physics.SetActive(ActiveState.INACTIVE);

		GetGame().GetCallqueue().CallLater(DeleteProbeVehicle, PROBE_LIFETIME_MS, false, vehicle);
		Print(string.Format("CLW: catalog probe %1: ok=%2 pilot=%3 turret=%4 cargo=%5 crewWithOccupant=%6 (spawn deleted in %7 ms)", prefab, probe.m_bOk, probe.m_iPilot, probe.m_iTurret, probe.m_iCargo, probe.m_iCrew, PROBE_LIFETIME_MS), LogLevel.NORMAL);
		return probe;
	}

	protected void DeleteProbeVehicle(IEntity vehicle)
	{
		if (vehicle)
			SCR_EntityHelper.DeleteEntityAndChildren(vehicle);
	}

	// Default crew / passenger counts from the prefab's editable UI info (AI budget of the fill costs). False when the data is
	// missing, then the caller spawns a probe.
	protected bool ReadSeatsFromInfo(ResourceName prefab, CLW_VehicleProbe probe)
	{
		SCR_EditableVehicleUIInfo info = SCR_EditableVehicleUIInfo.Cast(SCR_EditableEntityUIInfo.ExtractEditableUIInfoFromPrefab(prefab));
		if (!info)
			return false;

		array<ref SCR_EntityBudgetValue> crewBudgets = {};
		array<ref SCR_EntityBudgetValue> passengerBudgets = {};
		info.GetFillBudgetCostsOfCrew(crewBudgets);
		info.GetFillBudgetCostsOfPassengers(passengerBudgets);
		int crew = -1;
		int passengers;
		foreach (SCR_EntityBudgetValue crewBudget : crewBudgets)
		{
			if (crewBudget && crewBudget.GetBudgetType() == EEditableEntityBudget.AI)
				crew = crewBudget.GetBudgetValue();
		}

		foreach (SCR_EntityBudgetValue passengerBudget : passengerBudgets)
		{
			if (passengerBudget && passengerBudget.GetBudgetType() == EEditableEntityBudget.AI)
				passengers = passengerBudget.GetBudgetValue();
		}

		if (crew <= 0)
			return false;

		probe.m_iCrew = crew;
		probe.m_iCargo = passengers;
		probe.m_iPilot = 1;
		if (info.HasEntityLabel(EEditableEntityLabel.TRAIT_ARMED))
			probe.m_iTurret = 1;

		probe.m_bOk = true;
		return true;
	}

	// Member character prefabs of a group prefab (SCR_AIGroup.m_aUnitPrefabSlots). False when unreadable.
	protected bool GetGroupSlots(ResourceName prefab, notnull array<ResourceName> slots)
	{
		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
			return false;

		BaseResourceObject object = resource.GetResource();
		if (!object)
			return false;

		BaseContainer container = object.ToBaseContainer();	// [verify] merged-with-ancestors view of the prefab
		if (!container)
			return false;

		array<ResourceName> raw = {};
		if (!container.Get("m_aUnitPrefabSlots", raw))	// [verify] BaseContainer.Get into an array<ResourceName>
			return false;

		foreach (ResourceName slot : raw)
		{
			if (!slot.IsEmpty())
				slots.Insert(slot);
		}

		return !slots.IsEmpty();
	}

	// Group size: slots when readable, else the GROUPSIZE label, else a default of 4.
	protected int CountGroupMembers(ResourceName prefab)
	{
		array<ResourceName> slots = {};
		if (GetGroupSlots(prefab, slots))
			return slots.Count();

		array<EEditableEntityLabel> labels = {};
		GetLabels(prefab, labels);
		if (labels.Contains(EEditableEntityLabel.GROUPSIZE_LARGE))
			return 8;
		if (labels.Contains(EEditableEntityLabel.GROUPSIZE_MEDIUM))
			return 5;
		if (labels.Contains(EEditableEntityLabel.GROUPSIZE_SMALL))
			return 3;

		Print("CLW: catalog could not read the size of group " + prefab + ", assuming 4", LogLevel.WARNING);
		return 4;
	}

	protected void GetLabels(ResourceName prefab, notnull array<EEditableEntityLabel> labels)
	{
		if (prefab.IsEmpty())
			return;

		SCR_EditableEntityUIInfo info = SCR_EditableEntityUIInfo.ExtractEditableUIInfoFromPrefab(prefab);
		if (info)
			info.GetEntityLabels(labels);
	}

	// ---- auto-discovery ----

	// Add catalog-derived entries the list does not cover. Returns how many were added.
	protected int Discover(string factionKey, notnull array<ref CLW_ForceEntry> entries, notnull array<string> signatures)
	{
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!factionManager)
			return 0;

		SCR_Faction faction = SCR_Faction.Cast(factionManager.GetFactionByKey(factionKey));
		if (!faction)
		{
			Print("CLW: catalog discovery: no SCR_Faction " + factionKey, LogLevel.WARNING);
			return 0;
		}

		int added = DiscoverGroups(factionKey, faction, entries, signatures);
		added += DiscoverVehicles(factionKey, faction, entries, signatures);
		return added;
	}

	protected bool IsCovered(notnull array<ref CLW_ForceEntry> entries, ResourceName prefab, bool vehicle)
	{
		string wanted = PathOf(prefab);
		foreach (CLW_ForceEntry entry : entries)
		{
			if (vehicle && entry.HasVehicle() && PathOf(entry.m_VehiclePrefab) == wanted)
				return true;

			if (!vehicle && !entry.HasVehicle() && !entry.m_GroupPrefab.IsEmpty() && PathOf(entry.m_GroupPrefab) == wanted)
				return true;
		}

		return false;
	}

	protected bool AnySpawnerData(notnull array<SCR_EntityCatalogEntry> list)
	{
		foreach (SCR_EntityCatalogEntry entry : list)
		{
			if (entry && entry.HasEntityDataOfType(SCR_EntityCatalogSpawnerData))
				return true;
		}

		return false;
	}

	protected int SupplyCostOf(SCR_EntityCatalogEntry entry)
	{
		SCR_EntityCatalogSpawnerData data = SCR_EntityCatalogSpawnerData.Cast(entry.GetEntityDataOfType(SCR_EntityCatalogSpawnerData));
		if (data)
			return data.GetSupplyCost();

		return 0;
	}

	protected string DisplayName(SCR_EntityCatalogEntry entry, ResourceName prefab)
	{
		string name = entry.GetEntityName();
		if (!name.IsEmpty())
		{
			string translated = WidgetManager.Translate(name);
			if (!translated.IsEmpty() && translated.IndexOf("#") != 0)
				return translated;
		}

		return BaseName(prefab);
	}

	protected int DiscoverGroups(string factionKey, SCR_Faction faction, notnull array<ref CLW_ForceEntry> entries, notnull array<string> signatures)
	{
		SCR_EntityCatalog catalog = faction.GetFactionEntityCatalogOfType(EEntityCatalogType.GROUP, false);
		if (!catalog)
		{
			Print("CLW: catalog discovery: " + factionKey + " has no GROUP catalog", LogLevel.NORMAL);
			return 0;
		}

		array<SCR_EntityCatalogEntry> list = {};
		catalog.GetEntityList(list);
		bool needSpawnerData = AnySpawnerData(list);
		int added;
		foreach (SCR_EntityCatalogEntry entry : list)
		{
			if (!entry || !entry.IsEnabled())
				continue;

			ResourceName prefab = entry.GetPrefab();
			if (prefab.IsEmpty() || IsCovered(entries, prefab, false))
				continue;

			if (needSpawnerData && !entry.HasEntityDataOfType(SCR_EntityCatalogSpawnerData))
				continue;

			array<EEditableEntityLabel> labels = {};
			entry.GetEditableEntityLabels(labels);
			if (HasAny(labels, {EEditableEntityLabel.TRAIT_LOGISTICS, EEditableEntityLabel.TRAIT_ESSENTIAL, EEditableEntityLabel.ROLE_GUARD, EEditableEntityLabel.TRAIT_GUARD, EEditableEntityLabel.TRAIT_VEHICLE_CREW, EEditableEntityLabel.TRAIT_HELI_CREW, EEditableEntityLabel.GROUPTYPE_AIRMOBILE, EEditableEntityLabel.TRAIT_MORTAR}))
				continue;

			array<ResourceName> slots = {};
			int members;
			if (GetGroupSlots(prefab, slots))
				members = slots.Count();
			else
				members = CountGroupMembers(prefab);

			if (members < CLW_WarConfig.DISCOVER_MIN_GROUP_MEMBERS)
				continue;

			// roles from the member prefabs
			int scouts;
			int noncombat;
			int antiTank;
			foreach (ResourceName slotPrefab : slots)
			{
				array<EEditableEntityLabel> memberLabels = {};
				GetLabels(slotPrefab, memberLabels);
				if (memberLabels.Contains(EEditableEntityLabel.ROLE_ANTITANK))
					antiTank++;
				if (HasAny(memberLabels, {EEditableEntityLabel.ROLE_SCOUT, EEditableEntityLabel.ROLE_SHARPSHOOTER}))
					scouts++;
				if (HasAny(memberLabels, {EEditableEntityLabel.ROLE_MEDIC, EEditableEntityLabel.ROLE_SAPPER, EEditableEntityLabel.ROLE_RADIOOPERATOR}))
					noncombat++;
			}

			if (!slots.IsEmpty() && noncombat >= CLW_WarConfig.DISCOVER_NONCOMBAT_SHARE * slots.Count())
				continue;

			string kind = CLW_ForceKind.INFANTRY;
			if (!slots.IsEmpty() && scouts >= CLW_WarConfig.DISCOVER_RECON_SHARE * slots.Count())
				kind = CLW_ForceKind.RECON;

			int cost = CLW_WarConfig.DiscoveredGroupCost(members);

			CLW_ForceEntry fresh = new CLW_ForceEntry();
			m_iAutoCounter++;
			fresh.m_sType = "auto_" + kind + "_" + m_iAutoCounter;
			fresh.m_sName = DisplayName(entry, prefab);
			fresh.m_sKind = kind;
			fresh.m_iCost = cost;
			fresh.m_GroupPrefab = prefab;
			fresh.m_iWeight = CLW_WarConfig.DISCOVER_WEIGHT;
			fresh.m_bEnabled = true;
			fresh.m_sSource = "discovered";
			fresh.m_iGroupMembers = members;
			fresh.m_bChecked = true;
			// anti-armour: members carry ROLE_ANTITANK; member prefabs unreadable -> small group with the armour-piercing trait
			fresh.m_bAntiArmor = antiTank > 0;
			if (slots.IsEmpty() && members <= CLW_WarConfig.DISCOVER_AT_TEAM_MAX_MEMBERS && labels.Contains(EEditableEntityLabel.TRAIT_ARMORPIERCING))
				fresh.m_bAntiArmor = true;
			string sig = Signature(fresh) + "|" + cost;
			if (signatures.Contains(sig))
				continue;

			signatures.Insert(sig);
			entries.Insert(fresh);
			added++;
		}

		return added;
	}

	protected int ScaleCost(int raw, int floor)
	{
		return Math.Max(floor, Math.Round(raw * CLW_WarConfig.DISCOVER_COST_SCALE));
	}

	protected bool HasAny(notnull array<EEditableEntityLabel> labels, notnull array<EEditableEntityLabel> wanted)
	{
		foreach (EEditableEntityLabel label : wanted)
		{
			if (labels.Contains(label))
				return true;
		}

		return false;
	}

	// Biggest infantry entry (not recon) of the faction that fits capacity seats; null when none.
	protected CLW_ForceEntry PickCargoGroup(notnull array<ref CLW_ForceEntry> entries, int capacity)
	{
		CLW_ForceEntry best;
		foreach (CLW_ForceEntry entry : entries)
		{
			if (entry.m_sKind != CLW_ForceKind.INFANTRY || entry.HasVehicle() || entry.m_GroupPrefab.IsEmpty())
				continue;

			if (entry.m_iGroupMembers < 3 || entry.m_iGroupMembers > capacity)
				continue;

			if (!best || entry.m_iGroupMembers > best.m_iGroupMembers)
				best = entry;
		}

		return best;
	}

	protected int DiscoverVehicles(string factionKey, SCR_Faction faction, notnull array<ref CLW_ForceEntry> entries, notnull array<string> signatures)
	{
		SCR_EntityCatalog catalog = faction.GetFactionEntityCatalogOfType(EEntityCatalogType.VEHICLE, false);
		if (!catalog)
		{
			Print("CLW: catalog discovery: " + factionKey + " has no VEHICLE catalog", LogLevel.NORMAL);
			return 0;
		}

		array<SCR_EntityCatalogEntry> list = {};
		catalog.GetEntityList(list);
		bool needSpawnerData = AnySpawnerData(list);
		int added;
		foreach (SCR_EntityCatalogEntry entry : list)
		{
			if (!entry || !entry.IsEnabled())
				continue;

			ResourceName prefab = entry.GetPrefab();
			if (prefab.IsEmpty() || IsCovered(entries, prefab, true))
				continue;

			if (needSpawnerData && !entry.HasEntityDataOfType(SCR_EntityCatalogSpawnerData))
				continue;

			array<EEditableEntityLabel> labels = {};
			entry.GetEditableEntityLabels(labels);
			if (HasAny(labels, {EEditableEntityLabel.VEHICLE_HELICOPTER, EEditableEntityLabel.VEHICLE_AIRPLANE, EEditableEntityLabel.VEHICLE_TURRET, EEditableEntityLabel.TRAIT_MEDICAL, EEditableEntityLabel.TRAIT_REFUELING, EEditableEntityLabel.TRAIT_REARMING, EEditableEntityLabel.TRAIT_REPAIRING, EEditableEntityLabel.TRAIT_SERVICE, EEditableEntityLabel.TRAIT_ARSENAL, EEditableEntityLabel.TRAIT_LOGISTICS, EEditableEntityLabel.TRAIT_MORTAR, EEditableEntityLabel.TRAIT_VEHICLE_REARMING}))
				continue;

			// seats: real probe, else a label-based estimate
			CLW_VehicleProbe probe = ProbeVehicle(factionKey, prefab);
			int cargo;
			int turrets;
			int crew = -1;
			bool probed = probe && probe.m_bOk;
			if (probed)
			{
				cargo = probe.m_iCargo;
				turrets = probe.m_iTurret;
				crew = probe.m_iCrew;
			}
			else if (labels.Contains(EEditableEntityLabel.TRAIT_PASSENGERS_LARGE))
				cargo = CLW_WarConfig.DISCOVER_LARGE_CARGO_GUESS;
			else if (labels.Contains(EEditableEntityLabel.TRAIT_PASSENGERS_SMALL))
				cargo = 1;

			bool armed = turrets > 0 || labels.Contains(EEditableEntityLabel.TRAIT_ARMED);
			bool apc = labels.Contains(EEditableEntityLabel.VEHICLE_APC);
			bool car = labels.Contains(EEditableEntityLabel.VEHICLE_CAR);
			bool truck = labels.Contains(EEditableEntityLabel.VEHICLE_TRUCK);
			bool traitArmor = labels.Contains(EEditableEntityLabel.TRAIT_ARMOR);
			bool armored = apc || traitArmor;
			bool largeLabel = labels.Contains(EEditableEntityLabel.TRAIT_PASSENGERS_LARGE);
			int capacity = cargo;
			if (capacity < 3 && largeLabel)
				capacity = CLW_WarConfig.DISCOVER_LARGE_CARGO_GUESS;

			// armed truck / unarmoured armed vehicle with a large cargo = motorized; tank (armour, no APC/CAR) = armor whatever its
			// rider seats; armed APC/CAR with <= 2 cargo = armored_recon; mech needs armour or APC
			string kind = "";
			string rule = "";
			if (armed)
			{
				if (truck || (!armored && (largeLabel || cargo >= CLW_WarConfig.DISCOVER_MOTORIZED_MIN_CARGO)))
				{
					kind = CLW_ForceKind.MOTORIZED;
					rule = "armed truck / unarmoured with large cargo";
				}
				else if (traitArmor && !apc && !car)
				{
					kind = CLW_ForceKind.ARMOR;
					rule = "armour, no APC/CAR label";
				}
				else if (armored && cargo <= CLW_WarConfig.DISCOVER_LIGHT_MAX_CARGO)
				{
					kind = CLW_ForceKind.ARMORED_RECON;
					rule = "armoured APC/CAR, small cargo";
				}
				else if (armored)
				{
					kind = CLW_ForceKind.MECH;
					rule = "armoured with cargo";
				}
				else
				{
					kind = CLW_ForceKind.LIGHT;
					rule = "armed, unarmoured";
				}
			}
			else if (truck || largeLabel || cargo >= CLW_WarConfig.DISCOVER_MOTORIZED_MIN_CARGO)
			{
				kind = CLW_ForceKind.MOTORIZED;
				rule = "unarmed transport";
			}

			Print(string.Format("CLW: catalog discovery %1: %2 kind=%3 (%4) armed=%5 apc=%6 car=%7 truck=%8 armor=%9", factionKey, "" + prefab + " cargo=" + cargo, kind, rule, armed, apc, car, truck, traitArmor), LogLevel.NORMAL);

			if (kind.IsEmpty())
				continue;

			CLW_ForceEntry fresh = new CLW_ForceEntry();
			m_iAutoCounter++;
			fresh.m_sType = "auto_" + kind + "_" + m_iAutoCounter;
			fresh.m_sVehicleName = DisplayName(entry, prefab);
			fresh.m_sName = fresh.m_sVehicleName;
			fresh.m_sKind = kind;
			fresh.m_VehiclePrefab = prefab;
			fresh.m_iWeight = CLW_WarConfig.DISCOVER_WEIGHT;
			fresh.m_bEnabled = true;
			fresh.m_sSource = "discovered";
			fresh.m_iCargoSeats = -1;
			fresh.m_iCrewSeats = -1;
			fresh.m_bArmed = armed;
			if (probed)
			{
				fresh.m_iCargoSeats = cargo;
				fresh.m_iCrewSeats = crew;
			}

			int vehicleCost = CLW_WarConfig.DiscoveredVehicleCost(kind, armed);
			fresh.m_iCost = vehicleCost;

			string sig = Signature(fresh);
			if (signatures.Contains(sig))
				continue;

			// pair with an infantry group that fits (mech, motorized)
			if (kind == CLW_ForceKind.MECH || kind == CLW_ForceKind.MOTORIZED)
			{
				CLW_ForceEntry cargoEntry = PickCargoGroup(entries, capacity);
				if (cargoEntry)
				{
					fresh.m_CargoGroupPrefab = cargoEntry.m_GroupPrefab;
					fresh.m_iCargoMembers = cargoEntry.m_iGroupMembers;
					fresh.m_iCost = vehicleCost + cargoEntry.m_iCost;
					fresh.m_sName = fresh.m_sVehicleName + " + " + cargoEntry.m_sName;
				}
				else if (kind == CLW_ForceKind.MOTORIZED)
				{
					Print("CLW: catalog discovery " + factionKey + ": transport " + prefab + " skipped (no infantry group fits " + capacity + " seats)", LogLevel.NORMAL);
					continue;
				}
				else
				{
					Print("CLW: catalog discovery " + factionKey + ": " + prefab + " has no fitting infantry group, bought without embarked infantry", LogLevel.WARNING);
				}
			}

			signatures.Insert(sig);
			entries.Insert(fresh);
			added++;
		}

		return added;
	}
}

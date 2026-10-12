// Base services (M3 phase B, design section 3). Spawns the service compositions (command post, supply cache, arsenal, vehicle depot,
// medical, repair) of a side at its HQ base at war start and keeps a registry of them for later phases (requests, FOBs, logistics).
// Which prefab is a service of which kind comes from DATA: CLW_FactionProfile.m_aServices, with a kin fallback for pack factions that
// ship none (profile m_sServiceFallbackFaction, else a friendly faction that has services). No faction or prefab name in this code.
// Server only. Log prefix: "CLW: services".
//
// API for other phases:
//   CLW_Services.SpawnServices(sideKey, baseId, anchor, kinds)   reusable at an HQ or a FOB: kinds empty = every service of the profile
//   CLW_Services.Find(sideKey, kind)  / FindAt(sideKey, baseId, kind)   first matching service entity (null when none)
//   CLW_Services.GetAll(sideKey, kind, outEntities)   kind "" = all services of the side
//   CLW_SupplyLink.Find(sideKey, baseId)   the supply container link of a base (stock read/write)

// One spawned service.
class CLW_ServiceInstance
{
	string m_sSide;			// faction key the service belongs to
	string m_sBase;			// base id: "HQ" or a FOB id
	string m_sKind;			// CLW_ServiceKind
	string m_sSource;		// faction key of the profile the prefab came from (differs from m_sSide on a kin fallback)
	ResourceName m_Prefab;
	IEntity m_Entity;
	vector m_vPos;
	float m_fRadius;
}

class CLW_Services
{
	static const string BASE_HQ = "HQ";
	static const int PLACE_BATCHES = 4;				// search rings, each wider than the last
	static const int PLACE_ATTEMPTS = 20;			// random candidates per ring
	static const float PLACE_GAP_M = 4;				// free space between two services
	static const float PLACE_RING_M = 55;			// width of the first search ring beyond the spawn zone
	static const float PLACE_RING_GROW_M = 30;		// every further ring is this much wider
	static const float SPAWN_ZONE_DEFAULT_M = 30;	// keep-clear radius around the base anchor when the scenario gives none
	static const int LINK_DELAY_MS = 800;			// the services' resource components initialise after the spawn
	static const int LINK_RETRIES = 6;
	static const int ARSENAL_DELAY_MS = 1500;
	static const int ARSENAL_LOG_MS = 6000;
	static const int SYNC_MS = 1500;

	protected static ref array<ref CLW_ServiceInstance> s_aInst = {};
	protected static bool s_bPolling;

	// ---- lifecycle ----

	// Game start (server): statics survive a Play restart without a script reload, so forget everything of the previous session.
	static void Reset()
	{
		s_aInst.Clear();
		s_bPolling = false;
		CLW_SupplyLink.Reset();
		CLW_ArsenalPool.Reset();
	}

	// War SETUP -> RUNNING (CLW_GameMode.OnWarStarted, before the start forces are fielded, after the battle layout moved the HQs).
	static void OnWarStart(notnull CLW_WarState war)
	{
		if (!Replication.IsServer())
			return;

		if (!CLW_Capabilities.Has("services"))
		{
			Print("CLW: services capability is off: no base services are spawned", LogLevel.NORMAL);
			return;
		}

		CLW_Bases bases = CLW_GameMode.GetBases();
		if (!bases)
		{
			Print("CLW: services WARNING no bases object, nothing spawned", LogLevel.WARNING);
			return;
		}

		array<string> keys = {};
		war.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			vector anchor = bases.GetBasePos(key);
			if (anchor == vector.Zero)
			{
				Print("CLW: services WARNING no base position for " + key, LogLevel.WARNING);
				continue;
			}

			array<string> all = {};
			int placed = SpawnServices(key, BASE_HQ, anchor, all);
			Print(string.Format("CLW: services side %1 base %2: %3 services spawned around %4", key, BASE_HQ, placed, anchor), LogLevel.NORMAL);
		}

		if (!s_bPolling)
		{
			s_bPolling = true;
			GetGame().GetCallqueue().CallLater(PollLinks, SYNC_MS, true);
		}
	}

	// War over: stop the supply sync and remove the services.
	static void OnWarEnded()
	{
		if (!Replication.IsServer())
			return;

		GetGame().GetCallqueue().Remove(PollLinks);
		s_bPolling = false;
		int removed;
		foreach (CLW_ServiceInstance inst : s_aInst)
		{
			if (inst.m_Entity)
			{
				SCR_EntityHelper.DeleteEntityAndChildren(inst.m_Entity);
				removed++;
			}
		}

		Print(string.Format("CLW: services war over: %1 services removed", removed), LogLevel.NORMAL);
		s_aInst.Clear();
		CLW_SupplyLink.Reset();
	}

	// ---- queries ----

	// First service of the side with this kind (any base), null when none.
	static IEntity Find(string sideKey, string kind)
	{
		foreach (CLW_ServiceInstance inst : s_aInst)
		{
			if (inst.m_sSide == sideKey && inst.m_sKind == kind && inst.m_Entity)
				return inst.m_Entity;
		}

		return null;
	}

	static IEntity FindAt(string sideKey, string baseId, string kind)
	{
		foreach (CLW_ServiceInstance inst : s_aInst)
		{
			if (inst.m_sSide == sideKey && inst.m_sBase == baseId && inst.m_sKind == kind && inst.m_Entity)
				return inst.m_Entity;
		}

		return null;
	}

	// Appends the service entities of a side; kind "" = all kinds.
	static int GetAll(string sideKey, string kind, notnull array<IEntity> outEntities)
	{
		int count;
		foreach (CLW_ServiceInstance inst : s_aInst)
		{
			if (inst.m_sSide != sideKey || !inst.m_Entity)
				continue;

			if (!kind.IsEmpty() && inst.m_sKind != kind)
				continue;

			outEntities.Insert(inst.m_Entity);
			count++;
		}

		return count;
	}

	// Same with the instance data (kind, base, position).
	static int GetInstances(string sideKey, string baseId, notnull array<CLW_ServiceInstance> outInstances)
	{
		int count;
		foreach (CLW_ServiceInstance inst : s_aInst)
		{
			if (inst.m_sSide != sideKey || !inst.m_Entity)
				continue;

			if (!baseId.IsEmpty() && inst.m_sBase != baseId)
				continue;

			outInstances.Insert(inst);
			count++;
		}

		return count;
	}

	// ---- spawning ----

	// Spawns the services of 'sideKey' around 'anchor' (an HQ or a FOB). kinds: the kinds wanted, empty = every service of the profile.
	// Positions: free spots on a ring around the anchor (outside the spawn zone), no overlap with each other, dry, flat, snapped to the
	// terrain; a profile offset pins a service. Returns the number spawned. Safe to call again for the same base (existing ones count
	// as occupied space; a kind that already exists at the base is not spawned twice).
	static int SpawnServices(string sideKey, string baseId, vector anchor, notnull array<string> kinds)
	{
		if (!Replication.IsServer())
			return 0;

		array<ref CLW_ServiceEntry> entries = {};
		string source = ResolveEntries(sideKey, entries);
		if (entries.IsEmpty())
		{
			Print(string.Format("CLW: services side %1 has no service data (no profile services, no kin fallback): no base services", sideKey), LogLevel.WARNING);
			return 0;
		}

		if (source != sideKey)
			Print(string.Format("CLW: services side %1 uses the services of %2 (kin fallback)", sideKey, source), LogLevel.NORMAL);

		float keepClear = SPAWN_ZONE_DEFAULT_M;
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario)
		{
			CLW_FactionSetup setup = scenario.FindFaction(sideKey);
			if (setup && setup.m_fSpawnRadius > 0)
				keepClear = setup.m_fSpawnRadius;
		}

		// occupied space: services already at this base
		array<vector> placedPos = {};
		array<float> placedRad = {};
		foreach (CLW_ServiceInstance existing : s_aInst)
		{
			if (existing.m_sSide == sideKey && existing.m_sBase == baseId && existing.m_Entity)
			{
				placedPos.Insert(existing.m_vPos);
				placedRad.Insert(existing.m_fRadius);
			}
		}

		array<string> order = {};
		OrderedKinds(order);
		int spawned;
		foreach (string kind : order)
		{
			if (!kinds.IsEmpty() && !kinds.Contains(kind))
				continue;

			foreach (CLW_ServiceEntry entry : entries)
			{
				if (entry.m_sKind != kind)
					continue;

				if (FindAt(sideKey, baseId, kind))
					continue;	// that kind is there already

				if (SpawnOne(sideKey, baseId, anchor, entry, source, keepClear, placedPos, placedRad))
					spawned++;
			}
		}

		if (spawned > 0)
		{
			GetGame().GetCallqueue().CallLater(LinkBase, LINK_DELAY_MS, false, sideKey, baseId, 0);
			GetGame().GetCallqueue().CallLater(SetupArsenals, ARSENAL_DELAY_MS, false, sideKey, baseId);
		}

		return spawned;
	}

	protected static bool SpawnOne(string sideKey, string baseId, vector anchor, CLW_ServiceEntry entry, string source, float keepClear, notnull array<vector> placedPos, notnull array<float> placedRad)
	{
		Resource resource = Resource.Load(entry.m_Prefab);
		if (!resource || !resource.IsValid())
		{
			Print(string.Format("CLW: services ERROR %1 %2: cannot load prefab %3", sideKey, entry.m_sKind, entry.m_Prefab), LogLevel.ERROR);
			return false;
		}

		float radius = entry.m_fRadius;
		if (radius <= 0)
			radius = CLW_ServiceKind.DefaultRadius(entry.m_sKind);

		vector spot;
		string via;
		if (entry.m_vOffset[0] != 0 || entry.m_vOffset[2] != 0)
		{
			spot = Vector(anchor[0] + entry.m_vOffset[0], 0, anchor[2] + entry.m_vOffset[2]);
			CLW_Placement.SurfaceAt(spot);
			via = "offset";
		}
		else
		{
			via = FindSpot(anchor, keepClear + radius, radius, placedPos, placedRad, spot);
		}

		float yaw = entry.m_fYaw;
		if (yaw < 0)
			yaw = CLW_Placement.YawFromDir(anchor[0] - spot[0], anchor[2] - spot[2]);

		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		Math3D.AnglesToMatrix(Vector(yaw, 0, 0), params.Transform);
		params.Transform[3] = spot;
		IEntity entity = GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params);
		if (!entity)
		{
			Print(string.Format("CLW: services ERROR %1 %2: spawn failed (%3)", sideKey, entry.m_sKind, entry.m_Prefab), LogLevel.ERROR);
			return false;
		}

		CLW_ServiceInstance inst = new CLW_ServiceInstance();
		inst.m_sSide = sideKey;
		inst.m_sBase = baseId;
		inst.m_sKind = entry.m_sKind;
		inst.m_sSource = source;
		inst.m_Prefab = entry.m_Prefab;
		inst.m_Entity = entity;
		inst.m_vPos = spot;
		inst.m_fRadius = radius;
		s_aInst.Insert(inst);
		placedPos.Insert(spot);
		placedRad.Insert(radius);

		array<IEntity> tree = {};
		CollectHierarchy(entity, tree);
		int reaffiliated = AdoptFaction(tree, source, sideKey);
		int spawnPoints = DisableSpawnPoints(tree);
		Print(string.Format("CLW: services placed %1 %2 %3 at %4 yaw=%5 r=%6 via=%7", sideKey, baseId, entry.m_sKind, spot, Math.Round(yaw), radius, via), LogLevel.NORMAL);
		Print(string.Format("CLW: services   %1 %2 entities=%3 reaffiliated=%4 spawnPointsOff=%5 prefab=%6", sideKey, entry.m_sKind, tree.Count(), reaffiliated, spawnPoints, entry.m_Prefab.GetPath()), LogLevel.NORMAL);
		return true;
	}

	// Free spot on rings around the anchor: outside minDist, no overlap with the placed services (+ gap), dry, flat, nothing solid in a
	// cylinder. Rings widen; the last resort ignores the solid check, then forces a spot. Returns how the spot was found.
	protected static string FindSpot(vector anchor, float minDist, float radius, notnull array<vector> placedPos, notnull array<float> placedRad, out vector spot)
	{
		float cylRadius = Math.Min(radius * 0.5, 4);
		for (int pass = 0; pass < 2; pass++)
		{
			for (int ring = 0; ring < PLACE_BATCHES; ring++)
			{
				float maxDist = minDist + PLACE_RING_M + ring * PLACE_RING_GROW_M;
				for (int attempt = 0; attempt < PLACE_ATTEMPTS; attempt++)
				{
					float angle = Math.RandomFloat(0, Math.PI2);
					float dist = Math.RandomFloat(minDist, maxDist);
					vector cand = anchor + Vector(Math.Cos(angle) * dist, 0, Math.Sin(angle) * dist);
					CLW_Placement.SurfaceAt(cand);
					if (CLW_Placement.IsWater(cand) || !CLW_Placement.SlopeOk(cand))
						continue;

					if (Overlaps(cand, radius, placedPos, placedRad))
						continue;

					if (pass == 0 && !CLW_Placement.IsFree(cand, cylRadius, 2.5, 0, 0))
						continue;

					spot = cand;
					if (pass == 0)
						return "free";

					return "no-solid-check";
				}
			}
		}

		// forced: a spot on the minimum ring that does not overlap, rotated by the number already placed
		float forcedAngle = placedPos.Count() * 1.1;
		spot = anchor + Vector(Math.Cos(forcedAngle) * (minDist + placedPos.Count() * 6), 0, Math.Sin(forcedAngle) * (minDist + placedPos.Count() * 6));
		CLW_Placement.SurfaceAt(spot);
		return "forced";
	}

	protected static bool Overlaps(vector pos, float radius, notnull array<vector> placedPos, notnull array<float> placedRad)
	{
		foreach (int i, vector other : placedPos)
		{
			if (vector.DistanceXZ(pos, other) < radius + placedRad[i] + PLACE_GAP_M)
				return true;
		}

		return false;
	}

	// ---- data resolution ----

	// Fixed spawn order: the building that defines the base first, then stock, arsenal, depot, medical, repair.
	protected static void OrderedKinds(notnull array<string> order)
	{
		order.Insert(CLW_ServiceKind.COMMAND_POST);
		order.Insert(CLW_ServiceKind.SUPPLY);
		order.Insert(CLW_ServiceKind.ARSENAL);
		order.Insert(CLW_ServiceKind.VEHICLE_DEPOT);
		order.Insert(CLW_ServiceKind.MEDICAL);
		order.Insert(CLW_ServiceKind.REPAIR);
	}

	// The service entries for a side: its own profile, else the profile named by m_sServiceFallbackFaction, else the profile of a friendly
	// faction (scenario profile list) that has services. Returns the faction key the entries came from, "" when there are none.
	static string ResolveEntries(string sideKey, notnull array<ref CLW_ServiceEntry> entries)
	{
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		CLW_FactionProfile own;
		if (catalog)
			own = catalog.FindProfile(sideKey);

		if (HasServices(own))
		{
			AppendEntries(own, entries);
			return sideKey;
		}

		if (own && !own.m_sServiceFallbackFaction.IsEmpty() && catalog)
		{
			CLW_FactionProfile named = catalog.FindProfile(own.m_sServiceFallbackFaction);
			if (HasServices(named))
			{
				AppendEntries(named, entries);
				return own.m_sServiceFallbackFaction;
			}

			Print(string.Format("CLW: services side %1 names fallback faction %2 but that has no services", sideKey, own.m_sServiceFallbackFaction), LogLevel.WARNING);
		}

		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario && scenario.m_aProfileConfigs)
		{
			foreach (ResourceName path : scenario.m_aProfileConfigs)
			{
				CLW_FactionProfile candidate = CLW_FactionProfile.Load(path);
				if (!candidate || candidate.m_sFactionKey == sideKey || !HasServices(candidate))
					continue;

				if (!AreKin(sideKey, candidate.m_sFactionKey))
					continue;

				AppendEntries(candidate, entries);
				return candidate.m_sFactionKey;
			}
		}

		return "";
	}

	protected static bool HasServices(CLW_FactionProfile profile)
	{
		if (!profile || !profile.m_aServices)
			return false;

		foreach (CLW_ServiceEntry entry : profile.m_aServices)
		{
			if (entry && entry.m_bEnabled && !entry.m_Prefab.IsEmpty() && CLW_ServiceKind.IsKnown(entry.m_sKind))
				return true;
		}

		return false;
	}

	protected static void AppendEntries(CLW_FactionProfile profile, notnull array<ref CLW_ServiceEntry> entries)
	{
		foreach (CLW_ServiceEntry entry : profile.m_aServices)
		{
			if (entry && entry.m_bEnabled && !entry.m_Prefab.IsEmpty() && CLW_ServiceKind.IsKnown(entry.m_sKind))
				entries.Insert(entry);
		}
	}

	// Friendly in the faction data of either one (the vanilla friendly lists; pack factions list the vanilla faction they are built on).
	static bool AreKin(string keyA, string keyB)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (!manager)
			return false;

		SCR_Faction first = SCR_Faction.Cast(manager.GetFactionByKey(keyA));
		SCR_Faction second = SCR_Faction.Cast(manager.GetFactionByKey(keyB));
		if (!first || !second)
			return false;

		return first.DoCheckIfFactionFriendly(second) || second.DoCheckIfFactionFriendly(first);
	}

	// ---- hierarchy helpers ----

	static void CollectHierarchy(IEntity root, notnull array<IEntity> outEntities)
	{
		if (!root)
			return;

		outEntities.Insert(root);
		IEntity child = root.GetChildren();
		while (child)
		{
			CollectHierarchy(child, outEntities);
			child = child.GetSibling();
		}
	}

	// A service built from a kin faction's prefab carries that faction's affiliation: move every affiliation that still has the source
	// faction as default to the side's faction (arsenal item pool, base logic). Returns how many were changed.
	protected static int AdoptFaction(notnull array<IEntity> tree, string sourceKey, string sideKey)
	{
		if (sourceKey == sideKey)
			return 0;

		FactionManager manager = GetGame().GetFactionManager();
		if (!manager)
			return 0;

		Faction side = manager.GetFactionByKey(sideKey);
		if (!side)
			return 0;

		int changed;
		foreach (IEntity entity : tree)
		{
			FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(entity.FindComponent(FactionAffiliationComponent));
			if (!affiliation || affiliation.GetDefaultFactionKey() != sourceKey)
				continue;

			affiliation.SetAffiliatedFaction(side);
			changed++;
		}

		return changed;
	}

	// The HQ composition brings its own supplies spawn point; our one base spawn point is the only one in play.
	protected static int DisableSpawnPoints(notnull array<IEntity> tree)
	{
		int off;
		foreach (IEntity entity : tree)
		{
			SCR_SpawnPoint point = SCR_SpawnPoint.Cast(entity);
			if (!point)
				continue;

			point.SetSpawnPointEnabled_S(false);
			off++;
		}

		return off;
	}

	// ---- supply link + arsenal (delayed: the compositions initialise their components after the spawn) ----

	protected static void LinkBase(string sideKey, string baseId, int attempt)
	{
		array<IEntity> entities = {};
		array<CLW_ServiceInstance> insts = {};
		GetInstances(sideKey, baseId, insts);
		foreach (CLW_ServiceInstance inst : insts)
		{
			CollectHierarchy(inst.m_Entity, entities);
		}

		CLW_SupplyLink link = CLW_SupplyLink.GetOrCreate(sideKey, baseId);
		int found = link.Collect(entities);
		if (found == 0 && attempt < LINK_RETRIES)
		{
			GetGame().GetCallqueue().CallLater(LinkBase, LINK_DELAY_MS, false, sideKey, baseId, attempt + 1);
			return;
		}

		if (found == 0)
		{
			Print(string.Format("CLW: services supply link %1 %2: no supply container found in the services (the faction has no supply service?), no stock link", sideKey, baseId), LogLevel.WARNING);
			return;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			link.Push(war);
	}

	protected static void SetupArsenals(string sideKey, string baseId)
	{
		bool kitCost = CLW_Capabilities.Has("kitCost");
		array<CLW_ServiceInstance> insts = {};
		GetInstances(sideKey, baseId, insts);
		int count;
		foreach (CLW_ServiceInstance inst : insts)
		{
			array<IEntity> tree = {};
			CollectHierarchy(inst.m_Entity, tree);
			foreach (IEntity entity : tree)
			{
				SCR_ArsenalComponent arsenal = SCR_ArsenalComponent.Cast(entity.FindComponent(SCR_ArsenalComponent));
				if (!arsenal)
					continue;

				count++;
				// kits made from kin items must be saveable, plain faction-only would reject them
				arsenal.SetArsenalSaveType(SCR_EArsenalSaveType.FRIENDLY_AND_FACTION_ITEMS_ONLY);

				SCR_ResourceComponent resources = SCR_ResourceComponent.FindResourceComponent(entity);
				string cost = "default";
				if (resources)
				{
					if (resources.CanResourceTypeEnabledBeChanged())
					{
						if (!kitCost)
							resources.SetResourceTypeEnabled(false);
						cost = "on";
						if (!kitCost)
							cost = "free";
					}
					else
					{
						cost = "fixed";
					}
				}

				Print(string.Format("CLW: services arsenal %1 %2 at %3: kitCost=%4 (%5) saveType=FRIENDLY_AND_FACTION_ITEMS_ONLY", sideKey, baseId, inst.m_vPos, kitCost, cost), LogLevel.NORMAL);
				GetGame().GetCallqueue().CallLater(LogArsenal, ARSENAL_LOG_MS, false, sideKey, entity);
			}
		}

		if (count == 0)
			Print(string.Format("CLW: services arsenal %1 %2: none in the services (kit cost: nothing to switch)", sideKey, baseId), LogLevel.NORMAL);
	}

	protected static void LogArsenal(string sideKey, IEntity entity)
	{
		if (!entity)
			return;

		SCR_ArsenalComponent arsenal = SCR_ArsenalComponent.Cast(entity.FindComponent(SCR_ArsenalComponent));
		if (!arsenal)
			return;

		array<ResourceName> prefabs = {};
		arsenal.GetAvailablePrefabs(prefabs);
		string factionKey = "none";
		if (arsenal.GetAssignedFaction())
			factionKey = arsenal.GetAssignedFaction().GetFactionKey();

		Print(string.Format("CLW: services arsenal side %1 faction %2 items=%3 enabled=%4 usingSupplies=%5", sideKey, factionKey, prefabs.Count(), arsenal.IsArsenalEnabled(), arsenal.IsArsenalUsingSupplies()), LogLevel.NORMAL);
	}

	protected static void PollLinks()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;

		CLW_SupplyLink.PollAll(war);
	}
}

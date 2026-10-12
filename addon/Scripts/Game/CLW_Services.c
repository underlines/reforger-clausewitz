// Base services (M3 phase B, design section 3). Spawns the service compositions (command post, supply cache, arsenal, vehicle depot,
// repair) of a side at its HQ base at war start and keeps a registry of them for later phases (requests, FOBs, logistics).
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

// Placement distances of one service (built by CLW_Services.TuningFor from the constants and the profile entry).
class CLW_PlaceTuning
{
	string m_sKind;
	float m_fGap;			// free space to the other services
	float m_fOverlap;		// share of the circumscribed footprint radii that must be free
	float m_fInner;			// clear ground around the anchor beyond the footprint radius
	float m_fRing;			// first search ring width
	float m_fGrow;			// every further ring is this much wider
	float m_fPinSlack;		// shift allowed for an authored offset
	float m_fCompactMax;	// logging limit
	float m_fRingMin;		// profile ring (distance of the centre from the anchor), 0 = automatic
	float m_fRingMax;
	bool m_bTouch;			// command post: starts right next to the spawn point (tent) at the anchor
}

class CLW_Services
{
	static const string BASE_HQ = "HQ";
	// Compact base layout (owner playtest 2026-10-12: services were 50-80 m apart). Generic engine-level numbers, the clearance checks of
	// CLW_FreeSpot (objects, trees, water, slope, roads) are unchanged: the search starts tight and only grows when the ground requires it.
	// Round 3 (owner 2026-10-12: still ~10 m between items): every distance below is the M4 round-2 value ("full") times PLACE_SCALE (0.7 = 30 %
	// tighter) for everything except the vehicle depot (PLACE_SCALE_DEPOT 1.0: vehicles need room). The command post sits right at the
	// anchor (the base spawn point, "tent"), the arsenal 2-4 m and the supply cache 5-8 m from it (per-kind gaps, overridable in the profile).
	static const float PLACE_SCALE = 0.7;
	static const float PLACE_SCALE_DEPOT = 1.0;
	static const int PLACE_BATCHES = 4;				// search widths, each wider than the last
	static const float PLACE_GAP_M = 3;				// full: free space between the footprints of two services
	static const float PLACE_OVERLAP_FACTOR = 0.8;	// full: footprint radius is the circumscribed circle: 80 % of it is enough, CheckPose blocks real overlaps
	static const float PLACE_INNER_M = 5;			// full: clear ground around the anchor beyond the footprint radius (capped by the scenario spawn radius)
	static const float PLACE_RING_M = 14;			// full: width of the first search ring beyond the inner radius
	static const float PLACE_RING_GROW_M = 12;		// full: every further ring is this much wider
	static const float PLACE_PIN_SLACK_M = 8;		// full: an authored offset may shift this far to reach clear ground before the ring search starts
	static const float PLACE_COMPACT_MAX_M = 35;	// full: farther than this from the anchor is logged with the reason
	static const float PLACE_TENT_RADIUS_M = 2;		// the spawn point (tent) at the anchor: the command post edge starts this far from the anchor
	static const float PLACE_TENT_GAP_M = 0.75;		// gap between the tent and the command post (owner: 0.5-1 m)
	static const float PLACE_GAP_ARSENAL_M = 2.5;	// arsenal: free space to the command post / other services (owner: 2-4 m)
	static const float PLACE_GAP_SUPPLY_M = 5.5;	// supply cache: free space to the others (owner: 5-8 m)
	static const int SUPPLY_CHECK_MS = 10000;		// depot / arsenal supply check (the consumers see the linked stock only after the first poll)
	static const float SPAWN_ZONE_DEFAULT_M = 30;	// keep-clear radius around the base anchor when the scenario gives none
	static const int DEPOT_DELAY_MS = 1800;
	static const int LINK_DELAY_MS = 800;			// the services' resource components initialise after the spawn
	static const int LINK_RETRIES = 6;
	static const int ARSENAL_DELAY_MS = 1500;
	static const int ARSENAL_LOG_MS = 6000;
	static const int SYNC_MS = 1500;

	protected static ref array<ref CLW_ServiceInstance> s_aInst = {};
	protected static string s_sPlaceWhy;	// reject summary of the strict passes that failed (why a service had to go farther)
	protected static bool s_bPolling;

	// ---- lifecycle ----

	// Game start (server): statics survive a Play restart without a script reload, so forget everything of the previous session.
	static void Reset()
	{
		s_aInst.Clear();
		s_bPolling = false;
		CLW_SupplyLink.Reset();
		CLW_ArsenalPool.Reset();
		CLW_RequestTerminals.Reset();
		CLW_StructureMarkers.Reset();
		CLW_PropResolver.Reset();
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
			Print(string.Format("CLW: services side %1 base %2: %3 services spawned around %4 grid %5", key, BASE_HQ, placed, anchor, CLW_Places.Grid(anchor)), LogLevel.NORMAL);
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

		CLW_RequestTerminals.RemoveAll();
		CLW_StructureMarkers.RemoveAll();
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

	// Deletes the services of one base (a lost FOB) and its supply link. The link goes first: its containers die with the entities and
	// a vanished stock must not be read as a withdrawal from the side ledger. Returns the number of services removed.
	static int RemoveBase(string sideKey, string baseId)
	{
		if (!Replication.IsServer() || baseId == BASE_HQ)
			return 0;

		CLW_SupplyLink.Remove(sideKey, baseId);
		CLW_StructureMarkers.RemoveBase(sideKey, baseId);
		int removed;
		for (int i = s_aInst.Count() - 1; i >= 0; i--)
		{
			CLW_ServiceInstance inst = s_aInst[i];
			if (inst.m_sSide != sideKey || inst.m_sBase != baseId)
				continue;

			if (inst.m_Entity)
				SCR_EntityHelper.DeleteEntityAndChildren(inst.m_Entity);

			s_aInst.Remove(i);
			removed++;
		}

		return removed;
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
			GetGame().GetCallqueue().CallLater(SetupDepots, DEPOT_DELAY_MS, false, sideKey, baseId);
		}

		return spawned;
	}

	// FOB kit (owner request 2026-10-12: a FOB was an invisible spawn point): a compact cluster around 'center' (the FOB spawn point):
	// arsenal box, supply box (the FOB stock, linked like the base supply so the arsenal draws from it), sandbags. The entries come from
	// the profile's m_aFobKit (CLW_PropResolver.CollectKit), the prefab of every entry from CLW_PropResolver.Resolve(role). Offsets are in
	// the cluster frame (x right, z forward) turned by clusterYaw; no search per piece: the whole cluster spot was chosen free by the caller.
	// The pieces are registered as services of the FOB (kind arsenal / supply / fortification), so RemoveBase deletes them with the FOB.
	// Returns the number spawned, 0 when the side has no kit.
	static int SpawnFobKit(string sideKey, string fobId, vector center, float clusterYaw)
	{
		if (!Replication.IsServer())
			return 0;

		array<ref CLW_ServiceEntry> kit = {};
		string kitSource = CLW_PropResolver.CollectKit(sideKey, kit);
		if (kit.IsEmpty())
			return 0;

		float rad = clusterYaw * Math.DEG2RAD;
		float c = Math.Cos(rad);
		float s = Math.Sin(rad);
		int spawned;
		foreach (CLW_ServiceEntry entry : kit)
		{
			string role = entry.GetRole();
			string propSource;
			string sourceKey;
			CLW_ServiceEntry resolved = CLW_PropResolver.ResolveEntry(sideKey, role, propSource, sourceKey);
			if (!resolved)
				continue;

			Resource resource = Resource.Load(resolved.m_Prefab);
			if (!resource || !resource.IsValid())
			{
				Print(string.Format("CLW: services ERROR %1 %2 %3: cannot load prefab %4", sideKey, fobId, role, resolved.m_Prefab), LogLevel.ERROR);
				continue;
			}

			vector spot = Vector(center[0] + entry.m_vOffset[0] * c + entry.m_vOffset[2] * s, 0, center[2] - entry.m_vOffset[0] * s + entry.m_vOffset[2] * c);
			CLW_Placement.SurfaceAt(spot);
			float yaw = entry.m_fYaw;
			if (yaw < 0)
				yaw = CLW_Placement.YawFromDir(center[0] - spot[0], center[2] - spot[2]);
			else
				yaw += clusterYaw;

			EntitySpawnParams params = new EntitySpawnParams();
			params.TransformMode = ETransformMode.WORLD;
			Math3D.AnglesToMatrix(Vector(yaw, 0, 0), params.Transform);
			params.Transform[3] = spot;
			IEntity entity = GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params);
			if (!entity)
			{
				Print(string.Format("CLW: services ERROR %1 %2 %3: spawn failed (%4)", sideKey, fobId, role, resolved.m_Prefab), LogLevel.ERROR);
				continue;
			}

			float radius = entry.m_fRadius;
			if (radius <= 0)
				radius = CLW_ServiceKind.DefaultRadius(entry.m_sKind);

			CLW_ServiceInstance inst = new CLW_ServiceInstance();
			inst.m_sSide = sideKey;
			inst.m_sBase = fobId;
			inst.m_sKind = entry.m_sKind;
			inst.m_sSource = sourceKey;
			inst.m_Prefab = resolved.m_Prefab;
			inst.m_Entity = entity;
			inst.m_vPos = spot;
			inst.m_fRadius = radius;
			s_aInst.Insert(inst);

			array<IEntity> tree = {};
			CollectHierarchy(entity, tree);
			AdoptFaction(tree, sourceKey, sideKey);
			DisableSpawnPoints(tree);
			if (entry.m_sKind != CLW_ServiceKind.FORTIFICATION)
				CLW_StructureMarkers.Add(sideKey, fobId, entry.m_sKind, spot, entry.ShowsMapMarker());

			spawned++;
			Print(string.Format("CLW: services fob kit %1 %2 %3 at %4 yaw=%5 (%6)", sideKey, fobId, role, spot, Math.Round(yaw), propSource), LogLevel.NORMAL);
		}

		if (spawned > 0)
		{
			GetGame().GetCallqueue().CallLater(LinkBase, LINK_DELAY_MS, false, sideKey, fobId, 0);
			GetGame().GetCallqueue().CallLater(SetupArsenals, ARSENAL_DELAY_MS, false, sideKey, fobId);
		}

		Print(string.Format("CLW: services fob kit %1 %2: %3 of %4 pieces placed around %5 (kit of %6)", sideKey, fobId, spawned, kit.Count(), center, kitSource), LogLevel.NORMAL);
		return spawned;
	}

	protected static bool SpawnOne(string sideKey, string baseId, vector anchor, CLW_ServiceEntry entry, string source, float keepClear, notnull array<vector> placedPos, notnull array<float> placedRad)
	{
		// the prefab of the role comes from the resolver (single choke point, CLW_PropResolver); the entry only carries the placement data
		string propSource;
		string propKey;
		ResourceName prefabName;
		CLW_ServiceEntry resolved = CLW_PropResolver.ResolveEntry(sideKey, CLW_PropResolver.SERVICE_PREFIX + entry.m_sKind, propSource, propKey);
		if (resolved)
			prefabName = resolved.m_Prefab;

		if (prefabName.IsEmpty())
			prefabName = entry.m_Prefab;
		else if (!propKey.IsEmpty())
			source = propKey;	// the faction the prefab really came from (an override or a set: "*" = whatever the prefab says)

		Resource resource = Resource.Load(prefabName);
		if (!resource || !resource.IsValid())
		{
			Print(string.Format("CLW: services ERROR %1 %2: cannot load prefab %3", sideKey, entry.m_sKind, prefabName), LogLevel.ERROR);
			return false;
		}

		float radius = entry.m_fRadius;
		if (radius <= 0)
			radius = CLW_ServiceKind.DefaultRadius(entry.m_sKind);

		// footprint: data override, else the extent of the prefab's child objects, else a square from the radius (no test spawn)
		float height = CLW_SpotConfig.COMPOSITION_HEIGHT_M;
		if (entry.m_sKind == CLW_ServiceKind.ARSENAL)
			height = 2.5;
		CLW_Footprint foot = CLW_FreeSpot.FootprintOf(prefabName, entry.m_vFootprint, radius, height);
		radius = Math.Max(radius, foot.Radius());
		Print(string.Format("CLW: place %1 %2 %3 footprint half %4 x %5 (offset %6 %7) height %8 from %9", sideKey, baseId, entry.m_sKind, Math.Round(foot.m_fHalfX * 10) / 10, Math.Round(foot.m_fHalfZ * 10) / 10, Math.Round(foot.m_fOffsetX), Math.Round(foot.m_fOffsetZ), foot.m_fHeight, foot.m_sSource), LogLevel.NORMAL);

		CLW_PlaceTuning tune = TuningFor(entry);
		vector spot;
		string via;
		float yaw = entry.m_fYaw;
		string placeLabel = sideKey + " " + baseId + " " + entry.m_sKind;
		s_sPlaceWhy = "";
		bool pinned;
		if (entry.m_vOffset[0] != 0 || entry.m_vOffset[2] != 0)
		{
			// authored offset first: the exact spot, then a short search around it (strict, then relaxed), only then the ring search
			vector pin = Vector(anchor[0] + entry.m_vOffset[0], 0, anchor[2] + entry.m_vOffset[2]);
			string pinWhy;
			for (int pinPass = 0; pinPass < 2 && !pinned; pinPass++)
			{
				pinned = SearchPass(placeLabel + " (offset)", anchor, pin, 0, tune.m_fPinSlack, pinPass, foot, tune, entry.m_fYaw, placedPos, placedRad, spot, yaw, pinWhy);
			}

			if (pinned)
			{
				via = "offset";
				float pinShift = vector.DistanceXZ(spot, pin);
				if (pinShift > 1.5)
					via = "offset+" + Math.Round(pinShift).ToString() + "m";
			}
			else
			{
				s_sPlaceWhy = "authored offset blocked: " + pinWhy;
			}
		}

		if (!pinned)
			via = FindSpot(placeLabel, anchor, keepClear, foot, tune, entry.m_fYaw, placedPos, placedRad, spot, yaw);

		float anchorDist = vector.DistanceXZ(spot, anchor);
		if (anchorDist > tune.m_fCompactMax)
			Print(string.Format("CLW: place %1 went %2 m from the base point (compact limit %3 m) via=%4: %5", placeLabel, Math.Round(anchorDist), Math.Round(tune.m_fCompactMax), via, s_sPlaceWhy), LogLevel.NORMAL);

		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		Math3D.AnglesToMatrix(Vector(yaw, 0, 0), params.Transform);
		params.Transform[3] = spot;
		IEntity entity = GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params);
		if (!entity)
		{
			Print(string.Format("CLW: services ERROR %1 %2: spawn failed (%3)", sideKey, entry.m_sKind, prefabName), LogLevel.ERROR);
			return false;
		}

		CLW_ServiceInstance inst = new CLW_ServiceInstance();
		inst.m_sSide = sideKey;
		inst.m_sBase = baseId;
		inst.m_sKind = entry.m_sKind;
		inst.m_sSource = source;
		inst.m_Prefab = prefabName;
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
		CLW_StructureMarkers.Add(sideKey, baseId, entry.m_sKind, spot, entry.ShowsMapMarker());	// map marker (own side, important structures only) + dashboard structures (all)
		CLW_RequestTerminals.OnServicePlaced(sideKey, baseId, entry.m_sKind, spot, radius);	// M3 phase Q: request terminal next to the command post / depot
		Print(string.Format("CLW: services placed %1 %2 %3 at %4 yaw=%5 r=%6 via=%7 dist=%8 grid=%9 gap=%10 scale=%11", sideKey, baseId, entry.m_sKind, spot, Math.Round(yaw), radius, via, Math.Round(anchorDist), CLW_Places.Grid(spot), tune.m_fGap, tune.m_fOverlap), LogLevel.NORMAL);
		Print(string.Format("CLW: services   %1 %2 entities=%3 reaffiliated=%4 spawnPointsOff=%5 prefab=%6", sideKey, entry.m_sKind, tree.Count(), reaffiliated, spawnPoints, prefabName.GetPath()), LogLevel.NORMAL);
		return true;
	}

	// Max terrain grade over a service footprint per placement pass (strict, relaxed, lenient).
	protected static float PlaceGrade(int pass)
	{
		if (pass <= 0)
			return 0.08;
		if (pass == 1)
			return 0.14;

		return 0.25;
	}

	// One search pass around 'center' between two radii. Pass 0 strict (8 % grade over the footprint), pass 1 relaxed (wider, trees ignored,
	// 14 %), pass 2 lenient (25 %, objects still block). Roads keep their clearance in every pass. 'why' receives the reject summary on failure.
	protected static bool SearchPass(string label, vector anchor, vector center, float minRadius, float maxRadius, int pass, CLW_Footprint foot, CLW_PlaceTuning tune, float fixedYaw, notnull array<vector> placedPos, notnull array<float> placedRad, out vector spot, out float yaw, out string why)
	{
		float footRadius = foot.Radius();
		CLW_SpotParams p = new CLW_SpotParams();
		p.m_sLabel = label;
		if (pass == 1)
			p.m_sLabel = label + " (relaxed)";
		else if (pass >= 2)
			p.m_sLabel = label + " (lenient)";

		p.m_vCenter = center;
		p.m_vFaceTarget = anchor;
		p.SetFootprint(foot);
		p.m_fMinRadius = minRadius;
		p.m_fMaxRadius = maxRadius;
		p.m_fMaxHeightRange = Math.Max(1.2, 2 * footRadius * PlaceGrade(pass));	// height range over the footprint diameter
		p.m_fFixedYaw = fixedYaw;
		if (pass >= 1)
		{
			p.m_fMaxRadius = p.m_fMaxRadius * 1.6;
			p.m_bBlockTrees = false;
			p.m_fMargin = 0.3;
		}

		foreach (int i, vector other : placedPos)
		{
			p.AddAvoid(other, (footRadius + placedRad[i]) * tune.m_fOverlap + tune.m_fGap);
		}

		CLW_SpotResult res = new CLW_SpotResult();
		if (CLW_FreeSpot.FindFreeSpot(p, res))
		{
			spot = res.m_vPos;
			yaw = res.m_fYaw;
			return true;
		}

		why = "pass " + pass.ToString() + " " + res.Summary();
		return false;
	}

	// Free spot around the anchor for a footprint: starts right beside the anchor (footprint radius + PLACE_INNER_M) and grows outward in
	// ring batches, so a service lands as close to the HQ point as the ground allows: clear of the placed services (+ gap), dry, flat and with
	// nothing solid under the footprint (CLW_FreeSpot). Passes: strict, relaxed, lenient (see SearchPass; DS e2e 2026-10-12: a fixed
	// 1.2-3.7 m height range rejected 95 % of Arland). The last resort is a best-effort search that ignores only the slope (road + object
	// clearance kept, "slope-ignored"); a blind forced spot on the minimum ring (logged as FORCED) only when even that fails.
	// s_sPlaceWhy receives what blocked the nearer passes. Returns how the spot was found.
	protected static string FindSpot(string label, vector anchor, float keepClear, CLW_Footprint foot, CLW_PlaceTuning tune, float fixedYaw, notnull array<vector> placedPos, notnull array<float> placedRad, out vector spot, out float yaw)
	{
		float footRadius = foot.Radius();
		float minDist = footRadius + Math.Min(tune.m_fInner, keepClear);
		if (tune.m_bTouch)
			minDist = Math.Max(foot.m_fHalfX, foot.m_fHalfZ) + PLACE_TENT_RADIUS_M + PLACE_TENT_GAP_M;	// the tent (spawn point) at the anchor, the composition right next to it
		if (tune.m_fRingMin > 0)
			minDist = tune.m_fRingMin;

		float maxDist = minDist + tune.m_fRing + (PLACE_BATCHES - 1) * tune.m_fGrow;
		if (tune.m_fRingMax > 0)
			maxDist = Math.Max(tune.m_fRingMax, minDist + 3);

		Print(string.Format("CLW: place %1 search ring %2..%3 m from the base point (gap %4, touch=%5, foot %6 x %7)", label, Math.Round(minDist * 10) / 10, Math.Round(maxDist * 10) / 10, tune.m_fGap, tune.m_bTouch, Math.Round(foot.m_fHalfX * 10) / 10, Math.Round(foot.m_fHalfZ * 10) / 10), LogLevel.NORMAL);
		string why;
		for (int pass = 0; pass < 3; pass++)
		{
			if (SearchPass(label, anchor, anchor, minDist, maxDist, pass, foot, tune, fixedYaw, placedPos, placedRad, spot, yaw, why))
			{
				if (pass == 0)
					return "free";
				if (pass == 1)
					return "relaxed";

				return "lenient";
			}

			if (!s_sPlaceWhy.IsEmpty())
				s_sPlaceWhy += "; ";
			s_sPlaceWhy += why;
		}

		// best effort: only the slope limit is dropped (object + road + water clearance kept), wider ring
		CLW_SpotParams bp = new CLW_SpotParams();
		bp.m_sLabel = label + " (slope ignored)";
		bp.m_vCenter = anchor;
		bp.SetFootprint(foot);
		bp.m_fMinRadius = minDist;
		bp.m_fMaxRadius = maxDist * 2.2;
		bp.m_fMaxHeightRange = 100000;
		bp.m_fFixedYaw = fixedYaw;
		bp.m_bBlockTrees = false;
		bp.m_fMargin = 0.3;
		bp.m_iMaxTries = 1200;
		foreach (int bi, vector bother : placedPos)
		{
			bp.AddAvoid(bother, (footRadius + placedRad[bi]) * tune.m_fOverlap + tune.m_fGap);
		}

		CLW_SpotResult bres = new CLW_SpotResult();
		if (CLW_FreeSpot.FindFreeSpot(bp, bres))
		{
			spot = bres.m_vPos;
			yaw = bres.m_fYaw;
			Print(string.Format("CLW: place %1 steep ground accepted at %2 (range %3 m): object and road clearance kept", label, spot, Math.Round(bres.m_fRange * 10) / 10), LogLevel.WARNING);
			return "slope-ignored";
		}

		// forced: a spot on the minimum ring that does not overlap, rotated by the number already placed
		float forcedAngle = placedPos.Count() * 1.1;
		spot = anchor + Vector(Math.Cos(forcedAngle) * (minDist + placedPos.Count() * 6), 0, Math.Sin(forcedAngle) * (minDist + placedPos.Count() * 6));
		CLW_Placement.SurfaceAt(spot);
		yaw = fixedYaw;
		if (yaw < 0)
			yaw = CLW_Placement.YawFromDir(anchor[0] - spot[0], anchor[2] - spot[2]);

		Print(string.Format("CLW: place %1 FORCED at %2 (no clear ground found: the composition may overlap objects)", label, spot), LogLevel.WARNING);
		return "forced";
	}

	protected static bool Overlaps(vector pos, float radius, notnull array<vector> placedPos, notnull array<float> placedRad)
	{
		foreach (int i, vector other : placedPos)
		{
			if (vector.DistanceXZ(pos, other) < radius + placedRad[i] + PLACE_GAP_M * PLACE_SCALE)
				return true;
		}

		return false;
	}

	// Distances for one service: the constants times the scale of its kind (depot 1.0, everything else 0.7), the per-kind gap, the profile's
	// own gap / ring when authored.
	protected static CLW_PlaceTuning TuningFor(notnull CLW_ServiceEntry entry)
	{
		float k = PLACE_SCALE;
		if (entry.m_sKind == CLW_ServiceKind.VEHICLE_DEPOT)
			k = PLACE_SCALE_DEPOT;

		CLW_PlaceTuning t = new CLW_PlaceTuning();
		t.m_sKind = entry.m_sKind;
		t.m_fGap = PLACE_GAP_M * k;
		if (entry.m_sKind == CLW_ServiceKind.ARSENAL)
			t.m_fGap = PLACE_GAP_ARSENAL_M;
		else if (entry.m_sKind == CLW_ServiceKind.SUPPLY)
			t.m_fGap = PLACE_GAP_SUPPLY_M;

		if (entry.m_fGapM >= 0)
			t.m_fGap = entry.m_fGapM;

		t.m_fOverlap = PLACE_OVERLAP_FACTOR * k;
		t.m_fInner = PLACE_INNER_M * k;
		t.m_fRing = PLACE_RING_M * k;
		t.m_fGrow = PLACE_RING_GROW_M * k;
		t.m_fPinSlack = PLACE_PIN_SLACK_M * k;
		t.m_fCompactMax = PLACE_COMPACT_MAX_M * k;
		t.m_fRingMin = entry.m_fRingMinM;
		t.m_fRingMax = entry.m_fRingMaxM;
		t.m_bTouch = entry.m_sKind == CLW_ServiceKind.COMMAND_POST;
		return t;
	}

	// ---- data resolution ----

	// Fixed spawn order: the building that defines the base first, then the arsenal (closest to it), stock, depot, repair.
	protected static void OrderedKinds(notnull array<string> order)
	{
		order.Insert(CLW_ServiceKind.COMMAND_POST);
		order.Insert(CLW_ServiceKind.ARSENAL);
		order.Insert(CLW_ServiceKind.SUPPLY);
		order.Insert(CLW_ServiceKind.VEHICLE_DEPOT);
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

		// no profile in the chain has services (a pack faction without a profile or a friendly vanilla one): admin overrides, shipped
		// pack sets and the vanilla set can still supply the base services (CLW_PropResolver.Synthesize, M4 WP3)
		if (CLW_PropResolver.Synthesize(sideKey, entries))
			return CLW_PropResolver.SYNTH_SOURCE;

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
	static int AdoptFaction(notnull array<IEntity> tree, string sourceKey, string sideKey)
	{
		if (sourceKey == sideKey)
			return 0;

		FactionManager manager = GetGame().GetFactionManager();
		if (!manager)
			return 0;

		Faction side = manager.GetFactionByKey(sideKey);
		if (!side)
			return 0;

		// "*" = the prefab came from an admin override or a style set: every affiliation with another default faction moves to the side
		bool any = sourceKey == "*";
		int changed;
		foreach (IEntity entity : tree)
		{
			FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(entity.FindComponent(FactionAffiliationComponent));
			if (!affiliation)
				continue;

			string defaultKey = affiliation.GetDefaultFactionKey();
			if (any)
			{
				if (defaultKey.IsEmpty() || defaultKey == sideKey)
					continue;
			}
			else if (defaultKey != sourceKey)
				continue;

			affiliation.SetAffiliatedFaction(side);
			changed++;
		}

		return changed;
	}

	// The HQ composition brings its own supplies spawn point; our one base spawn point is the only one in play.
	// Deleted, not disabled: a disabled point still shows in the deploy menu and was preselected ("spawn point disabled",
	// owner could not spawn, DS e2e 2026-10-12).
	protected static int DisableSpawnPoints(notnull array<IEntity> tree)
	{
		int off;
		foreach (IEntity entity : tree)
		{
			SCR_SpawnPoint point = SCR_SpawnPoint.Cast(entity);
			if (!point)
				continue;

			point.SetSpawnPointEnabled_S(false);
			SCR_EntityHelper.DeleteEntityAndChildren(point);
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

					// pack boxes may ship with a zero buy multiplier (RHS arsenal boxes): kits would be free although kit cost is on
					SCR_ResourceConsumer kitConsumer = resources.GetConsumer(EResourceGeneratorID.DEFAULT, EResourceType.SUPPLIES);
					if (kitCost && kitConsumer && kitConsumer.GetBuyMultiplier() <= 0)
					{
						kitConsumer.SetBuyMultiplier(1);
						cost += ", buy multiplier 0 -> 1";
					}
				}

				Print(string.Format("CLW: services arsenal %1 %2 at %3: kitCost=%4 (%5) saveType=FRIENDLY_AND_FACTION_ITEMS_ONLY service=%6 prefab=%7", sideKey, baseId, inst.m_vPos, kitCost, cost, inst.m_sKind, inst.m_Prefab.GetPath()), LogLevel.NORMAL);
				GetGame().GetCallqueue().CallLater(LogArsenal, ARSENAL_LOG_MS, false, sideKey, entity, inst.m_sKind + " " + inst.m_Prefab.GetPath() + " at " + inst.m_vPos.ToString());
			}
		}

		if (count == 0)
			Print(string.Format("CLW: services arsenal %1 %2: none in the services (kit cost: nothing to switch)", sideKey, baseId), LogLevel.NORMAL);
	}

	// Vehicle depot (owner playtest 2026-10-12: "request vehicle" showed vanilla vehicles for an RHS side). The vanilla spawner builds its
	// list from the catalog of the depot's AFFILIATED faction (SCR_CatalogEntitySpawnerComponent.SetCurrentFactionCatalog, refreshed on
	// OnFactionChanged), only serves requesters of that faction, and pays from its own SCR_ResourceComponent consumer (supplies containers in
	// range, the ones CLW_SupplyLink mirrors) when the prefab enables supply consumption (all vanilla depots do). AdoptFaction skips a depot
	// whose profile key equals the side, so a pack side that reuses the vanilla depot prefab kept the vanilla faction. Here the depot is
	// affiliated with the side whenever it is not already, the catalog is counted, and an empty catalog falls back to a kin faction that has one.
	protected static void SetupDepots(string sideKey, string baseId)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (!manager)
			return;

		Faction side = manager.GetFactionByKey(sideKey);
		array<CLW_ServiceInstance> insts = {};
		GetInstances(sideKey, baseId, insts);
		foreach (CLW_ServiceInstance inst : insts)
		{
			if (inst.m_sKind != CLW_ServiceKind.VEHICLE_DEPOT)
				continue;

			array<IEntity> tree = {};
			CollectHierarchy(inst.m_Entity, tree);
			foreach (IEntity depotEntity : tree)
			{
				SCR_CatalogEntitySpawnerComponent spawner = SCR_CatalogEntitySpawnerComponent.Cast(depotEntity.FindComponent(SCR_CatalogEntitySpawnerComponent));
				FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(depotEntity.FindComponent(FactionAffiliationComponent));
				if (!spawner || !affiliation)
					continue;

				string was = affiliation.GetDefaultFactionKey();
				if (affiliation.GetAffiliatedFaction())
					was = affiliation.GetAffiliatedFaction().GetFactionKey();

				if (side && affiliation.GetAffiliatedFaction() != side)
					affiliation.SetAffiliatedFaction(side);

				string usedKey = sideKey;
				string depotSource = "side catalog";
				string listInfo;
				int vehicles = spawner.CLW_RebuildDepotList(listInfo);	// side catalog + friendly (kin) vehicle catalogs, the same list on every machine
				if (vehicles == 0 && side)
				{
					array<Faction> factions = {};
					manager.GetFactionsList(factions);
					foreach (Faction candidate : factions)
					{
						if (!candidate || candidate == side || !AreKin(sideKey, candidate.GetFactionKey()))
							continue;

						affiliation.SetAffiliatedFaction(candidate);
						int kinVehicles = spawner.CLW_RebuildDepotList(listInfo);
						if (kinVehicles > 0)
						{
							vehicles = kinVehicles;
							usedKey = candidate.GetFactionKey();
							depotSource = "kin fallback, the side has no vehicle catalog";
							break;
						}
					}

					if (vehicles == 0)
					{
						affiliation.SetAffiliatedFaction(side);
						spawner.CLW_RebuildDepotList(listInfo);
						depotSource = "NO vehicle catalog on the side or its kin";
					}
				}

				string supplyInfo = "no consumer";
				SCR_ResourceComponent resources = spawner.GetSpawnerResourceComponent();
				if (resources)
				{
					SCR_ResourceConsumer consumer = resources.GetConsumer(EResourceGeneratorID.DEFAULT, EResourceType.SUPPLIES);
					if (consumer)
						supplyInfo = "consumer sees " + consumer.GetAggregatedResourceValue().ToString();
				}

				CLW_SupplyLink link = CLW_SupplyLink.Find(sideKey, baseId);
				string linkInfo = "no supply link";
				if (link)
					linkInfo = "link stock " + link.GetStock().ToString();

				Print(string.Format("CLW: services depot %1 faction %2 catalog vehicles=%3 (source %4; prefab faction was %5) supplies: consumption=%6 %7, %8", sideKey, usedKey, vehicles, depotSource, was, spawner.IsSuppliesConsumptionEnabled(), supplyInfo, linkInfo), LogLevel.NORMAL);
				LogDepotList(sideKey, usedKey, spawner, listInfo);
			}
		}

		// P7: the consumers see the linked stock only after the supply link was polled; the read above is early (1.8 s), this one is the real check.
		GetGame().GetCallqueue().CallLater(CheckSupplies, SUPPLY_CHECK_MS - DEPOT_DELAY_MS, false, sideKey, baseId);
	}

	// Depot list summary: how many vehicles, from which source, how many of them are in the side pool (what /request vehicle buys from).
	protected static void LogDepotList(string sideKey, string usedKey, notnull SCR_CatalogEntitySpawnerComponent spawner, string listInfo)
	{
		array<string> poolPaths = {};
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (catalog)
		{
			array<CLW_ForceEntry> entries = {};
			catalog.GetEntries(sideKey, entries);
			foreach (CLW_ForceEntry poolEntry : entries)
			{
				if (!poolEntry.HasVehicle())
					continue;

				string path = CLW_ForceCatalog.PathOf(poolEntry.m_VehiclePrefab);
				if (!poolPaths.Contains(path))
					poolPaths.Insert(path);
			}
		}

		int total, inPool;
		SCR_EntityCatalogEntry listed = spawner.GetEntryAtIndex(total);
		while (listed)
		{
			if (poolPaths.Contains(CLW_ForceCatalog.PathOf(listed.GetPrefab())))
				inPool++;

			total++;
			listed = spawner.GetEntryAtIndex(total);
		}

		string source = "faction " + usedKey;
		if (spawner.CLW_GetKinCount() > 0)
			source = "sidepool";

		Print(string.Format("CLW: services depot %1 list vehicles=%2 source=%3 (pool %4, %5 of them listed) %6", sideKey, total, source, poolPaths.Count(), inPool, listInfo), LogLevel.NORMAL);
		if (spawner.CLW_GetKinCount() == 0 && spawner.CLW_GetKinFactions() > 0)
			Print(string.Format("CLW: services depot %1 custom list unsupported: %2 friendly faction(s) but none adds a vehicle the faction catalog lacks, the list stays the faction catalog", sideKey, spawner.CLW_GetKinFactions()), LogLevel.NORMAL);
	}

	// Depot / arsenal supply check, SUPPLY_CHECK_MS after the services were spawned (P7: the 1.8 s read said "consumer sees 0").
	protected static void CheckSupplies(string sideKey, string baseId)
	{
		CLW_SupplyLink link = CLW_SupplyLink.Find(sideKey, baseId);
		string linkInfo = "no supply link";
		if (link)
			linkInfo = "link stock " + link.GetStock().ToString();

		array<CLW_ServiceInstance> insts = {};
		GetInstances(sideKey, baseId, insts);
		int checks;
		foreach (CLW_ServiceInstance inst : insts)
		{
			if (inst.m_sKind != CLW_ServiceKind.VEHICLE_DEPOT && inst.m_sKind != CLW_ServiceKind.ARSENAL)
				continue;

			array<IEntity> tree = {};
			CollectHierarchy(inst.m_Entity, tree);
			foreach (IEntity entity : tree)
			{
				SCR_ResourceComponent resources = SCR_ResourceComponent.FindResourceComponent(entity);
				if (!resources)
					continue;

				SCR_ResourceConsumer consumer = resources.GetConsumer(EResourceGeneratorID.DEFAULT, EResourceType.SUPPLIES);
				if (!consumer)
					continue;

				checks++;
				Print(string.Format("CLW: services supply check %1 %2 %3: consumer sees %4 (%5)", sideKey, baseId, inst.m_sKind, consumer.GetAggregatedResourceValue(), linkInfo), LogLevel.NORMAL);
			}
		}

		if (checks == 0)
			Print(string.Format("CLW: services supply check %1 %2: no depot / arsenal consumer found (%3)", sideKey, baseId, linkInfo), LogLevel.NORMAL);
	}

	// Number of entries in the spawner's current asset list (the vanilla list is protected; GetEntryAtIndex returns null past the end).
	protected static int DepotCatalogCount(notnull SCR_CatalogEntitySpawnerComponent spawner)
	{
		int count;
		while (spawner.GetEntryAtIndex(count))
		{
			count++;
		}

		return count;
	}

	protected static void LogArsenal(string sideKey, IEntity entity, string what)
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

		Print(string.Format("CLW: services arsenal side %1 faction %2 items=%3 enabled=%4 usingSupplies=%5 service=%6", sideKey, factionKey, prefabs.Count(), arsenal.IsArsenalEnabled(), arsenal.IsArsenalUsingSupplies(), what), LogLevel.NORMAL);
	}

	protected static void PollLinks()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;

		CLW_SupplyLink.PollAll(war);
	}
}

// Vehicle depot list = the side pool (round 3, owner P2: the depot showed only the affiliated faction's catalog, mostly vanilla-looking vehicles
// for an RHS side). The vanilla spawner fills m_aAssetList from the affiliated faction's catalog in SetCurrentFactionCatalog; here the VEHICLE
// catalogs of the friendly (kin) factions are appended, own entries first, a vehicle model that is already listed is skipped (same rule as
// CLW_SidePool.IsDuplicate). The list is built from faction data only, so the server and every client build the same list: the request RPC
// sends an INDEX into this list (SCR_SpawnerRequestComponent.RequestCatalogEntitySpawn), a different list on the client would buy the wrong
// vehicle. Server-only exclusions (overrides.json "sides") therefore do not apply to the depot. Only while a war runs (CLW_ArsenalPool.IsActive),
// only for spawners that offer vehicles. Known cosmetic gap: the "vehicle ready" popup names the vehicle by its index in the player's own faction catalog.
modded class SCR_CatalogEntitySpawnerComponent
{
	protected int m_iCLWKinAdded;		// entries added from kin catalogs by the last build
	protected int m_iCLWKinFactions;	// friendly factions that were looked at
	protected bool m_bCLWRefreshQueued;

	protected override void SetCurrentFactionCatalog()
	{
		super.SetCurrentFactionCatalog();
		CLW_ExtendDepotList();
	}

	// Rebuilds the list now (server: after the depot was affiliated with its side). Returns the number of entries; info = what was merged.
	int CLW_RebuildDepotList(out string info)
	{
		SetCurrentFactionCatalog();
		info = string.Format("[own %1 + kin %2 from %3 friendly faction(s)]", m_aAssetList.Count() - m_iCLWKinAdded, m_iCLWKinAdded, m_iCLWKinFactions);
		return m_aAssetList.Count();
	}

	int CLW_GetKinCount()
	{
		return m_iCLWKinAdded;
	}

	int CLW_GetKinFactions()
	{
		return m_iCLWKinFactions;
	}

	protected void CLW_ExtendDepotList()
	{
		m_iCLWKinAdded = 0;
		m_iCLWKinFactions = 0;
		if (!m_aCatalogTypes || !m_aCatalogTypes.Contains(EEntityCatalogType.VEHICLE))
			return;

		if (!CLW_ArsenalPool.IsActive())
		{
			// a client under a Game Master takeover learns about the war a few seconds after the depot faction replicated: build once more later
			if (!Replication.IsServer() && !m_bCLWRefreshQueued)
			{
				m_bCLWRefreshQueued = true;
				GetGame().GetCallqueue().CallLater(CLW_RefreshDepotList, 20000, false);
			}

			return;
		}

		SCR_Faction faction = SCR_Faction.Cast(GetFaction());
		if (!faction)
			return;

		array<SCR_Faction> kin = {};
		CLW_ArsenalPool.GetKin(faction, kin);
		m_iCLWKinFactions = kin.Count();
		if (kin.IsEmpty())
			return;

		map<string, bool> seen = new map<string, bool>();
		foreach (SCR_EntityCatalogEntry own : m_aAssetList)
		{
			if (own)
				seen.Set(CLW_SidePool.BaseOf(own.GetPrefab()), true);
		}

		string detail;
		foreach (SCR_Faction kinFaction : kin)
		{
			SCR_EntityCatalog catalog = kinFaction.GetFactionEntityCatalogOfType(EEntityCatalogType.VEHICLE);
			if (!catalog)
				continue;

			array<SCR_EntityCatalogEntry> extra = {};
			array<typename> includedDataClasses = {};
			includedDataClasses.Insert(SCR_EntityCatalogSpawnerData);
			catalog.GetFullFilteredEntityList(extra, m_aAllowedLabels, m_aIgnoredLabels, includedDataClasses, null, m_bNeedAllLabels);
			int added;
			foreach (SCR_EntityCatalogEntry entry : extra)
			{
				if (!entry)
					continue;

				string baseName = CLW_SidePool.BaseOf(entry.GetPrefab());
				if (seen.Contains(baseName))
					continue;

				seen.Set(baseName, true);
				m_aAssetList.Insert(entry);
				added++;
			}

			m_iCLWKinAdded += added;
			detail += " " + kinFaction.GetFactionKey() + "=" + added;
			added = 0;
		}

		if (m_iCLWKinAdded > 0)
		{
			AssignUserActions();	// the actions were filled with the shorter list by the base call
			if (Replication.IsServer())
				Print(string.Format("CLW: services depot list merged %1 kin vehicle(s) into %2 own:%3", m_iCLWKinAdded, m_aAssetList.Count() - m_iCLWKinAdded, detail), LogLevel.NORMAL);
		}
	}

	protected void CLW_RefreshDepotList()
	{
		if (!GetOwner())
			return;

		SetCurrentFactionCatalog();
		Print(string.Format("CLW: services depot list (client refresh) entries=%1 kin=%2", m_aAssetList.Count(), m_iCLWKinAdded), LogLevel.NORMAL);
	}

	override protected void OnDelete(IEntity owner)
	{
		GetGame().GetCallqueue().Remove(CLW_RefreshDepotList);
		super.OnDelete(owner);
	}
}

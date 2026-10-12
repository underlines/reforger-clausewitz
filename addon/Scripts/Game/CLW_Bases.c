// Main bases and respawn points (spec section 3). Server only. Positions and the spawn point prefab per faction come
// from the scenario. Spawns one spawn point per faction at the base, disables the scene-placed ones of those factions.
class CLW_Bases
{
	protected ref array<string> m_aKeys = {};
	protected ref array<vector> m_aPos = {};
	protected ref array<SCR_SpawnPoint> m_aOwnSpawns = {};	// our own spawned points (same order as m_aKeys, may hold null)
	protected ref array<ref CLW_BasePlacement> m_aPlace = {};	// placement state per base (same order as m_aKeys)
	protected bool m_bStarted;
	protected float m_fSeparationScale = 1;		// battle size distance scale (ApplyLayout)
	protected float m_fAuthoredDistance;		// HQ distance of the authored (scenario) layout
	protected float m_fHqDistance;				// HQ distance after the layout
	protected string m_sBattleSize;
	static const float LAYOUT_MIN_CHANGE = 0.03;		// scale changes below 3 % keep the authored positions
	static const float LAYOUT_SITE_RADIUS_MIN_M = 120;	// site search radius around a scaled position: at least this
	static const float LAYOUT_SITE_RADIUS_FRAC = 0.08;	// ... or this fraction of the HQ distance
	static const float LAYOUT_HQ_MIN_FRAC = 0.6;		// the second HQ / a FOB keeps this fraction of the wanted distance from the first
	static const float LAYOUT_HUMAN_MOVE_M = 150;		// humans this close to the old HQ are carried to the new one

	void Init(notnull CLW_ScenarioConfig scenario)
	{
		if (!Replication.IsServer())
			return;

		m_bStarted = true;
		BaseWorld world = GetGame().GetWorld();
		foreach (CLW_FactionSetup setup : scenario.m_aFactions)
		{
			vector pos = setup.m_vBasePos;
			pos[1] = world.GetSurfaceY(pos[0], pos[2]);
			int nav;
			float roadDist;
			float score = EvaluateBase(setup.m_sKey, pos, nav, roadDist);
			bool water = IsWater(world, pos);
			if (water || roadDist < 0 || roadDist > CLW_WarConfig.PLACE_BASE_ROAD_WARN_M)
			{
				Print(string.Format("CLW: bases WARNING %1 base %2 is not usable (water=%3 navmesh=%4 nearestRoad=%5 m; surfaceY=%6 ocean=%7)", setup.m_sKey, pos, water, nav, roadDist, pos[1], world.GetOceanHeight(pos[0], pos[2])), LogLevel.WARNING);
				if (setup.m_vBaseFallbackPos != vector.Zero)
				{
					vector alt = setup.m_vBaseFallbackPos;
					alt[1] = world.GetSurfaceY(alt[0], alt[2]);
					int altNav;
					float altRoad;
					float altScore = EvaluateBase(setup.m_sKey + " fallback", alt, altNav, altRoad);
					if (IsWater(world, alt))
						Print(string.Format("CLW: bases WARNING %1 fallback base %2 is water too, keeping the main one", setup.m_sKey, alt), LogLevel.WARNING);
					else if (water || altScore < score)
					{
						pos = alt;
						nav = altNav;
						roadDist = altRoad;
						Print(string.Format("CLW: bases %1 using fallback base %2 (score %3 < %4)", setup.m_sKey, pos, altScore, score), LogLevel.NORMAL);
					}
					else
						Print(string.Format("CLW: bases %1 fallback base %2 is not better (score %3 >= %4), keeping the main one", setup.m_sKey, alt, altScore, score), LogLevel.NORMAL);
				}
			}

			m_aKeys.Insert(setup.m_sKey);
			m_aPos.Insert(pos);
			m_aOwnSpawns.Insert(null);
			m_aPlace.Insert(new CLW_BasePlacement(setup, pos));
			Print(string.Format("CLW: bases %1 base at %2: navmesh=%3 nearestRoad=%4 (%5)", setup.m_sKey, pos, nav, roadDist, setup.m_sBaseLabel), LogLevel.NORMAL);
		}

		DisableSceneSpawns();
		foreach (int i, CLW_FactionSetup spawnSetup : scenario.m_aFactions)
		{
			m_aOwnSpawns[i] = SpawnBasePoint(spawnSetup, m_aPos[i]);
		}

		// scene entities may finish initialising after us: second pass
		GetGame().GetCallqueue().CallLater(DisableSceneSpawns, CLW_WarConfig.SCENE_SPAWN_RECHECK_MS);
		// AI agents (needed to ask the navmesh) may not exist yet at Init: validate once more, log only
		GetGame().GetCallqueue().CallLater(RecheckBases, CLW_WarConfig.SCENE_SPAWN_RECHECK_MS * 2);
	}

	// Navmesh/road facts of a base position; returns a badness score (lower = better): 1000 off navmesh, + road distance
	// (unknown navmesh counts as fine, no road as 500). Logs nothing.
	protected float EvaluateBase(string label, vector pos, out int nav, out float roadDist)
	{
		vector snapped;
		nav = CLW_Placement.NavmeshCheck(pos, CLW_WarConfig.PLACE_BASE_NAVMESH_TOL_M, snapped);
		roadDist = CLW_Placement.NearestRoadDistance(pos);
		float score = 0;
		// the navmesh answer is logged only: the query is unreliable (unknown -1 at Init, 0 on a road junction); roads decide
		if (roadDist < 0)
			score += 500;
		else
			score += roadDist;

		return score;
	}

	protected void RecheckBases()
	{
		foreach (int i, string key : m_aKeys)
		{
			int nav;
			float roadDist;
			EvaluateBase(key, m_aPos[i], nav, roadDist);
			Print(string.Format("CLW: bases %1 base at %2: navmesh=%3 nearestRoad=%4 (recheck, navmesh -1 = unknown)", key, m_aPos[i], nav, roadDist), LogLevel.NORMAL);
			if (roadDist < 0 || roadDist > CLW_WarConfig.PLACE_BASE_ROAD_WARN_M)
				Print(string.Format("CLW: bases WARNING %1 base %2 is not usable (navmesh=%3 nearestRoad=%4 m): vehicles will use fallback ground", key, m_aPos[i], nav, roadDist), LogLevel.WARNING);
		}
	}

	// The scenario sides were assigned other factions (CLW_Sides.Apply): the base positions stay with the side slots,
	// the factions on them change. Re-keys our spawn points and disables the scene spawn points of the new factions.
	void Rekey(notnull CLW_ScenarioConfig scenario)
	{
		if (!Replication.IsServer() || !m_bStarted)
			return;

		foreach (int i, CLW_FactionSetup setup : scenario.m_aFactions)
		{
			if (i >= m_aKeys.Count())
				break;

			string oldKey = m_aKeys[i];
			m_aKeys[i] = setup.m_sKey;
			SCR_SpawnPoint own = m_aOwnSpawns[i];
			if (own)
			{
				own.SetFactionKey(setup.m_sKey);
				own.SetSpawnPointEnabled_S(true);
			}
			else if (!setup.m_SpawnPointPrefab.IsEmpty())
			{
				m_aOwnSpawns[i] = SpawnBasePoint(setup, m_aPos[i]);
			}

			Print(string.Format("CLW: bases side %1 re-keyed %2 -> %3 (spawn point %4)", i, oldKey, setup.m_sKey, m_aOwnSpawns[i]), LogLevel.NORMAL);
		}

		DisableSceneSpawns();
		GetGame().GetCallqueue().CallLater(DisableSceneSpawns, CLW_WarConfig.SCENE_SPAWN_RECHECK_MS);
	}

	// ---- battle layout (HQ separation scales with the battle size) ----

	// Called once when the war starts (CLW_WarMode.ApplyBattleLayout), after the sides are applied. The scenario positions are the
	// AUTHORED (full size) layout: both HQs move toward / away from their midpoint so that the distance becomes authored * scale
	// (never below minHq, never above the authored distance unless scale > 1), then each is snapped to a usable site (dry, flat,
	// near a road, navmesh) by CLW_Placement.FindSite. The hand-placed slots of a moved base belong to the authored position and are
	// dropped (automatic road spots / ground are used). Our spawn point and humans standing at the old HQ are moved along.
	// Outputs: mid = midpoint of the authored HQs, factor = applied scale (1 = nothing moved), for the objective layout.
	void ApplyLayout(notnull CLW_ScenarioConfig scenario, string sizeName, float scale, float minHq, out vector mid, out float factor)
	{
		factor = 1;
		if (!Replication.IsServer() || !m_bStarted || m_aKeys.Count() != 2 || scenario.m_aFactions.Count() != 2)
		{
			Print("CLW: bases layout skipped: bases not started or not two sides", LogLevel.WARNING);
			return;
		}

		vector authoredA = m_aPos[0];
		vector authoredB = m_aPos[1];
		mid = Vector((authoredA[0] + authoredB[0]) * 0.5, 0, (authoredA[2] + authoredB[2]) * 0.5);
		float authored = vector.DistanceXZ(authoredA, authoredB);
		m_fAuthoredDistance = authored;
		m_fHqDistance = authored;
		m_fSeparationScale = scale;
		m_sBattleSize = sizeName;
		if (authored < 1)
			return;

		float wanted = authored * scale;
		if (wanted < minHq)
			wanted = Math.Min(minHq, authored);

		float k = wanted / authored;
		Print(string.Format("CLW: bases layout battleSize=%1 distanceScale=%2 authoredHqDistance=%3 m wanted=%4 m (min %5 m)", sizeName, scale, Math.Round(authored), Math.Round(wanted), Math.Round(minHq)), LogLevel.NORMAL);
		if (Math.AbsFloat(k - 1) < LAYOUT_MIN_CHANGE)
		{
			Print(string.Format("CLW: bases layout battleSize=%1 chosenSeparation=%2 hqDistance=%3 m (authored positions kept)", sizeName, scale, Math.Round(authored)), LogLevel.NORMAL);
			return;
		}

		factor = k;
		float searchRadius = Math.Max(LAYOUT_SITE_RADIUS_MIN_M, wanted * LAYOUT_SITE_RADIUS_FRAC);
		array<vector> placed = {};
		array<vector> sites = {};
		foreach (int i, string key : m_aKeys)
		{
			vector target = mid + (m_aPos[i] - mid) * k;
			vector site;
			float roadDist;
			float score;
			bool ok = CLW_Placement.FindSite(target, searchRadius, placed, wanted * LAYOUT_HQ_MIN_FRAC, site, roadDist, score);
			if (!ok)
				Print(string.Format("CLW: bases WARNING %1 no good site near %2 (search %3 m): using the raw scaled position", key, target, Math.Round(searchRadius)), LogLevel.WARNING);
			Print(string.Format("CLW: bases layout %1 wish=%2 site=%3 moved=%4 m nearestRoad=%5 m score=%6", key, target, site, Math.Round(vector.DistanceXZ(site, target)), roadDist, score), LogLevel.NORMAL);
			placed.Insert(site);
			sites.Insert(site);
		}

		float actual = vector.DistanceXZ(sites[0], sites[1]);
		foreach (int j, string moveKey : m_aKeys)
		{
			MoveBase(scenario, j, sites[j]);
		}

		m_fHqDistance = actual;
		Print(string.Format("CLW: bases layout battleSize=%1 chosenSeparation=%2 hqDistance=%3 m (authored %4 m, wanted %5 m) %6 at %7, %8 at %9", sizeName, scale, Math.Round(actual), Math.Round(authored), Math.Round(wanted), m_aKeys[0], m_aPos[0], m_aKeys[1], m_aPos[1]), LogLevel.NORMAL);
		if (actual < wanted * 0.8 || actual > wanted * 1.2)
			Print(string.Format("CLW: bases WARNING hq distance %1 m is far from the wanted %2 m (terrain forced the sites)", Math.Round(actual), Math.Round(wanted)), LogLevel.WARNING);
	}

	// Moves base i to a new site: positions, placement state, own spawn point, humans standing at the old HQ.
	protected void MoveBase(notnull CLW_ScenarioConfig scenario, int i, vector site)
	{
		vector old = m_aPos[i];
		CLW_FactionSetup setup = scenario.m_aFactions[i];
		m_aPos[i] = site;
		setup.m_vBasePos = site;
		setup.m_aVehicleSlots = new array<ref CLW_SpawnSlot>();	// authored slots belong to the old position
		setup.m_aInfantrySlots = new array<ref CLW_SpawnSlot>();
		m_aPlace[i] = new CLW_BasePlacement(setup, site);
		SCR_SpawnPoint own = m_aOwnSpawns[i];
		if (own)
			own.SetOrigin(site);	// [verify] spawn point transform replicates to clients

		int moved = MoveHumans(old, site);
		Print(string.Format("CLW: bases %1 moved %2 -> %3 (slots dropped, spawn point moved=%4, humans moved=%5)", m_aKeys[i], old, site, own != null, moved), LogLevel.NORMAL);
	}

	// Players on foot within LAYOUT_HUMAN_MOVE_M of the old HQ keep their offset from it at the new HQ.
	protected int MoveHumans(vector oldPos, vector newPos)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return 0;

		int count;
		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			IEntity entity = players.GetPlayerControlledEntity(playerId);
			if (!entity || entity.GetParent())
				continue;

			vector origin = entity.GetOrigin();
			if (vector.DistanceXZ(origin, oldPos) > LAYOUT_HUMAN_MOVE_M)
				continue;

			vector target = newPos + Vector(origin[0] - oldPos[0], 0, origin[2] - oldPos[2]);
			target[1] = GetGame().GetWorld().GetSurfaceY(target[0], target[2]);
			entity.SetOrigin(target);	// [verify] enough for a character; the physics follow on the next frame
			count++;
		}

		return count;
	}

	// Distance scale of the chosen battle size (1 before the war started / when no layout was applied).
	float GetSeparationScale()
	{
		return m_fSeparationScale;
	}

	// An authored distance (m, e.g. FOB-to-HQ or between FOBs) scaled by the battle size: smaller battle, shorter distances.
	float ScaleDistance(float authoredMeters)
	{
		return authoredMeters * m_fSeparationScale;
	}

	// Current distance between the two HQs (m), 0 when unknown.
	float GetHqDistance()
	{
		if (m_fHqDistance > 0)
			return m_fHqDistance;

		if (m_aPos.Count() == 2)
			return vector.DistanceXZ(m_aPos[0], m_aPos[1]);

		return 0;
	}

	// HOOK for future FOBs: a usable site for a forward base of a faction, ScaleDistance(authoredDistanceM) from its HQ in the
	// direction of 'toward' (an objective, the enemy HQ, ...), dry / flat / near a road, away from the HQ itself. False = nothing found
	// (site is then the raw wish). The caller owns spawning; this only decides where.
	bool FindFobSite(string factionKey, float authoredDistanceM, vector toward, out vector site)
	{
		vector hq = GetBasePos(factionKey);
		if (hq == vector.Zero)
			return false;

		float dist = ScaleDistance(authoredDistanceM);
		float dx = toward[0] - hq[0];
		float dz = toward[2] - hq[2];
		float len = Math.Sqrt(dx * dx + dz * dz);
		if (len < 1)
			return false;

		vector wish = hq + Vector(dx / len * dist, 0, dz / len * dist);
		array<vector> avoid = {};
		avoid.Insert(hq);
		float roadDist;
		float score;
		bool ok = CLW_Placement.FindSite(wish, Math.Max(LAYOUT_SITE_RADIUS_MIN_M, dist * LAYOUT_SITE_RADIUS_FRAC), avoid, dist * LAYOUT_HQ_MIN_FRAC, site, roadDist, score);
		Print(string.Format("CLW: bases fob site for %1: authored %2 m scaled %3 m (battleSize=%4) -> %5 (found=%6)", factionKey, Math.Round(authoredDistanceM), Math.Round(dist), m_sBattleSize, site, ok), LogLevel.NORMAL);
		return ok;
	}

	// Base center (surface Y) of a faction; vector.Zero when unknown.
	vector GetBasePos(string factionKey)
	{
		int i = m_aKeys.Find(factionKey);
		if (i < 0)
			return vector.Zero;

		return m_aPos[i];
	}

	// Kept for other callers: an infantry spot (slot or free navmesh ground) within radius of the faction's base.
	// cylinderRadius/cylinderHeight are used when a vehicle-sized spot is wanted (>= 1.5 m radius) -> vehicle finder without yaw.
	bool FindSpawnSpot(string factionKey, float radius, out vector spot, float cylinderRadius = 0.5, float cylinderHeight = 2)
	{
		string via;
		if (cylinderRadius >= 1.5)
		{
			float yaw;
			return FindVehicleSpot(factionKey, radius, cylinderRadius, cylinderHeight, spot, yaw, via);
		}

		return FindInfantrySpot(factionKey, radius, spot, via);
	}

	// Vehicle parking spot: scenario slot -> road spot (aligned) -> navmesh ground. yaw in degrees. via: slot|road|navmesh|fallback.
	bool FindVehicleSpot(string factionKey, float radius, float cylinderRadius, float cylinderHeight, out vector spot, out float yaw, out string via)
	{
		int i = m_aKeys.Find(factionKey);
		if (i < 0 || i >= m_aPlace.Count())
			return false;

		return m_aPlace[i].FindVehicleSpot(radius, cylinderRadius, cylinderHeight, spot, yaw, via);
	}

	// Infantry spot: scenario slot -> free dry navmesh ground within radius of the base. via: slot|navmesh|fallback.
	bool FindInfantrySpot(string factionKey, float radius, out vector spot, out string via)
	{
		int i = m_aKeys.Find(factionKey);
		if (i < 0 || i >= m_aPlace.Count())
			return false;

		return m_aPlace[i].FindInfantrySpot(radius, spot, via);
	}

	// Our spawn point entity of a faction (null when none was spawned).
	SCR_SpawnPoint GetSpawnPoint(string factionKey)
	{
		int i = m_aKeys.Find(factionKey);
		if (i < 0)
			return null;

		return m_aOwnSpawns[i];
	}

	protected bool IsWater(BaseWorld world, vector pos)
	{
		return pos[1] <= world.GetOceanHeight(pos[0], pos[2]) + CLW_WarConfig.WATER_MARGIN_M;
	}

	protected SCR_SpawnPoint SpawnBasePoint(CLW_FactionSetup setup, vector pos)
	{
		if (setup.m_SpawnPointPrefab.IsEmpty())
			return null;

		Resource resource = Resource.Load(setup.m_SpawnPointPrefab);
		if (!resource || !resource.IsValid())
		{
			Print("CLW: bases spawn point prefab failed to load: " + setup.m_SpawnPointPrefab, LogLevel.ERROR);
			return null;
		}

		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = pos;
		SCR_SpawnPoint point = SCR_SpawnPoint.Cast(GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), params));
		if (!point)
		{
			Print("CLW: bases spawn point prefab is not a SCR_SpawnPoint: " + setup.m_SpawnPointPrefab, LogLevel.ERROR);
			return null;
		}

		point.SetFactionKey(setup.m_sKey);
		if (setup.m_fSpawnRadius > 0)
			point.SetSpawnRadius(setup.m_fSpawnRadius);
		point.SetSpawnPointEnabled_S(true);
		Print(string.Format("CLW: bases spawn point for %1 created at %2 radius=%3", setup.m_sKey, pos, point.GetSpawnRadius()), LogLevel.NORMAL);
		return point;
	}

	// Scene-placed spawn points of the scenario factions are switched off (ours stay on).
	protected void DisableSceneSpawns()
	{
		array<SCR_SpawnPoint> all = {};
		array<SCR_SpawnPoint> source = SCR_SpawnPoint.GetSpawnPoints();
		if (source)
			all.Copy(source);

		int disabled;
		foreach (SCR_SpawnPoint point : all)
		{
			if (!point || m_aOwnSpawns.Contains(point))
				continue;

			if (!m_aKeys.Contains(point.GetFactionKey()))
				continue;

			if (point.IsSpawnPointEnabled())
			{
				point.SetSpawnPointEnabled_S(false);
				disabled++;
			}
		}

		Print(string.Format("CLW: bases scene spawn points disabled=%1 (total spawn points %2)", disabled, all.Count()), LogLevel.NORMAL);
	}
}

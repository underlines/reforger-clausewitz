// Main bases and respawn points (spec section 3). Server only. Positions and the spawn point prefab per faction come
// from the scenario. Spawns one spawn point per faction at the base, disables the scene-placed ones of those factions.
class CLW_Bases
{
	protected ref array<string> m_aKeys = {};
	protected ref array<vector> m_aPos = {};
	protected ref array<SCR_SpawnPoint> m_aOwnSpawns = {};	// our own spawned points (same order as m_aKeys, may hold null)
	protected ref array<ref CLW_BasePlacement> m_aPlace = {};	// placement state per base (same order as m_aKeys)
	protected bool m_bStarted;

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

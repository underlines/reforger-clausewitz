// Main bases and respawn points (spec section 3). Server only. Positions and the spawn point prefab per faction come
// from the scenario. Spawns one spawn point per faction at the base, disables the scene-placed ones of those factions.
class CLW_Bases
{
	protected ref array<string> m_aKeys = {};
	protected ref array<vector> m_aPos = {};
	protected ref array<SCR_SpawnPoint> m_aOwnSpawns = {};	// our own spawned points (same order as m_aKeys, may hold null)
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
			if (IsWater(world, pos))
			{
				Print(string.Format("CLW: bases WARNING %1 base %2 is water/unsuitable (surfaceY=%3 ocean=%4)", setup.m_sKey, pos, pos[1], world.GetOceanHeight(pos[0], pos[2])), LogLevel.WARNING);
				if (setup.m_vBaseFallbackPos != vector.Zero)
				{
					vector alt = setup.m_vBaseFallbackPos;
					alt[1] = world.GetSurfaceY(alt[0], alt[2]);
					if (IsWater(world, alt))
						Print(string.Format("CLW: bases WARNING %1 fallback base %2 is water too, keeping the main one", setup.m_sKey, alt), LogLevel.WARNING);
					else
					{
						pos = alt;
						Print(string.Format("CLW: bases %1 using fallback base %2", setup.m_sKey, pos), LogLevel.NORMAL);
					}
				}
			}

			m_aKeys.Insert(setup.m_sKey);
			m_aPos.Insert(pos);
			m_aOwnSpawns.Insert(null);
			Print(string.Format("CLW: bases %1 base at %2 (%3)", setup.m_sKey, pos, setup.m_sBaseLabel), LogLevel.NORMAL);
		}

		DisableSceneSpawns();
		foreach (int i, CLW_FactionSetup spawnSetup : scenario.m_aFactions)
		{
			m_aOwnSpawns[i] = SpawnBasePoint(spawnSetup, m_aPos[i]);
		}

		// scene entities may finish initialising after us: second pass
		GetGame().GetCallqueue().CallLater(DisableSceneSpawns, CLW_WarConfig.SCENE_SPAWN_RECHECK_MS);
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

	// A free spot within radius of the faction's base: cylinder test via SCR_WorldTools, surface Y, never water.
	// Falls back to a spread random point when the helper finds nothing. Returns false if the base is unknown.
	bool FindSpawnSpot(string factionKey, float radius, out vector spot, float cylinderRadius = 0.5, float cylinderHeight = 2)
	{
		vector center = GetBasePos(factionKey);
		if (center == vector.Zero)
			return false;

		BaseWorld world = GetGame().GetWorld();
		vector found;
		if (SCR_WorldTools.FindEmptyTerrainPosition(found, center, radius, cylinderRadius, cylinderHeight))
		{
			found[1] = world.GetSurfaceY(found[0], found[2]);
			if (!IsWater(world, found))
			{
				spot = found;
				return true;
			}
		}

		for (int attempt = 0; attempt < 8; attempt++)
		{
			float angle = Math.RandomFloat(0, Math.PI2);
			float dist = Math.RandomFloat(0, radius);
			vector candidate = center + Vector(Math.Cos(angle) * dist, 0, Math.Sin(angle) * dist);
			candidate[1] = world.GetSurfaceY(candidate[0], candidate[2]);
			if (!IsWater(world, candidate))
			{
				spot = candidate;
				return true;
			}
		}

		Print("CLW: bases no free spot near " + factionKey + " base, using the center", LogLevel.WARNING);
		spot = center;
		return true;
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

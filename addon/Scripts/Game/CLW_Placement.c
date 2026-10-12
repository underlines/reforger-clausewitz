// Believable spawn placement (server). Vehicles are parked on the road next to the base, aligned with the road; infantry
// stands on free navmesh ground. All numbers are in CLW_WarConfig (PLACE_*); hand-placed spots come from the scenario
// (CLW_FactionSetup.m_aVehicleSlots / m_aInfantrySlots). Nothing here knows a prefab, a faction or a coordinate.
//
// APIs: AIWorld.GetRoadNetworkManager (RoadNetworkManager.GetRoadsInAABB / GetClosestRoad, BaseRoad.GetPoints),
// AIPathfindingComponent.GetClosestPositionOnNavmesh (taken from any live AI agent), SCR_WorldTools.TraceCilinderUtil /
// FindEmptyTerrainPosition, BaseWorld.GetSurfaceY / GetOceanHeight, QueryEntitiesBySphere.

// QueryEntitiesBySphere callback: counts vehicles and characters.
class CLW_PlacementQuery
{
	vector m_vOrigin;
	float m_fVehicleClear;
	float m_fCharClear;
	bool m_bBlocked;

	bool OnEntity(IEntity entity)
	{
		if (!entity || m_bBlocked)
			return true;

		float dist = Math.Sqrt(vector.DistanceSqXZ(entity.GetOrigin(), m_vOrigin));
		if (Vehicle.Cast(entity))
		{
			if (dist < m_fVehicleClear)
				m_bBlocked = true;
		}
		else if (ChimeraCharacter.Cast(entity))
		{
			if (dist < m_fCharClear)
				m_bBlocked = true;
		}

		return true;
	}
}

class CLW_Placement
{
	protected static AIPathfindingComponent s_Pathfinding;	// weak, from some live agent
	protected static AIAgent s_PathAgent;					// weak, the agent s_Pathfinding belongs to
	protected static bool s_bNavmeshCalibrated;				// the query answered "yes" for the agent's own position at least once
	protected static bool s_bUncalibratedLogged;

	static void SurfaceAt(inout vector pos)
	{
		pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
	}

	// Terrain at/below the ocean surface (pos[1] must be the surface height).
	static bool IsWater(vector pos)
	{
		return pos[1] <= GetGame().GetWorld().GetOceanHeight(pos[0], pos[2]) + CLW_WarConfig.WATER_MARGIN_M;
	}

	// Slope check from four surface samples 2 m around the point.
	static bool SlopeOk(vector pos)
	{
		BaseWorld world = GetGame().GetWorld();
		float dx = world.GetSurfaceY(pos[0] + 2, pos[2]) - world.GetSurfaceY(pos[0] - 2, pos[2]);
		float dz = world.GetSurfaceY(pos[0], pos[2] + 2) - world.GetSurfaceY(pos[0], pos[2] - 2);
		float slopeTan = Math.Sqrt(dx * dx + dz * dz) / 4;
		return slopeTan <= CLW_WarConfig.PLACE_MAX_SLOPE_TAN;
	}

	// ---- navmesh ----

	protected static AIPathfindingComponent GetPathfinding()
	{
		if (s_Pathfinding)
			return s_Pathfinding;

		AIWorld aiWorld = GetGame().GetAIWorld();
		if (!aiWorld)
			return null;

		array<AIAgent> agents = {};
		aiWorld.GetAIAgents(agents);
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			AIPathfindingComponent path = AIPathfindingComponent.Cast(agent.FindComponent(AIPathfindingComponent)); // [verify] component sits on the agent entity
			if (path)
			{
				s_Pathfinding = path;
				s_PathAgent = agent;
				s_bNavmeshCalibrated = false;
				return path;
			}
		}

		return null;
	}

	// 1 = a navmesh point lies within tolerance (snapped is that point), 0 = none within tolerance, -1 = cannot tell (no AI agent
	// exists yet, so no pathfinding component to ask). The navmesh asked is the one of that agent (soldiers); roads are walkable too.
	static int NavmeshCheck(vector pos, float tolerance, out vector snapped)
	{
		snapped = pos;
		AIPathfindingComponent path = GetPathfinding();
		if (!path)
			return -1;

		vector found;
		// calibration: a query at the agent's own position must find the navmesh, else the answer "0" is meaningless (navmesh not
		// loaded yet / not the layer we ask) and reported as unknown (-1)
		if (!s_bNavmeshCalibrated)
		{
			IEntity self;
			if (s_PathAgent)
				self = s_PathAgent.GetControlledEntity();
			vector selfFound;
			if (!self || !path.GetClosestPositionOnNavmesh(self.GetOrigin(), Vector(2.5, 6, 2.5), selfFound))
			{
				if (!s_bUncalibratedLogged)
				{
					s_bUncalibratedLogged = true;
					Print("CLW: place navmesh query not usable yet (agent's own position is not on the navmesh it reports): navmesh = unknown (-1)", LogLevel.NORMAL);
				}
				return -1;
			}

			s_bNavmeshCalibrated = true;
			s_bUncalibratedLogged = false;
			Print("CLW: place navmesh query calibrated (agent's own position found)", LogLevel.NORMAL);
		}

		vector half = Vector(tolerance, 6, tolerance);
		if (path.GetClosestPositionOnNavmesh(pos, half, found))
		{
			snapped = found;
			return 1;
		}

		return 0;
	}

	// ---- occupancy ----

	// Nothing solid in a cylinder at pos, no vehicle within vehicleClear and no character within charClear.
	static bool IsFree(vector pos, float cylRadius, float cylHeight, float vehicleClear, float charClear)
	{
		vector probe = pos;
		probe[1] = probe[1] + 0.3;
		if (SCR_WorldTools.TraceCilinderUtil(probe, cylRadius, cylHeight))
			return false;

		return CountAround(pos, vehicleClear, charClear);
	}

	// Occupancy only (no solid-geometry trace): hand-placed scenario slots are trusted to be clear ground.
	static bool IsUnoccupied(vector pos, float vehicleClear, float charClear)
	{
		return CountAround(pos, vehicleClear, charClear);
	}

	protected static bool CountAround(vector pos, float vehicleClear, float charClear)
	{
		float radius = Math.Max(vehicleClear, charClear);
		CLW_PlacementQuery query = new CLW_PlacementQuery();
		query.m_vOrigin = pos;
		query.m_fVehicleClear = vehicleClear;
		query.m_fCharClear = charClear;
		GetGame().GetWorld().QueryEntitiesBySphere(pos, radius, query.OnEntity, null, EQueryEntitiesFlags.DYNAMIC);
		return !query.m_bBlocked;
	}

	// ---- roads ----

	static RoadNetworkManager GetRoadManager()
	{
		AIWorld aiWorld = GetGame().GetAIWorld();
		ChimeraAIWorld chimera = ChimeraAIWorld.Cast(aiWorld);
		if (!chimera)
			return null;

		return chimera.GetRoadNetworkManager();
	}

	// Distance (m) from pos to the nearest road, -1 when there is none / no road network.
	static float NearestRoadDistance(vector pos)
	{
		RoadNetworkManager mgr = GetRoadManager();
		if (!mgr)
			return -1;

		BaseRoad road;
		float dist = -1;
		mgr.GetClosestRoad(pos, road, dist, false); // [verify] returns int (index/flag), distance is the out value
		if (!road)
			return -1;

		return dist;
	}

	// Squared XZ distance from point to segment a-b; t = position on the segment 0..1.
	static float SegmentDistSq(float px, float pz, float ax, float az, float bx, float bz, out float t)
	{
		float vx = bx - ax;
		float vz = bz - az;
		float len2 = vx * vx + vz * vz;
		t = 0;
		if (len2 > 0.0001)
			t = Math.Clamp(((px - ax) * vx + (pz - az) * vz) / len2, 0, 1);

		float cx = ax + vx * t - px;
		float cz = az + vz * t - pz;
		return cx * cx + cz * cz;
	}

	static float YawFromDir(float dx, float dz)
	{
		float yaw = Math.Atan2(dx, dz) * Math.RAD2DEG;
		while (yaw < 0)
			yaw += 360;
		while (yaw >= 360)
			yaw -= 360;

		return yaw;
	}

	// Heading of the road at pos (nearest road within 30 m), pointing away from 'awayFrom'. False when no road there.
	static bool RoadYawAt(vector pos, vector awayFrom, out float yaw)
	{
		RoadNetworkManager mgr = GetRoadManager();
		if (!mgr)
			return false;

		BaseRoad road;
		float dist = -1;
		mgr.GetClosestRoad(pos, road, dist, false);
		if (!road || dist > 30)
			return false;

		array<vector> pts = {};
		road.GetPoints(pts);
		if (pts.Count() < 2)
			return false;

		float bestSq = 1000000000;
		float bdx = 0;
		float bdz = 0;
		for (int i = 0; i < pts.Count() - 1; i++)
		{
			float t;
			float sq = SegmentDistSq(pos[0], pos[2], pts[i][0], pts[i][2], pts[i + 1][0], pts[i + 1][2], t);
			if (sq < bestSq)
			{
				bestSq = sq;
				bdx = pts[i + 1][0] - pts[i][0];
				bdz = pts[i + 1][2] - pts[i][2];
			}
		}

		if (bdx == 0 && bdz == 0)
			return false;

		if (bdx * (pos[0] - awayFrom[0]) + bdz * (pos[2] - awayFrom[2]) < 0)
		{
			bdx = -bdx;
			bdz = -bdz;
		}

		yaw = YawFromDir(bdx, bdz);
		return true;
	}

	// ---- humans ----

	// Distance (m, XZ) from pos to the nearest player-controlled entity (a player aboard a vehicle counts); 1e9 when there is none.
	static float NearestPlayerDistance(vector pos)
	{
		float best = 1000000000;
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return best;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			IEntity entity = players.GetPlayerControlledEntity(playerId);
			if (!entity)
				continue;

			float d = vector.DistanceXZ(pos, entity.GetOrigin());
			if (d < best)
				best = d;
		}

		return best;
	}

	// ---- unstuck spots ----

	// A free, dry spot for a stuck vehicle within [minDist, maxDist] of 'from' that lies TOWARD 'toward' (closer to it than 'from' by
	// at least minDist / 2). Roads first (best = closest to the target, yaw along the road pointing at the target), else free
	// navmesh ground in a cone toward the target. yaw = Enforce heading. False when nothing suitable exists.
	static bool FindSpotToward(vector from, vector toward, float minDist, float maxDist, float cylRadius, float cylHeight, out vector spot, out float yaw, out string via)
	{
		float baseDist = vector.DistanceXZ(from, toward);
		float dirX = toward[0] - from[0];
		float dirZ = toward[2] - from[2];
		float clear = Math.Max(CLW_WarConfig.PLACE_SLOT_CLEAR_M, cylRadius * 2);
		RoadNetworkManager mgr = GetRoadManager();
		bool found;
		float bestToTarget = 1000000000;
		if (mgr)
		{
			array<BaseRoad> roads = {};
			mgr.GetRoadsInAABB(Vector(from[0] - maxDist, -10000, from[2] - maxDist), Vector(from[0] + maxDist, 10000, from[2] + maxDist), roads);
			foreach (BaseRoad road : roads)
			{
				if (!road || road.GetWidth() < CLW_WarConfig.PLACE_ROAD_MIN_WIDTH_M)
					continue;

				array<vector> pts = {};
				road.GetPoints(pts);
				for (int i = 0; i < pts.Count() - 1; i++)
				{
					float dx = pts[i + 1][0] - pts[i][0];
					float dz = pts[i + 1][2] - pts[i][2];
					float len = Math.Sqrt(dx * dx + dz * dz);
					if (len < 0.5)
						continue;

					for (float d = 0; d < len; d += CLW_WarConfig.PLACE_ROAD_STEP_M)
					{
						vector p = Vector(pts[i][0] + dx * d / len, 0, pts[i][2] + dz * d / len);
						float away = vector.DistanceXZ(p, from);
						if (away < minDist || away > maxDist)
							continue;

						float toTarget = vector.DistanceXZ(p, toward);
						if (toTarget > baseDist - minDist * 0.5 || toTarget >= bestToTarget)
							continue;

						SurfaceAt(p);
						if (IsWater(p) || !SlopeOk(p) || !IsFree(p, cylRadius, cylHeight, clear, clear))
							continue;

						float hx = dx;
						float hz = dz;
						if (hx * (toward[0] - p[0]) + hz * (toward[2] - p[2]) < 0)
						{
							hx = -hx;
							hz = -hz;
						}

						bestToTarget = toTarget;
						spot = p;
						yaw = YawFromDir(hx, hz);
						found = true;
					}
				}
			}
		}

		if (found)
		{
			via = "road";
			return true;
		}

		// open ground in a cone toward the target
		float angle0 = Math.Atan2(dirX, dirZ);
		for (int attempt = 0; attempt < CLW_WarConfig.PLACE_GROUND_ATTEMPTS; attempt++)
		{
			float angle = angle0 + Math.RandomFloat(-0.7, 0.7);
			float dist = Math.RandomFloat(minDist, maxDist);
			vector candidate = from + Vector(Math.Sin(angle) * dist, 0, Math.Cos(angle) * dist);
			SurfaceAt(candidate);
			if (IsWater(candidate) || !SlopeOk(candidate) || !IsFree(candidate, cylRadius, cylHeight, clear, clear))
				continue;

			if (vector.DistanceXZ(candidate, toward) > baseDist - minDist * 0.5)
				continue;

			vector snapped;
			if (NavmeshCheck(candidate, CLW_WarConfig.PLACE_NAVMESH_TOL_M, snapped) == 0)
				continue;

			spot = candidate;
			yaw = YawFromDir(toward[0] - candidate[0], toward[2] - candidate[2]);
			via = "ground";
			return true;
		}

		return false;
	}

	// ---- ground ----

	// A free, dry, not-too-steep spot on the navmesh within 'radius' of 'center' (minRadius = inner ring, 0 = anywhere).
	// via = "navmesh" when the navmesh confirmed it, "fallback" when the navmesh could not be asked (no AI agent yet) or nothing
	// better existed. Always returns a position (the center as the very last resort, via "fallback").
	static bool FindGround(vector center, float minRadius, float radius, float cylRadius, float cylHeight, float charClear, out vector spot, out string via)
	{
		BaseWorld world = GetGame().GetWorld();
		vector unchecked;
		bool haveUnchecked;
		for (int attempt = 0; attempt < CLW_WarConfig.PLACE_GROUND_ATTEMPTS; attempt++)
		{
			float angle = Math.RandomFloat(0, Math.PI2);
			float dist = Math.Sqrt(Math.RandomFloat(minRadius * minRadius, radius * radius));
			vector candidate = center + Vector(Math.Cos(angle) * dist, 0, Math.Sin(angle) * dist);
			SurfaceAt(candidate);
			if (IsWater(candidate) || !SlopeOk(candidate))
				continue;

			if (!IsFree(candidate, cylRadius, cylHeight, charClear, charClear))
				continue;

			vector snapped;
			int nav = NavmeshCheck(candidate, CLW_WarConfig.PLACE_NAVMESH_TOL_M, snapped);
			if (nav == 1)
			{
				snapped[1] = world.GetSurfaceY(snapped[0], snapped[2]);
				if (!IsWater(snapped))
				{
					spot = snapped;
					via = "navmesh";
					return true;
				}
			}
			else if (nav == -1 && !haveUnchecked)
			{
				unchecked = candidate;
				haveUnchecked = true;
			}
		}

		if (haveUnchecked)
		{
			spot = unchecked;
			via = "fallback";
			return true;
		}

		// engine helper (cylinder test only), still never water
		vector found;
		if (SCR_WorldTools.FindEmptyTerrainPosition(found, center, radius, cylRadius, cylHeight))
		{
			SurfaceAt(found);
			if (!IsWater(found))
			{
				spot = found;
				via = "fallback";
				return true;
			}
		}

		spot = center;
		SurfaceAt(spot);
		via = "fallback";
		return false;
	}
}

// Placement state of one base: the optional scenario slots (round-robin cursors) and the automatic road spots (computed once).
class CLW_BasePlacement
{
	vector m_vCenter;
	CLW_FactionSetup m_Setup;	// weak, owned by the scenario
	ref array<vector> m_aRoadPos = {};
	ref array<float> m_aRoadYaw = {};
	protected int m_iVehicleCursor;
	protected int m_iInfantryCursor;
	protected int m_iRoadBuilds;

	void CLW_BasePlacement(CLW_FactionSetup setup, vector center)
	{
		m_Setup = setup;
		SetCenter(center);
	}

	// (Re)computes the road spots for a base center.
	void SetCenter(vector center)
	{
		m_vCenter = center;
		m_aRoadPos.Clear();
		m_aRoadYaw.Clear();
		BuildRoadSpots();
	}

	int GetRoadSpotCount()
	{
		return m_aRoadPos.Count();
	}

	// Road points within PLACE_ROAD_SEARCH_M, nearest road first (other roads count PLACE_ROAD_OFF_PENALTY_M further away), picked
	// greedily at least PLACE_ROAD_SPACING_M apart, each with the road direction pointing away from the base.
	protected void BuildRoadSpots()
	{
		m_iRoadBuilds++;
		RoadNetworkManager mgr = CLW_Placement.GetRoadManager();
		if (!mgr)
			return;

		float searchR = CLW_WarConfig.PLACE_ROAD_SEARCH_M;
		array<BaseRoad> roads = {};
		mgr.GetRoadsInAABB(Vector(m_vCenter[0] - searchR, -10000, m_vCenter[2] - searchR), Vector(m_vCenter[0] + searchR, 10000, m_vCenter[2] + searchR), roads);
		if (roads.IsEmpty())
			return;

		BaseRoad nearest;
		float nearestDist;
		mgr.GetClosestRoad(m_vCenter, nearest, nearestDist, false);

		// resample every usable road into candidate points
		array<vector> candPos = {};
		array<float> candYaw = {};
		array<float> candScore = {};
		float step = CLW_WarConfig.PLACE_ROAD_STEP_M;
		foreach (BaseRoad road : roads)
		{
			if (!road || road.GetWidth() < CLW_WarConfig.PLACE_ROAD_MIN_WIDTH_M)
				continue;

			float penalty = 0;
			if (road != nearest)
				penalty = CLW_WarConfig.PLACE_ROAD_OFF_PENALTY_M;

			array<vector> pts = {};
			road.GetPoints(pts);
			for (int i = 0; i < pts.Count() - 1; i++)
			{
				float dx = pts[i + 1][0] - pts[i][0];
				float dz = pts[i + 1][2] - pts[i][2];
				float len = Math.Sqrt(dx * dx + dz * dz);
				if (len < 0.5)
					continue;

				for (float d = 0; d < len; d += step)
				{
					vector p = Vector(pts[i][0] + dx * d / len, 0, pts[i][2] + dz * d / len);
					float dist = Math.Sqrt(vector.DistanceSqXZ(p, m_vCenter));
					if (dist > searchR)
						continue;

					float ddx = dx;
					float ddz = dz;
					if (ddx * (p[0] - m_vCenter[0]) + ddz * (p[2] - m_vCenter[2]) < 0)
					{
						ddx = -ddx;
						ddz = -ddz;
					}

					CLW_Placement.SurfaceAt(p);
					candPos.Insert(p);
					candYaw.Insert(CLW_Placement.YawFromDir(ddx, ddz));
					candScore.Insert(dist + penalty);
				}
			}
		}

		// greedy pick: lowest score first, at least SPACING from every picked spot
		float spacingSq = CLW_WarConfig.PLACE_ROAD_SPACING_M * CLW_WarConfig.PLACE_ROAD_SPACING_M;
		array<bool> used = {};
		used.Resize(candPos.Count());
		while (m_aRoadPos.Count() < CLW_WarConfig.PLACE_ROAD_MAX_SPOTS)
		{
			int best = -1;
			float bestScore = 1000000000;
			for (int c = 0; c < candPos.Count(); c++)
			{
				if (used[c] || candScore[c] >= bestScore)
					continue;

				bool tooClose;
				tooClose = false;
				foreach (vector taken : m_aRoadPos)
				{
					if (vector.DistanceSqXZ(taken, candPos[c]) < spacingSq)
					{
						tooClose = true;
						break;
					}
				}

				if (tooClose)
				{
					used[c] = true;
					continue;
				}

				if (CLW_Placement.IsWater(candPos[c]) || !CLW_Placement.SlopeOk(candPos[c]))
				{
					used[c] = true;
					continue;
				}

				best = c;
				bestScore = candScore[c];
			}

			if (best < 0)
				break;

			used[best] = true;
			m_aRoadPos.Insert(candPos[best]);
			m_aRoadYaw.Insert(candYaw[best]);
		}

		Print(string.Format("CLW: place %1 base %2: %3 road candidates -> %4 vehicle road spots (nearest road %5 m)", m_Setup.m_sKey, m_vCenter, candPos.Count(), m_aRoadPos.Count(), nearestDist), LogLevel.NORMAL);
	}

	// ---- vehicles ----

	// Scenario slots in order -> road spots (nearest first) -> free navmesh ground near the base. via: slot | road | navmesh | fallback.
	bool FindVehicleSpot(float radius, float cylRadius, float cylHeight, out vector pos, out float yaw, out string via)
	{
		float clear = Math.Max(CLW_WarConfig.PLACE_SLOT_CLEAR_M, cylRadius * 2);

		// the road network may not have been ready at Init: retry once on first use
		if (m_aRoadPos.IsEmpty() && m_iRoadBuilds < 2)
			BuildRoadSpots();

		// 1. scenario slots
		if (m_Setup.m_aVehicleSlots && !m_Setup.m_aVehicleSlots.IsEmpty())
		{
			int n = m_Setup.m_aVehicleSlots.Count();
			for (int k = 0; k < n; k++)
			{
				int idx = (m_iVehicleCursor + k) % n;
				CLW_SpawnSlot slot = m_Setup.m_aVehicleSlots[idx];
				if (!slot)
					continue;

				vector slotPos = slot.m_vPos;
				CLW_Placement.SurfaceAt(slotPos);
				// slots are trusted ground (data Y is 0, the surface Y is applied above): only vehicles / characters on it block
				if (!CLW_Placement.IsUnoccupied(slotPos, clear, CLW_WarConfig.PLACE_SLOT_CHAR_CLEAR_M))
				{
					Print(string.Format("CLW: place %1 vehicle slot %2 at %3 occupied, trying the next", m_Setup.m_sKey, idx, slotPos), LogLevel.NORMAL);
					continue;
				}

				m_iVehicleCursor = idx + 1;
				pos = slotPos;
				yaw = slot.m_fYaw;
				if (yaw < 0 && !CLW_Placement.RoadYawAt(slotPos, m_vCenter, yaw))
					yaw = CLW_Placement.YawFromDir(slotPos[0] - m_vCenter[0], slotPos[2] - m_vCenter[2]);
				via = "slot";
				return true;
			}
		}

		// 2. automatic road spots
		for (int r = 0; r < m_aRoadPos.Count(); r++)
		{
			vector roadPos = m_aRoadPos[r];
			if (!CLW_Placement.IsFree(roadPos, cylRadius, cylHeight, clear, clear))
				continue;

			vector snapped;
			if (CLW_Placement.NavmeshCheck(roadPos, CLW_WarConfig.PLACE_NAVMESH_TOL_M + 1, snapped) == 0)
			{
				Print(string.Format("CLW: place %1 road spot %2 at %3 skipped: off the navmesh", m_Setup.m_sKey, r, roadPos), LogLevel.NORMAL);
				continue;
			}

			pos = roadPos;
			yaw = m_aRoadYaw[r];
			via = "road";
			return true;
		}

		// 3. ground near the base
		vector ground;
		string groundVia;
		CLW_Placement.FindGround(m_vCenter, 0, radius, cylRadius, cylHeight, clear, ground, groundVia);
		pos = ground;
		yaw = CLW_Placement.YawFromDir(ground[0] - m_vCenter[0], ground[2] - m_vCenter[2]);
		via = groundVia;
		return true;
	}

	// ---- infantry ----

	// Scenario infantry slots (round-robin, skip occupied) -> navmesh ground within the radius. via: slot | navmesh | fallback.
	bool FindInfantrySpot(float radius, out vector pos, out string via)
	{
		if (m_Setup.m_aInfantrySlots && !m_Setup.m_aInfantrySlots.IsEmpty())
		{
			int n = m_Setup.m_aInfantrySlots.Count();
			for (int k = 0; k < n; k++)
			{
				int idx = (m_iInfantryCursor + k) % n;
				CLW_SpawnSlot slot = m_Setup.m_aInfantrySlots[idx];
				if (!slot)
					continue;

				vector slotPos = slot.m_vPos;
				CLW_Placement.SurfaceAt(slotPos);
				if (!CLW_Placement.IsFree(slotPos, 0.5, 2, CLW_WarConfig.PLACE_SLOT_CLEAR_M, CLW_WarConfig.PLACE_SLOT_CLEAR_M))
					continue;

				m_iInfantryCursor = idx + 1;
				pos = slotPos;
				via = "slot";
				return true;
			}
		}

		CLW_Placement.FindGround(m_vCenter, 0, radius, 0.5, 2, CLW_WarConfig.PLACE_CHAR_CLEAR_M, pos, via);
		return true;
	}
}

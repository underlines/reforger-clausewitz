// Random start positions (mode param startPositions = random). Picks a random axis and centre inside the playable area, puts the two
// HQs on it at the battle-size distance (down to a data tolerance below it, never above), validates each HQ with CLW_FreeSpot (dry, flat,
// clear core, free ring for the base services, road near, no dense town) and retries; the objectives are then carried over from the
// authored layout by a similarity transform (rotate + scale + move so the authored HQ line lands on the new one) and snapped to a
// nearby map place / kept / nudged. No coordinate, prefab or faction name in this code: everything content-specific is in
// CLW_RandomLayoutSetup (scenario data, defaults below). Server only. Log prefix: "CLW: layout".
//
// Reproducibility: all choices of this class come from a RandomGenerator seeded with the logged seed; the spot searches inside
// CLW_FreeSpot use the global random generator for their ring start angle, so the final HQ position can differ by a few metres
// between two runs with the same seed.

[BaseContainerProps()]
class CLW_RandomLayoutSetup
{
	[Attribute("350", UIWidgets.EditBox, "HQs stay this far (m) inside the terrain edge")]
	float m_fEdgeMarginM;

	[Attribute("0.15", UIWidgets.EditBox, "HQ distance = battle-size distance minus up to this fraction (never above it)")]
	float m_fDistanceTolerance;

	[Attribute("30", UIWidgets.EditBox, "Random axis / centre attempts before falling back to the fixed layout")]
	int m_iMaxAttempts;

	[Attribute("200", UIWidgets.EditBox, "An HQ may move this far (m) from its random wish to find clear ground")]
	float m_fHqSearchRadiusM;

	[Attribute("22", UIWidgets.EditBox, "Half size (m) of the clear square core of an HQ (spawn zone)")]
	float m_fHqCoreHalfM;

	[Attribute("0.12", UIWidgets.EditBox, "Max terrain grade (height range / width) over the HQ core and a service spot")]
	float m_fMaxGrade;

	[Attribute("1", UIWidgets.CheckBox, "An HQ needs a road within m_fHqRoadMaxM")]
	bool m_bRequireRoad;

	[Attribute("150", UIWidgets.EditBox, "Max distance (m) of the HQ to the nearest road")]
	float m_fHqRoadMaxM;

	[Attribute("7", UIWidgets.EditBox, "Half size (m) of the service footprint tested in the ring around the HQ")]
	float m_fServiceHalfM;

	[Attribute("40", UIWidgets.EditBox, "Service ring inner radius (m) around the HQ")]
	float m_fRingInnerM;

	[Attribute("130", UIWidgets.EditBox, "Service ring outer radius (m)")]
	float m_fRingOuterM;

	[Attribute("24", UIWidgets.EditBox, "Samples on the service ring")]
	int m_iRingSamples;

	[Attribute("8", UIWidgets.EditBox, "Samples that must be clear for the ring to count as enough room")]
	int m_iRingMinFree;

	[Attribute("150", UIWidgets.EditBox, "Radius (m) in which buildings are counted to keep an HQ out of a town")]
	float m_fTownRadiusM;

	[Attribute("30", UIWidgets.EditBox, "Max buildings within m_fTownRadiusM of an HQ (-1 = HQs may stand in towns)")]
	int m_iMaxBuildingsNear;

	[Attribute("1", UIWidgets.EditBox, "Of 7 samples along the HQ line this many may be water (sea / big lake)")]
	int m_iMaxWaterSamples;

	[Attribute("350", UIWidgets.EditBox, "An objective snaps to a map place of a snap type within this distance (m) of its transformed position")]
	float m_fSnapRadiusM;

	[Attribute(desc: "Map descriptor type texts (substring of e.g. name_village) an objective may snap to")]
	ref array<string> m_aSnapTypes;

	[Attribute("8", UIWidgets.EditBox, "Half size (m) of the footprint tested for an objective that is kept / nudged")]
	float m_fObjectiveHalfM;

	[Attribute("200", UIWidgets.EditBox, "An objective may be nudged this far (m) to clear ground")]
	float m_fNudgeRadiusM;

	// Same values as the attribute defaults, for a scenario without the block.
	void SetDefaults()
	{
		m_fEdgeMarginM = 350;
		m_fDistanceTolerance = 0.15;
		m_iMaxAttempts = 30;
		m_fHqSearchRadiusM = 200;
		m_fHqCoreHalfM = 22;
		m_fMaxGrade = 0.12;
		m_bRequireRoad = true;
		m_fHqRoadMaxM = 150;
		m_fServiceHalfM = 7;
		m_fRingInnerM = 40;
		m_fRingOuterM = 130;
		m_iRingSamples = 24;
		m_iRingMinFree = 8;
		m_fTownRadiusM = 150;
		m_iMaxBuildingsNear = 30;
		m_iMaxWaterSamples = 1;
		m_fSnapRadiusM = 350;
		m_fObjectiveHalfM = 8;
		m_fNudgeRadiusM = 200;
		FillSnapTypes();
	}

	void FillSnapTypes()
	{
		if (m_aSnapTypes && !m_aSnapTypes.IsEmpty())
			return;

		m_aSnapTypes = {"village", "town", "city", "settlement"};
	}
}

// QueryEntitiesBySphere callback: counts buildings.
class CLW_BuildingCounter
{
	int m_iCount;

	bool OnEntity(IEntity entity)
	{
		if (entity && Building.Cast(entity))
			m_iCount++;

		return true;
	}
}

class CLW_RandomLayout
{
	protected ref CLW_RandomLayoutSetup m_Cfg;
	protected ref RandomGenerator m_Rng = new RandomGenerator();
	protected static GenericTerrainEntity s_Terrain;
	protected static bool s_bTerrainSearched;

	int m_iSeed;
	int m_iAttempts;
	vector m_vHqA;
	vector m_vHqB;
	vector m_vCentre;
	float m_fAxisDeg;
	// authored -> new transform (valid after a successful Generate)
	vector m_vMid0;
	vector m_vMid;
	float m_fRotRad;
	float m_fScale = 1;

	// Creates the generator for a scenario; seed <= 0 = time based (a fresh random seed). Logs the seed.
	static CLW_RandomLayout Create(notnull CLW_ScenarioConfig scenario, int seed)
	{
		CLW_RandomLayout layout = new CLW_RandomLayout();
		layout.m_Cfg = scenario.m_RandomLayout;
		if (!layout.m_Cfg)
		{
			layout.m_Cfg = new CLW_RandomLayoutSetup();
			layout.m_Cfg.SetDefaults();
		}
		else
			layout.m_Cfg.FillSnapTypes();

		layout.m_iSeed = seed;
		string source = "param";
		if (layout.m_iSeed <= 0)
		{
			layout.m_iSeed = Math.RandomInt(1, 9999999);
			source = "time based";
		}

		layout.m_Rng.SetSeed(layout.m_iSeed);
		Print(string.Format("CLW: layout random seed=%1 (%2)", layout.m_iSeed, source), LogLevel.NORMAL);
		return layout;
	}

	// ---- playable area ----

	// Terrain bounds (cached terrain entity lookup), intersected with the world bound box; the world box alone when no terrain entity exists.
	static void GetPlayableBounds(out vector bmin, out vector bmax)
	{
		BaseWorld world = GetGame().GetWorld();
		world.GetBoundBox(bmin, bmax);
		if (!s_bTerrainSearched)
		{
			s_bTerrainSearched = true;
			for (int scene = 0; scene < world.GetNumSubScenes() && !s_Terrain; scene++)
			{
				int count = world.GetNumEntities(scene);
				for (int i = 0; i < count; i++)
				{
					GenericTerrainEntity terrain = GenericTerrainEntity.Cast(world.FindEntityByID(world.GetEntity(scene, i)));
					if (terrain)
					{
						s_Terrain = terrain;
						break;
					}
				}
			}
		}

		if (!s_Terrain)
			return;

		vector tMin, tMax;
		s_Terrain.GetTerrainBoundBox(tMin, tMax);
		if (tMax[0] <= tMin[0] || tMax[2] <= tMin[2])
			return;

		bmin[0] = Math.Max(bmin[0], tMin[0]);
		bmin[2] = Math.Max(bmin[2], tMin[2]);
		bmax[0] = Math.Min(bmax[0], tMax[0]);
		bmax[2] = Math.Min(bmax[2], tMax[2]);
	}

	// ---- HQ generation ----

	// Random HQ pair at 'wanted' metres (minus the tolerance, not below min(minHq, wanted)). authoredA / authoredB are the scenario HQs
	// (they define the transform for the objectives). False after m_iMaxAttempts failed attempts. On success m_vHqA / m_vHqB are set.
	bool Generate(vector authoredA, vector authoredB, float wanted, float minHq)
	{
		vector bmin, bmax;
		GetPlayableBounds(bmin, bmax);
		float margin = m_Cfg.m_fEdgeMarginM;
		float floorDist = Math.Min(minHq, wanted);
		Print(string.Format("CLW: layout random area %1 .. %2 margin %3 m, wanted HQ distance %4 m (floor %5 m, tolerance %6)", bmin, bmax, Math.Round(margin), Math.Round(wanted), Math.Round(floorDist), m_Cfg.m_fDistanceTolerance), LogLevel.NORMAL);
		// HQ A first on valid land (random wish inside the area), then HQ B at the wanted distance in up to DIRS directions from A:
		// the first version picked a random centre + axis and lost almost every attempt to the sea or to slopes on Arland
		// (DS test 2026-10-12: 0 of 30).
		int DIRS = 12;
		for (int attempt = 1; attempt <= m_Cfg.m_iMaxAttempts; attempt++)
		{
			m_iAttempts = attempt;
			vector wishA = Vector(m_Rng.RandFloatXY(bmin[0] + margin, bmax[0] - margin), 0, m_Rng.RandFloatXY(bmin[2] + margin, bmax[2] - margin));
			string why;
			vector posA;
			if (!FindHq("A", wishA, posA, why))
			{
				Print(string.Format("CLW: layout random attempt %1: HQ A wish %2 rejected: %3", attempt, wishA, why), LogLevel.NORMAL);
				continue;
			}

			float startHeading = m_Rng.RandFloatXY(0, Math.PI2);
			int tried;
			for (int d = 0; d < DIRS; d++)
			{
				float heading = startHeading + Math.PI2 * d / DIRS;
				float dist = wanted * (1 - m_Rng.RandFloatXY(0, m_Cfg.m_fDistanceTolerance));
				dist = Math.Max(dist, floorDist);
				vector wishB = posA + Vector(Math.Sin(heading) * dist, 0, Math.Cos(heading) * dist);
				if (wishB[0] < bmin[0] + margin || wishB[0] > bmax[0] - margin || wishB[2] < bmin[2] + margin || wishB[2] > bmax[2] - margin)
					continue;	// outside the playable area

				int wet = WaterSamples(posA, wishB);
				if (wet > m_Cfg.m_iMaxWaterSamples)
					continue;

				tried++;
				vector posB;
				if (!FindHq("B", wishB, posB, why))
					continue;

				float actual = vector.DistanceXZ(posA, posB);
				if (actual > wanted * 1.1 || actual < floorDist * 0.9)
					continue;

				m_vHqA = posA;
				m_vHqB = posB;
				m_vCentre = (posA + posB) * 0.5;
				m_fAxisDeg = heading * Math.RAD2DEG;
				SetTransform(authoredA, authoredB);
				Print(string.Format("CLW: layout random seed=%1 axis=%2 centre=%3 hqA=%4 hqB=%5 (attempts %6, direction %7 of %8)", m_iSeed, Math.Round(m_fAxisDeg), m_vCentre, m_vHqA, m_vHqB, m_iAttempts, d + 1, DIRS), LogLevel.NORMAL);
				Print(string.Format("CLW: layout random HQ distance %1 m (wanted %2 m), grid A %3 B %4", Math.Round(actual), Math.Round(wanted), CLW_Places.Grid(m_vHqA), CLW_Places.Grid(m_vHqB)), LogLevel.NORMAL);
				return true;
			}

			Print(string.Format("CLW: layout random attempt %1: HQ A ok at %2, no HQ B in %3 directions (%4 reached the HQ check, last: %5)", attempt, posA, DIRS, tried, why), LogLevel.NORMAL);
		}

		Print(string.Format("CLW: layout random seed=%1: no valid HQ pair in %2 attempts", m_iSeed, m_iAttempts), LogLevel.WARNING);
		return false;
	}

	protected int WaterSamples(vector a, vector b)
	{
		int wet;
		for (int i = 0; i < 7; i++)
		{
			float t = i / 6.0;
			vector sample = a + (b - a) * t;
			CLW_Placement.SurfaceAt(sample);
			if (CLW_Placement.IsWater(sample))
				wet++;
		}

		return wet;
	}

	// Clear, flat, dry HQ ground near 'wish' with room for the services and a road near; false (why = reason) when not.
	protected bool FindHq(string label, vector wish, out vector pos, out string why)
	{
		vector probe = wish;
		CLW_Placement.SurfaceAt(probe);
		if (CLW_Placement.IsWater(probe) || CLW_Placement.NearWater(probe, 25))
		{
			why = "water at the wish";
			return false;
		}

		float coreHalf = m_Cfg.m_fHqCoreHalfM;
		CLW_SpotParams p = new CLW_SpotParams();
		p.m_sLabel = "hq " + label;
		p.m_vCenter = wish;
		p.m_fMinRadius = 0;
		p.m_fMaxRadius = m_Cfg.m_fHqSearchRadiusM;
		p.m_fHalfX = coreHalf;
		p.m_fHalfZ = coreHalf;
		p.m_fHeight = 3;
		p.m_fMaxHeightRange = 2 * 1.414 * coreHalf * m_Cfg.m_fMaxGrade;
		p.m_fRoadWeight = 0.5;
		p.m_iMaxTries = 150;
		p.m_iValidTarget = 3;
		CLW_SpotResult res = new CLW_SpotResult();
		if (!CLW_FreeSpot.FindFreeSpot(p, res))
		{
			why = "no clear core ground (" + res.Summary() + ")";
			return false;
		}

		vector spot = res.m_vPos;
		if (CLW_Placement.NearWater(spot, 25))
		{
			why = "water near " + spot;
			return false;
		}

		float road = CLW_Placement.NearestRoadDistance(spot);
		if (m_Cfg.m_bRequireRoad && (road < 0 || road > m_Cfg.m_fHqRoadMaxM))
		{
			why = string.Format("nearest road %1 m at %2", Math.Round(road), spot);
			return false;
		}

		int free = CountFreeRing(spot);
		if (free < m_Cfg.m_iRingMinFree)
		{
			why = string.Format("service ring has %1 of %2 clear samples (need %3) at %4", free, m_Cfg.m_iRingSamples, m_Cfg.m_iRingMinFree, spot);
			return false;
		}

		if (m_Cfg.m_iMaxBuildingsNear >= 0)
		{
			CLW_BuildingCounter counter = new CLW_BuildingCounter();
			GetGame().GetWorld().QueryEntitiesBySphere(spot, m_Cfg.m_fTownRadiusM, counter.OnEntity, null, EQueryEntitiesFlags.STATIC);
			if (counter.m_iCount > m_Cfg.m_iMaxBuildingsNear)
			{
				why = string.Format("%1 buildings within %2 m at %3 (max %4)", counter.m_iCount, Math.Round(m_Cfg.m_fTownRadiusM), spot, m_Cfg.m_iMaxBuildingsNear);
				return false;
			}
		}

		pos = spot;
		Print(string.Format("CLW: layout random HQ %1 ok at %2 (wish moved %3 m, road %4 m, ring %5/%6 clear)", label, spot, Math.Round(vector.DistanceXZ(spot, wish)), Math.Round(road), free, m_Cfg.m_iRingSamples), LogLevel.NORMAL);
		return true;
	}

	// Number of clear service-sized spots among iRingSamples points on the annulus around the HQ.
	protected int CountFreeRing(vector center)
	{
		CLW_SpotParams p = new CLW_SpotParams();
		p.m_sLabel = "hq ring";
		p.m_fHalfX = m_Cfg.m_fServiceHalfM;
		p.m_fHalfZ = m_Cfg.m_fServiceHalfM;
		p.m_fHeight = CLW_SpotConfig.COMPOSITION_HEIGHT_M;
		p.m_fMargin = 0.5;
		p.m_fRoadClearance = 0;
		p.m_fMaxHeightRange = 2 * 1.414 * m_Cfg.m_fServiceHalfM * m_Cfg.m_fMaxGrade;
		int samples = Math.Max(1, m_Cfg.m_iRingSamples);
		float offset = m_Rng.RandFloatXY(0, Math.PI2);
		int free;
		for (int i = 0; i < samples; i++)
		{
			float angle = offset + Math.PI2 * i / samples;
			int band = i - (i / 3) * 3 + 1;	// 1..3 (no '%' inside a float expression: "Unknown operator '%'")
			float radius = m_Cfg.m_fRingInnerM + (m_Cfg.m_fRingOuterM - m_Cfg.m_fRingInnerM) * band / 3.0;
			vector cand = center + Vector(Math.Cos(angle) * radius, 0, Math.Sin(angle) * radius);
			float range;
			float surfaceY;
			string reason = CLW_FreeSpot.CheckPose(p, cand, 0, range, surfaceY);
			if (reason.IsEmpty())
				free++;
		}

		return free;
	}

	// ---- authored -> new transform (objectives) ----

	protected void SetTransform(vector authoredA, vector authoredB)
	{
		m_vMid0 = Vector((authoredA[0] + authoredB[0]) * 0.5, 0, (authoredA[2] + authoredB[2]) * 0.5);
		m_vMid = Vector((m_vHqA[0] + m_vHqB[0]) * 0.5, 0, (m_vHqA[2] + m_vHqB[2]) * 0.5);
		float oldDist = vector.DistanceXZ(authoredA, authoredB);
		float newDist = vector.DistanceXZ(m_vHqA, m_vHqB);
		m_fScale = 1;
		if (oldDist > 1)
			m_fScale = newDist / oldDist;

		float oldHeading = Math.Atan2(authoredB[0] - authoredA[0], authoredB[2] - authoredA[2]);
		float newHeading = Math.Atan2(m_vHqB[0] - m_vHqA[0], m_vHqB[2] - m_vHqA[2]);
		m_fRotRad = newHeading - oldHeading;
	}

	float GetScale()
	{
		return m_fScale;
	}

	// An authored position carried to the new HQ line: same place relative to the HQs (along and across the line, scaled).
	vector TransformPoint(vector authored)
	{
		float dx = (authored[0] - m_vMid0[0]) * m_fScale;
		float dz = (authored[2] - m_vMid0[2]) * m_fScale;
		float cs = Math.Cos(m_fRotRad);
		float sn = Math.Sin(m_fRotRad);
		vector result = Vector(m_vMid[0] + dx * cs + dz * sn, 0, m_vMid[2] - dx * sn + dz * cs);
		CLW_Placement.SurfaceAt(result);
		return result;
	}

	// ---- objectives ----

	// Decides where an objective goes: the authored position carried over by the transform, then (1) snapped to a nearby map place of a
	// snap type, (2) kept when its spot is clear, (3) nudged to clear ground within the data radius. how = "snapped to <name>" | "kept" |
	// "nudged" | "kept (no clear spot)". The caller still enforces HQ clearance and objective spacing on the result.
	void PlanObjective(vector authored, out vector pos, out string how)
	{
		vector wish = TransformPoint(authored);
		float placeDist;
		vector placePos;
		string placeName = CLW_Places.NearestOfTypes(wish, m_Cfg.m_fSnapRadiusM, m_Cfg.m_aSnapTypes, placeDist, placePos);
		if (!placeName.IsEmpty())
		{
			vector snap = placePos;
			CLW_Placement.SurfaceAt(snap);
			if (!CLW_Placement.IsWater(snap) && CLW_Placement.SlopeOk(snap))
			{
				pos = snap;
				how = "snapped to " + placeName;
				return;
			}
		}

		CLW_SpotParams p = new CLW_SpotParams();
		p.m_sLabel = "objective spot";
		p.m_vCenter = wish;
		p.m_fHalfX = m_Cfg.m_fObjectiveHalfM;
		p.m_fHalfZ = m_Cfg.m_fObjectiveHalfM;
		p.m_fHeight = 3;
		p.m_fMargin = 0.5;
		p.m_fRoadClearance = 0;
		p.m_fMaxHeightRange = 2 * 1.414 * m_Cfg.m_fObjectiveHalfM * m_Cfg.m_fMaxGrade;
		float range;
		float surfaceY;
		if (CLW_FreeSpot.CheckPose(p, wish, 0, range, surfaceY).IsEmpty())
		{
			pos = wish;
			how = "kept";
			return;
		}

		p.m_fMinRadius = 0;
		p.m_fMaxRadius = m_Cfg.m_fNudgeRadiusM;
		p.m_iMaxTries = 150;
		p.m_iValidTarget = 3;
		CLW_SpotResult res = new CLW_SpotResult();
		if (CLW_FreeSpot.FindFreeSpot(p, res))
		{
			pos = res.m_vPos;
			how = "nudged";
			return;
		}

		pos = wish;
		how = "kept (no clear spot)";
	}
}

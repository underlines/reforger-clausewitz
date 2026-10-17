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
	[Attribute("250", UIWidgets.EditBox, "HQs stay this far (m) inside the terrain edge")]
	float m_fEdgeMarginM;

	[Attribute("0.25", UIWidgets.EditBox, "HQ distance = battle-size distance minus up to this fraction (never above it)")]
	float m_fDistanceTolerance;

	[Attribute("120", UIWidgets.EditBox, "Attempts (one dry HQ A wish checked each) before falling back to the fixed layout")]
	int m_iMaxAttempts;

	[Attribute("25000", UIWidgets.EditBox, "Wall clock budget (ms) of one HQ search, then it gives up like after the last attempt (0 = unlimited). The search runs on the server main thread")]
	int m_iTimeBudgetMs;

	[Attribute("300", UIWidgets.EditBox, "Wish samples per attempt to find dry land (wet samples are free, they do not use up an attempt)")]
	int m_iLandSamples;

	[Attribute("0.45", UIWidgets.EditBox, "Share of HQ A wishes taken near a named map place (villages, towns...)")]
	float m_fPlaceWishFraction;

	[Attribute("0.35", UIWidgets.EditBox, "Share of HQ A wishes taken near a vehicle road (the rest is uniform land)")]
	float m_fRoadWishFraction;

	[Attribute("200", UIWidgets.EditBox, "A place wish is moved this far (m, min) from the place, out of the town")]
	float m_fPlaceOffsetMinM;

	[Attribute("450", UIWidgets.EditBox, "A place wish is moved this far (m, max) from the place")]
	float m_fPlaceOffsetMaxM;

	[Attribute("40", UIWidgets.EditBox, "A road wish is moved this far (m, min) from the road line")]
	float m_fRoadOffsetMinM;

	[Attribute("120", UIWidgets.EditBox, "A road wish is moved this far (m, max) from the road line")]
	float m_fRoadOffsetMaxM;

	[Attribute("200", UIWidgets.EditBox, "An HQ may move this far (m) from its random wish to find clear ground")]
	float m_fHqSearchRadiusM;

	[Attribute("12", UIWidgets.EditBox, "Half size (m) of the clear square core of an HQ (spawn zone)")]
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

	[Attribute("-1", UIWidgets.EditBox, "Max buildings within m_fTownRadiusM of an HQ (-1 = no limit: HQs may stand in villages and town centres)")]
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

	// ---- urban sites (WP11): HQs in / at the edge of villages and towns, very near a road. A value of 0 below = the built-in default
	// (FillSnapTypes), so a data block written before these fields existed gets the urban search too. ----

	[Attribute("0", UIWidgets.CheckBox, "1 = switch the urban HQ site search OFF (old rules: open ground, out of towns)")]
	bool m_bNoUrban;

	[Attribute("12", UIWidgets.EditBox, "Urban: half size (m) of the clear square core of an HQ (the compact base: command post ~20x21 m)")]
	float m_fUrbanCoreHalfM;

	[Attribute("8", UIWidgets.EditBox, "Urban: preferred distance (m) of the HQ to the nearest vehicle road, lower end")]
	float m_fRoadPreferMinM;

	[Attribute("30", UIWidgets.EditBox, "Urban: preferred distance to the nearest vehicle road, upper end")]
	float m_fRoadPreferMaxM;

	[Attribute("60", UIWidgets.EditBox, "Urban: an HQ farther than this (m) from a vehicle road is rejected")]
	float m_fRoadRejectM;

	[Attribute("250", UIWidgets.EditBox, "Urban: an HQ within this distance (m) of a village / town centre (snap types) costs nothing extra, farther costs more")]
	float m_fPlaceNearM;

	[Attribute("130", UIWidgets.EditBox, "Urban: a place wish is taken this far (m, max) from the place centre (min 0: inside the village)")]
	float m_fUrbanPlaceRadiusM;

	[Attribute("12", UIWidgets.EditBox, "Urban: inner radius (m) of the ring where service-sized room (arsenal, supply, depot) is counted")]
	float m_fUrbanRingInnerM;

	[Attribute("45", UIWidgets.EditBox, "Urban: outer radius (m) of that ring")]
	float m_fUrbanRingOuterM;

	[Attribute("5", UIWidgets.EditBox, "Urban: ring samples that must be clear (small clearable objects count as clear)")]
	int m_iUrbanRingMinFree;

	[Attribute("100000", UIWidgets.EditBox, "Urban: max buildings within m_fTownRadiusM (default = no limit: only room for the base and a road matter; negative = no limit too)")]
	int m_iUrbanMaxBuildings;

	[Attribute("60", UIWidgets.EditBox, "Urban: candidate sites tried per HQ")]
	int m_iSiteSamples;

	[Attribute("5", UIWidgets.EditBox, "Urban: valid candidate sites scored per HQ before the best is taken")]
	int m_iSiteValid;

	[Attribute("1", UIWidgets.EditBox, "Urban: weight of the road distance cost")]
	float m_fWeightRoad;

	[Attribute("1", UIWidgets.EditBox, "Urban: weight of the settlement distance cost")]
	float m_fWeightPlace;

	[Attribute("1", UIWidgets.EditBox, "Urban: weight of the room cost (free service ring samples)")]
	float m_fWeightRoom;

	// ---- pair variety (M4.6): several valid HQ pairs are collected, one is picked at random, the sides are tossed ----

	[Attribute("6", UIWidgets.EditBox, "Valid HQ pairs to collect before one is picked at random (1 = take the first)")]
	int m_iPairTarget;

	[Attribute("6000", UIWidgets.EditBox, "Once one pair is found, collecting stops after this many ms (bounds the cost of the pair search)")]
	int m_iCollectBudgetMs;

	[Attribute("300", UIWidgets.EditBox, "A new pair counts as a duplicate when both HQs are within this distance (m) of an already collected pair")]
	float m_fPairSeparationM;

	void FillUrbanDefaults()
	{
		if (m_fUrbanCoreHalfM <= 0)
			m_fUrbanCoreHalfM = 12;
		if (m_fRoadPreferMinM <= 0)
			m_fRoadPreferMinM = 8;
		if (m_fRoadPreferMaxM <= 0)
			m_fRoadPreferMaxM = 30;
		if (m_fRoadRejectM <= 0)
			m_fRoadRejectM = 60;
		if (m_fPlaceNearM <= 0)
			m_fPlaceNearM = 250;
		if (m_fUrbanPlaceRadiusM <= 0)
			m_fUrbanPlaceRadiusM = 130;
		if (m_fUrbanRingInnerM <= 0)
			m_fUrbanRingInnerM = 12;
		if (m_fUrbanRingOuterM <= 0)
			m_fUrbanRingOuterM = 45;
		if (m_iUrbanRingMinFree <= 0)
			m_iUrbanRingMinFree = 5;
		if (m_iUrbanMaxBuildings == 0)
			m_iUrbanMaxBuildings = 100000;
		if (m_iPairTarget <= 0)
			m_iPairTarget = 6;
		if (m_iCollectBudgetMs <= 0)
			m_iCollectBudgetMs = 6000;
		if (m_fPairSeparationM <= 0)
			m_fPairSeparationM = 300;
		if (m_iSiteSamples <= 0)
			m_iSiteSamples = 60;
		if (m_iSiteValid <= 0)
			m_iSiteValid = 5;
		if (m_fWeightRoad <= 0)
			m_fWeightRoad = 1;
		if (m_fWeightPlace <= 0)
			m_fWeightPlace = 1;
		if (m_fWeightRoom <= 0)
			m_fWeightRoom = 1;
	}

	// Same values as the attribute defaults, for a scenario without the block.
	void SetDefaults()
	{
		m_fEdgeMarginM = 250;
		m_fDistanceTolerance = 0.25;
		m_iMaxAttempts = 120;
		m_iLandSamples = 300;
		m_iTimeBudgetMs = 25000;
		m_fPlaceWishFraction = 0.45;
		m_fRoadWishFraction = 0.35;
		m_fPlaceOffsetMinM = 200;
		m_fPlaceOffsetMaxM = 450;
		m_fRoadOffsetMinM = 40;
		m_fRoadOffsetMaxM = 120;
		m_fHqSearchRadiusM = 200;
		m_fHqCoreHalfM = 12;
		m_fMaxGrade = 0.12;
		m_bRequireRoad = true;
		m_fHqRoadMaxM = 150;
		m_fServiceHalfM = 7;
		m_fRingInnerM = 40;
		m_fRingOuterM = 130;
		m_iRingSamples = 24;
		m_iRingMinFree = 8;
		m_fTownRadiusM = 150;
		m_iMaxBuildingsNear = -1;
		m_iMaxWaterSamples = 1;
		m_fSnapRadiusM = 350;
		m_fObjectiveHalfM = 8;
		m_fNudgeRadiusM = 200;
		FillSnapTypes();
	}

	void FillSnapTypes()
	{
		FillUrbanDefaults();
		if (m_iLandSamples <= 0)
		{
			// a data block written before the land-only wish fields existed: take the built-in values for them
			m_iLandSamples = 300;
			m_iTimeBudgetMs = 25000;
			m_fPlaceWishFraction = 0.45;
			m_fRoadWishFraction = 0.35;
			m_fPlaceOffsetMinM = 200;
			m_fPlaceOffsetMaxM = 450;
			m_fRoadOffsetMinM = 40;
			m_fRoadOffsetMaxM = 120;
		}

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
	protected ref map<string, int> m_mRejects = new map<string, int>();
	protected int m_iDrySkipped;
	protected ref array<vector> m_aPlaceWishes = {};
	protected ref array<BaseRoad> m_aRoadWishes = {};

	int m_iSeed;
	int m_iAttempts;
	vector m_vHqA;
	vector m_vHqB;
	vector m_vCentre;
	float m_fAxisDeg;
	// statistics of the last FindHqPair (valid after it returned true)
	int m_iPairsFound;
	int m_iPairPicked;
	bool m_bSidesFlipped;
	int m_iSearchMs;
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
			// Math.RandomInt alone repeats across server starts (the global generator starts from a fixed state): mix the wall clock and
			// the uptime ticks, and seed the global generator with it too so the CLW_FreeSpot ring angles differ per start as well.
			int clock = System.GetUnixTime();
			int ticks = System.GetTickCount();
			int mixed = (clock * 31 + ticks * 7919) ^ (clock >> 7);
			layout.m_iSeed = (mixed & 0x7FFFFF) % 9999999 + 1;
			Math.Randomize(layout.m_iSeed);
			source = "time based, fresh per start";
		}

		layout.m_Rng.SetSeed(layout.m_iSeed);
		Print(string.Format("CLW: layout random seed=%1 (%2)", layout.m_iSeed, source), LogLevel.NORMAL);
		return layout;
	}

	// A second, looser search for a map where the strict rules found nothing (CLW_AutoScenario only; the data object stays untouched):
	// no road needed, towns allowed, a smaller clear core and fewer free ring samples, more attempts and a wider move radius.
	void Relax()
	{
		CLW_RandomLayoutSetup loose = new CLW_RandomLayoutSetup();
		loose.SetDefaults();
		loose.m_fEdgeMarginM = m_Cfg.m_fEdgeMarginM;
		loose.m_fDistanceTolerance = Math.Max(m_Cfg.m_fDistanceTolerance, 0.3);
		loose.m_iMaxAttempts = m_Cfg.m_iMaxAttempts * 2;
		loose.m_iLandSamples = m_Cfg.m_iLandSamples;
		loose.m_iTimeBudgetMs = m_Cfg.m_iTimeBudgetMs;
		loose.m_fHqSearchRadiusM = m_Cfg.m_fHqSearchRadiusM * 1.5;
		loose.m_fHqCoreHalfM = Math.Min(m_Cfg.m_fHqCoreHalfM, 8);
		loose.m_fMaxGrade = m_Cfg.m_fMaxGrade * 1.5;
		loose.m_bRequireRoad = false;
		loose.m_iRingMinFree = Math.Min(m_Cfg.m_iRingMinFree, 2);
		loose.m_iMaxBuildingsNear = -1;
		loose.m_iMaxWaterSamples = Math.Max(m_Cfg.m_iMaxWaterSamples, 2);
		loose.m_aSnapTypes = m_Cfg.m_aSnapTypes;
		loose.m_bNoUrban = true;	// the old open-ground rules
		loose.m_iPairTarget = m_Cfg.m_iPairTarget;
		loose.m_iCollectBudgetMs = m_Cfg.m_iCollectBudgetMs;
		loose.m_fPairSeparationM = m_Cfg.m_fPairSeparationM;
		m_Cfg = loose;
		Print("CLW: layout random search relaxed (no road needed, towns allowed, smaller core, double attempts)", LogLevel.WARNING);
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
		if (!FindHqPair(wanted, minHq))
			return false;

		SetTransform(authoredA, authoredB);
		return true;
	}

	// The HQ search alone (no authored layout needed; used by CLW_AutoScenario at game start and by Generate): HQ A on valid land, then
	// HQ B at the wanted distance in up to DIRS directions from A. Wishes for A come from LAND ONLY (a dry wish is sampled within a budget
	// that does not use up attempts), preferably near map places and roads (they sit on open ground and a road is required anyway).
	// An attempt = one dry HQ A wish that was checked. The rejection histogram is logged once per search. Sets m_vHqA / m_vHqB / m_vCentre /
	// m_fAxisDeg / m_iAttempts.
	bool FindHqPair(float wanted, float minHq)
	{
		m_mRejects.Clear();
		m_iDrySkipped = 0;
		vector bmin, bmax;
		GetPlayableBounds(bmin, bmax);
		float margin = m_Cfg.m_fEdgeMarginM;
		float floorDist = Math.Min(minHq, wanted);
		Print(string.Format("CLW: layout random area %1 .. %2 margin %3 m, wanted HQ distance %4 m (floor %5 m, tolerance %6, max attempts %7)", bmin, bmax, Math.Round(margin), Math.Round(wanted), Math.Round(floorDist), m_Cfg.m_fDistanceTolerance, m_Cfg.m_iMaxAttempts), LogLevel.NORMAL);
		PrepareWishSources(bmin, bmax, margin);
		// HQ A first on valid land, then HQ B at the wanted distance in up to DIRS directions from A: the first version picked a random
		// centre + axis and lost almost every attempt to the sea or to slopes on Arland (DS test 2026-10-12: 0 of 30); the second drew
		// uniform wishes, 25 of 60 attempts were "water at the wish" and 25 "no clear core ground" (forest).
		int DIRS = 12;
		int startTick = System.GetTickCount();
		// Several valid pairs are collected (bounded by the pair target, the collect budget after the first pair, the attempts and the
		// overall time budget), then one is picked at random and the sides are tossed: taking the first pair and giving side A the
		// first HQ found put A mostly west / B east in 2-3 spots on Arland whatever the seed.
		array<vector> pairsA = {};
		array<vector> pairsB = {};
		array<float> pairsHeading = {};
		array<int> pairsAttempt = {};
		int pairTarget = Math.Max(1, m_Cfg.m_iPairTarget);
		for (int attempt = 1; attempt <= m_Cfg.m_iMaxAttempts; attempt++)
		{
			int elapsed = System.GetTickCount() - startTick;
			if (m_Cfg.m_iTimeBudgetMs > 0 && elapsed > m_Cfg.m_iTimeBudgetMs)
			{
				Reject("time budget used up");
				Print(string.Format("CLW: layout random search stopped after %1 attempts: time budget of %2 ms used up (%3 pairs collected)", attempt - 1, m_Cfg.m_iTimeBudgetMs, pairsA.Count()), LogLevel.WARNING);
				break;
			}

			if (!pairsA.IsEmpty() && (pairsA.Count() >= pairTarget || (m_Cfg.m_iCollectBudgetMs > 0 && elapsed > m_Cfg.m_iCollectBudgetMs)))
				break;

			m_iAttempts = attempt;
			vector wishA;
			string wishSource;
			if (!SampleLandWish(bmin, bmax, margin, wishA, wishSource))
			{
				Reject("no dry wish within budget");
				Print(string.Format("CLW: layout random attempt %1: no dry wish found in %2 samples", attempt, m_Cfg.m_iLandSamples), LogLevel.NORMAL);
				continue;
			}

			string why;
			vector posA;
			if (!FindHq("A", wishA, posA, why))
			{
				Print(string.Format("CLW: layout random attempt %1: HQ A wish %2 (%3) rejected: %4", attempt, wishA, wishSource, why), LogLevel.NORMAL);
				continue;
			}

			float startHeading = m_Rng.RandFloatXY(0, Math.PI2);
			int tried;
			bool pairFound;
			bool duplicate;
			for (int d = 0; d < DIRS; d++)
			{
				float heading = startHeading + Math.PI2 * d / DIRS;
				float dist = wanted * (1 - m_Rng.RandFloatXY(0, m_Cfg.m_fDistanceTolerance));
				dist = Math.Max(dist, floorDist);
				vector wishB = posA + Vector(Math.Sin(heading) * dist, 0, Math.Cos(heading) * dist);
				if (!InsideArea(wishB, bmin, bmax, margin))
				{
					Reject("B direction outside the area");
					continue;	// outside the playable area
				}

				int wet = WaterSamples(posA, wishB);
				if (wet > m_Cfg.m_iMaxWaterSamples)
				{
					Reject("B line crosses water");
					continue;
				}

				vector dryB = wishB;
				CLW_Placement.SurfaceAt(dryB);
				if (CLW_Placement.IsWater(dryB) || CLW_Placement.NearWater(dryB, 25))
				{
					Reject("B wish wet");
					continue;
				}

				tried++;
				vector posB;
				if (!FindHq("B", wishB, posB, why))
					continue;

				float actual = vector.DistanceXZ(posA, posB);
				if (actual > wanted * 1.1 || actual < floorDist * 0.9)
				{
					Reject("B distance off");
					continue;
				}

				if (IsDuplicatePair(pairsA, pairsB, posA, posB))
				{
					Reject("duplicate pair");
					duplicate = true;
					break;	// next attempt: another HQ A wish
				}

				pairsA.Insert(posA);
				pairsB.Insert(posB);
				pairsHeading.Insert(heading);
				pairsAttempt.Insert(attempt);
				pairFound = true;
				Print(string.Format("CLW: layout random pair %1 collected (attempt %2, direction %3 of %4, %5 ms): A %6 grid %7, B %8 grid %9, distance %10 m", pairsA.Count(), attempt, d + 1, DIRS, System.GetTickCount() - startTick, posA, CLW_Places.Grid(posA), posB, CLW_Places.Grid(posB), Math.Round(actual)), LogLevel.NORMAL);
				break;
			}

			if (!pairFound && !duplicate)
			{
				Reject("HQ A ok, no HQ B");
				Print(string.Format("CLW: layout random attempt %1: HQ A ok at %2, no HQ B in %3 directions (%4 reached the HQ check, last: %5)", attempt, posA, DIRS, tried, why), LogLevel.NORMAL);
			}
		}

		int spent = System.GetTickCount() - startTick;
		if (pairsA.IsEmpty())
		{
			Print(string.Format("CLW: layout random seed=%1: no valid HQ pair in %2 attempts (%3 ms)", m_iSeed, m_iAttempts, spent), LogLevel.WARNING);
			LogRejects("failed");
			return false;
		}

		// random pick + coin toss for the sides, both from the seeded generator
		int picked = m_Rng.RandInt(0, pairsA.Count());
		bool flip = m_Rng.RandFloat01() < 0.5;
		vector pickA = pairsA[picked];
		vector pickB = pairsB[picked];
		float pickHeading = pairsHeading[picked];
		if (flip)
		{
			pickA = pairsB[picked];
			pickB = pairsA[picked];
			pickHeading = pickHeading + Math.PI;
		}

		m_vHqA = pickA;
		m_vHqB = pickB;
		m_vCentre = (pickA + pickB) * 0.5;
		m_fAxisDeg = pickHeading * Math.RAD2DEG;
		if (m_fAxisDeg >= 360)
			m_fAxisDeg = m_fAxisDeg - 360;
		m_iPairsFound = pairsA.Count();
		m_iPairPicked = picked + 1;
		m_bSidesFlipped = flip;
		m_iSearchMs = spent;
		float pickedDist = vector.DistanceXZ(pickA, pickB);
		Print(string.Format("CLW: layout random seed=%1 axis=%2 centre=%3 hqA=%4 hqB=%5 (attempts %6)", m_iSeed, Math.Round(m_fAxisDeg), m_vCentre, m_vHqA, m_vHqB, m_iAttempts), LogLevel.NORMAL);
		Print(string.Format("CLW: layout random HQ distance %1 m (wanted %2 m), grid A %3 B %4", Math.Round(pickedDist), Math.Round(wanted), CLW_Places.Grid(m_vHqA), CLW_Places.Grid(m_vHqB)), LogLevel.NORMAL);
		Print(string.Format("CLW: layout random HQ pick: %1 valid pair(s) found, picked #%2 (found at attempt %3), sides flipped=%4, search took %5 ms", m_iPairsFound, m_iPairPicked, pairsAttempt[picked], flip, spent), LogLevel.NORMAL);
		LogRejects("found");
		return true;
	}

	// True when both HQs of the new pair lie within m_fPairSeparationM of the same collected pair (one-to-one, either orientation).
	protected bool IsDuplicatePair(notnull array<vector> pairsA, notnull array<vector> pairsB, vector posA, vector posB)
	{
		float sep = m_Cfg.m_fPairSeparationM;
		for (int i = 0; i < pairsA.Count(); i++)
		{
			if (vector.DistanceXZ(pairsA[i], posA) < sep && vector.DistanceXZ(pairsB[i], posB) < sep)
				return true;

			if (vector.DistanceXZ(pairsA[i], posB) < sep && vector.DistanceXZ(pairsB[i], posA) < sep)
				return true;
		}

		return false;
	}

	protected void Reject(string reason)
	{
		int n;
		m_mRejects.Find(reason, n);
		m_mRejects.Set(reason, n + 1);
	}

	// One line per search: how often each rejection reason happened (histogram), plus the wet wish samples skipped for free.
	protected void LogRejects(string outcome)
	{
		string text = "";
		foreach (string reason, int count : m_mRejects)
		{
			if (!text.IsEmpty())
				text += "; ";
			text += reason + "=" + count;
		}

		if (text.IsEmpty())
			text = "none";

		Print(string.Format("CLW: layout random rejections (%1, %2 attempts, %3 wet wishes resampled for free): %4", outcome, m_iAttempts, m_iDrySkipped, text), LogLevel.NORMAL);
	}

	// ---- wishes: land only, near places / roads ----

	// Collects the sources for HQ A wishes once per search: typed map places (snap types, else any typed place) and the roads wide
	// enough for vehicles.
	protected void PrepareWishSources(vector bmin, vector bmax, float margin)
	{
		m_aPlaceWishes.Clear();
		m_aRoadWishes.Clear();
		array<ref CLW_Place> places = {};
		CLW_Places.GetAll(places);
		array<vector> anyTyped = {};
		foreach (CLW_Place place : places)
		{
			if (place.m_sType.IsEmpty() || !InsideArea(place.m_vPos, bmin, bmax, margin))
				continue;

			anyTyped.Insert(place.m_vPos);
			foreach (string part : m_Cfg.m_aSnapTypes)
			{
				if (place.m_sType.Contains(part))
				{
					m_aPlaceWishes.Insert(place.m_vPos);
					break;
				}
			}
		}

		if (m_aPlaceWishes.IsEmpty())
			m_aPlaceWishes.Copy(anyTyped);

		RoadNetworkManager mgr = CLW_Placement.GetRoadManager();
		if (mgr && m_Cfg.m_fRoadWishFraction > 0)
		{
			array<BaseRoad> roads = {};
			mgr.GetRoadsInAABB(Vector(bmin[0], -10000, bmin[2]), Vector(bmax[0], 10000, bmax[2]), roads);
			foreach (BaseRoad road : roads)
			{
				if (road && road.GetWidth() >= CLW_WarConfig.PLACE_ROAD_MIN_WIDTH_M)
					m_aRoadWishes.Insert(road);
			}
		}

		Print(string.Format("CLW: layout random wish sources: %1 map places, %2 vehicle roads (place wish %3, road wish %4, rest uniform land)", m_aPlaceWishes.Count(), m_aRoadWishes.Count(), m_Cfg.m_fPlaceWishFraction, m_Cfg.m_fRoadWishFraction), LogLevel.NORMAL);
	}

	protected bool InsideArea(vector pos, vector bmin, vector bmax, float margin)
	{
		return pos[0] >= bmin[0] + margin && pos[0] <= bmax[0] - margin && pos[2] >= bmin[2] + margin && pos[2] <= bmax[2] - margin;
	}

	// One wish for HQ A on dry land. Samples (place / road / uniform by the data fractions) until one is dry and inside the area, at most
	// m_iLandSamples times; wet samples are only counted (m_iDrySkipped), they do not use up an attempt.
	protected bool SampleLandWish(vector bmin, vector bmax, float margin, out vector wish, out string source)
	{
		for (int i = 0; i < m_Cfg.m_iLandSamples; i++)
		{
			float roll = m_Rng.RandFloat01();
			vector cand;
			string src = "uniform";
			bool got;
			if (roll < m_Cfg.m_fPlaceWishFraction && !m_aPlaceWishes.IsEmpty())
			{
				cand = m_aPlaceWishes[m_Rng.RandInt(0, m_aPlaceWishes.Count())];
				float angle = m_Rng.RandFloatXY(0, Math.PI2);
				float radius = m_Rng.RandFloatXY(m_Cfg.m_fPlaceOffsetMinM, Math.Max(m_Cfg.m_fPlaceOffsetMinM + 1, m_Cfg.m_fPlaceOffsetMaxM));	// out of the town: it has many buildings
				if (!m_Cfg.m_bNoUrban)
					radius = m_Rng.RandFloatXY(0, m_Cfg.m_fUrbanPlaceRadiusM);	// urban: inside / at the edge of the village
				cand = cand + Vector(Math.Sin(angle) * radius, 0, Math.Cos(angle) * radius);
				src = "place";
				got = true;
			}
			else if (roll < m_Cfg.m_fPlaceWishFraction + m_Cfg.m_fRoadWishFraction && !m_aRoadWishes.IsEmpty())
			{
				got = RandomRoadPoint(cand);
				if (got)
				{
					float roadAngle = m_Rng.RandFloatXY(0, Math.PI2);
					float roadRadius = m_Rng.RandFloatXY(m_Cfg.m_fRoadOffsetMinM, Math.Max(m_Cfg.m_fRoadOffsetMinM + 1, m_Cfg.m_fRoadOffsetMaxM));
					if (!m_Cfg.m_bNoUrban)
						roadRadius = m_Rng.RandFloatXY(m_Cfg.m_fRoadPreferMinM, m_Cfg.m_fRoadPreferMaxM + 40);
					cand = cand + Vector(Math.Sin(roadAngle) * roadRadius, 0, Math.Cos(roadAngle) * roadRadius);
					src = "road";
				}
			}

			if (!got)
				cand = Vector(m_Rng.RandFloatXY(bmin[0] + margin, bmax[0] - margin), 0, m_Rng.RandFloatXY(bmin[2] + margin, bmax[2] - margin));

			if (!InsideArea(cand, bmin, bmax, margin))
				continue;

			CLW_Placement.SurfaceAt(cand);
			if (CLW_Placement.IsWater(cand) || CLW_Placement.NearWater(cand, 25))
			{
				m_iDrySkipped++;
				continue;
			}

			wish = cand;
			source = src;
			return true;
		}

		return false;
	}

	// A random point on a random vehicle road (a random point of a random segment of its polyline).
	protected bool RandomRoadPoint(out vector point)
	{
		for (int tries = 0; tries < 8; tries++)
		{
			BaseRoad road = m_aRoadWishes[m_Rng.RandInt(0, m_aRoadWishes.Count())];
			if (!road)
				continue;

			array<vector> pts = {};
			road.GetPoints(pts);
			if (pts.Count() < 2)
				continue;

			int seg = m_Rng.RandInt(0, pts.Count() - 1);
			float t = m_Rng.RandFloat01();
			point = pts[seg] + (pts[seg + 1] - pts[seg]) * t;
			return true;
		}

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
			Reject("water at the wish");
			return false;
		}

		if (!m_Cfg.m_bNoUrban)
			return FindHqUrban(label, wish, pos, why);

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
			Reject("no clear core ground");
			return false;
		}

		vector spot = res.m_vPos;
		if (CLW_Placement.NearWater(spot, 25))
		{
			why = "water near " + spot;
			Reject("water near the spot");
			return false;
		}

		float road = CLW_Placement.NearestRoadDistance(spot);
		if (m_Cfg.m_bRequireRoad && (road < 0 || road > m_Cfg.m_fHqRoadMaxM))
		{
			why = string.Format("nearest road %1 m at %2", Math.Round(road), spot);
			Reject("road too far");
			return false;
		}

		int free = CountFreeRing(spot);
		if (free < m_Cfg.m_iRingMinFree)
		{
			Reject("service ring too small");
			why = string.Format("service ring has %1 of %2 clear samples (need %3) at %4", free, m_Cfg.m_iRingSamples, m_Cfg.m_iRingMinFree, spot);
			return false;
		}

		if (m_Cfg.m_iMaxBuildingsNear >= 0)
		{
			CLW_BuildingCounter counter = new CLW_BuildingCounter();
			GetGame().GetWorld().QueryEntitiesBySphere(spot, m_Cfg.m_fTownRadiusM, counter.OnEntity, null, EQueryEntitiesFlags.STATIC);
			if (counter.m_iCount > m_Cfg.m_iMaxBuildingsNear)
			{
				Reject("too many buildings");
				why = string.Format("%1 buildings within %2 m at %3 (max %4)", counter.m_iCount, Math.Round(m_Cfg.m_fTownRadiusM), spot, m_Cfg.m_iMaxBuildingsNear);
				return false;
			}
		}

		pos = spot;
		Print(string.Format("CLW: layout random HQ %1 ok at %2 (wish moved %3 m, road %4 m, ring %5/%6 clear)", label, spot, Math.Round(vector.DistanceXZ(spot, wish)), Math.Round(road), free, m_Cfg.m_iRingSamples), LogLevel.NORMAL);
		return true;
	}

	// ---- urban HQ site (WP11): in / at the edge of a village or town, very near a vehicle road ----

	protected void SiteReject(map<string, int> local, string reason)
	{
		Reject("site " + reason);
		int n;
		local.Find(reason, n);
		local.Set(reason, n + 1);
	}

	protected int CountBuildings(vector spot)
	{
		CLW_BuildingCounter counter = new CLW_BuildingCounter();
		GetGame().GetWorld().QueryEntitiesBySphere(spot, m_Cfg.m_fTownRadiusM, counter.OnEntity, null, EQueryEntitiesFlags.STATIC);
		return counter.m_iCount;
	}

	// Samples candidate sites around the wish (around villages / towns of the snap types within the search radius first, then around the wish),
	// road-anchored: a point 8..30 m (data) beside the nearest vehicle road. A candidate needs a vehicle road within m_fRoadRejectM, a clear
	// compact core (small clearable objects count as clear, CLW_PlaceClear), not too many buildings and enough service room around it. The valid
	// ones are scored (lower = better): road distance outside the preferred band, distance to the nearest settlement centre beyond
	// m_fPlaceNearM, missing room, building density, off-navmesh, distance moved from the wish. The best of m_iSiteValid valid ones wins.
	protected bool FindHqUrban(string label, vector wish, out vector pos, out string why)
	{
		vector bmin, bmax;
		GetPlayableBounds(bmin, bmax);
		float areaMargin = m_Cfg.m_fEdgeMarginM;
		CLW_SpotParams core = new CLW_SpotParams();
		core.m_sLabel = "hq core " + label;
		core.m_fHalfX = m_Cfg.m_fUrbanCoreHalfM;
		core.m_fHalfZ = m_Cfg.m_fUrbanCoreHalfM;
		core.m_fHeight = 3;
		core.m_fRoadClearance = 0;	// the road is meant to be close: the services keep their own road clearance
		core.m_fMaxHeightRange = 2 * 1.414 * m_Cfg.m_fUrbanCoreHalfM * m_Cfg.m_fMaxGrade;
		core.m_bClearSmall = true;

		// settlements near the wish: sample around them
		array<vector> nearPlaces = {};
		array<ref CLW_Place> places = {};
		CLW_Places.GetAll(places);
		foreach (CLW_Place place : places)
		{
			if (place.m_sType.IsEmpty() || vector.DistanceXZ(place.m_vPos, wish) > m_Cfg.m_fHqSearchRadiusM)
				continue;

			foreach (string typePart : m_Cfg.m_aSnapTypes)
			{
				if (place.m_sType.Contains(typePart))
				{
					nearPlaces.Insert(place.m_vPos);
					break;
				}
			}
		}

		map<string, int> siteRej = new map<string, int>();
		int valid;
		int sampled;
		float bestScore = 1000000000;
		vector bestPos;
		float bestRoad;
		float bestRoadCost;
		float bestPlaceDist;
		string bestPlaceName;
		float bestPlaceCost;
		int bestFree;
		float bestRoomCost;
		int bestBuild;
		float bestDensCost;
		int bestNav;
		int bestClear;
		float bestMoved;
		int ringSamples = Math.Max(1, m_Cfg.m_iRingSamples);
		for (int s = 0; s < m_Cfg.m_iSiteSamples && valid < m_Cfg.m_iSiteValid; s++)
		{
			sampled++;
			vector centre = wish;
			float radiusMax = m_Cfg.m_fHqSearchRadiusM;
			if (!nearPlaces.IsEmpty() && m_Rng.RandFloat01() < 0.7)
			{
				centre = nearPlaces[m_Rng.RandInt(0, nearPlaces.Count())];
				radiusMax = m_Cfg.m_fUrbanPlaceRadiusM;
			}

			float ang = m_Rng.RandFloatXY(0, Math.PI2);
			float rad = Math.Sqrt(m_Rng.RandFloat01()) * radiusMax;
			vector cand = centre + Vector(Math.Sin(ang) * rad, 0, Math.Cos(ang) * rad);
			CLW_RoadHit seek;
			if (s % 3 != 2 && CLW_FreeSpot.NearestRoad(cand, m_Cfg.m_fRoadRejectM, CLW_WarConfig.PLACE_ROAD_MIN_WIDTH_M, seek))
			{
				float side = 1;
				if (m_Rng.RandFloat01() < 0.5)
					side = -1;

				float offset = m_Rng.RandFloatXY(m_Cfg.m_fRoadPreferMinM, m_Cfg.m_fRoadPreferMaxM);
				cand = seek.m_vPoint + Vector(-seek.m_vDir[2] * side * offset, 0, seek.m_vDir[0] * side * offset);
			}

			if (!InsideArea(cand, bmin, bmax, areaMargin))
			{
				SiteReject(siteRej, "outside area");
				continue;
			}

			CLW_Placement.SurfaceAt(cand);
			if (CLW_Placement.IsWater(cand) || CLW_Placement.NearWater(cand, 25))
			{
				SiteReject(siteRej, "water");
				continue;
			}

			CLW_RoadHit roadHit;
			if (!CLW_FreeSpot.NearestRoad(cand, m_Cfg.m_fRoadRejectM, CLW_WarConfig.PLACE_ROAD_MIN_WIDTH_M, roadHit))
			{
				SiteReject(siteRej, "road too far");
				continue;
			}

			int build = CountBuildings(cand);
			if (m_Cfg.m_iUrbanMaxBuildings > 0 && build > m_Cfg.m_iUrbanMaxBuildings)
			{
				SiteReject(siteRej, "too many buildings");
				continue;
			}

			float range;
			float surfaceY;
			string reason = CLW_FreeSpot.CheckPose(core, cand, 0, range, surfaceY);
			if (!reason.IsEmpty())
			{
				SiteReject(siteRej, "core " + reason);
				continue;
			}

			int clearCount = 0;
			if (core.m_aCollect)
				clearCount = core.m_aCollect.Count();

			int free = CountFreeRingEx(cand, m_Cfg.m_fUrbanRingInnerM, m_Cfg.m_fUrbanRingOuterM, true);
			if (free < m_Cfg.m_iUrbanRingMinFree)
			{
				SiteReject(siteRej, "service ring too small");
				continue;
			}

			// ---- score ----
			float roadDist = roadHit.m_fDist;
			float roadCost = 0;
			if (roadDist < m_Cfg.m_fRoadPreferMinM)
				roadCost = (m_Cfg.m_fRoadPreferMinM - roadDist) * 0.5;
			else if (roadDist > m_Cfg.m_fRoadPreferMaxM)
				roadCost = (roadDist - m_Cfg.m_fRoadPreferMaxM) * 0.5;

			float placeDist;
			vector placePos;
			string placeName = CLW_Places.NearestOfTypes(cand, 5000, m_Cfg.m_aSnapTypes, placeDist, placePos);
			placeDist = Math.Min(placeDist, m_Cfg.m_fPlaceNearM + 500);
			float placeCost = placeDist / m_Cfg.m_fPlaceNearM;
			if (placeDist > m_Cfg.m_fPlaceNearM)
				placeCost = 1 + (placeDist - m_Cfg.m_fPlaceNearM) / 50;

			float roomCost = (ringSamples - free) * 8.0 / ringSamples;
			float densCost = 0;
			if (build > 40)
				densCost = (build - 40) * 0.003;	// a town centre is as good as open ground: only a tiny tie-breaker

			vector snapped;
			int nav = CLW_Placement.NavmeshCheck(cand, CLW_WarConfig.PLACE_BASE_NAVMESH_TOL_M, snapped);
			float navCost = 0;
			if (nav == 0)
				navCost = 4;

			float moved = vector.DistanceXZ(cand, wish);
			float score = m_Cfg.m_fWeightRoad * roadCost + m_Cfg.m_fWeightPlace * placeCost + m_Cfg.m_fWeightRoom * roomCost + densCost + navCost + range * 2 + moved * 0.005;
			valid++;
			if (score < bestScore)
			{
				bestScore = score;
				bestPos = cand;
				bestRoad = roadDist;
				bestRoadCost = roadCost;
				bestPlaceDist = placeDist;
				bestPlaceName = placeName;
				bestPlaceCost = placeCost;
				bestFree = free;
				bestRoomCost = roomCost;
				bestBuild = build;
				bestDensCost = densCost;
				bestNav = nav;
				bestClear = clearCount;
				bestMoved = moved;
			}
		}

		if (valid == 0)
		{
			string text = "";
			foreach (string key, int count : siteRej)
			{
				if (!text.IsEmpty())
					text += "; ";
				text += key + "=" + count;
			}

			why = string.Format("no urban site in %1 samples (%2 settlements near the wish): %3", sampled, nearPlaces.Count(), text);
			Reject("no urban site");
			return false;
		}

		pos = bestPos;
		string line = string.Format("CLW: layout HQ %1 site score %2 at %3 grid %4 (wish moved %5 m, %6 valid of %7 samples, %8 settlements near the wish): ", label, Math.Round(bestScore * 10) / 10, bestPos, CLW_Places.Grid(bestPos), Math.Round(bestMoved), valid, sampled, nearPlaces.Count());
		line += string.Format("road=%1 m (cost %2) place='%3' %4 m (cost %5) ", Math.Round(bestRoad * 10) / 10, Math.Round(bestRoadCost * 10) / 10, bestPlaceName, Math.Round(bestPlaceDist), Math.Round(bestPlaceCost * 10) / 10);
		line += string.Format("room=%1/%2 clear samples (cost %3) buildings=%4 (cost %5) navmesh=%6 clearable objects in the core=%7", bestFree, ringSamples, Math.Round(bestRoomCost * 10) / 10, bestBuild, Math.Round(bestDensCost * 10) / 10, bestNav, bestClear);
		Print(line, LogLevel.NORMAL);
		return true;
	}

	// Number of clear service-sized spots among iRingSamples points on the annulus around the HQ.
	protected int CountFreeRing(vector center)
	{
		return CountFreeRingEx(center, m_Cfg.m_fRingInnerM, m_Cfg.m_fRingOuterM, false);
	}

	// Same with the ring radii given; clearSmall = small clearable objects (CLW_PlaceClear) do not count as blocked.
	protected int CountFreeRingEx(vector center, float ringInner, float ringOuter, bool clearSmall)
	{
		CLW_SpotParams p = new CLW_SpotParams();
		p.m_bClearSmall = clearSmall;
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
			float radius = ringInner + (ringOuter - ringInner) * band / 3.0;
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

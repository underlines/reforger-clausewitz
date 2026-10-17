// FOB sites (M3 phase F, design section 4): where a side may establish a forward operating base.
// Candidates come from DATA (CLW_ScenarioConfig.m_aFobSites: absolute positions or positions relative to the HQ line, so they follow the
// battle-size layout) plus, M4.6, computed ones per objective and side (on the own side of the objective, in a distance band from it and away from
// the own HQ, so every flank gets one; the HQ-line sites are only a last resort). The LLM commander no longer picks from this pool: it places its
// FOB with the two-step tool (CLW_MapDigest + CLW_FobPlacer, which adds validated "llm" sites with AddDynamic); the pool is the RULE doctrine's
// candidate list (CLW_FobDoctrine). Every candidate is validated once at war start (after the battle layout
// moved the HQs/objectives): dry, flat, near a road, away from both bases, the objectives and the other sites. PickSites() then filters
// the pool for one side at the moment of choosing (enemy contacts, existing FOBs, own half of the map) and orders it toward the contested
// centre. No coordinates and no faction/prefab names in this code. Server only. Log prefix: "CLW: fob site".

// One site of the scenario data. m_vPos != 0: absolute wish position. Otherwise relative: m_fAlong (0 = first side's HQ, 1 = second side's
// HQ) on the HQ line, m_fLateral metres to its left (scaled by the battle size).
[BaseContainerProps()]
class CLW_FobSiteSetup
{
	[Attribute("", UIWidgets.EditBox, "Short unique id, e.g. FS_A (shown to the LLM)")]
	string m_sId;

	[Attribute("", UIWidgets.EditBox, "Display name; empty = 'FOB site near <place>'")]
	string m_sName;

	[Attribute("-1", UIWidgets.EditBox, "Side slot that may use it: 0 = first scenario faction, 1 = second, -1 = either")]
	int m_iSide;

	[Attribute("0 0 0", UIWidgets.EditBox, "Absolute position wish (x y z); 0 0 0 = use m_fAlong / m_fLateral")]
	vector m_vPos;

	[Attribute("0.5", UIWidgets.EditBox, "Relative: fraction of the way from the first HQ to the second (0..1)")]
	float m_fAlong;

	[Attribute("0", UIWidgets.EditBox, "Relative: metres to the left of the HQ line (first -> second HQ), scaled with the battle size")]
	float m_fLateral;

	[Attribute("0", UIWidgets.EditBox, "Smallest battle size index (0 = small) that offers this site")]
	int m_iMinBattleSize;
}

// A validated candidate.
class CLW_FobSite
{
	string m_sId;
	string m_sName;
	vector m_vPos;
	int m_iSide;			// -1 either, 0 / 1 scenario side slot
	string m_sSource;		// data | computed | llm
	float m_fRoadDist;
	string m_sObjective;	// computed per objective (M4.6): the objective this candidate was made for ("" = data / line / llm)
}

class CLW_FobSites
{
	static const float ROAD_MAX_M = 60;				// a site must have a road this close (the builder truck has to reach it)
	static const float BASE_CLEAR_M = 250;			// authored distance from an HQ, scaled with the battle size
	static const float BASE_CLEAR_FLOOR_M = 150;
	// objective clearance: CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M beyond the capture radius
	static const float SPACING_M = 220;				// authored distance between two sites, scaled
	static const float SPACING_FLOOR_M = 120;
	static const float SEARCH_RADIUS_MIN_M = 80;
	static const float SEARCH_RADIUS_FRAC = 0.04;	// of the HQ distance
	static const float LATERAL_FRAC = 0.05;			// computed sites: offset from the HQ line, fraction of the HQ distance
	static const int MAX_COMPUTED = 6;
	static const int MIN_SITES_WARN = 4;	// fewer pool sites than this is logged as a warning
	// M4.6 rule fallback: candidates per objective on the own side of it
	static const float OBJ_BAND_FRAC = 0.3;			// target distance from the objective, fraction of its distance from the own HQ ...
	static const float OBJ_BAND_MAX_FRAC = 0.5;		// ... never further than this fraction (or lo + OBJ_BAND_SLACK_M, whichever is larger)
	static const float OBJ_BAND_MARGIN_M = 60;		// beyond capture radius + FOB_MIN_OBJECTIVE_DIST_M
	static const float OBJ_BAND_SLACK_M = 210;
	static const float HQ_AWAY_FACTOR = 1.5;		// at least this many base clearances away from the own HQ
	static const float COVER_M = 900;				// an objective this close to a live / planned own FOB counts as covered (authored, scaled)
	static const int MAX_PER_SIDE = 8;				// objective candidates per side
	static const int MIN_PER_SIDE = 2;				// fewer than this and the old HQ-line sites are added as a last resort
	static const int MAX_POOL = 64;					// pool size cap (data + computed + dynamic)

	protected static ref array<ref CLW_FobSite> s_aSites = {};
	protected static ref map<string, string> s_mLastPick = new map<string, string>();	// side -> "<site>@<front>" last logged (one pick line per change)
	protected static int s_iComputed;
	protected static int s_iDynamic;

	static void Reset()
	{
		s_aSites.Clear();
		s_mLastPick.Clear();
		s_iComputed = 0;
		s_iDynamic = 0;
		CLW_MapDigest.Reset();		// the digest (terrain classes) is per world / war; rebuilt on first use
		CLW_FobPlanner.Reset();
	}

	// A site the HQ commander (LLM) picked and CLW_FobPlacer validated: added to the pool so ESTABLISH_FOB {site} and the builder logic
	// (site switch, relocation) treat it like any other. Null when the pool is full.
	static CLW_FobSite AddDynamic(string side, vector pos, float roadDist)
	{
		if (s_aSites.Count() >= MAX_POOL)
		{
			Print("CLW: fob site pool full, dynamic site refused", LogLevel.WARNING);
			return null;
		}

		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		s_iDynamic++;
		CLW_FobSite entry = new CLW_FobSite();
		entry.m_sId = "FSL" + s_iDynamic;
		entry.m_vPos = pos;
		if (scenario)
			entry.m_iSide = scenario.GetSideIndex(side);
		else
			entry.m_iSide = -1;
		entry.m_sSource = "llm";
		entry.m_fRoadDist = roadDist;
		string place = CLW_Places.PlaceLabel(pos, CLW_GameMode.GetControlPoints());
		if (place.IsEmpty())
			entry.m_sName = "FOB site " + entry.m_sId;
		else
			entry.m_sName = "FOB site near " + place;
		s_aSites.Insert(entry);
		return entry;
	}

	// Drops a pool site (a dynamic site whose ESTABLISH_FOB did not go through).
	static void RemoveSite(string id)
	{
		for (int i = s_aSites.Count() - 1; i >= 0; i--)
		{
			if (s_aSites[i].m_sId == id)
			{
				s_aSites.Remove(i);
				return;
			}
		}
	}

	static int Count()
	{
		return s_aSites.Count();
	}

	// All validated sites (any side).
	static void GetSites(notnull array<CLW_FobSite> outSites)
	{
		foreach (CLW_FobSite site : s_aSites)
		{
			outSites.Insert(site);
		}
	}

	static CLW_FobSite FindSite(string id)
	{
		if (id.IsEmpty())
			return null;

		foreach (CLW_FobSite site : s_aSites)
		{
			if (site.m_sId == id)
				return site;
		}

		return null;
	}

	// ---- building the pool ----

	// Validates the data sites and adds computed ones. Call at war start, after the battle layout.
	static void Build()
	{
		Reset();
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!scenario || !bases || !war || !scenario.m_aFactions || scenario.m_aFactions.Count() != 2)
		{
			Print("CLW: fob site pool skipped: no scenario / bases / two sides", LogLevel.WARNING);
			return;
		}

		vector hqFirst = bases.GetBasePos(scenario.GetFactionKey(0));
		vector hqSecond = bases.GetBasePos(scenario.GetFactionKey(1));
		float hqDist = vector.DistanceXZ(hqFirst, hqSecond);
		if (hqFirst == vector.Zero || hqSecond == vector.Zero || hqDist < 100)
		{
			Print("CLW: fob site pool skipped: HQ positions unknown", LogLevel.WARNING);
			return;
		}

		int sizeIndex;
		CLW_WarMode mode = war.GetMode();
		if (mode)
			sizeIndex = Math.Max(0, mode.GetBattleSizeIndex());

		float scale = bases.GetSeparationScale();
		float dirX = (hqSecond[0] - hqFirst[0]) / hqDist;
		float dirZ = (hqSecond[2] - hqFirst[2]) / hqDist;
		float leftX = -dirZ;	// left of the direction of travel (x = east, z = north)
		float leftZ = dirX;
		float baseClear = Math.Max(BASE_CLEAR_FLOOR_M, bases.ScaleDistance(BASE_CLEAR_M));
		float spacing = Math.Max(SPACING_FLOOR_M, bases.ScaleDistance(SPACING_M));
		float radius = Math.Max(SEARCH_RADIUS_MIN_M, hqDist * SEARCH_RADIUS_FRAC);

		array<vector> avoid = {};
		array<float> avoidDist = {};
		array<vector> objPos = {};
		array<float> objNeed = {};
		avoid.Insert(hqFirst);
		avoidDist.Insert(baseClear);
		avoid.Insert(hqSecond);
		avoidDist.Insert(baseClear);
		if (points)
		{
			for (int pi = 0; pi < points.Count(); pi++)
			{
				CLW_ControlPoint point = points.Get(pi);
				if (!point)
					continue;

				float need = point.m_fRadius + CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M;
				avoid.Insert(point.m_vPos);
				avoidDist.Insert(need);
				objPos.Insert(point.m_vPos);
				objNeed.Insert(need);
			}
		}

		// 1. data
		int fromData;
		if (scenario.m_aFobSites)
		{
			foreach (CLW_FobSiteSetup setup : scenario.m_aFobSites)
			{
				if (!setup || setup.m_sId.IsEmpty())
				{
					Print("CLW: fob site in the scenario data without an id ignored", LogLevel.WARNING);
					continue;
				}

				if (setup.m_iMinBattleSize > sizeIndex)
				{
					Print(string.Format("CLW: fob site %1 skipped: needs battle size >= %2 (is %3)", setup.m_sId, setup.m_iMinBattleSize, sizeIndex), LogLevel.NORMAL);
					continue;
				}

				vector wish = setup.m_vPos;
				if (wish == vector.Zero)
				{
					float lateral = setup.m_fLateral * Math.Max(0.3, scale);
					wish = Vector(hqFirst[0] + dirX * hqDist * setup.m_fAlong + leftX * lateral, 0, hqFirst[2] + dirZ * hqDist * setup.m_fAlong + leftZ * lateral);
				}

				vector hqOwn = hqFirst;
				if (setup.m_iSide == 1 || (setup.m_iSide < 0 && vector.DistanceXZ(wish, hqSecond) < vector.DistanceXZ(wish, hqFirst)))
					hqOwn = hqSecond;

				vector before = wish;
				if (!ShiftClear(wish, objPos, objNeed, hqOwn))
				{
					Print(string.Format("CLW: fob site %1 (data) dropped: no position %2 m clear of the objectives found from wish %3", setup.m_sId, Math.Round(CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M), before), LogLevel.NORMAL);
					continue;
				}

				if (vector.DistanceXZ(wish, before) > 1)
					Print(string.Format("CLW: fob site %1 (data) shifted from %2 to %3: too close to an objective (min %4 m clear)", setup.m_sId, before, wish, Math.Round(CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M)), LogLevel.NORMAL);

				if (TryAdd(setup.m_sId, setup.m_sName, wish, setup.m_iSide, "data", radius, spacing, avoid, avoidDist, points))
					fromData++;
			}
		}

		// 2. M4.6 rule fallback: one candidate per objective and side, on the own side of the objective (toward that side's HQ), at a sensible
		// distance band from it (beyond capture radius + FOB_MIN_OBJECTIVE_DIST_M, about 30 % of the way to the HQ, but at least
		// HQ_AWAY_FACTOR base clearances away from the HQ), so every flank objective gets one. A straight try first, then +-30 degrees
		// around the objective so the snap (road / dry / flat) has a second and third chance.
		int fromObjective = 0;
		if (points)
		{
			float frontMin = bases.ScaleDistance(CLW_FobManager.FRONT_MIN_M);
			for (int sideSlot = 0; sideSlot < 2; sideSlot++)
			{
				vector hqSide = hqFirst;
				if (sideSlot == 1)
					hqSide = hqSecond;

				int madeForSide = 0;
				for (int oi = 0; oi < points.Count(); oi++)
				{
					if (madeForSide >= MAX_PER_SIDE)
						break;

					CLW_ControlPoint obj = points.Get(oi);
					if (!obj)
						continue;

					float objDist = vector.DistanceXZ(obj.m_vPos, hqSide);
					if (objDist < frontMin)
						continue;	// close to this HQ: the HQ itself covers it, no FOB needed

					float oNeed = obj.m_fRadius + CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M;
					float oLo = oNeed + OBJ_BAND_MARGIN_M;
					float oHi = Math.Max(oLo + OBJ_BAND_SLACK_M, objDist * OBJ_BAND_MAX_FRAC);
					float oMaxFrom = objDist - baseClear * HQ_AWAY_FACTOR;
					if (oMaxFrom < oLo)
					{
						Print(string.Format("CLW: fob site objective %1 side %2: no band (objective %3 m from the HQ, needs %4 m clear of the objective and %5 m clear of the HQ)", obj.m_sId, sideSlot, Math.Round(objDist), Math.Round(oLo), Math.Round(baseClear * HQ_AWAY_FACTOR)), LogLevel.NORMAL);
						continue;
					}

					float oWant = Math.Min(Math.Clamp(objDist * OBJ_BAND_FRAC, oLo, oHi), oMaxFrom);
					float ux = (hqSide[0] - obj.m_vPos[0]) / objDist;
					float uz = (hqSide[2] - obj.m_vPos[2]) / objDist;
					bool solved = false;
					array<float> angles = {0, 0.5236, -0.5236};
					foreach (float ang : angles)
					{
						if (solved)
							break;

						float cs = Math.Cos(ang);
						float sn = Math.Sin(ang);
						vector cand = Vector(obj.m_vPos[0] + (ux * cs - uz * sn) * oWant, 0, obj.m_vPos[2] + (ux * sn + uz * cs) * oWant);
						if (!ShiftClear(cand, objPos, objNeed, hqSide))
							continue;

						s_iComputed++;
						if (TryAdd("FSO" + obj.m_sId + "_" + sideSlot, "", cand, sideSlot, "computed", radius, spacing, avoid, avoidDist, points))
						{
							s_aSites[s_aSites.Count() - 1].m_sObjective = obj.m_sId;
							solved = true;
							madeForSide++;
							fromObjective++;
						}
					}

					if (!solved)
						Print(string.Format("CLW: fob site objective %1 side %2: no acceptable ground in the band %3-%4 m from the objective", obj.m_sId, sideSlot, Math.Round(oLo), Math.Round(oWant)), LogLevel.NORMAL);
				}
			}
		}

		// 3. last resort: computed along the HQ line, fractions of the way, first half = first side. Only for a side with fewer than
		// MIN_PER_SIDE candidates so far (no objectives / no ground near them). Every fraction tries the lateral offsets in turn
		// (alternating sides, then twice as far out) until a site is accepted, so a fraction whose first spot has no road or is forest
		// does not cost a site.
		array<float> fractions = {0.22, 0.35, 0.65, 0.78, 0.45, 0.55, 0.15, 0.85};
		array<float> laterals = {1, -1, 2.5, -2.5, 0};
		int fromComputed;
		foreach (int fi, float fraction : fractions)
		{
			if (fromComputed >= MAX_COMPUTED)
				break;

			int side = 0;
			if (fraction > 0.5)
				side = 1;
			if (fraction == 0.5)
				side = -1;

			if (side >= 0 && CountForSide(side) >= MIN_PER_SIDE)
				continue;

			vector hqNear = hqFirst;
			if (side == 1)
				hqNear = hqSecond;

			float firstSign = 1;
			if (fi % 2 == 1)
				firstSign = -1;

			bool added;
			foreach (float lateralFactor : laterals)
			{
				if (added)
					break;

				float offset = hqDist * LATERAL_FRAC * lateralFactor * firstSign;
				vector computedWish = Vector(hqFirst[0] + dirX * hqDist * fraction + leftX * offset, 0, hqFirst[2] + dirZ * hqDist * fraction + leftZ * offset);
				if (!ShiftClear(computedWish, objPos, objNeed, hqNear))
					continue;

				s_iComputed++;
				if (TryAdd("FSC" + s_iComputed, "", computedWish, side, "computed", radius, spacing, avoid, avoidDist, points))
				{
					fromComputed++;
					added = true;
				}
			}
		}

		if (s_aSites.Count() < MIN_SITES_WARN)
			Print(string.Format("CLW: fob site WARNING only %1 sites on this map (roads / clear ground too sparse near the HQ line): FOB builds will be limited", s_aSites.Count()), LogLevel.WARNING);

		Print(string.Format("CLW: fob site pool ready: %1 sites (%2 from data, %3 per objective, %4 along the HQ line), hq distance %5 m, base clear %6 m, spacing %7 m", s_aSites.Count(), fromData, fromObjective, fromComputed, Math.Round(hqDist), Math.Round(baseClear), Math.Round(spacing)), LogLevel.NORMAL);
	}

	// Pool sites usable by a side slot (0 / 1) or by either side.
	protected static int CountForSide(int sideSlot)
	{
		int count;
		foreach (CLW_FobSite site : s_aSites)
		{
			if (site.m_iSide == sideSlot || site.m_iSide < 0)
				count++;
		}

		return count;
	}

	// Moves 'wish' straight away from the objective it violates most until every objective is 'objNeed' clear (+5 m). Degenerate
	// direction (wish on the objective): toward 'hq'. False when no clear position is found in a few steps.
	protected static bool ShiftClear(inout vector wish, notnull array<vector> objPos, notnull array<float> objNeed, vector hq)
	{
		for (int iter = 0; iter < 12; iter++)
		{
			int worst = -1;
			float worstGap = 0;
			for (int i = 0; i < objPos.Count(); i++)
			{
				float gap = objNeed[i] - vector.DistanceXZ(wish, objPos[i]);
				if (gap > worstGap)
				{
					worstGap = gap;
					worst = i;
				}
			}

			if (worst < 0)
				return true;

			vector obj = objPos[worst];
			float dx = wish[0] - obj[0];
			float dz = wish[2] - obj[2];
			float len = Math.Sqrt(dx * dx + dz * dz);
			if (len < 1)
			{
				dx = hq[0] - obj[0];
				dz = hq[2] - obj[2];
				len = Math.Sqrt(dx * dx + dz * dz);
				if (len < 1)
					return false;
			}

			float push = objNeed[worst] + 5;
			wish[0] = obj[0] + dx / len * push;
			wish[2] = obj[2] + dz / len * push;
		}

		return false;
	}

	// A valid free site near 'around' (builder relocation): dry, flat, clear of the HQs, the objectives (FOB_MIN_OBJECTIVE_DIST_M) and the
	// extra positions. No road needed. False when there is none within the search radius.
	static bool FindNear(vector around, float searchRadius, notnull array<vector> extraAvoid, notnull array<float> extraDist, out vector site)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		array<vector> avoid = {};
		array<float> avoidDist = {};
		if (scenario && bases && scenario.m_aFactions && scenario.m_aFactions.Count() == 2)
		{
			float baseClear = Math.Max(BASE_CLEAR_FLOOR_M, bases.ScaleDistance(BASE_CLEAR_M));
			for (int b = 0; b < 2; b++)
			{
				vector hq = bases.GetBasePos(scenario.GetFactionKey(b));
				if (hq != vector.Zero)
				{
					avoid.Insert(hq);
					avoidDist.Insert(baseClear);
				}
			}
		}

		if (points)
		{
			for (int pi = 0; pi < points.Count(); pi++)
			{
				CLW_ControlPoint point = points.Get(pi);
				if (!point)
					continue;

				avoid.Insert(point.m_vPos);
				avoidDist.Insert(point.m_fRadius + CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M);
			}
		}

		for (int xi = 0; xi < extraAvoid.Count(); xi++)
		{
			avoid.Insert(extraAvoid[xi]);
			avoidDist.Insert(extraDist[xi]);
		}

		float roadDist;
		float score;
		if (!CLW_Placement.FindSiteEx(around, searchRadius, avoid, avoidDist, site, roadDist, score))
			return false;

		// room for the FOB cluster: clear of buildings, fences, props and trees (CLW_FreeSpot), shifting the site if needed
		return CLW_FreeSpot.RefineSite("fob relocation", site, avoid, avoidDist);
	}

	protected static bool TryAdd(string id, string name, vector wish, int side, string source, float radius, float spacing, notnull array<vector> avoid, notnull array<float> avoidDist, CLW_ControlPoints points)
	{
		foreach (CLW_FobSite existing : s_aSites)
		{
			if (existing.m_sId == id)
			{
				Print("CLW: fob site duplicate id " + id + " ignored", LogLevel.WARNING);
				return false;
			}
		}

		vector site;
		float roadDist;
		float score;
		if (!CLW_Placement.FindSiteEx(wish, radius, avoid, avoidDist, site, roadDist, score))
		{
			Print(string.Format("CLW: fob site %1 (%2) rejected: no acceptable ground near %3 (search %4 m)", id, source, wish, Math.Round(radius)), LogLevel.NORMAL);
			return false;
		}

		// room for the FOB cluster: clear of buildings, fences, props and trees (CLW_FreeSpot), the site shifts to clear ground nearby
		vector refined = site;
		if (!CLW_FreeSpot.RefineSite("fob site " + id, refined, avoid, avoidDist))
		{
			Print(string.Format("CLW: fob site %1 (%2) rejected at %3: no clear ground for the FOB cluster within %4 m", id, source, site, Math.Round(CLW_SpotConfig.FOB_REFINE_RADIUS_M)), LogLevel.NORMAL);
			return false;
		}

		if (vector.DistanceXZ(refined, site) > 1)
		{
			Print(string.Format("CLW: fob site %1 (%2) shifted from %3 to %4: obstacles / slope at the first choice", id, source, site, refined), LogLevel.NORMAL);
			site = refined;
			roadDist = CLW_Placement.NearestRoadDistance(site);
		}

		if (roadDist < 0 || roadDist > ROAD_MAX_M)
		{
			Print(string.Format("CLW: fob site %1 (%2) rejected at %3: nearest road %4 m (limit %5 m)", id, source, site, Math.Round(roadDist), ROAD_MAX_M), LogLevel.NORMAL);
			return false;
		}

		CLW_FobSite entry = new CLW_FobSite();
		entry.m_sId = id;
		entry.m_vPos = site;
		entry.m_iSide = side;
		entry.m_sSource = source;
		entry.m_fRoadDist = roadDist;
		entry.m_sName = name;
		if (entry.m_sName.IsEmpty())
		{
			string place = CLW_Places.PlaceLabel(site, points);
			if (place.IsEmpty())
				entry.m_sName = "FOB site " + id;
			else
				entry.m_sName = "FOB site near " + place;
		}

		s_aSites.Insert(entry);
		avoid.Insert(site);
		avoidDist.Insert(spacing);
		Print(string.Format("CLW: fob site %1 '%2' side=%3 at %4 grid %5 road=%6 m (%7, wish %8)", id, entry.m_sName, side, site, CLW_Places.Grid(site), Math.Round(roadDist), source, wish), LogLevel.NORMAL);
		return true;
	}

	// ---- choosing ----

	// Mean position of the objectives (the contested centre); the midpoint of the HQs when there are none.
	static vector Centre()
	{
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		vector sum = vector.Zero;
		int count = 0;
		if (points)
		{
			for (int i = 0; i < points.Count(); i++)
			{
				CLW_ControlPoint point = points.Get(i);
				if (!point)
					continue;

				sum = sum + point.m_vPos;
				count++;
			}
		}

		if (count > 0)
			return Vector(sum[0] / count, 0, sum[2] / count);

		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (scenario && bases)
		{
			vector first = bases.GetBasePos(scenario.GetFactionKey(0));
			vector second = bases.GetBasePos(scenario.GetFactionKey(1));
			return Vector((first[0] + second[0]) * 0.5, 0, (first[2] + second[2]) * 0.5);
		}

		return vector.Zero;
	}

	// The objective the side fights for next (owner playtest 2026-10-12: FOBs go toward the front, not behind the HQ): of the objectives
	// the side does not hold, the nearest to its HQ that is held by the enemy or contested; else the nearest not held. Null when there is none.
	static CLW_ControlPoint FrontObjective(string side)
	{
		// M4.6: objectives an own live / planned FOB already covers come last, so a second FOB goes to the next flank
		CLW_ControlPoint uncovered = FrontObjectiveEx(side, true);
		if (uncovered)
			return uncovered;

		return FrontObjectiveEx(side, false);
	}

	// An own live or planned FOB stands within COVER_M (scaled) of the objective.
	static bool IsCovered(string side, CLW_ControlPoint point)
	{
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (!point || !bases)
			return false;

		float cover = bases.ScaleDistance(COVER_M);
		array<CLW_Fob> fobs = {};
		CLW_FobManager.GetFobs(side, fobs);
		foreach (CLW_Fob fob : fobs)
		{
			if (vector.DistanceXZ(fob.m_vPos, point.m_vPos) < cover)
				return true;
		}

		return false;
	}

	protected static CLW_ControlPoint FrontObjectiveEx(string side, bool skipCovered)
	{
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (!points || !bases)
			return null;

		vector hq = bases.GetBasePos(side);
		CLW_ControlPoint hot;
		CLW_ControlPoint any;
		float hotDist = 1000000;
		float anyDist = 1000000;
		for (int i = 0; i < points.Count(); i++)
		{
			CLW_ControlPoint point = points.Get(i);
			if (!point || point.m_sOwner == side)
				continue;

			if (skipCovered && IsCovered(side, point))
				continue;

			float dist = vector.DistanceXZ(point.m_vPos, hq);
			if ((point.m_bContested || !point.m_sOwner.IsEmpty()) && dist < hotDist)
			{
				hotDist = dist;
				hot = point;
			}

			if (dist < anyDist)
			{
				anyDist = dist;
				any = point;
			}
		}

		if (hot)
			return hot;

		return any;
	}

	// Front direction term of a site: distance to the front objective + half the sideways offset from the HQ->front line, plus the HQ->front
	// distance when the site lies behind or at the HQ (progress along the line below 15 %). Lower = better. detail = log text.
	static float FrontScore(vector hq, CLW_ControlPoint front, CLW_FobSite site, out string detail)
	{
		float dx = front.m_vPos[0] - hq[0];
		float dz = front.m_vPos[2] - hq[2];
		float len = Math.Max(1, Math.Sqrt(dx * dx + dz * dz));
		float rx = site.m_vPos[0] - hq[0];
		float rz = site.m_vPos[2] - hq[2];
		float progress = (rx * dx + rz * dz) / (len * len);
		float lateral = Math.AbsFloat(rx * dz - rz * dx) / len;
		float toFront = vector.DistanceXZ(site.m_vPos, front.m_vPos);
		float score = toFront + lateral * 0.5;
		if (progress < 0.15)
			score += len;

		detail = string.Format("toFront %1 m, lateral %2 m, progress %3 percent of the way to the front", Math.Round(toFront), Math.Round(lateral), Math.Round(progress * 100));
		return score;
	}

	// Up to 'max' currently usable sites for the side, best first: toward the front objective (FrontScore), else nearest to the contested
	// centre. The top pick is logged when it changes. Returns the count.
	static int PickSites(string side, int max, notnull array<CLW_FobSite> outSites)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario || max <= 0)
			return 0;

		int sideIndex = scenario.GetSideIndex(side);
		vector centre = Centre();
		CLW_ControlPoint front = FrontObjective(side);
		CLW_Bases ownBases = CLW_GameMode.GetBases();
		vector ownHq;
		if (ownBases)
			ownHq = ownBases.GetBasePos(side);
		array<CLW_FobSite> ranked = {};
		array<float> distances = {};
		array<string> details = {};
		foreach (CLW_FobSite site : s_aSites)
		{
			if (site.m_iSide >= 0 && site.m_iSide != sideIndex)
				continue;

			string why;
			if (!CLW_FobManager.CheckSite(side, site, why))
				continue;

			string detail;
			float distance;
			if (front && ownHq != vector.Zero)
				distance = FrontScore(ownHq, front, site, detail);
			else
			{
				distance = vector.DistanceXZ(site.m_vPos, centre);
				detail = "toCentre " + Math.Round(distance) + " m (no front objective)";
			}

			int at = ranked.Count();
			for (int k = 0; k < ranked.Count(); k++)
			{
				if (distance < distances[k])
				{
					at = k;
					break;
				}
			}

			ranked.InsertAt(site, at);
			distances.InsertAt(distance, at);
			details.InsertAt(detail, at);
		}

		if (!ranked.IsEmpty())
		{
			string frontId = "centre";
			if (front)
				frontId = front.m_sId;
			string pickKey = ranked[0].m_sId + "@" + frontId;
			string lastKey;
			if (!s_mLastPick.Find(side, lastKey) || lastKey != pickKey)
			{
				s_mLastPick.Set(side, pickKey);
				Print(string.Format("CLW: fob site pick %1: %2 '%3' score %4 (%5), front %6, %7 usable", side, ranked[0].m_sId, ranked[0].m_sName, Math.Round(distances[0]), details[0], frontId, ranked.Count()), LogLevel.NORMAL);
			}
		}

		foreach (int n, CLW_FobSite picked : ranked)
		{
			if (n >= max)
				break;

			outSites.Insert(picked);
		}

		return outSites.Count();
	}
}

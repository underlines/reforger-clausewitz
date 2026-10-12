// FOB sites (M3 phase F, design section 4): where a side may establish a forward operating base.
// Candidates come from DATA (CLW_ScenarioConfig.m_aFobSites: absolute positions or positions relative to the HQ line, so they follow the
// battle-size layout) plus a few computed ones along the HQ line. Every candidate is validated once at war start (after the battle layout
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
	string m_sSource;		// data | computed
	float m_fRoadDist;
}

class CLW_FobSites
{
	static const float ROAD_MAX_M = 60;				// a site must have a road this close (the builder truck has to reach it)
	static const float BASE_CLEAR_M = 250;			// authored distance from an HQ, scaled with the battle size
	static const float BASE_CLEAR_FLOOR_M = 150;
	static const float OBJECTIVE_CLEAR_M = 100;		// beyond the capture radius
	static const float SPACING_M = 220;				// authored distance between two sites, scaled
	static const float SPACING_FLOOR_M = 120;
	static const float SEARCH_RADIUS_MIN_M = 80;
	static const float SEARCH_RADIUS_FRAC = 0.04;	// of the HQ distance
	static const float LATERAL_FRAC = 0.05;			// computed sites: offset from the HQ line, fraction of the HQ distance
	static const int MAX_COMPUTED = 6;

	protected static ref array<ref CLW_FobSite> s_aSites = {};
	protected static int s_iComputed;

	static void Reset()
	{
		s_aSites.Clear();
		s_iComputed = 0;
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

				avoid.Insert(point.m_vPos);
				avoidDist.Insert(point.m_fRadius + OBJECTIVE_CLEAR_M);
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

				if (TryAdd(setup.m_sId, setup.m_sName, wish, setup.m_iSide, "data", radius, spacing, avoid, avoidDist, points))
					fromData++;
			}
		}

		// 2. computed along the HQ line: fractions of the way, alternating sides of the line, first half = first side
		array<float> fractions = {0.22, 0.35, 0.65, 0.78, 0.45, 0.55};
		int fromComputed;
		foreach (int fi, float fraction : fractions)
		{
			if (fromComputed >= MAX_COMPUTED)
				break;

			float offset = hqDist * LATERAL_FRAC;
			if (fi % 2 == 1)
				offset = -offset;

			vector computedWish = Vector(hqFirst[0] + dirX * hqDist * fraction + leftX * offset, 0, hqFirst[2] + dirZ * hqDist * fraction + leftZ * offset);
			int side = 0;
			if (fraction > 0.5)
				side = 1;
			if (fraction == 0.5)
				side = -1;

			s_iComputed++;
			if (TryAdd("FSC" + s_iComputed, "", computedWish, side, "computed", radius, spacing, avoid, avoidDist, points))
				fromComputed++;
		}

		Print(string.Format("CLW: fob site pool ready: %1 sites (%2 from data, %3 computed), hq distance %4 m, base clear %5 m, spacing %6 m", s_aSites.Count(), fromData, fromComputed, Math.Round(hqDist), Math.Round(baseClear), Math.Round(spacing)), LogLevel.NORMAL);
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

	// Up to 'max' currently usable sites for the side, nearest to the contested centre first (the frontline). Returns the count.
	static int PickSites(string side, int max, notnull array<CLW_FobSite> outSites)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario || max <= 0)
			return 0;

		int sideIndex = scenario.GetSideIndex(side);
		vector centre = Centre();
		array<CLW_FobSite> ranked = {};
		array<float> distances = {};
		foreach (CLW_FobSite site : s_aSites)
		{
			if (site.m_iSide >= 0 && site.m_iSide != sideIndex)
				continue;

			string why;
			if (!CLW_FobManager.CheckSite(side, site, why))
				continue;

			float distance = vector.DistanceXZ(site.m_vPos, centre);
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

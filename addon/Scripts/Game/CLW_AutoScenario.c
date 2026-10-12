// Auto scenario (M4 WP1): a scenario for ANY map without hand-written positions. Which scenario a world gets is decided here
// (FindForWorld: Configs/CLW/Scenarios/<world file name>.conf, then a scenario whose m_aWorlds lists the world, else the auto scenario).
//
// The auto scenario is Configs/CLW/Scenarios/_Auto.conf (everything that is not map specific: side slots with default factions,
// economy, war modes, start forces, the limits below). At game start, BEFORE CLW_Bases / CLW_ControlPoints are created (players spawn
// at the HQ during the SETUP phase), Fill() writes the map specific parts into that scenario object:
//   - HQ A / HQ B: the random-layout HQ search (CLW_RandomLayout.FindHqPair; strict, then relaxed, then a raw last resort), seed logged
//   - objectives: named map places (CLW_Places) in the band between the HQs, one per slot along the HQ line so they are spread out,
//     towns / villages first, then other places; slots without a place get a point on the HQ line validated by CLW_FreeSpot.
//     m_aObjectiveCounts says how many objectives each battle size uses: the objectives nearest the middle of the line are kept in
//     small battles (CLW_ObjectiveSetup.m_iMinBattleSize), and the outermost kept ones start owned by the side next to them.
//   - vehicle / infantry slots stay empty: CLW_BasePlacement finds road / free ground spots around the HQ by itself
//   - FOB sites stay empty: CLW_FobSites computes them from the HQ line
// With startPositions=fixed the war keeps these positions (scaled by the battle size like any authored layout); with =random the
// war start runs the same search again (CLW_WarMode.ApplyBattleLayout). Server only. Log prefixes: "CLW: scenario", "CLW: auto".

[BaseContainerProps()]
class CLW_AutoSetup
{
	[Attribute("2200", UIWidgets.EditBox, "HQ distance (m) of a full size battle; capped by m_fMapFraction of the playable area")]
	float m_fHqDistanceM;

	[Attribute("0.45", UIWidgets.EditBox, "The HQ distance is at most this fraction of the shorter playable side of the map")]
	float m_fMapFraction;

	[Attribute("600", UIWidgets.EditBox, "Smallest HQ distance (m) of the search")]
	float m_fMinHqDistanceM;

	[Attribute("0", UIWidgets.EditBox, "Seed of the game start search (0 = time based; logged)")]
	int m_iSeed;

	[Attribute(desc: "Objectives used by the battle sizes, ascending (index = battle size index): the last number is the most objectives placed")]
	ref array<int> m_aObjectiveCounts;

	[Attribute("250", UIWidgets.EditBox, "Minimum distance (m) between two objectives")]
	float m_fObjectiveSpacingM;

	[Attribute("0.15", UIWidgets.EditBox, "Objectives lie between this fraction of the way from HQ A to HQ B ...")]
	float m_fAlongMin;

	[Attribute("0.85", UIWidgets.EditBox, "... and this one")]
	float m_fAlongMax;

	[Attribute("0.35", UIWidgets.EditBox, "A place may lie this fraction of the HQ distance to the side of the HQ line")]
	float m_fMaxLateralFrac;

	[Attribute("350", UIWidgets.EditBox, "Objectives keep this distance (m) from both HQs")]
	float m_fHqClearM;

	[Attribute("80", UIWidgets.EditBox, "Capture radius (m) of an objective at a town / city")]
	float m_fRadiusTownM;

	[Attribute("50", UIWidgets.EditBox, "Capture radius (m) of any other objective")]
	float m_fRadiusSmallM;

	[Attribute("8", UIWidgets.EditBox, "Half size (m) of the clear footprint of an objective on the HQ line (no place found)")]
	float m_fObjectiveHalfM;

	[Attribute("250", UIWidgets.EditBox, "Search radius (m) around a point of the HQ line for a clear objective spot")]
	float m_fLineSearchM;

	[Attribute(desc: "Map descriptor type texts of places tried first (substring of e.g. name_village)")]
	ref array<string> m_aPrimaryTypes;

	[Attribute(desc: "Map descriptor type texts of places worth 2 points and the big radius")]
	ref array<string> m_aTownTypes;

	[Attribute(desc: "Map descriptor type texts never used as an objective (water features...)")]
	ref array<string> m_aExcludeTypes;

	// Built-in values for fields a data block does not carry (the loader leaves absent fields at 0 / null).
	void ApplyDefaults()
	{
		if (m_fHqDistanceM <= 0)
			m_fHqDistanceM = 2200;
		if (m_fMapFraction <= 0)
			m_fMapFraction = 0.45;
		if (m_fMinHqDistanceM <= 0)
			m_fMinHqDistanceM = 600;
		if (!m_aObjectiveCounts || m_aObjectiveCounts.IsEmpty())
			m_aObjectiveCounts = {4, 5, 7};
		if (m_fObjectiveSpacingM <= 0)
			m_fObjectiveSpacingM = 250;
		if (m_fAlongMax <= m_fAlongMin)
		{
			m_fAlongMin = 0.15;
			m_fAlongMax = 0.85;
		}
		if (m_fMaxLateralFrac <= 0)
			m_fMaxLateralFrac = 0.35;
		if (m_fHqClearM <= 0)
			m_fHqClearM = 350;
		if (m_fRadiusTownM <= 0)
			m_fRadiusTownM = 80;
		if (m_fRadiusSmallM <= 0)
			m_fRadiusSmallM = 50;
		if (m_fObjectiveHalfM <= 0)
			m_fObjectiveHalfM = 8;
		if (m_fLineSearchM <= 0)
			m_fLineSearchM = 250;
		if (!m_aPrimaryTypes || m_aPrimaryTypes.IsEmpty())
			m_aPrimaryTypes = {"village", "town", "city", "settlement"};
		if (!m_aTownTypes || m_aTownTypes.IsEmpty())
			m_aTownTypes = {"town", "city"};
		if (!m_aExcludeTypes)
			m_aExcludeTypes = {};
	}
}

// One objective candidate / pick of the auto scenario.
class CLW_AutoObjective
{
	string m_sName;
	vector m_vPos;
	float m_fAlong;		// 0 = HQ A, 1 = HQ B
	float m_fLateral;	// m to the left of the HQ line
	int m_iTier;		// 0 = primary place type, 1 = other place
	bool m_bTown;
	string m_sSource;	// place | line | line (unchecked)
}

class CLW_AutoScenario
{
	static const ResourceName AUTO_PATH = "Configs/CLW/Scenarios/_Auto.conf";
	static const string SCENARIO_DIR = "Configs/CLW/Scenarios";
	static const string AUTO_FILE = "_Auto.conf";
	static const string LETTERS = "ABCDEFGHIJKLMNOP";

	protected static bool s_bActive;
	protected static int s_iSeed;

	// True when the running scenario is the auto scenario.
	static bool IsAuto()
	{
		return s_bActive;
	}

	// Seed of the game start HQ search (0 when the scenario is not the auto one).
	static int GetSeed()
	{
		return s_iSeed;
	}

	// ---- which scenario for which world ----

	// File name of the loaded world without folder and extension ("CLW_Eden"), "" when unknown.
	static string WorldName()
	{
		string file = GetGame().GetWorldFile();
		if (file.IsEmpty())
			return "";

		return FilePath.StripExtension(FilePath.StripPath(file));
	}

	// Scenario resource for the world: Configs/CLW/Scenarios/<world>.conf (source "by-world"), else the first scenario (not _Auto) that
	// lists the world in m_aWorlds (source "by-listing"). "" when there is none.
	static ResourceName FindForWorld(string worldName, out string source)
	{
		source = "";
		if (worldName.IsEmpty())
			return "";

		string wanted = worldName + ".conf";
		wanted.ToLower();
		array<string> files = SCR_FileIOHelper.FindFiles(SCENARIO_DIR, ".conf");	// [verify] returns resource-relative paths on a packed addon
		Print(string.Format("CLW: scenario search for world '%1': %2 scenario files in %3", worldName, files.Count(), SCENARIO_DIR), LogLevel.NORMAL);
		foreach (string file : files)
		{
			string baseName = FilePath.StripPath(file);
			string lower = baseName;
			lower.ToLower();
			if (lower == wanted && baseName != AUTO_FILE)
			{
				source = "by-world";
				return SCENARIO_DIR + "/" + baseName;
			}
		}

		if (files.IsEmpty() && FileIO.FileExists(SCENARIO_DIR + "/" + worldName + ".conf"))	// [verify] listing unavailable: ask for the file itself
		{
			source = "by-world";
			return SCENARIO_DIR + "/" + worldName + ".conf";
		}

		foreach (string listed : files)
		{
			string listedName = FilePath.StripPath(listed);
			if (listedName == AUTO_FILE || listedName.Substring(0, 1) == "_")
				continue;

			ResourceName path = SCENARIO_DIR + "/" + listedName;
			CLW_ScenarioConfig peek = CLW_ScenarioConfig.Peek(path);
			if (peek && peek.ListsWorld(worldName))
			{
				source = "by-listing";
				return path;
			}
		}

		return "";
	}

	// Loads _Auto.conf and fills the map specific parts. Null when the template does not load.
	static CLW_ScenarioConfig Create(string worldName)
	{
		Print("CLW: auto scenario: loading the template " + AUTO_PATH + " (its positions are placeholders until filled)", LogLevel.NORMAL);
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Load(AUTO_PATH);
		if (!scenario)
		{
			Print("CLW: auto scenario template failed to load: " + AUTO_PATH, LogLevel.ERROR);
			return null;
		}

		s_bActive = true;
		Fill(scenario, worldName);
		scenario.LogSummary(AUTO_PATH + " (filled)");
		return scenario;
	}

	// ---- filling ----

	// HQs and objectives into the scenario object (before CLW_Bases / CLW_ControlPoints read it).
	static void Fill(notnull CLW_ScenarioConfig scenario, string worldName)
	{
		CLW_AutoSetup cfg = scenario.m_Auto;
		if (!cfg)
			cfg = new CLW_AutoSetup();
		cfg.ApplyDefaults();

		if (!scenario.m_aFactions || scenario.m_aFactions.Count() != 2)
		{
			Print("CLW: auto scenario: the template needs exactly 2 faction slots", LogLevel.ERROR);
			return;
		}

		CLW_Places.Ensure();
		array<ref CLW_Place> places = {};
		CLW_Places.GetAll(places);
		vector bmin, bmax;
		CLW_RandomLayout.GetPlayableBounds(bmin, bmax);
		float minSide = Math.Min(bmax[0] - bmin[0], bmax[2] - bmin[2]);
		float wanted = Math.Min(cfg.m_fHqDistanceM, cfg.m_fMapFraction * minSide);
		float minHq = Math.Min(cfg.m_fMinHqDistanceM, wanted);
		Print(string.Format("CLW: auto world='%1' area %2 .. %3 (shorter side %4 m): %5 map places, wanted HQ distance %6 m (data %7 m, map fraction %8)", worldName, bmin, bmax, Math.Round(minSide), places.Count(), Math.Round(wanted), Math.Round(cfg.m_fHqDistanceM), cfg.m_fMapFraction), LogLevel.NORMAL);

		CLW_RandomLayout layout = CLW_RandomLayout.Create(scenario, cfg.m_iSeed);
		s_iSeed = layout.m_iSeed;
		string how = "strict search";
		bool found = layout.FindHqPair(wanted, minHq);
		if (!found)
		{
			layout.Relax();
			found = layout.FindHqPair(wanted, minHq);
			how = "relaxed search";
		}

		vector hqA;
		vector hqB;
		if (found)
		{
			hqA = layout.m_vHqA;
			hqB = layout.m_vHqB;
		}
		else
		{
			how = "LAST RESORT (no valid HQ pair found)";
			Print("CLW: auto ERROR no HQ pair found even with the relaxed search: using raw land near the map centre", LogLevel.ERROR);
			LastResortHqs(bmin, bmax, wanted, hqA, hqB);
		}

		scenario.m_aFactions[0].m_vBasePos = hqA;
		scenario.m_aFactions[0].m_vBaseFallbackPos = vector.Zero;
		scenario.m_aFactions[1].m_vBasePos = hqB;
		scenario.m_aFactions[1].m_vBaseFallbackPos = vector.Zero;
		scenario.m_sName = "Auto " + worldName;
		float actual = vector.DistanceXZ(hqA, hqB);
		Print(string.Format("CLW: auto HQs via %1, seed=%2: %3 at %4 (grid %5), %6 at %7 (grid %8), distance %9 m", how, s_iSeed, scenario.m_aFactions[0].m_sKey, hqA, CLW_Places.Grid(hqA), scenario.m_aFactions[1].m_sKey, hqB, CLW_Places.Grid(hqB), Math.Round(actual)), LogLevel.NORMAL);

		BuildObjectives(scenario, cfg, hqA, hqB);
	}

	// Two dry sites on a line through the map centre (west / east), whatever the rules say.
	protected static void LastResortHqs(vector bmin, vector bmax, float wanted, out vector hqA, out vector hqB)
	{
		vector centre = Vector((bmin[0] + bmax[0]) * 0.5, 0, (bmin[2] + bmax[2]) * 0.5);
		array<vector> avoid = {};
		array<float> avoidDist = {};
		vector wishA = centre - Vector(wanted * 0.5, 0, 0);
		vector wishB = centre + Vector(wanted * 0.5, 0, 0);
		float roadDist;
		float score;
		CLW_Placement.FindSiteEx(wishA, 800, avoid, avoidDist, hqA, roadDist, score);
		CLW_Placement.FindSiteEx(wishB, 800, avoid, avoidDist, hqB, roadDist, score);
	}

	protected static bool TypeMatches(string type, array<string> parts)
	{
		if (!parts)
			return false;

		foreach (string part : parts)
		{
			if (type.Contains(part))
				return true;
		}

		return false;
	}

	protected static bool FitsSpacing(vector pos, notnull array<ref CLW_AutoObjective> chosen, float spacing)
	{
		foreach (CLW_AutoObjective other : chosen)
		{
			if (other && vector.DistanceXZ(pos, other.m_vPos) < spacing)
				return false;
		}

		return true;
	}

	// Objectives: one per slot along the HQ line, from places in the band, else a validated point on the line.
	protected static void BuildObjectives(notnull CLW_ScenarioConfig scenario, notnull CLW_AutoSetup cfg, vector hqA, vector hqB)
	{
		float hqDist = vector.DistanceXZ(hqA, hqB);
		if (hqDist < 100)
		{
			Print("CLW: auto objectives skipped: HQs closer than 100 m", LogLevel.ERROR);
			return;
		}

		float dirX = (hqB[0] - hqA[0]) / hqDist;
		float dirZ = (hqB[2] - hqA[2]) / hqDist;
		int slots = cfg.m_aObjectiveCounts[cfg.m_aObjectiveCounts.Count() - 1];
		string letters = LETTERS;
		slots = Math.Clamp(slots, 1, letters.Length());

		// candidates: named places in the band between the HQs, on dry usable ground
		array<ref CLW_Place> places = {};
		CLW_Places.GetAll(places);
		array<ref CLW_AutoObjective> cands = {};
		int skippedType;
		int skippedBand;
		int skippedGround;
		foreach (CLW_Place place : places)
		{
			if (place.m_sType.IsEmpty() || TypeMatches(place.m_sType, cfg.m_aExcludeTypes))
			{
				skippedType++;
				continue;
			}

			float rx = place.m_vPos[0] - hqA[0];
			float rz = place.m_vPos[2] - hqA[2];
			float along = (rx * dirX + rz * dirZ) / hqDist;
			float lateral = -rx * dirZ + rz * dirX;
			if (along < cfg.m_fAlongMin || along > cfg.m_fAlongMax || Math.AbsFloat(lateral) > cfg.m_fMaxLateralFrac * hqDist
				|| vector.DistanceXZ(place.m_vPos, hqA) < cfg.m_fHqClearM || vector.DistanceXZ(place.m_vPos, hqB) < cfg.m_fHqClearM)
			{
				skippedBand++;
				continue;
			}

			vector pos = place.m_vPos;
			CLW_Placement.SurfaceAt(pos);
			if (CLW_Placement.IsWater(pos) || !CLW_Placement.SlopeOk(pos))
			{
				skippedGround++;
				continue;
			}

			CLW_AutoObjective cand = new CLW_AutoObjective();
			cand.m_sName = place.m_sName;
			cand.m_vPos = pos;
			cand.m_fAlong = along;
			cand.m_fLateral = lateral;
			cand.m_bTown = TypeMatches(place.m_sType, cfg.m_aTownTypes);
			cand.m_iTier = 1;
			if (TypeMatches(place.m_sType, cfg.m_aPrimaryTypes))
				cand.m_iTier = 0;
			cand.m_sSource = "place";
			cands.Insert(cand);
		}

		Print(string.Format("CLW: auto objectives: %1 slots, %2 place candidates in the band (skipped: %3 type, %4 outside band/HQ clearance, %5 water/slope)", slots, cands.Count(), skippedType, skippedBand, skippedGround), LogLevel.NORMAL);

		array<ref CLW_AutoObjective> chosen = {};
		chosen.Resize(slots);
		float slotWidth = (cfg.m_fAlongMax - cfg.m_fAlongMin) / slots;
		for (int pass = 0; pass < 2; pass++)
		{
			// pass 0: places inside their slot; pass 1: the slots still empty accept a place from the neighbouring slots too
			for (int k = 0; k < slots; k++)
			{
				if (chosen[k])
					continue;

				float lo = cfg.m_fAlongMin + slotWidth * k - pass * slotWidth * 0.5;
				float hi = cfg.m_fAlongMin + slotWidth * (k + 1) + pass * slotWidth * 0.5;
				CLW_AutoObjective best;
				float bestKey = 1000000000;
				foreach (CLW_AutoObjective option : cands)
				{
					if (chosen.Contains(option) || option.m_fAlong < lo || option.m_fAlong > hi)
						continue;

					if (!FitsSpacing(option.m_vPos, chosen, cfg.m_fObjectiveSpacingM))
						continue;

					float key = option.m_iTier * 100000 + Math.AbsFloat(option.m_fLateral);
					if (key < bestKey)
					{
						bestKey = key;
						best = option;
					}
				}

				if (best)
					chosen[k] = best;
			}
		}

		// slots without a place: a point on the HQ line, validated by CLW_FreeSpot
		int fromLine;
		for (int s = 0; s < slots; s++)
		{
			if (chosen[s])
				continue;

			float centre = cfg.m_fAlongMin + slotWidth * (s + 0.5);
			vector wish = Vector(hqA[0] + dirX * hqDist * centre, 0, hqA[2] + dirZ * hqDist * centre);
			CLW_Placement.SurfaceAt(wish);
			vector spot;
			float yaw;
			string source = "line";
			if (!CLW_FreeSpot.FindSmall("auto objective", wish, 0, cfg.m_fLineSearchM, cfg.m_fObjectiveHalfM, 3, spot, yaw))
			{
				if (CLW_Placement.IsWater(wish))
				{
					Print(string.Format("CLW: auto objective slot %1 dropped: the HQ line point %2 is water and no clear spot is near", s, wish), LogLevel.WARNING);
					continue;
				}

				spot = wish;
				source = "line (unchecked)";
			}

			if (!FitsSpacing(spot, chosen, cfg.m_fObjectiveSpacingM * 0.6) || vector.DistanceXZ(spot, hqA) < cfg.m_fHqClearM * 0.6 || vector.DistanceXZ(spot, hqB) < cfg.m_fHqClearM * 0.6)
			{
				Print(string.Format("CLW: auto objective slot %1 dropped: spot %2 too close to another objective or an HQ", s, spot), LogLevel.NORMAL);
				continue;
			}

			CLW_AutoObjective lineObj = new CLW_AutoObjective();
			lineObj.m_vPos = spot;
			lineObj.m_fAlong = ((spot[0] - hqA[0]) * dirX + (spot[2] - hqA[2]) * dirZ) / hqDist;
			lineObj.m_fLateral = -(spot[0] - hqA[0]) * dirZ + (spot[2] - hqA[2]) * dirX;
			lineObj.m_sSource = source;
			chosen[s] = lineObj;
			fromLine++;
		}

		// final list in HQ-line order
		array<CLW_AutoObjective> picks = {};
		foreach (CLW_AutoObjective pick : chosen)
		{
			if (pick)
				picks.Insert(pick);
		}

		// rank by centrality (0 = nearest the middle of the line): a smaller battle keeps the most central ones
		array<int> ranks = {};
		foreach (CLW_AutoObjective a : picks)
		{
			int rank;
			foreach (CLW_AutoObjective b : picks)
			{
				if (a != b && Math.AbsFloat(b.m_fAlong - 0.5) < Math.AbsFloat(a.m_fAlong - 0.5))
					rank++;
			}

			ranks.Insert(rank);
			rank = 0;
		}

		// start owners are not picked here: the battle layout is not known yet. The scenario resolves them at war start from the
		// final HQ positions (CLW_ControlPoints.ResolveStartOwners, mode "nearest")
		if (scenario.m_sStartOwnership != "none")
			scenario.m_sStartOwnership = "nearest";
		if (scenario.m_iStartOwnedPerSide < 0)
			scenario.m_iStartOwnedPerSide = 1;

		scenario.m_aObjectives = new array<ref CLW_ObjectiveSetup>();
		foreach (int n, CLW_AutoObjective obj : picks)
		{
			CLW_ObjectiveSetup setup = new CLW_ObjectiveSetup();
			setup.m_sId = letters.Substring(n, 1);
			setup.m_sName = obj.m_sName;
			if (setup.m_sName.IsEmpty())
				setup.m_sName = "Objective " + setup.m_sId;
			setup.m_vPos = obj.m_vPos;
			setup.m_fRadius = cfg.m_fRadiusSmallM;
			setup.m_iValue = 1;
			if (obj.m_bTown)
			{
				setup.m_fRadius = cfg.m_fRadiusTownM;
				setup.m_iValue = 2;
			}

			setup.m_iMinBattleSize = MinBattleSize(cfg.m_aObjectiveCounts, ranks[n]);
			scenario.m_aObjectives.Insert(setup);
			Print(string.Format("CLW: auto objective %1 '%2' at %3 grid %4 (%5, along %6, side %7 m)", setup.m_sId, setup.m_sName, setup.m_vPos, CLW_Places.Grid(setup.m_vPos), obj.m_sSource, Math.Round(obj.m_fAlong * 100) / 100, Math.Round(obj.m_fLateral)), LogLevel.NORMAL);
			Print(string.Format("CLW: auto objective %1 radius %2 value %3 minBattleSize %4 centrality rank %5", setup.m_sId, Math.Round(setup.m_fRadius), setup.m_iValue, setup.m_iMinBattleSize, ranks[n]), LogLevel.NORMAL);
		}

		Print(string.Format("CLW: auto objectives ready: %1 of %2 slots (%3 from places, %4 from the HQ line)", picks.Count(), slots, picks.Count() - fromLine, fromLine), LogLevel.NORMAL);
		if (picks.Count() < 3)
			Print("CLW: auto WARNING fewer than 3 objectives on this map: the war will end mostly by tickets / time", LogLevel.WARNING);
	}

	// Smallest battle size index whose objective count covers the given centrality rank.
	protected static int MinBattleSize(notnull array<int> counts, int rank)
	{
		foreach (int size, int count : counts)
		{
			if (count > rank)
				return size;
		}

		return Math.Max(0, counts.Count() - 1);
	}
}

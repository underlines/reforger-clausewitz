// FOB placement by the HQ commander (M4.6, dev/notes/fob-tool-design.md). Server only.
//   CLW_FobPlacer   validates a wish position (from the LLM's zoom pick) and SNAPS it to the nearest valid ground: dry, flat, inside the map,
//                   clear of both HQs, of every objective (capture radius + CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M), of live FOBs and of known enemy
//                   contacts, room for the FOB cluster (CLW_FreeSpot), a road within CLW_FobSites.ROAD_MAX_M (the builder truck has to reach it),
//                   own half of the map. Success = a dynamic pool site (CLW_FobSites.AddDynamic) that goes through the normal ESTABLISH_FOB {site}
//                   path; failure = a one-sentence reason the model can act on (retry once). The same rule lists feed the keep-out glyph `x` of the
//                   zoom digest (CLW_MapDigest), so the model sees what the game would refuse.
//   CLW_FobPlanner  per-side streak of LLM failures / declines. Two in a row hand the decision to the rule doctrine (CLW_FobDoctrine).
// Log prefixes: "CLW: fob place", "CLW: fob decision", "CLW: fob fallback".

class CLW_FobPlacer
{
	static const float SNAP_RADIUS_M = 250;		// search radius around the wish for valid ground
	static const float EDGE_MARGIN_M = 150;		// CLW_Placement.FindSiteEx keeps this far from the world edge

	// The positions a FOB must keep away from and how far: both HQs, every objective, every live FOB (any side), optionally the known
	// enemy contacts of 'side'. Same distances as CLW_FobSites / CLW_FobManager.CheckSite.
	static void BuildAvoid(string side, notnull array<vector> avoidPos, notnull array<float> avoidDist, bool withContacts)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (scenario && bases && scenario.m_aFactions && scenario.m_aFactions.Count() == 2)
		{
			float baseClear = Math.Max(CLW_FobSites.BASE_CLEAR_FLOOR_M, bases.ScaleDistance(CLW_FobSites.BASE_CLEAR_M));
			float spacing = Math.Max(CLW_FobManager.SPACING_FLOOR_M, bases.ScaleDistance(CLW_FobManager.SPACING_M));
			for (int b = 0; b < 2; b++)
			{
				string key = scenario.GetFactionKey(b);
				vector hq = bases.GetBasePos(key);
				if (hq != vector.Zero)
				{
					avoidPos.Insert(hq);
					avoidDist.Insert(baseClear);
				}

				array<CLW_Fob> fobs = {};
				CLW_FobManager.GetFobs(key, fobs);
				foreach (CLW_Fob fob : fobs)
				{
					avoidPos.Insert(fob.m_vPos);
					avoidDist.Insert(spacing);
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

				avoidPos.Insert(point.m_vPos);
				avoidDist.Insert(point.m_fRadius + CLW_WarConfig.FOB_MIN_OBJECTIVE_DIST_M);
			}
		}

		if (!withContacts)
			return;

		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (commander && commander.GetRules() && commander.GetRules().GetBook())
		{
			array<CLW_Contact> contacts = {};
			commander.GetRules().GetBook().GetContacts(contacts);
			foreach (CLW_Contact contact : contacts)
			{
				avoidPos.Insert(contact.m_vPos);
				avoidDist.Insert(CLW_FobManager.ENEMY_CONTACT_M);
			}
		}
	}

	// The position is on the enemy half of the map (closer to the enemy HQ than to the own one).
	static bool IsEnemyHalf(string side, vector pos)
	{
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!bases || !war)
			return false;

		vector own = bases.GetBasePos(side);
		vector enemy = bases.GetBasePos(war.GetEnemyKey(side));
		return own != vector.Zero && enemy != vector.Zero && vector.DistanceXZ(pos, enemy) < vector.DistanceXZ(pos, own);
	}

	// True when 'pos' violates a placement distance rule (or is on the enemy half). why = the first rule hit. avoid lists from BuildAvoid.
	static bool KeepOutIn(string side, vector pos, notnull array<vector> avoidPos, notnull array<float> avoidDist, out string why)
	{
		for (int i = 0; i < avoidPos.Count(); i++)
		{
			float dist = vector.DistanceXZ(pos, avoidPos[i]);
			if (dist < avoidDist[i])
			{
				why = string.Format("%1 m from %2 (needs %3 m)", Math.Round(dist), Describe(i, avoidPos[i]), Math.Round(avoidDist[i]));
				return true;
			}
		}

		if (IsEnemyHalf(side, pos))
		{
			why = "on the enemy half of the map";
			return true;
		}

		return false;
	}

	// What an avoid entry is, for the reason text: the place label near it (an HQ, objective, FOB or contact position).
	protected static string Describe(int index, vector pos)
	{
		return "a protected position (HQ, objective, FOB or enemy contact) at grid " + CLW_Places.Grid(pos);
	}

	// Why the WISH itself is no good (shown to the model when no valid ground exists nearby).
	static string Explain(string side, vector wish)
	{
		vector probe = wish;
		CLW_Placement.SurfaceAt(probe);
		string text = "";
		if (CLW_Placement.IsWater(probe))
			text = "the spot is water";
		else if (CLW_Placement.NearWater(probe, 25))
			text = "the spot is on the shore";
		else if (!CLW_Placement.SlopeOk(probe))
			text = "the spot is too steep";

		array<vector> avoidPos = {};
		array<float> avoidDist = {};
		BuildAvoid(side, avoidPos, avoidDist, true);
		string rule;
		if (KeepOutIn(side, probe, avoidPos, avoidDist, rule))
		{
			if (!text.IsEmpty())
				text += "; ";
			text += rule;
		}

		if (text.IsEmpty())
			text = "terrain, protected positions and the enemy half leave no valid ground";

		return text;
	}

	// Validates and snaps 'wish' (x, z) for 'side'. On success 'site' is a new dynamic pool site (id FSL<n>) ready for ESTABLISH_FOB {site}.
	// On failure 'why' is the reason for the model (and the log).
	static bool Resolve(string side, vector wish, out CLW_FobSite site, out string why)
	{
		vector target = Vector(wish[0], 0, wish[2]);
		CLW_Placement.SurfaceAt(target);
		BaseWorld world = GetGame().GetWorld();
		vector mins;
		vector maxs;
		world.GetBoundBox(mins, maxs);
		if (target[0] < mins[0] || target[0] > maxs[0] || target[2] < mins[2] || target[2] > maxs[2])
		{
			why = "the position is outside the map";
			Print(string.Format("CLW: fob place %1: wish %2 rejected: %3", side, target, why), LogLevel.NORMAL);
			return false;
		}

		array<vector> avoidPos = {};
		array<float> avoidDist = {};
		BuildAvoid(side, avoidPos, avoidDist, true);

		vector found;
		float roadDist;
		float score;
		if (!CLW_Placement.FindSiteEx(target, SNAP_RADIUS_M, avoidPos, avoidDist, found, roadDist, score))
		{
			why = string.Format("no valid ground within %1 m of that spot (%2)", Math.Round(SNAP_RADIUS_M), Explain(side, target));
			Print(string.Format("CLW: fob place %1: wish %2 grid %3 rejected: %4", side, target, CLW_Places.Grid(target), why), LogLevel.NORMAL);
			return false;
		}

		// room for the FOB cluster (buildings, fences, props, trees): the site may shift to clear ground nearby
		vector refined = found;
		if (!CLW_FreeSpot.RefineSite("fob place " + side, refined, avoidPos, avoidDist))
		{
			why = string.Format("no room for the FOB near that spot: the nearest valid ground at grid %1 is cluttered by buildings, fences or trees within %2 m", CLW_Places.Grid(found), Math.Round(CLW_SpotConfig.FOB_REFINE_RADIUS_M));
			Print(string.Format("CLW: fob place %1: wish %2 -> ground %3 rejected: %4", side, target, found, why), LogLevel.NORMAL);
			return false;
		}

		if (vector.DistanceXZ(refined, found) > 1)
			roadDist = CLW_Placement.NearestRoadDistance(refined);

		if (roadDist < 0 || roadDist > CLW_FobSites.ROAD_MAX_M)
		{
			string roadText = "has no road at all";
			if (roadDist >= 0)
				roadText = "is " + Math.Round(roadDist) + " m from the nearest road";
			why = string.Format("the best valid ground near that spot (grid %1) %2, the builder truck needs a road within %3 m: choose a spot beside a road ('=' or '-' in the zoom)", CLW_Places.Grid(refined), roadText, CLW_FobSites.ROAD_MAX_M);
			Print(string.Format("CLW: fob place %1: wish %2 -> ground %3 rejected: %4", side, target, refined, why), LogLevel.NORMAL);
			return false;
		}

		CLW_FobSite candidate = CLW_FobSites.AddDynamic(side, refined, roadDist);
		if (!candidate)
		{
			why = "the site pool is full";
			return false;
		}

		string checkWhy;
		if (!CLW_FobManager.CheckSite(side, candidate, checkWhy))
		{
			CLW_FobSites.RemoveSite(candidate.m_sId);
			why = checkWhy + " (near " + CLW_Places.Grid(refined) + ")";
			Print(string.Format("CLW: fob place %1: wish %2 -> ground %3 rejected: %4", side, target, refined, why), LogLevel.NORMAL);
			return false;
		}

		site = candidate;
		Print(string.Format("CLW: fob place %1: wish %2 grid %3 -> site %4 '%5' at %6 grid %7 (moved %8 m, road %9 m)", side, target, CLW_Places.Grid(target), candidate.m_sId, candidate.m_sName, refined, CLW_Places.Grid(refined), Math.Round(vector.DistanceXZ(target, refined)), Math.Round(roadDist)), LogLevel.NORMAL);
		return true;
	}
}

// Failure / decline streak per side. The LLM commander declining (explicitly, or by a rejected spot after the one retry, or a failing zoom
// call) twice in a row hands the next FOB to the rule doctrine; a success resets the streak.
class CLW_FobPlanner
{
	static const int FALLBACK_AT = 2;

	protected static ref map<string, int> s_mStreak = new map<string, int>();

	static void Reset()
	{
		s_mStreak.Clear();
	}

	static int Streak(string side)
	{
		int n;
		s_mStreak.Find(side, n);
		return n;
	}

	static void NoteSuccess(string side)
	{
		s_mStreak.Set(side, 0);
	}

	// kind: "declined" | "failed". True when the rule doctrine should place a FOB now (the streak restarts).
	static bool NoteFailure(string side, string kind, string reason)
	{
		int n = Streak(side) + 1;
		s_mStreak.Set(side, n);
		Print(string.Format("CLW: fob decision %1: LLM %2 (%3), streak %4 of %5", side, kind, reason, n, FALLBACK_AT), LogLevel.NORMAL);
		if (n < FALLBACK_AT)
			return false;

		s_mStreak.Set(side, 0);
		Print(string.Format("CLW: fob fallback %1: the LLM commander gave no usable FOB twice in a row, the rule doctrine places one", side), LogLevel.NORMAL);
		return true;
	}
}

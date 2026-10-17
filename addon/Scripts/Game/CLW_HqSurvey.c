// HQ position survey (M4.6 dev helper): runs the random HQ search for N fixed seeds and logs how many distinct HQ spots came out and
// how the sides split. Debug only: it runs at game start when the dedicated server is launched with `-clwHqSurvey` (optionally
// `-clwHqSurvey 30` = number of seeds, default 20; call site CLW_AutoScenario.Fill) and costs about N times one HQ search on the
// server main thread, so never use it on a live server. Start once per map and read the "CLW: hqsurvey" lines of the log.
// Seeds 1..N are explicit, so a survey is reproducible (up to the few metres the CLW_FreeSpot ring angle varies, see CLW_RandomLayout).
// Server only. Log prefix: "CLW: hqsurvey".

class CLW_HqSurvey
{
	static const string CLI_SURVEY = "clwHqSurvey";
	static const int DEFAULT_SEEDS = 20;
	static const float SPOT_MERGE_M = 400;	// HQs closer than this count as the same spot

	// Number of seeds when the launch argument is present, else 0.
	static int RequestedSeeds()
	{
		string value;
		if (!System.GetCLIParam(CLI_SURVEY, value))
			return 0;

		int count = value.Trim().ToInt();
		if (count <= 0)
			count = DEFAULT_SEEDS;

		return Math.Min(count, 200);
	}

	static void Run(notnull CLW_ScenarioConfig scenario, string worldName, float wanted, float minHq, int seeds)
	{
		int startTick = System.GetTickCount();
		array<vector> spots = {};
		array<int> spotCounts = {};
		int found;
		int aWest;
		int aEast;
		int flips;
		int totalMs;
		for (int seed = 1; seed <= seeds; seed++)
		{
			CLW_RandomLayout layout = CLW_RandomLayout.Create(scenario, seed);
			bool ok = layout.FindHqPair(wanted, minHq);
			if (!ok)
			{
				layout.Relax();
				ok = layout.FindHqPair(wanted, minHq);
			}

			if (!ok)
			{
				Print(string.Format("CLW: hqsurvey seed=%1 world='%2': no HQ pair", seed, worldName), LogLevel.WARNING);
				continue;
			}

			found++;
			totalMs += layout.m_iSearchMs;
			if (layout.m_bSidesFlipped)
				flips++;

			if (layout.m_vHqA[0] < layout.m_vHqB[0])
				aWest++;
			else
				aEast++;

			CountSpot(spots, spotCounts, layout.m_vHqA);
			CountSpot(spots, spotCounts, layout.m_vHqB);
			Print(string.Format("CLW: hqsurvey seed=%1: A %2 (grid %3), B %4 (grid %5), %6 valid pairs, picked #%7, flipped=%8, %9 ms", seed, layout.m_vHqA, CLW_Places.Grid(layout.m_vHqA), layout.m_vHqB, CLW_Places.Grid(layout.m_vHqB), layout.m_iPairsFound, layout.m_iPairPicked, layout.m_bSidesFlipped, layout.m_iSearchMs), LogLevel.NORMAL);
		}

		string spotText = "";
		for (int i = 0; i < spots.Count(); i++)
		{
			if (!spotText.IsEmpty())
				spotText += ", ";

			spotText += CLW_Places.Grid(spots[i]) + "x" + spotCounts[i];
		}

		Print(string.Format("CLW: hqsurvey SUMMARY world='%1': %2 of %3 seeds found a pair; %4 distinct HQ spots (merge radius %5 m) over %6 HQs; side A west of B in %7, east in %8; coin toss flipped %9 times", worldName, found, seeds, spots.Count(), Math.Round(SPOT_MERGE_M), found * 2, aWest, aEast, flips) + string.Format("; avg search %1 ms, survey took %2 ms", totalMs / Math.Max(1, found), System.GetTickCount() - startTick), LogLevel.NORMAL);
		Print(string.Format("CLW: hqsurvey spots (grid x count): %1", spotText), LogLevel.NORMAL);
	}

	protected static void CountSpot(notnull array<vector> spots, notnull array<int> counts, vector pos)
	{
		for (int i = 0; i < spots.Count(); i++)
		{
			if (vector.DistanceXZ(spots[i], pos) < SPOT_MERGE_M)
			{
				counts[i] = counts[i] + 1;
				return;
			}
		}

		spots.Insert(pos);
		counts.Insert(1);
	}
}

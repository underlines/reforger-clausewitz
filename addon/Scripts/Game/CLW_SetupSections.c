// Sections ("tabs") of the setup dialog (M4.6): which parameter id belongs to which section and in which order.
// Parameters registered by other files (CLW_Environment.AddParams, CLW_FriendlyMapSettings, CLW_HqPersona, a future weather rate, ...) are
// placed here BY KEY, so those files need no change. A key that is not listed lands in Advanced, except for the prefix rules in SectionOf.
class CLW_SetupSections
{
	static const int BASICS = 0;
	static const int FORCES = 1;
	static const int MAP = 2;
	static const int ENVIRONMENT = 3;
	static const int ADVANCED = 4;
	static const int COUNT = 5;

	// Known ids per section, in display order (the position inside the string is the sort key). Space separated, with leading/trailing space.
	protected static const string LIST_BASICS = " factionA factionB battleSize startPositions aiDifficultyA aiDifficultyB timeLimitMin ";
	protected static const string LIST_FORCES = " startTickets startSupplies incomeMultiplier ticketBleed holdAllWinMin hqTraitA hqTraitB garrisonSquads convoyMax ";
	protected static const string LIST_MAP = " startObjectives mapMe mapSquadmates mapSquads mapVehicles fobMax fobBuild fobCost fobBuildSec fobEarliestMin ";
	protected static const string LIST_ENVIRONMENT = " startHour daySpeed weatherMode weatherRate startYear startMonth startDay ";
	protected static const string LIST_ADVANCED = " idleDespawnMin hqReserve convoyMinLoad cap_logistics cap_kitCost cap_serverKitCost ";

	static string Name(int section)
	{
		if (section == BASICS)
			return "#CLW-Setup_SectionBasics";
		if (section == FORCES)
			return "#CLW-Setup_SectionForces";
		if (section == MAP)
			return "#CLW-Setup_SectionMap";
		if (section == ENVIRONMENT)
			return "#CLW-Setup_SectionEnvironment";

		return "#CLW-Setup_SectionAdvanced";
	}

	static int SectionOf(string id)
	{
		string needle = " " + id + " ";
		if (LIST_BASICS.Contains(needle))
			return BASICS;
		if (LIST_FORCES.Contains(needle))
			return FORCES;
		if (LIST_MAP.Contains(needle))
			return MAP;
		if (LIST_ENVIRONMENT.Contains(needle))
			return ENVIRONMENT;
		if (LIST_ADVANCED.Contains(needle))
			return ADVANCED;

		// unknown key of another file: by prefix, else Advanced
		if (id.StartsWith("weather") || id.StartsWith("wind") || id.StartsWith("rain") || id.StartsWith("fog") || id.StartsWith("time") || id.StartsWith("day"))
			return ENVIRONMENT;
		if (id.StartsWith("map") || id.StartsWith("fob"))
			return MAP;

		return ADVANCED;
	}

	// Sort key inside a section: position in the known list, unknown ids after all known ones (registration order is kept by a stable insert).
	static int OrderOf(string id)
	{
		string needle = " " + id + " ";
		int at = LIST_BASICS.IndexOf(needle);
		if (at < 0)
			at = LIST_FORCES.IndexOf(needle);
		if (at < 0)
			at = LIST_MAP.IndexOf(needle);
		if (at < 0)
			at = LIST_ENVIRONMENT.IndexOf(needle);
		if (at < 0)
			at = LIST_ADVANCED.IndexOf(needle);
		if (at < 0)
			return 100000;

		return at;
	}
}

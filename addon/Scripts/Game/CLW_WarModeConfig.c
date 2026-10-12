// Scenario-side defaults of the war modes (spec 2a). The scenario lists one config object per available mode
// (polymorphic array); CLW_WarModeRegistry maps config class -> mode class. New mode = new config class + mode class + registry line.

[BaseContainerProps()]
class CLW_WarModeConfig
{
	// Mode id this config belongs to (override in subclasses).
	string GetModeId()
	{
		return "";
	}
}

// Defaults of CLW_WarMode_ShortConflict ("short").
[BaseContainerProps()]
class CLW_WarModeConfig_Short : CLW_WarModeConfig
{
	[Attribute("60", UIWidgets.EditBox, "Time limit in minutes (15-180)")]
	int m_iTimeLimitMin;

	[Attribute("400", UIWidgets.EditBox, "Start tickets per faction (100-2000)")]
	int m_iStartTickets;

	[Attribute("400", UIWidgets.EditBox, "Start supplies per faction (0-2000)")]
	int m_iStartSupplies;

	[Attribute("1", UIWidgets.EditBox, "Income multiplier (0.25-3)")]
	float m_fIncomeMultiplier;

	[Attribute("medium", UIWidgets.EditBox, "Default battle size: one of m_aBattleSizes")]
	string m_sBattleSize;

	[Attribute()]
	ref array<string> m_aBattleSizes;

	[Attribute()]
	ref array<int> m_aAiCaps;			// per battle size, same order as m_aBattleSizes

	[Attribute()]
	ref array<int> m_aVehicleCaps;		// per battle size

	[Attribute()]
	ref array<int> m_aActiveAgentCaps;	// per battle size: AI agents per side kept out of the max LOD (activity management)

	[Attribute()]
	ref array<float> m_aIncomeScales;	// per battle size: bigger battles need more income to fill the bigger caps

	[Attribute()]
	ref array<float> m_aDistanceScales;	// per battle size: scale of the authored HQ separation (and of future FOB distances). 1 = as authored in the scenario

	[Attribute("600", UIWidgets.EditBox, "Never place the two HQs closer than this (m), whatever the scale")]
	float m_fMinHqDistanceM;

	[Attribute("250", UIWidgets.EditBox, "Objectives are kept at least this far (m) from both HQs (spawn safety)")]
	float m_fHqObjectiveClearM;

	[Attribute("120", UIWidgets.EditBox, "Scaled objectives keep this much free ground (m) between their capture circles (centre distance >= both radii + this)")]
	float m_fObjectiveGapM;

	[Attribute("fixed", UIWidgets.EditBox, "Default start positions: fixed (scenario HQs) or random (CLW_RandomLayout)")]
	string m_sStartPositions;

	[Attribute("0", UIWidgets.EditBox, "Default random seed (0 = time based, 0-9999999)")]
	int m_iRandomSeed;

	[Attribute("1", UIWidgets.CheckBox, "Ticket bleed for the side holding fewer points")]
	bool m_bTicketBleed;

	[Attribute("5", UIWidgets.EditBox, "Holding all points this many minutes wins (0-15, 0 = off)")]
	int m_iHoldAllWinMin;

	[Attribute("2", UIWidgets.EditBox, "Max FOBs per side at once (0-4, 0 = no FOBs)")]
	int m_iFobMax;

	[Attribute("timed", UIWidgets.EditBox, "FOB build mode: instant (done on arrival) or timed (progress while the builder is on site)")]
	string m_sFobBuild;

	[Attribute("300", UIWidgets.EditBox, "FOB cost in supplies (0-1000)")]
	int m_iFobCost;

	[Attribute("90", UIWidgets.EditBox, "Timed FOB build duration in seconds (10-600)")]
	int m_iFobBuildSec;

	override string GetModeId()
	{
		return "short";
	}
}

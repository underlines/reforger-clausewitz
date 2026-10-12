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

	[Attribute("1", UIWidgets.CheckBox, "Ticket bleed for the side holding fewer points")]
	bool m_bTicketBleed;

	[Attribute("5", UIWidgets.EditBox, "Holding all points this many minutes wins (0-15, 0 = off)")]
	int m_iHoldAllWinMin;

	override string GetModeId()
	{
		return "short";
	}
}

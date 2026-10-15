// Entries of the HQ radial menu (WP2, design: dev/notes/hq-comms-env-traits-design.md section 1b). Both use the vanilla commanding entry layout
// (icon + short text under it); the full name shows in the middle of the circle when the entry is pointed at.
// Log prefix: "CLW: hqradial".

// A leaf: performing it sends one HQ menu action (CLW_HqRadial.Perform).
class CLW_HqRadialEntry : SCR_SelectionMenuEntryCommand
{
	string m_sAction;
	string m_sArg;

	override protected void OnPerform()
	{
		CLW_HqRadial.Perform(m_sAction, m_sArg);
	}
}

// A level. The children are built when the entry is performed (CLW_HqRadial.Fill), so the long report chain
// (what -> count -> distance -> direction) never exists as a whole and the vehicle list is always current.
class CLW_HqRadialCategory : SCR_SelectionMenuCategoryEntry
{
	string m_sLevel;		// what Fill builds: report | count | dist | dir | requests | vehicle
	string m_sKind;			// report chain: inf | veh | armor | air | unknown
	int m_iCount;
	int m_iDist;
	protected string m_sCommandText;

	void SetCommandText(string text)
	{
		m_sCommandText = text;
	}

	override void SetEntryComponent(SCR_SelectionMenuEntryComponent entryComponent)
	{
		SCR_SelectionMenuEntryCommandComponent commandEntry = SCR_SelectionMenuEntryCommandComponent.Cast(entryComponent);
		if (commandEntry)
			commandEntry.SetCommandText(m_sCommandText);

		super.SetEntryComponent(entryComponent);
	}

	override protected void OnPerform()
	{
		m_aEntries = new array<ref SCR_SelectionMenuEntry>();
		CLW_HqRadial.Fill(this);
	}

	void CLW_HqRadialCategory()
	{
		SetCustomLayout(CLW_HqRadial.ENTRY_LAYOUT);
	}
}

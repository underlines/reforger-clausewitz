// Log helper for map markers: what a marker is and which factions its faction flags resolve to (bit n = faction index n of the
// FactionManager, SCR_MapMarkerBase.IsFaction). Used by the structure / objective / contact markers so a log shows per marker
// "type=PLACED_MILITARY flags=32 -> RHS_USAF" and a flag that points at the wrong faction is visible at once.
class CLW_MarkerLog
{
	// "all (flags 0)", "RHS_USAF", "RHS_USAF|RHS_AFRF", "?bit9" for an index the faction manager does not know.
	static string FlagKeys(int flags)
	{
		if (flags == 0)
			return "all (flags 0)";

		FactionManager factionManager = GetGame().GetFactionManager();
		string text;
		for (int bit = 0; bit < 31; bit++)
		{
			if (!(flags & (1 << bit)))
				continue;

			if (!text.IsEmpty())
				text += "|";

			Faction faction = null;
			if (factionManager)
				faction = factionManager.GetFactionByIndex(bit);
			if (faction)
				text += faction.GetFactionKey();
			else
				text += "?bit" + bit.ToString();
		}

		return text;
	}

	// "type=PLACED_MILITARY cfg=103 icon=8 flags=32 -> RHS_USAF".
	static string Describe(SCR_MapMarkerBase marker)
	{
		if (!marker)
			return "type=none";

		string typeName = typename.EnumToString(SCR_EMapMarkerType, marker.GetType());
		return "type=" + typeName + " cfg=" + marker.GetMarkerConfigID().ToString() + " icon=" + marker.GetFlags().ToString() + " flags=" + marker.GetMarkerFactionFlags().ToString() + " -> " + FlagKeys(marker.GetMarkerFactionFlags());
	}
}

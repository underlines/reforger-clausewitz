// The single "mission" map marker of one faction's RuleCommander: shown only to that faction, replaced on each new mission.
// Server only. Static markers cannot be edited after replication, so a changed mission is removed and re-inserted.
class CLW_MissionMarker
{
	protected ref SCR_MapMarkerBase m_Marker;

	// Replace the current mission marker by a new one at pos. text is the label shown on the map, e.g. "MISSION: Attack".
	void Show(string factionKey, string text, vector pos)
	{
		if (!Replication.IsServer())
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!manager || !factionManager)
			return;

		Faction faction = factionManager.GetFactionByKey(factionKey);
		if (!faction)
			return;

		Clear();

		// own-side symbol (friendly colour), land dimension, infantry icon; the label text tells it apart from contacts
		SCR_MapMarkerBase marker = manager.PrepareMilitaryMarker(GetIdentity(factionKey), EMilitarySymbolDimension.LAND, EMilitarySymbolIcon.INFANTRY);
		if (!marker)
		{
			Print("CLW: MissionMarker: PrepareMilitaryMarker failed", LogLevel.WARNING);
			return;
		}

		marker.SetWorldPos(pos[0], pos[2]);
		marker.SetMarkerFactionFlags(1 << factionManager.GetFactionIndex(faction));
		marker.SetCustomText(text);
		manager.InsertStaticMarker(marker, false, true);
		m_Marker = marker;
	}

	void Clear()
	{
		if (!m_Marker)
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (manager)
			manager.RemoveStaticMarker(m_Marker);

		m_Marker = null;
	}

	bool IsShown()
	{
		return m_Marker != null;
	}

	protected EMilitarySymbolIdentity GetIdentity(string factionKey)
	{
		// same mapping as CLW_ContactMarkers / the vanilla SCR_AIEnemyMarkingSystem
		if (factionKey == "US")
			return EMilitarySymbolIdentity.BLUFOR;

		if (factionKey == "USSR")
			return EMilitarySymbolIdentity.OPFOR;

		return EMilitarySymbolIdentity.INDFOR;
	}
}

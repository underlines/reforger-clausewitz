// Shows a faction's known enemy contacts as map markers, visible only to that faction.
// Server only. Static markers cannot be edited after replication, so a changed marker is removed and re-inserted.
class CLW_ContactMarkers
{
	static const float MOVE_THRESHOLD = 20;	// m: a contact must move farther than this before its marker is re-inserted

	protected ref map<int, ref SCR_MapMarkerBase> m_mMarkers = new map<int, ref SCR_MapMarkerBase>();

	// Create, move or update the markers of one faction's contacts. factionKey = the observer faction that sees them.
	void Sync(string factionKey, notnull array<CLW_Contact> contacts)
	{
		if (!Replication.IsServer())
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!manager || !factionManager)
			return;

		Faction observer = factionManager.GetFactionByKey(factionKey);
		if (!observer)
			return;

		int factionFlags = 1 << factionManager.GetFactionIndex(observer);

		foreach (CLW_Contact contact : contacts)
		{
			string text = contact.m_iCount.ToString();
			SCR_MapMarkerBase marker = m_mMarkers.Get(contact.m_iId);
			if (marker)
			{
				int pos[2];
				marker.GetWorldPos(pos);
				vector markerPos = Vector(pos[0], 0, pos[1]);
				if (vector.DistanceXZ(markerPos, contact.m_vPos) <= MOVE_THRESHOLD && marker.GetCustomText() == text)
					continue;

				manager.RemoveStaticMarker(marker);
				m_mMarkers.Remove(contact.m_iId);
			}

			// the symbol shows the enemy side, hostile infantry on the land dimension
			marker = manager.PrepareMilitaryMarker(GetIdentity(contact.m_sEnemyFaction), EMilitarySymbolDimension.LAND, EMilitarySymbolIcon.INFANTRY);
			if (!marker)
				continue;

			marker.SetWorldPos(contact.m_vPos[0], contact.m_vPos[2]);
			marker.SetMarkerFactionFlags(factionFlags);
			marker.SetCustomText(text);
			manager.InsertStaticMarker(marker, false, true);
			m_mMarkers.Insert(contact.m_iId, marker);
		}
	}

	// Remove the markers of expired contact ids.
	void Remove(notnull array<int> contactIds)
	{
		if (!Replication.IsServer())
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return;

		foreach (int id : contactIds)
		{
			SCR_MapMarkerBase marker = m_mMarkers.Get(id);
			if (!marker)
				continue;

			manager.RemoveStaticMarker(marker);
			m_mMarkers.Remove(id);
		}
	}

	protected EMilitarySymbolIdentity GetIdentity(string factionKey)
	{
		// same mapping as the vanilla SCR_AIEnemyMarkingSystem
		if (factionKey == "US")
			return EMilitarySymbolIdentity.BLUFOR;

		if (factionKey == "USSR")
			return EMilitarySymbolIdentity.OPFOR;

		return EMilitarySymbolIdentity.INDFOR;
	}
}

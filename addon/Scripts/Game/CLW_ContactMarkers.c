// Shows a faction's known enemy contacts as map markers, visible only to that faction.
// Server only. Static markers cannot be edited after replication, so a changed marker is removed and re-inserted.
class CLW_ContactMarkers
{
	static const float MOVE_THRESHOLD = 20;	// m: a contact must move farther than this before its marker is re-inserted

	protected ref map<int, ref SCR_MapMarkerBase> m_mMarkers = new map<int, ref SCR_MapMarkerBase>();
	protected ref map<int, string> m_mKinds = new map<int, string>();	// contact kind each marker was built for

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
			string text = MarkerText(contact);
			SCR_MapMarkerBase marker = m_mMarkers.Get(contact.m_iId);
			if (marker)
			{
				int pos[2];
				marker.GetWorldPos(pos);
				vector markerPos = Vector(pos[0], 0, pos[1]);
				string builtKind;
					m_mKinds.Find(contact.m_iId, builtKind);
					if (vector.DistanceXZ(markerPos, contact.m_vPos) <= MOVE_THRESHOLD && marker.GetCustomText() == text && builtKind == contact.m_sKind)
					continue;

				manager.RemoveStaticMarker(marker);
				m_mMarkers.Remove(contact.m_iId);
					m_mKinds.Remove(contact.m_iId);
			}

			// the symbol shows the enemy side, hostile infantry on the land dimension
			marker = manager.PrepareMilitaryMarker(GetIdentity(contact.m_sEnemyFaction), EMilitarySymbolDimension.LAND, IconFor(contact.m_sKind));
			if (!marker)
				continue;

			marker.SetWorldPos(contact.m_vPos[0], contact.m_vPos[2]);
			marker.SetMarkerFactionFlags(factionFlags);
			marker.SetCustomText(text);
			manager.InsertStaticMarker(marker, false, true);
			Print("CLW: markers contact " + contact.m_iId.ToString() + " '" + text + "' " + CLW_MarkerLog.Describe(marker), LogLevel.NORMAL);
			m_mMarkers.Insert(contact.m_iId, marker);
				m_mKinds.Set(contact.m_iId, contact.m_sKind);
		}
	}

	// Short label with size words instead of raw counts. Vehicles first, dismounts after.
	protected string MarkerText(CLW_Contact contact)
	{
		string text = "";
		if (contact.m_iVehicles > 0)
			text = CLW_Places.VehicleShort(contact.m_iVehicles);
		if (contact.m_iInfantry > 0)
		{
			if (!text.IsEmpty())
				text += " + ";
			text += CLW_Places.InfantryShort(contact.m_iInfantry);
		}
		if (text.IsEmpty())
			text = CLW_Places.InfantryShort(contact.m_iCount);

		return text;
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
			m_mKinds.Remove(id);
		}
	}

	// Remove every marker of this set (the faction objects are rebuilt at war start).
	void RemoveAll()
	{
		array<int> ids = {};
		foreach (int id, SCR_MapMarkerBase marker : m_mMarkers)
		{
			ids.Insert(id);
		}
		Remove(ids);
	}

	// Vehicle contacts use an armour symbol, everything else infantry. Looked up by name at runtime so a missing member
	// cannot break compilation. [verify] member names ARMOR / MECHANIZED / MOTORIZED of EMilitarySymbolIcon
	protected EMilitarySymbolIcon IconFor(string kind)
	{
		if (kind == CLW_Contact.KIND_VEHICLE)
		{
			array<string> names = {"ARMOR", "MECHANIZED", "MOTORIZED"};
			foreach (string iconName : names)
			{
				int value = typename.StringToEnum(EMilitarySymbolIcon, iconName);
				if (value > 0)
					return value;
			}
		}

		return EMilitarySymbolIcon.INFANTRY;
	}

	protected EMilitarySymbolIdentity GetIdentity(string factionKey)
	{
		// identity comes from the scenario data (faction setup), unknown factions show as INDFOR
		if (!CLW_ScenarioConfig.IsClwFaction(factionKey))
			return EMilitarySymbolIdentity.INDFOR;

		return CLW_ScenarioConfig.GetIdentity(factionKey);
	}
}

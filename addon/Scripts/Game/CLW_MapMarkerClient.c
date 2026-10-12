// Client side fix: server markers for a faction must survive the local player changing faction (M4 takeover playtest 2026-10-12:
// no own-side structure / objective marker on the map of a Game Master world).
//
// Why they went missing. Vanilla SCR_MapMarkerManagerComponent.OnAddSynchedMarker (client) drops a replicated marker whose faction flags do
// not include the LOCAL player's faction, for good: it is not kept anywhere. A war starts in SETUP with the players spawned on the scenario's
// default side (US / USSR slot defaults); StartWar then re-keys the sides (e.g. RHS_USAF / RHS_AFRF) and the server inserts all markers of
// the new sides, while the connected client is still on the old faction (the move to the new side comes seconds later, CLW_Sides.MovePlayers).
// Every marker arrives with flags of a faction the client is not in -> dropped. When the client then switches faction,
// OnPlayerFactionChanged only REMOVES markers of other factions ("TODO if it matters" in the vanilla source), it never brings dropped ones back.
// With the default sides (no re-key) the faction never changes, which is why the wrapper worlds looked fine.
//
// Fix: keep every marker the faction filter drops (by marker ID) and give them back when the local faction changes to one they are for.
// Client only (a dedicated server never filters). A marker removed by the server (OnRemoveSynchedMarker) leaves the kept list too.
modded class SCR_MapMarkerManagerComponent
{
	protected ref map<int, ref SCR_MapMarkerBase> m_mCLWKept;

	protected map<int, ref SCR_MapMarkerBase> CLW_Kept()
	{
		if (!m_mCLWKept)
			m_mCLWKept = new map<int, ref SCR_MapMarkerBase>();
		return m_mCLWKept;
	}

	protected bool CLW_IsListed(SCR_MapMarkerBase marker)
	{
		return m_aStaticMarkers.Contains(marker) || m_aDisabledMarkers.Contains(marker);
	}

	// A replicated marker arrived: when vanilla dropped it because of the local faction, keep it.
	override void OnAddSynchedMarker(SCR_MapMarkerBase marker)
	{
		super.OnAddSynchedMarker(marker);

		if (System.IsConsoleApp() || !marker)
			return;

		if (marker.GetMarkerID() == -1 || marker.GetMarkerFactionFlags() == 0 || CLW_IsListed(marker))
			return;

		CLW_Kept().Set(marker.GetMarkerID(), marker);
		Print("CLW: markers client kept back marker " + marker.GetMarkerID().ToString() + " '" + marker.GetCustomText() + "' (not for the local faction): " + CLW_MarkerLog.Describe(marker), LogLevel.NORMAL);
	}

	override void OnRemoveSynchedMarker(int markerID)
	{
		super.OnRemoveSynchedMarker(markerID);

		if (m_mCLWKept)
			m_mCLWKept.Remove(markerID);
	}

	// The local player got another faction: vanilla now deletes the markers of other factions. Keep them first, then restore the ones
	// that are for the new faction (all that were dropped earlier for the previous one included).
	override protected void OnPlayerFactionChanged(notnull FactionAffiliationComponent owner, Faction previousFaction, Faction newFaction)
	{
		if (!System.IsConsoleApp())
			CLW_KeepFactionMarkers();

		super.OnPlayerFactionChanged(owner, previousFaction, newFaction);

		if (System.IsConsoleApp() || !newFaction)
			return;

		FactionManager factionManager = GetGame().GetFactionManager();
		if (!factionManager)
			return;

		int newIndex = factionManager.GetFactionIndex(newFaction);
		map<int, ref SCR_MapMarkerBase> kept = CLW_Kept();
		array<int> restoreIds = {};
		foreach (int keptId, SCR_MapMarkerBase keptMarker : kept)
		{
			if (keptMarker && keptMarker.IsFaction(newIndex))
				restoreIds.Insert(keptId);
		}

		SCR_MapEntity mapEntity = SCR_MapEntity.GetMapInstance();
		bool mapOpen = mapEntity && mapEntity.IsOpen() && mapEntity.GetMapUIComponent(SCR_MapMarkersUI);
		foreach (int restoreId : restoreIds)
		{
			SCR_MapMarkerBase restored = kept.Get(restoreId);
			kept.Remove(restoreId);
			if (!restored || CLW_IsListed(restored))
				continue;

			m_aStaticMarkers.Insert(restored);
			if (mapOpen)
				restored.OnCreateMarker(true);
		}

		if (!restoreIds.IsEmpty())
		{
			CheckMarkersUserRestrictions();
			Print("CLW: markers client faction " + newFaction.GetFactionKey() + ": restored " + restoreIds.Count().ToString() + " kept marker(s), " + kept.Count().ToString() + " still kept for other factions", LogLevel.NORMAL);
		}
	}

	// Server markers with faction flags that are listed now (about to be filtered by the vanilla handler) go to the kept list.
	protected void CLW_KeepFactionMarkers()
	{
		map<int, ref SCR_MapMarkerBase> kept = CLW_Kept();
		array<SCR_MapMarkerBase> listed = GetStaticMarkers();
		foreach (SCR_MapMarkerBase disabledMarker : GetDisabledMarkers())
		{
			listed.Insert(disabledMarker);
		}

		foreach (SCR_MapMarkerBase listedMarker : listed)
		{
			if (!listedMarker || listedMarker.GetMarkerID() == -1 || listedMarker.GetMarkerFactionFlags() == 0)
				continue;

			if (!kept.Contains(listedMarker.GetMarkerID()))
				kept.Set(listedMarker.GetMarkerID(), listedMarker);
		}
	}
}

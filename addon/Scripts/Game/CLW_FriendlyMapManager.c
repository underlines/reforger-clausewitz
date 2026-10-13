// Local (never replicated) static markers for the friendly map layer (design: dev/notes/map-friendly-design.md 2.2).
// Second `modded class` of the marker manager (the first one is CLW_MapMarkerClient.c); all members are CLW_ prefixed.
modded class SCR_MapMarkerManagerComponent
{
	// Like OnAddSynchedMarker, but local: owner -1 (nobody can edit / delete it in the UI), no marker ID, no faction flags (survive faction changes).
	void CLW_AddLocalMarker(notnull SCR_MapMarkerBase marker)
	{
		marker.SetMarkerOwnerID(-1);
		m_aStaticMarkers.Insert(marker);

		SCR_MapEntity mapEnt = SCR_MapEntity.GetMapInstance();
		if (mapEnt && mapEnt.IsOpen() && mapEnt.GetMapUIComponent(SCR_MapMarkersUI))
			marker.OnCreateMarker(true);

		SCR_MapMarkerWidgetComponent comp = marker.GetMarkerComponent();
		if (comp)
			comp.SetEventListening(false);
	}

	// Vanilla RemoveStaticMarker only removes from the enabled list: a culled marker would stay as a ghost in m_aDisabledMarkers.
	void CLW_RemoveLocalMarker(notnull SCR_MapMarkerBase marker)
	{
		marker.OnDelete();
		m_aStaticMarkers.RemoveItem(marker);
		m_aDisabledMarkers.RemoveItem(marker);
	}

	// Chained after the CLW_MapMarkerClient.c layer: lets the friendly client layer hook the map events from the start.
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		CLW_FriendlyMapClient.Hook();
	}
}

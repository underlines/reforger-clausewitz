// Local (never replicated) static markers for the friendly map layer (design: dev/notes/map-friendly-design.md 2.2).
// Second `modded class` of the marker manager (the first one is CLW_MapMarkerClient.c); all members are CLW_ prefixed.
modded class SCR_MapMarkerManagerComponent
{
	// Like OnAddSynchedMarker, but local: owner -1 (nobody can edit / delete it in the UI), no marker ID, no faction flags (survive faction changes).
	void CLW_AddLocalMarker(notnull SCR_MapMarkerBase marker)
	{
		marker.SetMarkerOwnerID(-1);
		if (!CLW_IsLocalListed(marker))
			m_aStaticMarkers.Insert(marker);

		SCR_MapEntity mapEnt = SCR_MapEntity.GetMapInstance();
		if (mapEnt && mapEnt.IsOpen() && mapEnt.GetMapUIComponent(SCR_MapMarkersUI) && !marker.GetRootWidget())
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

	bool CLW_IsLocalListed(SCR_MapMarkerBase marker)
	{
		return m_aStaticMarkers.Contains(marker) || m_aDisabledMarkers.Contains(marker);
	}

	// Map-open check of our markers: counts the listed ones and the ones that own a widget, and creates a missing widget (vanilla
	// SCR_MapMarkersUI.CreateStaticMarkers should have; this is the safety net). Returns a short description for the log
	// (visible frame, culled count, first marker position and widget visibility) so a test can tell "no widget" from "culled" from "off screen".
	string CLW_CheckLocalMarkers(notnull array<SCR_MapMarkerBase> mine, out int listed, out int withWidget, out int ensured, out int visible)
	{
		listed = 0;
		withWidget = 0;
		ensured = 0;
		visible = 0;

		SCR_MapEntity mapEnt = SCR_MapEntity.GetMapInstance();
		bool canCreate = mapEnt && mapEnt.IsOpen() && mapEnt.GetMapUIComponent(SCR_MapMarkersUI);
		int culled = 0;
		string first = "";
		foreach (SCR_MapMarkerBase marker : mine)
		{
			if (!marker)
				continue;

			if (CLW_IsLocalListed(marker))
				listed++;

			if (m_aDisabledMarkers.Contains(marker))
				culled++;

			if (!marker.GetRootWidget() && canCreate)
			{
				marker.OnCreateMarker(true);
				if (marker.GetRootWidget())
					ensured++;
			}

			Widget root = marker.GetRootWidget();
			if (root)
			{
				withWidget++;
				if (root.IsVisible())
					visible++;
			}

			if (first.IsEmpty())
			{
				int pos[2];
				marker.GetWorldPos(pos);
				string rootState = "none";
				if (root)
					rootState = "set";

				first = " first=" + pos[0].ToString() + "," + pos[1].ToString() + " widget=" + rootState;
			}
		}

		string frame = "";
		if (mapEnt)
		{
			vector frameMin, frameMax;
			mapEnt.GetMapVisibleFrame(frameMin, frameMax);
			int minX = Math.Round(frameMin[0]);
			int minZ = Math.Round(frameMin[2]);
			int maxX = Math.Round(frameMax[0]);
			int maxZ = Math.Round(frameMax[2]);
			frame = " frame=" + minX.ToString() + "," + minZ.ToString() + "-" + maxX.ToString() + "," + maxZ.ToString();
		}

		return "listed=" + listed.ToString() + " culled=" + culled.ToString() + frame + first;
	}

	// Chained after the CLW_MapMarkerClient.c layer: lets the friendly client layer hook the map events from the start.
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		bool mapEntity = SCR_MapEntity.GetMapInstance() != null;
		if (!mapEntity)
			Print("CLW: friendly client marker manager started without a map entity, map events are hooked anyway (static invokers)", LogLevel.WARNING);

		CLW_FriendlyMapClient.Hook();
	}
}

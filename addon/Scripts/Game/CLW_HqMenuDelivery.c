// Vehicle delivery feedback (M4 WP4). CLW_RequestService.Handle calls Track() when a vehicle request was granted: the requester has
// already been told "vehicle delivered, grid ..." (hint + chat, the answer of Handle); this class puts a temporary own-side map marker
// (same marker approach as CLW_StructureMarkers: static military marker with the faction flags of the requester's side) at the vehicle
// and removes it after DELIVERY_MARKER_SEC, when the requester gets in, or when the vehicle is gone. A vehicle that is driven away by
// somebody else drags its marker along (re-inserted when it moved more than MOVE_M, static markers cannot be edited).
// Server only. Log prefix: "CLW: hqmenu delivery".

class CLW_VehicleDeliveryInfo
{
	IEntity m_Vehicle;
	int m_iPlayer;
	string m_sFaction;
	float m_fExpire;				// world time (s)
	vector m_vMarkerPos;
	ref SCR_MapMarkerBase m_Marker;
}

class CLW_VehicleDelivery
{
	static const float MOVE_M = 25;

	protected static ref array<ref CLW_VehicleDeliveryInfo> s_aList = {};
	protected static bool s_bPolling;

	// Game start (server): the markers of the previous session are gone with its marker manager.
	static void Reset()
	{
		s_aList.Clear();
		s_bPolling = false;
		GetGame().GetCallqueue().Remove(Poll);
	}

	static void Track(int playerId, string faction, IEntity vehicle)
	{
		if (!Replication.IsServer() || !vehicle)
			return;

		CLW_VehicleDeliveryInfo info = new CLW_VehicleDeliveryInfo();
		info.m_Vehicle = vehicle;
		info.m_iPlayer = playerId;
		info.m_sFaction = faction;
		info.m_fExpire = CLW_RequestService.Now() + CLW_HqMenuConfig.DELIVERY_MARKER_SEC;
		s_aList.Insert(info);
		bool placed = PlaceMarker(info);
		Print(string.Format("CLW: hqmenu delivery player=%1 faction=%2 vehicle at %3 grid %4 marker=%5 (removed after %6 s or when the requester gets in)", playerId, faction, vehicle.GetOrigin(), CLW_Places.Grid(vehicle.GetOrigin()), placed, CLW_HqMenuConfig.DELIVERY_MARKER_SEC), LogLevel.NORMAL);
		if (!s_bPolling)
		{
			s_bPolling = true;
			GetGame().GetCallqueue().CallLater(Poll, CLW_HqMenuConfig.DELIVERY_POLL_MS, true);
		}
	}

	protected static bool PlaceMarker(CLW_VehicleDeliveryInfo info)
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!manager || !factionManager || !info.m_Vehicle)
			return false;

		Faction side = factionManager.GetFactionByKey(info.m_sFaction);
		if (!side)
			return false;

		EMilitarySymbolIdentity identity = EMilitarySymbolIdentity.INDFOR;
		if (CLW_ScenarioConfig.IsClwFaction(info.m_sFaction))
			identity = CLW_ScenarioConfig.GetIdentity(info.m_sFaction);

		SCR_MapMarkerBase marker = manager.PrepareMilitaryMarker(identity, EMilitarySymbolDimension.LAND, EMilitarySymbolIcon.MOTORIZED);
		if (!marker)
			return false;

		vector pos = info.m_Vehicle.GetOrigin();
		marker.SetWorldPos(pos[0], pos[2]);
		marker.SetMarkerFactionFlags(1 << factionManager.GetFactionIndex(side));	// own side only
		marker.SetCustomText("#CLW-HqMenu_Marker");
		manager.InsertStaticMarker(marker, false, true);
		info.m_Marker = marker;
		info.m_vMarkerPos = pos;
		return true;
	}

	protected static void RemoveMarker(CLW_VehicleDeliveryInfo info)
	{
		if (!info.m_Marker)
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (manager)
			manager.RemoveStaticMarker(info.m_Marker);
		info.m_Marker = null;
	}

	protected static void Poll()
	{
		float now = CLW_RequestService.Now();
		for (int i = s_aList.Count() - 1; i >= 0; i--)
		{
			CLW_VehicleDeliveryInfo info = s_aList[i];
			string why;
			if (!info.m_Vehicle)
			{
				why = "vehicle gone";
			}
			else if (now >= info.m_fExpire)
			{
				why = "expired";
			}
			else
			{
				IEntity body = GetGame().GetPlayerManager().GetPlayerControlledEntity(info.m_iPlayer);
				if (body && CompartmentAccessComponent.GetVehicleIn(body) == info.m_Vehicle)
					why = "requester got in";
			}

			if (!why.IsEmpty())
			{
				RemoveMarker(info);
				s_aList.Remove(i);
				Print(string.Format("CLW: hqmenu delivery player=%1 marker removed: %2", info.m_iPlayer, why), LogLevel.NORMAL);
				continue;
			}

			// somebody else drives it away: move the marker along
			if (vector.DistanceXZ(info.m_vMarkerPos, info.m_Vehicle.GetOrigin()) > MOVE_M)
			{
				RemoveMarker(info);
				PlaceMarker(info);
			}
		}

		if (s_aList.IsEmpty())
		{
			s_bPolling = false;
			GetGame().GetCallqueue().Remove(Poll);
		}
	}

	// Rematch (CLW_Rematch.c): remove the delivery markers still on the map, then forget the list (Reset alone leaves the markers).
	static void RematchReset()
	{
		foreach (CLW_VehicleDeliveryInfo info : s_aList)
		{
			RemoveMarker(info);
		}

		Reset();
	}
}

// Vanilla group markers on the map, switched by the friendly-map setup (design: dev/notes/map-friendly-design.md 2.1).
// While no CLW picture is active the client layer answers "keep", so vanilla behaves exactly as without the mod.

modded class SCR_MapMarkerSquadLeader
{
	override void UpdateLocalVisibility()
	{
		if (!CLW_FriendlyMapClient.KeepVanillaLeader(GetGroup()))
		{
			// hidden: no super call (its SetLocalVisible(true) would create and delete the widget on every update)
			m_bDoLocalVisibilityUpdate = false;
			SetLocalVisible(false);
			return;
		}

		super.UpdateLocalVisibility();
	}
}

modded class SCR_MapMarkerSquadMember
{
	override void UpdateLocalVisibility()
	{
		if (!CLW_FriendlyMapClient.KeepVanillaMember())
		{
			m_bDoLocalVisibilityUpdate = false;
			SetLocalVisible(false);
			return;
		}

		super.UpdateLocalVisibility();
	}
}

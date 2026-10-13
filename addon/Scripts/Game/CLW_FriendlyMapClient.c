// Client map layer for friendly units (design: dev/notes/map-friendly-design.md 2.2 / 3 / WP-C). Client only.
// The server sends one picture string per player (CLW_FriendlyPic wire); this class keeps LOCAL PLACED_MILITARY markers in sync with it
// (SetWorldPos on the same object, no widget churn), draws the "me" arrow from the local entity, and tells the modded vanilla group markers
// (CLW_FriendlyMapSuppress.c) whether they may show. No CLW picture (non-CLW scenario, between wars) => vanilla is left untouched.
class CLW_FriendlyMarkerState
{
	SCR_MapMarkerBase m_Marker;
	string m_sKind;
	string m_sLabel;
	int m_iCount;
}

class CLW_FriendlyMapClient
{
	static const int ME_TICK_MS = 500;
	static const int REQUEST_GAP_MS = 10000;

	protected static ref CLW_FriendlyPic s_Pic;
	protected static ref map<string, ref CLW_FriendlyMarkerState> s_mMarkers;
	protected static SCR_MapMarkerBase s_MeMarker;
	protected static bool s_bHooked;
	protected static bool s_bMapOpen;
	protected static bool s_bTimerOn;
	protected static int s_iLastRequestMs = -1000000;
	protected static string s_sRefreshState = "";
	protected static bool s_bSymbolFailLogged;

	// ---- public API ----

	// Picture from the server (CLW_FriendlyPic.ToWire).
	static void Receive(string wire)
	{
		if (System.IsConsoleApp())
			return;

		CLW_FriendlyPic pic = CLW_FriendlyPic.FromWire(wire);
		if (!pic)
		{
			Print("CLW: friendly client got an unreadable picture: " + wire.Substring(0, Math.Min(wire.Length(), 120)), LogLevel.WARNING);
			return;
		}

		Hook();
		s_Pic = pic;
		Print(string.Format("CLW: friendly client received v%1 %2 entries=%3 mask=%4 active=%5", pic.m_iVersion, pic.m_sFaction, pic.m_aEntries.Count(), pic.m_iMask, pic.m_bActive), LogLevel.NORMAL);

		if (!pic.m_bActive)
		{
			RemoveAll();
			RemoveMe();
			RefreshVanilla();
			return;
		}

		Apply();
		RefreshIfChanged();
		if (s_bMapOpen)
			UpdateMe();
	}

	// An active picture was received and the local faction is still the one it was built for.
	static bool IsActive()
	{
		if (System.IsConsoleApp() || !s_Pic || !s_Pic.m_bActive || s_Pic.m_sFaction.IsEmpty())
			return false;

		return LocalFactionKey() == s_Pic.m_sFaction;
	}

	static int Mask()
	{
		if (!IsActive())
			return 0;

		return s_Pic.m_iMask;
	}

	// Vanilla leader marker of this group may show?
	static bool KeepVanillaLeader(SCR_AIGroup group)
	{
		if (!IsActive())
			return true;

		int mask = s_Pic.m_iMask;
		if (mask & 4)
			return true;

		if ((mask & 2) && group && group == LocalGroup())
			return true;

		return false;
	}

	static bool KeepVanillaMember()
	{
		if (!IsActive())
			return true;

		return (s_Pic.m_iMask & 2) != 0;
	}

	// Re-evaluates every vanilla leader / member marker (map open, picture state change, local faction change).
	static void RefreshVanilla()
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return;

		array<SCR_MapMarkerEntity> entities = manager.GetDynamicMarkers();
		if (!entities)
			return;

		foreach (SCR_MapMarkerEntity entity : entities)
		{
			if (!entity)
				continue;

			SCR_MapMarkerSquadLeader leader = SCR_MapMarkerSquadLeader.Cast(entity);
			if (leader)
			{
				leader.UpdateLocalVisibility();
				continue;
			}

			SCR_MapMarkerSquadMember member = SCR_MapMarkerSquadMember.Cast(entity);
			if (member)
				member.UpdateLocalVisibility();
		}
	}

	// Registers the map events once (called from the marker manager's OnPostInit and from Receive).
	static void Hook()
	{
		if (s_bHooked || System.IsConsoleApp())
			return;

		s_bHooked = true;
		SCR_MapEntity.GetOnMapOpenComplete().Insert(OnMapOpened);
		SCR_MapEntity.GetOnMapClose().Insert(OnMapClosed);
		Print("CLW: friendly client hooked the map events", LogLevel.NORMAL);
	}

	// ---- internals ----

	protected static map<string, ref CLW_FriendlyMarkerState> Markers()
	{
		if (!s_mMarkers)
			s_mMarkers = new map<string, ref CLW_FriendlyMarkerState>();

		return s_mMarkers;
	}

	protected static string LocalFactionKey()
	{
		Faction faction = SCR_FactionManager.SGetLocalPlayerFaction();
		if (!faction)
			return "";

		return faction.GetFactionKey();
	}

	protected static SCR_AIGroup LocalGroup()
	{
		PlayerController controller = GetGame().GetPlayerController();
		if (!controller)
			return null;

		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		if (!groups)
			return null;

		return groups.GetPlayerGroup(controller.GetPlayerId());
	}

	// Refresh the vanilla markers only when what they depend on changed (active flag, mask, faction).
	protected static void RefreshIfChanged()
	{
		string state = "off";
		if (IsActive())
			state = s_Pic.m_iMask.ToString() + "/" + s_Pic.m_sFaction;

		if (state == s_sRefreshState)
			return;

		s_sRefreshState = state;
		RefreshVanilla();
	}

	protected static void OnMapOpened(MapConfiguration config)
	{
		s_bMapOpen = true;
		CheckState();

		Apply();	// markers created while closed get their widgets from the map UI; this only re-enables the culled ones
		RefreshVanilla();
		UpdateMe();

		if (!s_bTimerOn)
		{
			s_bTimerOn = true;
			GetGame().GetCallqueue().CallLater(MeTick, ME_TICK_MS, true);
		}
	}

	protected static void OnMapClosed(MapConfiguration config)
	{
		s_bMapOpen = false;
		s_bTimerOn = false;
		GetGame().GetCallqueue().Remove(MeTick);
	}

	protected static void MeTick()
	{
		if (!s_bMapOpen)
		{
			s_bTimerOn = false;
			GetGame().GetCallqueue().Remove(MeTick);
			return;
		}

		CheckState();
		UpdateMe();
	}

	// Local faction changed / no picture yet: drop stale markers, restore vanilla, ask the server for a picture.
	protected static void CheckState()
	{
		string factionKey = LocalFactionKey();
		if (s_Pic && s_Pic.m_bActive && factionKey != s_Pic.m_sFaction)
		{
			Print("CLW: friendly client local faction '" + factionKey + "' differs from picture '" + s_Pic.m_sFaction + "', cleared", LogLevel.NORMAL);
			RemoveAll();
			RemoveMe();
			RefreshIfChanged();
		}

		if (factionKey.IsEmpty() || IsActive())
			return;

		int now = System.GetTickCount();
		if (now - s_iLastRequestMs < REQUEST_GAP_MS)
			return;

		s_iLastRequestMs = now;
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (controller)
		{
			Print("CLW: friendly client requests a picture (" + factionKey + ")", LogLevel.NORMAL);
			controller.CLW_FriendlyRequest();
		}
	}

	// Reconcile the local markers with the picture: same key + same look => move only; changed => recreate; missing => delete.
	protected static void Apply()
	{
		if (!IsActive())
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return;

		int mask = s_Pic.m_iMask;
		map<string, ref CLW_FriendlyMarkerState> markers = Markers();
		map<string, bool> seen = new map<string, bool>();

		foreach (CLW_FriendlyEntry entry : s_Pic.m_aEntries)
		{
			if (!entry || entry.m_sKey.IsEmpty())
				continue;

			bool allowed = false;
			if (entry.m_sType == "m")
				allowed = (mask & 2) != 0;
			else if (entry.m_sType == "s")
				allowed = (mask & 4) != 0;
			else if (entry.m_sType == "v")
				allowed = (mask & 8) != 0;

			if (!allowed)
				continue;

			string key = entry.m_sType + ":" + entry.m_sKey;
			int worldX = entry.m_iX5 * 5 + 2;
			int worldZ = entry.m_iZ5 * 5 + 2;
			seen.Set(key, true);

			CLW_FriendlyMarkerState state;
			if (markers.Find(key, state) && state && state.m_Marker)
			{
				if (state.m_sKind == entry.m_sKind && state.m_sLabel == entry.m_sLabel && state.m_iCount == entry.m_iCount)
				{
					state.m_Marker.SetWorldPos(worldX, worldZ);
					continue;
				}

				manager.CLW_RemoveLocalMarker(state.m_Marker);
				markers.Remove(key);
			}

			SCR_MapMarkerBase marker = CLW_FriendlySymbols.CreateMilitary(s_Pic.m_iIdentity, entry.m_sKind, entry.m_sType == "s", entry.m_sLabel, worldX, worldZ);
			if (!marker)
				continue;

			manager.CLW_AddLocalMarker(marker);
			CLW_FriendlyMarkerState created = new CLW_FriendlyMarkerState();
			created.m_Marker = marker;
			created.m_sKind = entry.m_sKind;
			created.m_sLabel = entry.m_sLabel;
			created.m_iCount = entry.m_iCount;
			markers.Set(key, created);
		}

		array<string> stale = {};
		foreach (string existing, CLW_FriendlyMarkerState existingState : markers)
		{
			if (!seen.Contains(existing))
				stale.Insert(existing);
		}

		foreach (string staleKey : stale)
		{
			CLW_FriendlyMarkerState gone;
			if (markers.Find(staleKey, gone) && gone && gone.m_Marker)
				manager.CLW_RemoveLocalMarker(gone.m_Marker);

			markers.Remove(staleKey);
		}

		if (!s_bMapOpen)
			return;

		// units that moved into view appear without a pan (the next frame's OnUpdate culls the out-of-view ones again)
		foreach (string liveKey, CLW_FriendlyMarkerState live : markers)
		{
			if (live && live.m_Marker)
				manager.SetStaticMarkerDisabled(live.m_Marker, false);
		}
	}

	// The "me" arrow from the local controlled entity (no server data needed beyond the switch).
	protected static void UpdateMe()
	{
		if (!s_bMapOpen)
			return;

		if (!IsActive() || (s_Pic.m_iMask & 1) == 0)
		{
			RemoveMe();
			return;
		}

		IEntity entity = SCR_PlayerController.GetLocalControlledEntity();
		if (!entity)
		{
			RemoveMe();
			return;
		}

		CharacterControllerComponent character = CharacterControllerComponent.Cast(entity.FindComponent(CharacterControllerComponent));
		if (character && character.GetLifeState() != ECharacterLifeState.ALIVE)
		{
			RemoveMe();
			return;
		}

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return;

		vector origin = entity.GetOrigin();
		vector angles = entity.GetYawPitchRoll();
		int degrees = Math.Round(angles[0]);
		int posX = Math.Round(origin[0]);
		int posZ = Math.Round(origin[2]);
		degrees = degrees % 360;
		if (degrees < 0)
			degrees += 360;

		if (!s_MeMarker)
		{
			s_MeMarker = CLW_FriendlySymbols.CreateMe();
			if (!s_MeMarker)
			{
				if (!s_bSymbolFailLogged)
				{
					s_bSymbolFailLogged = true;
					Print("CLW: friendly client cannot create the 'me' marker (no PLACED_CUSTOM entry in the marker config)", LogLevel.WARNING);
				}

				return;
			}

			s_MeMarker.SetWorldPos(posX, posZ);
			s_MeMarker.SetRotation(degrees);
			manager.CLW_AddLocalMarker(s_MeMarker);
		}

		s_MeMarker.SetWorldPos(posX, posZ);
		s_MeMarker.SetRotation(degrees);
		SCR_MapMarkerWidgetComponent comp = s_MeMarker.GetMarkerComponent();
		if (comp && s_MeMarker.GetRootWidget())
			comp.SetRotation(degrees);

		manager.SetStaticMarkerDisabled(s_MeMarker, false);
	}

	protected static void RemoveMe()
	{
		if (!s_MeMarker)
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (manager)
			manager.CLW_RemoveLocalMarker(s_MeMarker);

		s_MeMarker = null;
	}

	protected static void RemoveAll()
	{
		if (!s_mMarkers)
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (manager)
		{
			foreach (string key, CLW_FriendlyMarkerState state : s_mMarkers)
			{
				if (state && state.m_Marker)
					manager.CLW_RemoveLocalMarker(state.m_Marker);
			}
		}

		s_mMarkers.Clear();
	}
}

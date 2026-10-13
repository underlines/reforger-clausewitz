// Client map layer for friendly units (design: dev/notes/map-friendly-design.md 2.2 / 3 / WP-C). Client only.
// The server sends one picture string per player (CLW_FriendlyPic wire); this class keeps LOCAL PLACED_MILITARY markers in sync with it
// (SetWorldPos on the same object, no widget churn), draws the "me" marker (a PLACED_MILITARY symbol) from the local entity, and tells the modded vanilla group markers
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
	static const int MAP_CHECK_MS = 1500;

	protected static ref CLW_FriendlyPic s_Pic;
	protected static ref map<string, ref CLW_FriendlyMarkerState> s_mMarkers;
	protected static SCR_MapMarkerBase s_MeMarker;
	protected static int s_iApplyLogs;
	protected static ref map<string, bool> s_mOnce;
	protected static bool s_bHooked;
	protected static bool s_bMapOpen;
	protected static bool s_bTimerOn;
	protected static int s_iLastRequestMs = -1000000;
	protected static string s_sRefreshState = "";

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

	// Vanilla leader marker of this group may show? With an active CLW picture the answer is no: the picture already carries every
	// group the switches allow (own group as individual members, other groups as one squad symbol at the leader, human-led ones included),
	// so a vanilla leader marker would only draw the same unit a second time (the leader of the own group is also one of the members).
	// No picture (non-CLW scenario, between wars) => vanilla untouched.
	static bool KeepVanillaLeader(SCR_AIGroup group)
	{
		return !IsActive();
	}

	static bool KeepVanillaMember()
	{
		return !IsActive();
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

	// Registers the map events. Called from the marker manager's OnPostInit and from every Receive: the invokers are static and
	// ~SCR_MapEntity clears them, so a recreated map entity would silently drop the hook; Remove + Insert keeps it idempotent.
	static void Hook()
	{
		if (System.IsConsoleApp())
			return;

		SCR_MapEntity.GetOnMapOpenComplete().Remove(OnMapOpened);
		SCR_MapEntity.GetOnMapClose().Remove(OnMapClosed);
		SCR_MapEntity.GetOnMapOpenComplete().Insert(OnMapOpened);
		SCR_MapEntity.GetOnMapClose().Insert(OnMapClosed);
		if (!s_bHooked)
		{
			s_bHooked = true;
			Print("CLW: friendly client hooked the map events", LogLevel.NORMAL);
		}
	}

	// true the first time a key is seen (every failure path logs once per cause, never per picture)
	protected static bool Once(string key)
	{
		if (!s_mOnce)
			s_mOnce = new map<string, bool>();

		if (s_mOnce.Contains(key))
			return false;

		s_mOnce.Set(key, true);
		return true;
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

		Apply();	// markers created while closed got their widgets from SCR_MapMarkersUI.CreateStaticMarkers (it ran before this event); this re-enables the culled ones
		RefreshVanilla();
		UpdateMe();
		LogMapState("map open");

		GetGame().GetCallqueue().Remove(MapCheck);
		GetGame().GetCallqueue().CallLater(MapCheck, MAP_CHECK_MS, false);

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
		GetGame().GetCallqueue().Remove(MapCheck);
	}

	// One look at the markers a moment after the map opened (after the first frames of the vanilla update culled / placed them).
	protected static void MapCheck()
	{
		if (s_bMapOpen)
			LogMapState("map open +" + MAP_CHECK_MS.ToString() + "ms");
	}

	// All our local markers (members / squads / vehicles + me).
	protected static void CollectMine(notnull array<SCR_MapMarkerBase> mine)
	{
		if (s_mMarkers)
		{
			foreach (string key, CLW_FriendlyMarkerState state : s_mMarkers)
			{
				if (state && state.m_Marker)
					mine.Insert(state.m_Marker);
			}
		}

		if (s_MeMarker)
			mine.Insert(s_MeMarker);
	}

	// "CLW: friendly client map open markers=<n> widgetsCreated=<n> ..." (widgetsCreated = our markers that own a widget; ensured = ones we had to create ourselves).
	protected static void LogMapState(string what)
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
		{
			if (Once("mapstate-nomanager"))
				Print("CLW: friendly client map open but no marker manager instance", LogLevel.WARNING);

			return;
		}

		array<SCR_MapMarkerBase> mine = {};
		CollectMine(mine);
		int listed, withWidget, ensured, visible;
		string detail = manager.CLW_CheckLocalMarkers(mine, listed, withWidget, ensured, visible);
		int mapUi = 0;
		SCR_MapEntity mapEnt = SCR_MapEntity.GetMapInstance();
		if (mapEnt && mapEnt.GetMapUIComponent(SCR_MapMarkersUI))
			mapUi = 1;

		Print(string.Format("CLW: friendly client %1 markers=%2 widgetsCreated=%3 ensured=%4 visible=%5 %6 markersUi=%7 active=%8", what, mine.Count(), withWidget, ensured, visible, detail, mapUi, IsActiveInt()), LogLevel.NORMAL);

		if (mine.Count() > 0 && mapUi == 0 && Once("mapstate-noui"))
			Print("CLW: friendly client this map has no SCR_MapMarkersUI component, local markers cannot get widgets here", LogLevel.WARNING);

		if (mine.Count() > 0 && withWidget == 0 && Once("mapstate-nowidget-" + what))
			Print("CLW: friendly client " + what + ": " + mine.Count().ToString() + " marker(s) but none owns a widget (OnCreateMarker produced nothing: no map menu root / frame / layout?)", LogLevel.WARNING);
	}

	protected static int IsActiveInt()
	{
		if (IsActive())
			return 1;

		return 0;
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
		int mapOpen = 0;
		if (s_bMapOpen)
			mapOpen = 1;

		if (!IsActive())
		{
			string why = "no picture";
			if (s_Pic)
				why = string.Format("picture faction '%1' active=%2, local faction '%3'", s_Pic.m_sFaction, s_Pic.m_bActive, LocalFactionKey());

			if (Once("apply-inactive-" + why))
				Print("CLW: friendly client apply skipped, not active: " + why, LogLevel.WARNING);

			return;
		}

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
		{
			if (Once("apply-nomanager"))
				Print("CLW: friendly client apply skipped, no marker manager instance", LogLevel.WARNING);

			return;
		}

		int mask = s_Pic.m_iMask;
		map<string, ref CLW_FriendlyMarkerState> markers = Markers();
		map<string, bool> seen = new map<string, bool>();
		int created = 0;
		int existing = 0;
		int removed = 0;
		int skipped = 0;

		foreach (CLW_FriendlyEntry entry : s_Pic.m_aEntries)
		{
			if (!entry || entry.m_sKey.IsEmpty())
			{
				if (Once("apply-emptykey"))
					Print("CLW: friendly client apply: entry without a key skipped", LogLevel.WARNING);

				continue;
			}

			bool allowed = false;
			if (entry.m_sType == "m")
				allowed = (mask & 2) != 0;
			else if (entry.m_sType == "s")
				allowed = (mask & 4) != 0;
			else if (entry.m_sType == "v")
				allowed = (mask & 8) != 0;
			else if (Once("apply-type-" + entry.m_sType))
				Print("CLW: friendly client apply: unknown entry type '" + entry.m_sType + "' skipped", LogLevel.WARNING);

			if (!allowed)
			{
				skipped++;
				continue;
			}

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
					existing++;
					continue;
				}

				manager.CLW_RemoveLocalMarker(state.m_Marker);
				markers.Remove(key);
			}

			SCR_MapMarkerBase marker = CLW_FriendlySymbols.CreateMilitary(s_Pic.m_iIdentity, entry.m_sKind, entry.m_sType == "s", entry.m_sLabel, worldX, worldZ);
			if (!marker)
			{
				if (Once("apply-create-" + entry.m_sKind))
					Print("CLW: friendly client apply: no marker for entry " + key + " kind '" + entry.m_sKind + "' (see the PrepareMilitaryMarker warning)", LogLevel.WARNING);

				continue;
			}

			manager.CLW_AddLocalMarker(marker);
			CLW_FriendlyMarkerState createdState = new CLW_FriendlyMarkerState();
			createdState.m_Marker = marker;
			createdState.m_sKind = entry.m_sKind;
			createdState.m_sLabel = entry.m_sLabel;
			createdState.m_iCount = entry.m_iCount;
			markers.Set(key, createdState);
			created++;
		}

		array<string> stale = {};
		foreach (string existingKey, CLW_FriendlyMarkerState existingState : markers)
		{
			if (!seen.Contains(existingKey))
				stale.Insert(existingKey);
		}

		foreach (string staleKey : stale)
		{
			CLW_FriendlyMarkerState gone;
			if (markers.Find(staleKey, gone) && gone && gone.m_Marker)
				manager.CLW_RemoveLocalMarker(gone.m_Marker);

			markers.Remove(staleKey);
			removed++;
		}

		// compact: every change, plus the first few pictures (a steady picture every 4 s does not repeat)
		if (created > 0 || removed > 0 || s_iApplyLogs < 3)
		{
			s_iApplyLogs++;
			Print(string.Format("CLW: friendly client apply entries=%1 created=%2 existing=%3 removed=%4 mapOpen=%5 skipped=%6", s_Pic.m_aEntries.Count(), created, existing, removed, mapOpen, skipped), LogLevel.NORMAL);
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

	// The "me" marker from the local controlled entity (no server data needed beyond the switch): an infantry NATO symbol of the
	// player's faction identity labelled "You" (#CLW-Map_Me). The military symbol cannot rotate, so no heading.
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
		{
			if (Once("me-nomanager"))
				Print("CLW: friendly client cannot draw the 'me' marker, no marker manager instance", LogLevel.WARNING);

			return;
		}

		vector origin = entity.GetOrigin();
		int posX = Math.Round(origin[0]);
		int posZ = Math.Round(origin[2]);

		if (!s_MeMarker)
		{
			s_MeMarker = CLW_FriendlySymbols.CreateMilitary(s_Pic.m_iIdentity, CLW_ForceKind.INFANTRY, false, "#CLW-Map_Me", posX, posZ);
			if (!s_MeMarker)
			{
				if (Once("me-create"))
					Print("CLW: friendly client cannot create the 'me' marker (PrepareMilitaryMarker returned null for identity " + s_Pic.m_iIdentity.ToString() + ")", LogLevel.WARNING);

				return;
			}

			manager.CLW_AddLocalMarker(s_MeMarker);
			Print("CLW: friendly client 'me' marker created at " + posX.ToString() + "," + posZ.ToString(), LogLevel.NORMAL);
		}

		s_MeMarker.SetWorldPos(posX, posZ);
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

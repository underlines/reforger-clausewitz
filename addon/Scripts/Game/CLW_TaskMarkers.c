// FOB base markers + task markers on the in-game map (owner round 3, P5/P14 map visibility).
//   FOB base marker  one base-style marker (like the HQ command post) per live FOB of a side, own side only, never an objective:
//                    "FOB1 (planned)" -> "FOB1 (building 40 %)" -> "FOB1"; removed when the FOB is lost or failed (refunded).
//   Task marker      the vanilla map has no replicated line / arrow marker (SCR_MapDrawingUI lines are local to the drawing client), so a unit
//                    tied up by a FOB or a convoy gets a marker that follows it, labelled "<unit> -> <destination>" (builder / escort -> FOB,
//                    convoy truck -> destination base). Own side only.
// Static markers cannot be edited after replication: a changed marker is removed and re-inserted (same approach as CLW_ContactMarkers).
// Driven by one ticker (TICK_MS) started by CLW_StructureMarkers (first structure marker of a war) and stopped by its RemoveAll / Reset.
// Data comes from the public API only: CLW_FobManager.GetFobs, CLW_FobManager.ReservedFor (texts "builder of FOB1", "escort of FOB1",
// "convoy CV1 HQ -> FOB1"), CLW_Orders.UnitPos. Server only. Log prefix: "CLW: markers".
class CLW_TaskMarker
{
	string m_sKey;
	string m_sSide;
	string m_sText;
	vector m_vPos;
	ref SCR_MapMarkerBase m_Marker;
}

class CLW_TaskMarkers
{
	static const int TICK_MS = 5000;
	static const float MOVE_THRESHOLD = 25;		// m: a task marker is re-inserted only after the unit moved farther than this
	static const int BUILD_STEP = 10;			// % steps of the "building" label

	protected static ref map<string, ref CLW_TaskMarker> s_mFobMarkers = new map<string, ref CLW_TaskMarker>();
	protected static ref map<string, ref CLW_TaskMarker> s_mUnitMarkers = new map<string, ref CLW_TaskMarker>();
	protected static bool s_bTicking;
	protected static int s_iTicks;

	// Idempotent: starts the ticker.
	static void Start()
	{
		if (!Replication.IsServer() || s_bTicking)
			return;

		s_bTicking = true;
		GetGame().GetCallqueue().CallLater(Tick, TICK_MS, true);
		Print("CLW: markers FOB/task marker ticker started (" + TICK_MS.ToString() + " ms)", LogLevel.NORMAL);
	}

	// Stops the ticker and removes every marker of this class.
	static void Stop()
	{
		GetGame().GetCallqueue().Remove(Tick);
		s_bTicking = false;
		s_iTicks = 0;
		int removed = RemoveAllIn(s_mFobMarkers) + RemoveAllIn(s_mUnitMarkers);
		if (removed > 0)
			Print("CLW: markers FOB/task markers removed: " + removed.ToString(), LogLevel.NORMAL);
	}

	// A FOB was lost / removed: drop its base marker at once (the ticker would do it within TICK_MS).
	static void RemoveFob(string sideKey, string fobId)
	{
		string key = sideKey + "|" + fobId;
		CLW_TaskMarker entry = s_mFobMarkers.Get(key);
		if (!entry)
			return;

		RemoveMarker(entry);
		s_mFobMarkers.Remove(key);
		Print("CLW: markers fob " + fobId + " (" + sideKey + ") base marker removed", LogLevel.NORMAL);
	}

	// ---- tick ----

	protected static void Tick()
	{
		if (!Replication.IsServer())
			return;

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;

		s_iTicks++;
		array<string> sides = {};
		war.GetFactionKeys(sides);

		map<string, bool> seenFobs = new map<string, bool>();
		map<string, bool> seenUnits = new map<string, bool>();
		foreach (string side : sides)
		{
			SyncFobs(side, seenFobs);
			SyncUnits(side, seenUnits);
		}

		Prune(s_mFobMarkers, seenFobs, "fob base");
		Prune(s_mUnitMarkers, seenUnits, "task");
	}

	protected static void SyncFobs(string side, map<string, bool> seen)
	{
		array<CLW_Fob> fobs = {};
		CLW_FobManager.GetFobs(side, fobs);	// live ones (planned .. active)
		foreach (CLW_Fob fob : fobs)
		{
			if (!fob || fob.m_vPos == vector.Zero)
				continue;

			string key = side + "|" + fob.m_sId;
			seen.Set(key, true);
			string text = FobLabel(fob);
			CLW_TaskMarker entry = s_mFobMarkers.Get(key);
			if (entry && entry.m_sText == text)
				continue;

			if (entry)
				RemoveMarker(entry);

			entry = new CLW_TaskMarker();
			entry.m_sKey = key;
			entry.m_sSide = side;
			entry.m_sText = text;
			entry.m_vPos = fob.m_vPos;
			if (!Insert(entry, EMilitarySymbolDimension.INSTALLATION, EMilitarySymbolIcon.MOBILEHQ))
			{
				s_mFobMarkers.Remove(key);
				continue;
			}

			s_mFobMarkers.Set(key, entry);
			Print(string.Format("CLW: markers fob %1 (%2) base marker '%3' at %4 (grid %5, state %6) %7", fob.m_sId, side, text, fob.m_vPos, CLW_Places.Grid(fob.m_vPos), fob.StateName(), CLW_MarkerLog.Describe(entry.m_Marker)), LogLevel.NORMAL);
		}
	}

	protected static string FobLabel(CLW_Fob fob)
	{
		if (fob.m_eState == CLW_EFobState.BUILDING)
		{
			int percent = Math.Round(fob.m_fBuild);
			percent = (percent / BUILD_STEP) * BUILD_STEP;
			return fob.m_sId + " (building " + percent.ToString() + " %)";
		}

		if (fob.IsInProgress())
			return fob.m_sId + " (planned)";

		return fob.m_sId;
	}

	protected static void SyncUnits(string side, map<string, bool> seen)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (!registry || !commander || !commander.GetRules())
			return;

		CLW_Orders orders = commander.GetRules().GetOrders();
		if (!orders)
			return;

		array<string> ids = {};
		registry.GetIds(side, ids);
		foreach (string id : ids)
		{
			string task = CLW_FobManager.ReservedFor(id);
			if (task.IsEmpty())
				continue;

			string dest = DestinationOf(task);
			vector pos;
			if (dest.IsEmpty() || !orders.UnitPos(id, pos) || pos == vector.Zero)
				continue;

			string key = side + "|" + id;
			seen.Set(key, true);
			string text = UnitName(registry, id) + " -> " + dest;
			CLW_TaskMarker entry = s_mUnitMarkers.Get(key);
			if (entry && entry.m_sText == text && vector.DistanceXZ(entry.m_vPos, pos) <= MOVE_THRESHOLD)
				continue;

			bool changed = !entry || entry.m_sText != text;
			if (entry)
				RemoveMarker(entry);

			entry = new CLW_TaskMarker();
			entry.m_sKey = key;
			entry.m_sSide = side;
			entry.m_sText = text;
			entry.m_vPos = pos;
			if (!Insert(entry, EMilitarySymbolDimension.LAND, UnitIcon(registry.GetKind(id))))
			{
				s_mUnitMarkers.Remove(key);
				continue;
			}

			s_mUnitMarkers.Set(key, entry);
			if (changed)
				Print(string.Format("CLW: markers task %1 (%2) '%3' (%4) at %5 %6", id, side, text, task, pos, CLW_MarkerLog.Describe(entry.m_Marker)), LogLevel.NORMAL);
		}
	}

	// Unit label. WP1 display names: replace the body with the display name getter (the id is the fallback).
	protected static string UnitName(CLW_SquadRegistry registry, string id)
	{
		return id;
	}

	// "builder of FOB1" / "escort of FOB1" -> "FOB1"; "convoy CV1 HQ -> FOB1" -> "FOB1". "" when not understood.
	protected static string DestinationOf(string task)
	{
		int arrow = task.IndexOf(" -> ");
		if (arrow >= 0)
			return task.Substring(arrow + 4, task.Length() - arrow - 4);

		int of = task.IndexOf(" of ");
		if (of >= 0)
			return task.Substring(of + 4, task.Length() - of - 4);

		return "";
	}

	protected static EMilitarySymbolIcon UnitIcon(string kind)
	{
		if (kind == CLW_ForceKind.LOGISTICS)
			return EMilitarySymbolIcon.SUPPLY;
		if (kind == CLW_ForceKind.MOTORIZED)
			return EMilitarySymbolIcon.MOTORIZED;

		return EMilitarySymbolIcon.INFANTRY;
	}

	// ---- markers ----

	protected static bool Insert(CLW_TaskMarker entry, EMilitarySymbolDimension dimension, EMilitarySymbolIcon icon)
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!manager || !factionManager)
			return false;

		Faction side = factionManager.GetFactionByKey(entry.m_sSide);
		if (!side)
			return false;

		EMilitarySymbolIdentity identity = EMilitarySymbolIdentity.INDFOR;
		if (CLW_ScenarioConfig.IsClwFaction(entry.m_sSide))
			identity = CLW_ScenarioConfig.GetIdentity(entry.m_sSide);

		SCR_MapMarkerBase marker = manager.PrepareMilitaryMarker(identity, dimension, icon);
		if (!marker)
			return false;

		marker.SetWorldPos(entry.m_vPos[0], entry.m_vPos[2]);
		marker.SetMarkerFactionFlags(1 << factionManager.GetFactionIndex(side));	// own side only
		marker.SetCustomText(entry.m_sText);
		manager.InsertStaticMarker(marker, false, true);
		entry.m_Marker = marker;
		return true;
	}

	protected static void RemoveMarker(CLW_TaskMarker entry)
	{
		if (!entry || !entry.m_Marker)
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (manager)
			manager.RemoveStaticMarker(entry.m_Marker);
		entry.m_Marker = null;
	}

	protected static int RemoveAllIn(map<string, ref CLW_TaskMarker> markers)
	{
		int removed;
		foreach (string key, CLW_TaskMarker entry : markers)
		{
			if (entry && entry.m_Marker)
				removed++;
			RemoveMarker(entry);
		}

		markers.Clear();
		return removed;
	}

	// Markers whose subject is gone (FOB lost, unit no longer tasked / dead) are removed.
	protected static void Prune(map<string, ref CLW_TaskMarker> markers, map<string, bool> seen, string what)
	{
		array<string> gone = {};
		foreach (string key, CLW_TaskMarker entry : markers)
		{
			if (!seen.Contains(key))
				gone.Insert(key);
		}

		foreach (string goneKey : gone)
		{
			CLW_TaskMarker old = markers.Get(goneKey);
			Print("CLW: markers " + what + " marker " + goneKey + " removed ('" + old.m_sText + "')", LogLevel.NORMAL);
			RemoveMarker(old);
			markers.Remove(goneKey);
		}
	}
}

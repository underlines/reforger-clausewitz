// Base / FOB structure markers (owner request 2026-10-12: "players cannot find the arsenal"). One static military map marker per IMPORTANT
// placed service (command post, arsenal, vehicle depot, supply cache; medical, repair stay off the map: CLW_ServiceEntry.ShowsMapMarker),
// visible only to its own side (faction flags, same approach as CLW_ContactMarkers), plus the own-side FOB location marker (AddFobLocation).
// Also the registry behind the `structures` block of the situation JSON (dashboard, ALL services, no locations) and the SITREP hint line.
// Hooks: CLW_Services.SpawnOne (Add), CLW_Services.RemoveBase (RemoveBase: lost FOB), CLW_Services.OnWarEnded (RemoveAll), CLW_Services.Reset.
// Server only. Log prefix: "CLW: markers".
class CLW_StructureInfo
{
	string m_sSide;
	string m_sBase;
	string m_sKind;
	string m_sLabel;
	vector m_vPos;
	bool m_bMap = true;			// false: dashboard only (field hospital, repair bay)
	bool m_bLocation;			// a base location marker (FOB), not a structure: never in the dashboard structures block
	ref SCR_MapMarkerBase m_Marker;
}

class CLW_StructureMarkers
{
	static const int RETRY_MS = 2000;
	static const int RETRIES = 8;
	static const string LOCATION_KIND = "location";

	protected static ref array<ref CLW_StructureInfo> s_aInfos = {};
	protected static bool s_bRetrying;
	protected static int s_iRetry;

	// Game start: forget everything (the marker manager of the previous session is gone with it).
	static void Reset()
	{
		s_aInfos.Clear();
		s_bRetrying = false;
		s_iRetry = 0;
		GetGame().GetCallqueue().Remove(Retry);
	}

	// A service was placed: remember it and put its marker on the map of its side.
	// mapMarker false: only remembered for the dashboard structures block, no marker on the map.
	static void Add(string sideKey, string baseId, string kind, vector pos, bool mapMarker = true)
	{
		if (!Replication.IsServer())
			return;

		CLW_StructureInfo info = new CLW_StructureInfo();
		info.m_sSide = sideKey;
		info.m_sBase = baseId;
		info.m_sKind = kind;
		info.m_sLabel = LabelFor(baseId, kind);
		info.m_vPos = pos;
		info.m_bMap = mapMarker;
		Track(info);
	}

	// An active FOB: its location marker ("FOB1") on the map of its own side only. Removed with RemoveBase when the FOB is lost.
	static void AddFobLocation(string sideKey, string fobId, vector pos)
	{
		if (!Replication.IsServer())
			return;

		foreach (CLW_StructureInfo existing : s_aInfos)
		{
			if (existing.m_bLocation && existing.m_sSide == sideKey && existing.m_sBase == fobId)
				return;
		}

		CLW_StructureInfo info = new CLW_StructureInfo();
		info.m_sSide = sideKey;
		info.m_sBase = fobId;
		info.m_sKind = LOCATION_KIND;
		info.m_sLabel = fobId;
		info.m_vPos = pos;
		info.m_bLocation = true;
		Track(info);
	}

	protected static void Track(CLW_StructureInfo info)
	{
		s_aInfos.Insert(info);
		if (!info.m_bMap)
		{
			Print(string.Format("CLW: markers %1 %2 '%3' at %4: no map marker (dashboard only)", info.m_sSide, info.m_sBase, info.m_sLabel, info.m_vPos), LogLevel.NORMAL);
			return;
		}

		if (!Place(info) && !s_bRetrying)
		{
			s_bRetrying = true;
			s_iRetry = 0;
			GetGame().GetCallqueue().CallLater(Retry, RETRY_MS, true);
		}
	}

	// A lost FOB: remove the markers of its structures.
	static int RemoveBase(string sideKey, string baseId)
	{
		if (!Replication.IsServer())
			return 0;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		int removed;
		for (int i = s_aInfos.Count() - 1; i >= 0; i--)
		{
			CLW_StructureInfo info = s_aInfos[i];
			if (info.m_sSide != sideKey || info.m_sBase != baseId)
				continue;

			if (info.m_Marker && manager)
				manager.RemoveStaticMarker(info.m_Marker);

			s_aInfos.Remove(i);
			removed++;
		}

		if (removed > 0)
			Print(string.Format("CLW: markers %1 %2: %3 structure/location entries removed", sideKey, baseId, removed), LogLevel.NORMAL);

		return removed;
	}

	// War over: remove every structure marker.
	static void RemoveAll()
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		int removed;
		foreach (CLW_StructureInfo info : s_aInfos)
		{
			if (info.m_Marker && manager)
			{
				manager.RemoveStaticMarker(info.m_Marker);
				removed++;
			}
		}

		s_aInfos.Clear();
		GetGame().GetCallqueue().Remove(Retry);
		s_bRetrying = false;
		Print(string.Format("CLW: markers war over: %1 structure markers removed", removed), LogLevel.NORMAL);
	}

	// ---- queries ----

	// "structures" array body (without brackets) of one side for the situation JSON: {base, kind, label, pos:[x,z], grid}.
	static string StructuresJson(string sideKey)
	{
		string text = "";
		foreach (CLW_StructureInfo info : s_aInfos)
		{
			if (info.m_sSide != sideKey || info.m_bLocation)
				continue;

			if (!text.IsEmpty())
				text += ",";
			text += "{\"base\":\"" + CLW_Situation.Escape(info.m_sBase) + "\",\"kind\":\"" + CLW_Situation.Escape(info.m_sKind) + "\",\"label\":\"" + CLW_Situation.Escape(info.m_sLabel);
			text += "\",\"pos\":[" + Math.Round(info.m_vPos[0]) + "," + Math.Round(info.m_vPos[2]) + "],\"grid\":\"" + CLW_Places.Grid(info.m_vPos) + "\"}";
		}

		return text;
	}

	// Grid of the first structure of this kind at the side's HQ, "" when none.
	static string GridOf(string sideKey, string kind)
	{
		foreach (CLW_StructureInfo info : s_aInfos)
		{
			if (info.m_sSide == sideKey && info.m_sBase == CLW_Services.BASE_HQ && info.m_sKind == kind)
				return CLW_Places.Grid(info.m_vPos);
		}

		return "";
	}

	// ---- internals ----

	protected static void Retry()
	{
		s_iRetry++;
		bool pending;
		foreach (CLW_StructureInfo info : s_aInfos)
		{
			if (info.m_bMap && !info.m_Marker && !Place(info))
				pending = true;
		}

		if (!pending || s_iRetry >= RETRIES)
		{
			GetGame().GetCallqueue().Remove(Retry);
			s_bRetrying = false;
			if (pending)
				Print("CLW: markers WARNING gave up placing structure markers (no marker manager / faction)", LogLevel.WARNING);
		}
	}

	// Creates the marker of one structure. false = not possible yet (manager missing).
	protected static bool Place(CLW_StructureInfo info)
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!manager || !factionManager)
			return false;

		Faction side = factionManager.GetFactionByKey(info.m_sSide);
		if (!side)
			return true;	// unknown faction: nothing to show, do not retry

		EMilitarySymbolIdentity identity = EMilitarySymbolIdentity.INDFOR;
		if (CLW_ScenarioConfig.IsClwFaction(info.m_sSide))
			identity = CLW_ScenarioConfig.GetIdentity(info.m_sSide);

		SCR_MapMarkerBase marker = manager.PrepareMilitaryMarker(identity, EMilitarySymbolDimension.INSTALLATION, IconFor(info.m_sKind));
		if (!marker)
			return true;

		marker.SetWorldPos(info.m_vPos[0], info.m_vPos[2]);
		marker.SetMarkerFactionFlags(1 << factionManager.GetFactionIndex(side));	// own side only
		marker.SetCustomText(info.m_sLabel);
		manager.InsertStaticMarker(marker, false, true);
		info.m_Marker = marker;
		Print(string.Format("CLW: markers %1 %2 '%3' at %4 (grid %5)", info.m_sSide, info.m_sBase, info.m_sLabel, info.m_vPos, CLW_Places.Grid(info.m_vPos)), LogLevel.NORMAL);
		return true;
	}

	protected static EMilitarySymbolIcon IconFor(string kind)
	{
		if (kind == LOCATION_KIND)
			return EMilitarySymbolIcon.RELAY;	// FOB location (same icon family as the base marker of the control points)
		if (kind == CLW_ServiceKind.COMMAND_POST)
			return EMilitarySymbolIcon.MOBILEHQ;
		if (kind == CLW_ServiceKind.SUPPLY)
			return EMilitarySymbolIcon.SUPPLY;
		if (kind == CLW_ServiceKind.MEDICAL)
			return EMilitarySymbolIcon.MEDICAL;
		if (kind == CLW_ServiceKind.REPAIR)
			return EMilitarySymbolIcon.MAINTENANCE;
		if (kind == CLW_ServiceKind.VEHICLE_DEPOT)
			return EMilitarySymbolIcon.MOTORIZED;

		return EMilitarySymbolIcon.GRENADE;	// arsenal: weapons
	}

	// "Arsenal", "HQ command post", "Vehicle depot / requests", "FOB1 arsenal". The request terminal stands at the command post and the depot.
	protected static string LabelFor(string baseId, string kind)
	{
		string name = "Arsenal";
		if (kind == CLW_ServiceKind.COMMAND_POST)
			name = "Command post";
		else if (kind == CLW_ServiceKind.SUPPLY)
			name = "Supply cache";
		else if (kind == CLW_ServiceKind.MEDICAL)
			name = "Field hospital";
		else if (kind == CLW_ServiceKind.REPAIR)
			name = "Repair bay";
		else if (kind == CLW_ServiceKind.VEHICLE_DEPOT)
			name = "Vehicle depot";

		if ((kind == CLW_ServiceKind.COMMAND_POST || kind == CLW_ServiceKind.VEHICLE_DEPOT) && CLW_Capabilities.Has("requests"))
			name += " / requests";

		if (baseId == CLW_Services.BASE_HQ)
		{
			if (kind == CLW_ServiceKind.COMMAND_POST)
				return "HQ " + LowerFirst(name);

			return name;
		}

		return baseId + " " + LowerFirst(name);
	}

	protected static string LowerFirst(string text)
	{
		string head = text.Substring(0, 1);
		head.ToLower();
		return head + text.Substring(1, text.Length() - 1);
	}
}

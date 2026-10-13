// Player contact reports (M3 phase Q, VISION 8, capability "reports"). Two inputs, both end as a CLW_Contact with source "human" in
// the reporter's faction contact book (so the rule commander, the LLM situation and the enemy markers use it; it expires like every
// other contact, CLW_ContactBook.EXPIRE_SECONDS):
//   1. chat  /report <count> <inf|veh|armor> <dist m> <dir>      dir = N NE E SE S SW W NW or a bearing in degrees (0 = north)
//   2. enemy-side military symbols that players place on the map (vanilla SCR_MapMarkerManagerComponent static markers of
//      type PLACED_MILITARY with the ENEMY side's identity; the text may carry a number = size). Parsed on the server every
//      MARKER_POLL_MS; each marker becomes a contact once.
// Markers made by the server (owner <= 0: AI marking, our own contact markers) are ignored. Server only. Log prefix "CLW: report".
class CLW_Reports
{
	protected static ref array<int> s_aSeenMarkers = {};
	protected static ref map<int, float> s_mLastReport = new map<int, float>();	// player -> world time of the last /report
	protected static bool s_bPolling;

	static void Reset()
	{
		s_aSeenMarkers.Clear();
		s_mLastReport.Clear();
		if (s_bPolling)
			GetGame().GetCallqueue().Remove(PollMarkers);
		s_bPolling = false;
	}

	// War start: begin parsing the players' enemy markers (once).
	static void Start()
	{
		if (!Replication.IsServer() || s_bPolling)
			return;

		if (!CLW_Capabilities.Has("reports"))
		{
			Print("CLW: report capability is off: no /report, no marker parsing", LogLevel.NORMAL);
			return;
		}

		s_bPolling = true;
		GetGame().GetCallqueue().CallLater(PollMarkers, CLW_RequestConfig.MARKER_POLL_MS, true);
		Print("CLW: report marker parser running (enemy military symbols placed by players become contacts)", LogLevel.NORMAL);
	}

	// ---- chat /report ----

	// args: "<count> <inf|veh|armor> <dist> <dir>". Runs on the server; answers the player. Returns the answer.
	static string Chat(int playerId, string args)
	{
		if (!Replication.IsServer())
			return "";

		string reply;
		string replyWire;	// the same for the player, as a CLW_Loc wire
		bool ok = Make(playerId, args, reply, replyWire);
		string verdict = "REFUSED";
		if (ok)
			verdict = "ACCEPTED";

		Print(string.Format("CLW: report player=%1(%2) args='%3' -> %4: %5", playerId, CLW_RequestService.PlayerName(playerId), args, verdict, reply), LogLevel.NORMAL);
		CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Req_Granted", replyWire), true);
		return reply;
	}

	protected static bool Make(int playerId, string args, out string reply, out string replyWire)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
		{
			reply = "report not possible: the war has not started";
			replyWire = CLW_Loc.W("#CLW-Rep_NoWar");
			return false;
		}

		if (!CLW_Capabilities.Has("reports"))
		{
			reply = "contact reports are off in this war mode";
			replyWire = CLW_Loc.W("#CLW-Rep_Off");
			return false;
		}

		string faction = CLW_RequestService.FactionKeyOf(playerId);
		vector origin;
		if (faction.IsEmpty() || !CLW_RequestService.PlayerPos(playerId, origin))
		{
			reply = "you must be deployed on a side to report";
			replyWire = CLW_Loc.W("#CLW-Rep_NotDeployed");
			return false;
		}

		array<string> tokens = {};
		args.Split(" ", tokens, true);
		if (tokens.Count() < 4)
		{
			reply = "usage: /report <count> <inf|veh|armor> <distance m> <direction N|NE|E|SE|S|SW|W|NW or degrees>";
			replyWire = CLW_Loc.W("#CLW-Rep_Usage");
			return false;
		}

		if (!CLW_ModeParam.IsNumber(tokens[0]))
		{
			reply = "count must be a number";
			replyWire = CLW_Loc.W("#CLW-Rep_CountNumber");
			return false;
		}

		int count = Math.Clamp(tokens[0].ToInt(), 1, CLW_RequestConfig.REPORT_MAX_COUNT);
		string kindToken = tokens[1];
		kindToken.ToLower();
		string kind = CLW_Contact.KIND_UNKNOWN;
		if (kindToken == "inf" || kindToken == "infantry" || kindToken == "men")
			kind = CLW_Contact.KIND_INFANTRY;
		else if (kindToken == "veh" || kindToken == "vehicle" || kindToken == "vehicles" || kindToken == "armor" || kindToken == "armour" || kindToken == "tank" || kindToken == "tanks")
			kind = CLW_Contact.KIND_VEHICLE;

		if (kind == CLW_Contact.KIND_UNKNOWN)
		{
			reply = "type must be inf, veh or armor";
			replyWire = CLW_Loc.W("#CLW-Rep_Type");
			return false;
		}

		float distance;
		if (!ParseDistance(tokens[2], distance))
		{
			reply = "distance not understood (meters, e.g. 300 or 1.2km)";
			replyWire = CLW_Loc.W("#CLW-Rep_DistanceUnclear");
			return false;
		}

		if (distance < CLW_RequestConfig.REPORT_MIN_DIST_M || distance > CLW_RequestConfig.REPORT_MAX_DIST_M)
		{
			reply = string.Format("distance must be %1 to %2 m", CLW_RequestConfig.REPORT_MIN_DIST_M, CLW_RequestConfig.REPORT_MAX_DIST_M);
			replyWire = CLW_Loc.W("#CLW-Rep_DistanceRange", CLW_Loc.N(CLW_RequestConfig.REPORT_MIN_DIST_M), CLW_Loc.N(CLW_RequestConfig.REPORT_MAX_DIST_M));
			return false;
		}

		float bearing;
		string dirToken = tokens[3];
		if (!ParseBearing(dirToken, bearing))
		{
			reply = "direction not understood (N NE E SE S SW W NW or degrees 0-360)";
			replyWire = CLW_Loc.W("#CLW-Rep_DirectionUnclear");
			return false;
		}

		float last;
		float now = CLW_RequestService.Now();
		if (s_mLastReport.Find(playerId, last) && now - last < CLW_RequestConfig.REPORT_COOLDOWN_SEC)
		{
			reply = "wait a few seconds between reports";
			replyWire = CLW_Loc.W("#CLW-Rep_Wait");
			return false;
		}

		string enemy = war.GetEnemyKey(faction);
		if (enemy.IsEmpty())
		{
			reply = "no known enemy";
			replyWire = CLW_Loc.W("#CLW-Rep_NoEnemy");
			return false;
		}

		CLW_ContactBook book = BookOf(faction);
		if (!book)
		{
			reply = "HQ has no contact book";
			replyWire = CLW_Loc.W("#CLW-Rep_NoBook");
			return false;
		}

		float rad = bearing * Math.DEG2RAD;
		vector pos = origin + Vector(Math.Sin(rad) * distance, 0, Math.Cos(rad) * distance);	// x east, z north, bearing clockwise from north
		pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
		CLW_Contact contact = NewContact(enemy, pos, kind, count, "report:" + CLW_RequestService.PlayerName(playerId), CLW_RequestConfig.REPORT_CONFIDENCE);
		array<ref CLW_Contact> sightings = {};
		sightings.Insert(contact);
		book.Report(faction, sightings);
		s_mLastReport.Set(playerId, now);
		war.PushEvent(faction, "player " + CLW_RequestService.PlayerName(playerId) + " reports " + count + " " + kind + " at grid " + CLW_Places.Grid(pos));
		reply = string.Format("contact logged: %1 %2, grid %3 (%4 m %5). Thanks.", count, kind, CLW_Places.Grid(pos), Math.Round(distance), dirToken);
		replyWire = CLW_Loc.W("#CLW-Rep_Logged", CLW_Loc.N(count), "#CLW-Rep_Kind_" + kind, CLW_Places.Grid(pos), CLW_Loc.N(Math.Round(distance)), dirToken);
		return true;
	}

	protected static CLW_Contact NewContact(string enemy, vector pos, string kind, int count, string observer, float confidence)
	{
		CLW_Contact contact = new CLW_Contact();
		contact.m_sObserver = observer;
		contact.m_sEnemyFaction = enemy;
		contact.m_vPos = pos;
		contact.m_iCount = count;
		contact.m_sKind = kind;
		if (kind == CLW_Contact.KIND_VEHICLE)
			contact.m_iVehicles = count;
		else
			contact.m_iInfantry = count;

		contact.m_fLastSeen = CLW_RequestService.Now();
		contact.m_sSource = "human";
		contact.m_fConfidence = confidence;
		return contact;
	}

	protected static CLW_ContactBook BookOf(string factionKey)
	{
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(factionKey);
		if (!commander || !commander.GetRules())
			return null;

		return commander.GetRules().GetBook();
	}

	protected static bool EndsWith(string text, string suffix)
	{
		int length = text.Length();
		int tail = suffix.Length();
		if (length < tail)
			return false;

		return text.Substring(length - tail, tail) == suffix;
	}

	// "300", "300m", "1.2km", "1.2k"
	protected static bool ParseDistance(string text, out float meters)
	{
		string value = text;
		value.ToLower();
		float factor = 1;
		if (EndsWith(value, "km"))
		{
			factor = 1000;
			value = value.Substring(0, value.Length() - 2);
		}
		else if (EndsWith(value, "k"))
		{
			factor = 1000;
			value = value.Substring(0, value.Length() - 1);
		}
		else if (EndsWith(value, "m"))
		{
			value = value.Substring(0, value.Length() - 1);
		}

		if (!CLW_ModeParam.IsNumber(value))
			return false;

		meters = value.ToFloat() * factor;
		return meters == meters;
	}

	// Compass word or letters, or degrees; bearing clockwise from north.
	protected static bool ParseBearing(string text, out float bearing)
	{
		string value = text;
		value.ToLower();
		if (CLW_ModeParam.IsNumber(value))
		{
			bearing = value.ToFloat();
			return bearing == bearing && bearing >= 0 && bearing <= 360;
		}

		array<string> names = {"n", "ne", "e", "se", "s", "sw", "w", "nw"};
		int at = names.Find(value);
		if (at < 0)
		{
			array<string> words = {"north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest"};
			at = words.Find(value);
		}

		if (at < 0)
			return false;

		bearing = at * 45;
		return true;
	}

	// ---- map markers ----

	protected static void PollMarkers()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;

		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return;

		SCR_MapMarkerEntryMilitary military;
		SCR_MapMarkerConfig config = manager.GetMarkerConfig();
		if (config)
			military = SCR_MapMarkerEntryMilitary.Cast(config.GetMarkerEntryConfigByType(SCR_EMapMarkerType.PLACED_MILITARY));

		array<SCR_MapMarkerBase> markers = manager.GetStaticMarkers();
		if (!markers)
			return;

		array<int> present = {};
		foreach (SCR_MapMarkerBase marker : markers)
		{
			if (!marker)
				continue;

			int id = marker.GetMarkerID();
			present.Insert(id);
			if (s_aSeenMarkers.Contains(id))
				continue;

			s_aSeenMarkers.Insert(id);
			if (marker.GetType() != SCR_EMapMarkerType.PLACED_MILITARY)
				continue;

			int owner = marker.GetMarkerOwnerID();
			if (owner <= 0)
				continue;	// server markers: AI marking and our own contact markers

			ParseMarker(war, marker, owner, military);
		}

		// forget markers that were removed
		for (int i = s_aSeenMarkers.Count() - 1; i >= 0; i--)
		{
			if (!present.Contains(s_aSeenMarkers[i]))
				s_aSeenMarkers.Remove(i);
		}
	}

	protected static void ParseMarker(CLW_WarState war, SCR_MapMarkerBase marker, int owner, SCR_MapMarkerEntryMilitary military)
	{
		string faction = CLW_RequestService.FactionKeyOf(owner);
		if (faction.IsEmpty() || !military)
			return;

		int factionEntry = marker.GetMarkerConfigID() % 100;
		SCR_MarkerMilitaryFactionEntry entry = military.GetFactionEntry(factionEntry);
		if (!entry)
			return;

		// only the symbol of the ENEMY side counts as a contact report; own / other symbols are the players' own map notes
		string enemy = war.GetEnemyKey(faction);
		if (enemy.IsEmpty() || entry.GetFactionIdentity() != CLW_ScenarioConfig.GetIdentity(enemy))
			return;

		CLW_ContactBook book = BookOf(faction);
		if (!book)
			return;

		int cell[2];
		marker.GetWorldPos(cell);
		vector pos = Vector(cell[0], 0, cell[1]);
		pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
		string kind = CLW_Contact.KIND_INFANTRY;
		if (IsVehicleSymbol(marker.GetFlags()))
			kind = CLW_Contact.KIND_VEHICLE;

		int count = CountFromText(marker.GetCustomText());
		string owner_name = CLW_RequestService.PlayerName(owner);
		CLW_Contact contact = NewContact(enemy, pos, kind, count, "marker:" + owner_name, CLW_RequestConfig.MARKER_CONFIDENCE);
		array<ref CLW_Contact> sightings = {};
		sightings.Insert(contact);
		book.Report(faction, sightings);
		war.PushEvent(faction, "player " + owner_name + " marked " + count + " " + kind + " at grid " + CLW_Places.Grid(pos));
		Print(string.Format("CLW: report marker id=%1 player=%2(%3) faction=%4 -> contact %5 x%6 at %7 (text '%8')", marker.GetMarkerID(), owner, owner_name, faction, kind, count, pos, marker.GetCustomText()), LogLevel.NORMAL);
	}

	// Icon flags of the military symbol -> vehicle contact. Looked up by name so a missing enum member cannot break compilation.
	protected static bool IsVehicleSymbol(int flags)
	{
		array<string> names = {"ARMOR", "MECHANIZED", "MOTORIZED"};
		foreach (string iconName : names)
		{
			int value = typename.StringToEnum(EMilitarySymbolIcon, iconName);
			if (value > 0 && (flags & value) != 0)
				return true;
		}

		return false;
	}

	// First number in the marker text = the size; default when none.
	protected static int CountFromText(string text)
	{
		string digits = "";
		for (int i = 0; i < text.Length(); i++)
		{
			string c = text.Get(i);
			if ("0123456789".Contains(c))
				digits += c;
			else if (!digits.IsEmpty())
				break;
		}

		if (digits.IsEmpty())
			return CLW_RequestConfig.MARKER_DEFAULT_COUNT;

		return Math.Clamp(digits.ToInt(), 1, CLW_RequestConfig.REPORT_MAX_COUNT);
	}
}

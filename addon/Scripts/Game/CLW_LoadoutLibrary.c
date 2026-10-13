// Server loadout library (loadouts plan R3, contract dev/notes/loadouts-impl.md). The library is owned by the sidecar (dashboard loadout editor)
// and pulled by the game at war start (CLW_LoadoutSync: POST /loadouts/sync). This class holds the pulled library in memory, keeps the cache
// file $profile:Clausewitz/loadouts.json (the same JSON the sidecar replied with: the game never needs the sidecar running), validates every
// entry (R6) and answers the two questions other code asks: which kit does an AI member of role R get (PickKitForRole, G1) and has the admin
// set a squad composition for a kind (CompositionOverride, G1). Server only. Log prefix: "CLW: loadouts".
//
// Big strings: a vanilla loadout string can exceed 8191 characters, so this file never uses string.Format / Substring on a kit; it only moves
// them around, parses them with SCR_JsonLoadContext DTOs and concatenates with +.

// created-from block of a library entry
class CLW_LibraryOrigin
{
	string identityId;
	string playerName;
}

// One library entry. Field names are the JSON keys of the sidecar record (contract).
class CLW_LibraryEntry
{
	string id;
	string name;
	string role;			// ROLE_* label name, e.g. "ROLE_MEDIC"
	string faction;
	bool active;
	int order;				// 1..7 = spawn slot
	string loadout;			// vanilla loadout string (never replicated to clients)
	string basePrefab;
	ref array<string> items = {};
	ref array<string> addons = {};
	string gameVersion;
	ref CLW_LibraryOrigin createdFrom = new CLW_LibraryOrigin();
	string status;			// set by the game: ok | degraded | invalid | inactive
	ref array<string> missing = {};
}

// Admin squad compositions per CLW_ForceKind: one array per kind (a plain DTO, the engine reader needs fixed names). Names = CLW_ForceKind values.
class CLW_CompositionsDto
{
	ref array<string> infantry = {};
	ref array<string> recon = {};
	ref array<string> motorized = {};
	ref array<string> light = {};
	ref array<string> mech = {};
	ref array<string> armor = {};
	ref array<string> armored_recon = {};
	ref array<string> logistics = {};

	array<string> Get(string kind)
	{
		if (kind == CLW_ForceKind.INFANTRY)
			return infantry;
		if (kind == CLW_ForceKind.RECON)
			return recon;
		if (kind == CLW_ForceKind.MOTORIZED)
			return motorized;
		if (kind == CLW_ForceKind.LIGHT)
			return light;
		if (kind == CLW_ForceKind.MECH)
			return mech;
		if (kind == CLW_ForceKind.ARMOR)
			return armor;
		if (kind == CLW_ForceKind.ARMORED_RECON)
			return armored_recon;
		if (kind == CLW_ForceKind.LOGISTICS)
			return logistics;

		return null;
	}
}

// Whole reply of POST /loadouts/sync = content of loadouts.json (pins are read separately, their faction keys are free).
class CLW_LibraryReply
{
	int v;
	ref array<ref CLW_LibraryEntry> library = {};
	ref CLW_CompositionsDto compositions = new CLW_CompositionsDto();
}

class CLW_LoadoutLibrary
{
	static const string FILE_PATH = "$profile:Clausewitz/loadouts.json";
	static const string STATUS_OK = "ok";
	static const string STATUS_DEGRADED = "degraded";
	static const string STATUS_INVALID = "invalid";
	static const string STATUS_INACTIVE = "inactive";	// the entry's faction is not in this war

	protected static ref array<ref CLW_LibraryEntry> s_aEntries = {};
	protected static ref CLW_CompositionsDto s_Compositions = new CLW_CompositionsDto();
	protected static ref map<string, ref array<string>> s_mPins = new map<string, ref array<string>>();	// "<faction>|<kind>" -> library ids
	protected static string s_sReply;		// raw JSON of the library in use (pins are parsed from it once the war factions are known)
	protected static string s_sSource = "none";	// sidecar | cache | none

	// ---- loading ----

	static int Count()
	{
		return s_aEntries.Count();
	}

	static string Source()
	{
		return s_sSource;
	}

	// All entries (the live objects: their status is updated by Validate).
	static void GetAll(notnull array<CLW_LibraryEntry> result)
	{
		foreach (CLW_LibraryEntry entry : s_aEntries)
		{
			result.Insert(entry);
		}
	}

	// Reads $profile:Clausewitz/loadouts.json. False when there is none or it is unusable (the library stays as it was).
	static bool LoadCache()
	{
		if (!FileIO.FileExists(FILE_PATH))
		{
			Print("CLW: loadouts library: no cache file " + FILE_PATH + ", defaults only", LogLevel.NORMAL);
			return false;
		}

		string text = SCR_FileIOHelper.GetFileStringContent(FILE_PATH, false);
		return Accept(text, "cache", false);
	}

	// Takes a library JSON (sidecar reply or cache file). False (nothing changed) when it does not parse. writeCache: store it as the cache file.
	static bool Accept(string text, string source, bool writeCache)
	{
		if (text.IsEmpty())
		{
			Print("CLW: loadouts library " + source + ": empty text", LogLevel.WARNING);
			return false;
		}

		SCR_JsonLoadContext ctx = new SCR_JsonLoadContext();
		CLW_LibraryReply reply = new CLW_LibraryReply();
		if (!ctx.ImportFromString(text) || !ctx.ReadValue("", reply))
		{
			Print("CLW: loadouts library " + source + ": not valid JSON (" + text.Length().ToString() + " chars), ignored", LogLevel.WARNING);
			return false;
		}

		s_aEntries.Clear();
		foreach (CLW_LibraryEntry entry : reply.library)
		{
			if (entry && !entry.id.IsEmpty())
				s_aEntries.Insert(entry);
		}

		if (reply.compositions)
			s_Compositions = reply.compositions;
		else
			s_Compositions = new CLW_CompositionsDto();

		s_sReply = text;
		s_sSource = source;
		s_mPins.Clear();
		int active;
		foreach (CLW_LibraryEntry kept : s_aEntries)
		{
			if (kept.active)
				active++;
		}

		Print("CLW: loadouts library from " + source + ": " + s_aEntries.Count().ToString() + " entries (" + active.ToString() + " active), " + text.Length().ToString() + " chars", LogLevel.NORMAL);
		if (writeCache)
			WriteCache(text);

		return true;
	}

	protected static void WriteCache(string text)
	{
		FileIO.MakeDirectory(CLW_CatalogOverrides.FILE_DIR);
		FileHandle handle = FileIO.OpenFile(FILE_PATH, FileMode.WRITE);
		if (!handle)
		{
			Print("CLW: loadouts ERROR cannot write the cache " + FILE_PATH, LogLevel.ERROR);
			return;
		}

		handle.WriteLine(text);
		handle.Close();
		Print("CLW: loadouts cache written: " + FILE_PATH + " (" + text.Length().ToString() + " chars)", LogLevel.NORMAL);
	}

	// ---- pins ----

	// Pins of the war's factions: reply.pins.<faction>.<kind> = ordered library ids. Faction keys are free in the JSON, so the war's own keys
	// are opened one by one (StartObject), the kinds are the fixed CLW_ForceKind names. A pin that cannot be read is logged and ignored.
	static void ParsePins(notnull array<string> factionKeys)
	{
		s_mPins.Clear();
		if (s_sReply.IsEmpty())
			return;

		int pinned;
		foreach (string factionKey : factionKeys)
		{
			if (factionKey.IsEmpty())
				continue;

			SCR_JsonLoadContext ctx = new SCR_JsonLoadContext();
			if (!ctx.ImportFromString(s_sReply))
				return;

			if (!ctx.StartObject("pins"))
				return;	// no pins in the reply

			if (!ctx.StartObject(factionKey))
				continue;

			array<string> kinds = {CLW_ForceKind.INFANTRY, CLW_ForceKind.RECON, CLW_ForceKind.MOTORIZED, CLW_ForceKind.LIGHT, CLW_ForceKind.MECH, CLW_ForceKind.ARMOR, CLW_ForceKind.ARMORED_RECON, CLW_ForceKind.LOGISTICS};
			foreach (string kind : kinds)
			{
				array<string> ids = {};
				if (ctx.ReadValue(kind, ids) && !ids.IsEmpty())
				{
					s_mPins.Set(factionKey + "|" + kind, ids);
					pinned++;
				}
			}
		}

		Print("CLW: loadouts library pins: " + pinned.ToString() + " pinned kind lists for the war factions", LogLevel.NORMAL);
	}

	// ---- queries ----

	static CLW_LibraryEntry Find(string id)
	{
		if (id.IsEmpty())
			return null;

		foreach (CLW_LibraryEntry entry : s_aEntries)
		{
			if (entry.id == id)
				return entry;
		}

		return null;
	}

	static bool IsUsable(CLW_LibraryEntry entry)
	{
		return entry && !entry.loadout.IsEmpty() && (entry.status == STATUS_OK || entry.status == STATUS_DEGRADED);
	}

	// The active entry of the faction for spawn slot 1..7 (status ok / degraded), null when there is none.
	static CLW_LibraryEntry SlotEntry(string factionKey, int slot)
	{
		foreach (CLW_LibraryEntry entry : s_aEntries)
		{
			if (entry.active && entry.order == slot && entry.faction == factionKey && IsUsable(entry))
				return entry;
		}

		return null;
	}

	// ---- validation (R6) ----

	// Status of every entry: inactive (faction not in this war), else CLW_Kits.Validate against the faction (ok | degraded | invalid) plus the
	// missing prefabs. Logs one line per entry that is not ok.
	static void Validate(notnull array<string> warFactionKeys)
	{
		foreach (CLW_LibraryEntry entry : s_aEntries)
		{
			entry.missing.Clear();
			if (!warFactionKeys.Contains(entry.faction))
			{
				entry.status = STATUS_INACTIVE;
				continue;
			}

			array<string> missing = {};
			entry.status = CLW_Kits.Validate(entry.loadout, entry.faction, missing);
			foreach (string item : missing)
			{
				entry.missing.Insert(item);
			}

			if (entry.status != STATUS_OK)
				Print("CLW: loadouts validate entry " + entry.id + " '" + entry.name + "' faction=" + entry.faction + " " + entry.status + " missing=" + missing.Count().ToString(), LogLevel.WARNING);
		}
	}

	// ---- contract API (G1) ----

	// Kit for an AI member of role R (ROLE_* label name) in a squad of kind 'kind' of faction 'factionKey'; slot = member index (0 = leader)
	// for pinned lists. Pinned kits win, then active library kits with role R (random among several, status ok / degraded only).
	// "" when none (the caller falls back to the R2 role prefab).
	static string PickKitForRole(string factionKey, string kind, string role, int slot)
	{
		array<string> pins;
		if (s_mPins.Find(factionKey + "|" + kind, pins) && pins && !pins.IsEmpty())
		{
			int index = Math.Clamp(slot, 0, pins.Count() - 1);	// shorter list than the squad: the last pin repeats
			CLW_LibraryEntry pinned = Find(pins[index]);
			if (pinned && pinned.faction == factionKey && IsUsable(pinned))
				return pinned.loadout;
		}

		array<CLW_LibraryEntry> matches = {};
		foreach (CLW_LibraryEntry entry : s_aEntries)
		{
			if (entry.active && entry.faction == factionKey && entry.role == role && IsUsable(entry))
				matches.Insert(entry);
		}

		if (matches.IsEmpty())
			return "";

		CLW_LibraryEntry chosen = matches.GetRandomElement();
		return chosen.loadout;
	}

	// Composition override from the dashboard for a kind: fills 'roles' (ROLE_* names, replaces its content) and returns true; false when the
	// sidecar set none for the kind.
	static bool CompositionOverride(string kind, notnull array<string> roles)
	{
		if (!s_Compositions)
			return false;

		array<string> list = s_Compositions.Get(kind);
		if (!list || list.IsEmpty())
			return false;

		roles.Clear();
		foreach (string role : list)
		{
			if (!role.IsEmpty())
				roles.Insert(role);
		}

		return !roles.IsEmpty();
	}
}

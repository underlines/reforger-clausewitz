// Player custom loadouts (loadouts work package G2, R1). One saved kit per player per faction, keyed by identity (BackendApi identity id,
// fallback "n:<name>", the same key as CLW_PlayerLang) + faction key. Game-owned file: $profile:Clausewitz/player-loadouts.json
//   {"v":1,"records":[{identityId, playerName, faction, loadout, basePrefab, items:[prefab], addons:[guid], gameVersion, savedAt, status, missing:[prefab]}, ...]}
// Loaded once on first use, written on every change. The records are also what the sidecar gets (POST /loadouts/player, see
// CLW_ArsenalSaveHook.c) and what is put back into the vanilla saved-loadout slot when the player picks a faction again: the vanilla store
// (SCR_ArsenalManagerComponent.m_aPlayerLoadouts) lives in memory only unless the persistence system is on, so it is empty after a restart
// and holds ONE loadout per player, not one per faction.
// The JSON is built with '+' only (a kit string can exceed 8191 characters, see reforger-script-dev) and parsed with SCR_JsonLoadContext.
// Log prefix "CLW: loadouts".

// One player's saved kit for one faction (also the JSON record and the DTO the file is parsed into).
class CLW_CustomLoadoutRec
{
	string identityId;
	string playerName;
	string faction;
	string loadout;
	string basePrefab;
	ref array<string> items = {};
	ref array<string> addons = {};
	string gameVersion;
	int savedAt;
	string status;
	ref array<string> missing = {};
}

class CLW_CustomLoadoutFile
{
	int v;
	ref array<ref CLW_CustomLoadoutRec> records = {};
}

class CLW_CustomLoadouts
{
	static const string FILE_PATH = "$profile:Clausewitz/player-loadouts.json";
	static const string FILE_BAD = "$profile:Clausewitz/player-loadouts.json.bad";

	protected static ref map<string, ref CLW_CustomLoadoutRec> s_mRecords = new map<string, ref CLW_CustomLoadoutRec>();
	protected static bool s_bLoaded;

	// ---- contract API ----

	// Identity key of a connected player (BackendApi id, fallback "n:<name>"), "" when unknown.
	static string IdentityOf(int playerId)
	{
		return CLW_PlayerLang.KeyOf(playerId);
	}

	// The saved kit of this player for this faction, null if none.
	static CLW_CustomLoadoutRec Get(string identityId, string factionKey)
	{
		EnsureLoaded();
		CLW_CustomLoadoutRec rec;
		if (s_mRecords.Find(KeyOf(identityId, factionKey), rec))
			return rec;

		return null;
	}

	// Every record (for the sidecar sync).
	static void All(notnull array<ref CLW_CustomLoadoutRec> outList)
	{
		EnsureLoaded();
		for (int i = 0; i < s_mRecords.Count(); i++)
		{
			outList.Insert(s_mRecords.GetElement(i));
		}
	}

	// ---- more API for the save hook / the load action ----

	// The newest record of a player over all factions, null if none.
	static CLW_CustomLoadoutRec LatestOf(string identityId)
	{
		EnsureLoaded();
		CLW_CustomLoadoutRec best;
		for (int i = 0; i < s_mRecords.Count(); i++)
		{
			CLW_CustomLoadoutRec rec = s_mRecords.GetElement(i);
			if (rec.identityId != identityId)
				continue;

			if (!best || rec.savedAt >= best.savedAt)
				best = rec;
		}

		return best;
	}

	// Stores (or replaces) the kit of a player for a faction, validates it, writes the file. Returns the record.
	static CLW_CustomLoadoutRec Store(int playerId, string factionKey, string loadout)
	{
		EnsureLoaded();
		string identity = IdentityOf(playerId);
		if (identity.IsEmpty() || factionKey.IsEmpty() || loadout.IsEmpty())
		{
			Print("CLW: loadouts not stored for player " + playerId + " (identity '" + identity + "', faction '" + factionKey + "', kit " + loadout.Length() + " chars)", LogLevel.WARNING);
			return null;
		}

		CLW_CustomLoadoutRec rec = new CLW_CustomLoadoutRec();
		rec.identityId = identity;
		rec.faction = factionKey;
		rec.loadout = loadout;
		rec.basePrefab = CLW_Kits.BasePrefab(loadout);
		CLW_Kits.Items(loadout, rec.items);
		GameProject.GetLoadedAddons(rec.addons);
		rec.gameVersion = GetGame().GetBuildVersion();
		rec.savedAt = System.GetUnixTime();
		rec.status = CLW_Kits.Validate(loadout, factionKey, rec.missing);

		PlayerManager players = GetGame().GetPlayerManager();
		if (players)
			rec.playerName = players.GetPlayerName(playerId);

		s_mRecords.Set(KeyOf(identity, factionKey), rec);
		Save();
		return rec;
	}

	// Memory only is reset (a Play restart keeps statics); the file stays.
	static void Reset()
	{
		s_mRecords.Clear();
		s_bLoaded = false;
		CLW_ItemInfo.ClearCache();
	}

	// One record as a JSON object for the sidecar (POST body and sync): the file form plus "totalCost" and "itemInfo" (CLW_ItemInfo, cached;
	// both optional for the receiver). '+' only.
	static string RecordJson(notnull CLW_CustomLoadoutRec rec)
	{
		string json = RecordFileJson(rec);
		string info = CLW_ItemInfo.JsonFor(rec);
		if (info.IsEmpty())
			return json;

		return CLW_Str.DropLast(json) + info + "}";
	}

	// One record as the JSON object of the game file (the DTO shape, no item details). '+' only.
	static string RecordFileJson(notnull CLW_CustomLoadoutRec rec)
	{
		string json = "{\"identityId\":\"" + Esc(rec.identityId) + "\",\"playerName\":\"" + Esc(rec.playerName) + "\",\"faction\":\"" + Esc(rec.faction) + "\"";
		json += ",\"loadout\":\"" + Esc(rec.loadout) + "\"";
		json += ",\"basePrefab\":\"" + Esc(rec.basePrefab) + "\"";
		json += ",\"items\":" + StringArrayJson(rec.items);
		json += ",\"addons\":" + StringArrayJson(rec.addons);
		json += ",\"gameVersion\":\"" + Esc(rec.gameVersion) + "\"";
		json += ",\"savedAt\":" + rec.savedAt.ToString();
		json += ",\"status\":\"" + Esc(rec.status) + "\"";
		json += ",\"missing\":" + StringArrayJson(rec.missing) + "}";
		return json;
	}

	// ---- helpers ----

	protected static string KeyOf(string identityId, string factionKey)
	{
		return identityId + "|" + factionKey;
	}

	// JSON string escape for any length (chunked: the engine string functions are limited to 8191 characters).
	static string Esc(string text)
	{
		string result = "";
		int len = text.Length();
		int pos = 0;
		while (pos < len)
		{
			int count = Math.Min(4000, len - pos);
			string part = text.Substring(pos, count);
			part.Replace("\\", "\\\\");
			part.Replace("\"", "\\\"");
			part.Replace("\n", "\\n");
			part.Replace("\r", "\\r");
			part.Replace("\t", "\\t");
			result += part;
			pos += count;
		}

		return result;
	}

	protected static string StringArrayJson(notnull array<string> list)
	{
		string json = "[";
		for (int i = 0; i < list.Count(); i++)
		{
			if (i > 0)
				json += ",";

			json += "\"" + Esc(list[i]) + "\"";
		}

		return json + "]";
	}

	// ---- persistence ----

	protected static void EnsureLoaded()
	{
		if (s_bLoaded)
			return;

		s_bLoaded = true;
		s_mRecords.Clear();
		if (!FileIO.FileExists(FILE_PATH))
			return;

		string text = SCR_FileIOHelper.GetFileStringContent(FILE_PATH, false);
		if (text.IsEmpty())
			return;

		SCR_JsonLoadContext loader = new SCR_JsonLoadContext();
		CLW_CustomLoadoutFile file = new CLW_CustomLoadoutFile();
		if (!loader.ImportFromString(text) || !loader.ReadValue("", file))
		{
			// keep the unreadable file: the next save would overwrite it
			FileIO.CopyFile(FILE_PATH, FILE_BAD);
			Print("CLW: ERROR loadouts " + FILE_PATH + " is not valid JSON of the expected shape, starting empty (copy kept as " + FILE_BAD + ")", LogLevel.ERROR);
			return;
		}

		foreach (CLW_CustomLoadoutRec rec : file.records)
		{
			if (!rec || rec.identityId.IsEmpty() || rec.faction.IsEmpty() || rec.loadout.IsEmpty())
				continue;

			s_mRecords.Set(KeyOf(rec.identityId, rec.faction), rec);
		}

		Print("CLW: loadouts loaded " + s_mRecords.Count() + " custom loadouts from " + FILE_PATH, LogLevel.NORMAL);
	}

	protected static void Save()
	{
		FileIO.MakeDirectory(CLW_CatalogOverrides.FILE_DIR);
		FileHandle handle = FileIO.OpenFile(FILE_PATH, FileMode.WRITE);
		if (!handle)
		{
			Print("CLW: ERROR loadouts cannot write " + FILE_PATH, LogLevel.ERROR);
			return;
		}

		handle.WriteLine("{\"v\":1,\"records\":[");
		for (int i = 0; i < s_mRecords.Count(); i++)
		{
			string line = RecordFileJson(s_mRecords.GetElement(i));
			if (i < s_mRecords.Count() - 1)
				line += ",";

			handle.WriteLine(line);
		}

		handle.WriteLine("]}");
		handle.Close();
	}
}

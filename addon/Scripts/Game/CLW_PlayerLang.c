// Player language store (server side). Design: dev/notes/i18n-design.md section 3, API research: dev/notes/i18n-language-api.md.
// Resolve(playerId): player override (/hq language, HQ menu, setLanguage from the LLM) > language the client reported on join
// (WidgetManager.GetLanguage, see CLW_LangPlayerController.c) > server default (overrides.json "language") > "en".
// Players are keyed by their persistent identity (BackendApi identity id; fallback "n:<player name>" when the backend gives none, e.g. offline
// tests). Overrides persist in $profile:Clausewitz/player_lang.json and survive reconnects and restarts. Log lines: "CLW: lang ...".
class CLW_PlayerLang
{
	static const string DEFAULT_CODE = "en";
	static const string AUTO = "auto";
	static const string LANG_FILE = "$profile:Clausewitz/player_lang.json";
	static const string LETTERS = "abcdefghijklmnopqrstuvwxyz";

	protected static ref map<string, string> s_mOverride = new map<string, string>();	// identity key -> code (persisted)
	protected static ref map<string, string> s_mReported = new map<string, string>();	// identity key -> client game language (this run only)
	protected static bool s_bLoaded;
	protected static bool s_bDefaultRead;
	protected static string s_sServerDefault = DEFAULT_CODE;

	// ---- code handling ----

	// Normalise user/LLM/game input to a 2-letter lower-case ISO 639-1 code. "auto" stays "auto". "" = invalid.
	// Game locale spellings "de_DE" / "en-US" are cut to their language part.
	static string Normalize(string input)
	{
		string code = input;
		code = code.Trim();
		code.ToLower();
		if (code == AUTO)
			return AUTO;

		int len = code.Length();
		if (len > 2)
		{
			string sep = code.Get(2);
			if (sep == "_" || sep == "-")
			{
				code = code.Substring(0, 2);
				len = 2;
			}
		}

		if (len != 2)
			return "";

		int i = 0;
		while (i < 2)
		{
			if (LETTERS.IndexOf(code.Get(i)) < 0)
				return "";

			i++;
		}

		return code;
	}

	// true for a code that SetOverride accepts (2 letters, case-insensitive, or "auto").
	static bool IsValidCode(string code)
	{
		return Normalize(code) != "";
	}

	// ---- identity ----

	static string KeyOf(int playerId)
	{
		string key;
		BackendApi backend = GetGame().GetBackendApi();
		if (backend)
			key = backend.GetPlayerIdentityId(playerId);

		if (!key.IsEmpty())
			return key;

		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return "";

		string name = players.GetPlayerName(playerId);
		if (name.IsEmpty())
			return "";

		name.Replace("\"", "");
		name.Replace("\\", "");
		return "n:" + name;
	}

	// ---- resolution ----

	// The language code for this player (never empty).
	static string Resolve(int playerId)
	{
		EnsureLoaded();
		string key = KeyOf(playerId);
		if (!key.IsEmpty())
		{
			string code;
			if (s_mOverride.Find(key, code))
				return code;

			if (s_mReported.Find(key, code))
				return code;
		}

		return ServerDefault();
	}

	// Server default: overrides.json "language", else "en".
	static string ServerDefault()
	{
		if (!s_bDefaultRead)
			ReadServerDefault();

		return s_sServerDefault;
	}

	// Distinct language codes of the given players (for the sidecar langs[]). "en" is always included (canonical text), first.
	// langs is cleared and filled; created when null.
	static void GetLangsOfRecipients(array<int> playerIds, out array<string> langs)
	{
		if (!langs)
			langs = new array<string>();
		else
			langs.Clear();

		langs.Insert(DEFAULT_CODE);
		if (!playerIds)
			return;

		foreach (int playerId : playerIds)
		{
			string code = Resolve(playerId);
			if (langs.Find(code) < 0)
				langs.Insert(code);
		}
	}

	// ---- overrides ----

	// Set (or with "auto" clear) the override. false = invalid code, nothing changed.
	static bool SetOverride(int playerId, string code)
	{
		string norm = Normalize(code);
		if (norm.IsEmpty())
		{
			Print("CLW: lang override rejected for player " + playerId + ": '" + code + "' is not an ISO 639-1 code", LogLevel.NORMAL);
			return false;
		}

		if (norm == AUTO)
		{
			ClearOverride(playerId);
			return true;
		}

		EnsureLoaded();
		string key = KeyOf(playerId);
		if (key.IsEmpty())
		{
			Print("CLW: lang override for player " + playerId + " not stored (no identity)", LogLevel.WARNING);
			return false;
		}

		s_mOverride.Set(key, norm);
		Print("CLW: lang override player " + playerId + " (" + key + ") = " + norm, LogLevel.NORMAL);
		Save();
		return true;
	}

	static void ClearOverride(int playerId)
	{
		EnsureLoaded();
		string key = KeyOf(playerId);
		if (key.IsEmpty())
			return;

		if (s_mOverride.Contains(key))
		{
			s_mOverride.Remove(key);
			Save();
		}

		Print("CLW: lang override cleared player " + playerId + " (" + key + "), now " + Resolve(playerId), LogLevel.NORMAL);
	}

	// The stored override code of the player ("" = none, the language is automatic). Never "auto".
	static string GetOverride(int playerId)
	{
		EnsureLoaded();
		string key = KeyOf(playerId);
		if (key.IsEmpty())
			return "";

		string code;
		if (s_mOverride.Find(key, code))
			return code;

		return "";
	}

	// True when the player has a stored override (the menu can show "auto" otherwise).
	static bool HasOverride(int playerId)
	{
		EnsureLoaded();
		string key = KeyOf(playerId);
		return !key.IsEmpty() && s_mOverride.Contains(key);
	}

	// ---- client report (server side of the join RPC) ----

	static void OnClientReport(int playerId, string raw)
	{
		string code = Normalize(raw);
		if (code.IsEmpty() || code == AUTO)
		{
			Print("CLW: lang report from player " + playerId + " ignored, unusable value '" + raw + "'", LogLevel.NORMAL);
			return;
		}

		string key = KeyOf(playerId);
		if (key.IsEmpty())
		{
			Print("CLW: lang report from player " + playerId + " ignored (no identity yet)", LogLevel.WARNING);
			return;
		}

		s_mReported.Set(key, code);
		Print("CLW: lang report player " + playerId + " (" + key + ") raw='" + raw + "' -> " + code + ", resolves to " + Resolve(playerId), LogLevel.NORMAL);
	}

	// ---- persistence ----

	// Game start: statics survive a Play restart.
	static void Reset()
	{
		s_bLoaded = false;
		s_bDefaultRead = false;
		s_sServerDefault = DEFAULT_CODE;
		s_mOverride.Clear();
		s_mReported.Clear();
	}

	protected static void ReadServerDefault()
	{
		s_bDefaultRead = true;
		s_sServerDefault = DEFAULT_CODE;
		if (!FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
			return;

		string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
		if (text.IsEmpty())
			return;

		string error;
		CLW_JsonNode root = CLW_JsonReader.Parse(text, error);
		if (!root)
			return;

		string raw = root.FieldText("language");
		if (raw.IsEmpty())
			return;

		string code = Normalize(raw);
		if (code.IsEmpty() || code == AUTO)
		{
			Print("CLW: lang overrides.json \"language\" '" + raw + "' is not an ISO 639-1 code, using en", LogLevel.WARNING);
			return;
		}

		s_sServerDefault = code;
		Print("CLW: lang server default from overrides.json: " + code, LogLevel.NORMAL);
	}

	protected static void EnsureLoaded()
	{
		if (s_bLoaded)
			return;

		s_bLoaded = true;
		s_mOverride.Clear();
		if (!FileIO.FileExists(LANG_FILE))
			return;

		string text = SCR_FileIOHelper.GetFileStringContent(LANG_FILE, false);
		if (text.IsEmpty())
			return;

		string error;
		CLW_JsonNode root = CLW_JsonReader.Parse(text, error);
		if (!root)
		{
			Print("CLW: ERROR lang " + LANG_FILE + " is not valid JSON (" + error + "), overrides not loaded", LogLevel.ERROR);
			return;
		}

		CLW_JsonNode players = root.Field("players");
		if (!players || !players.IsObject())
			return;

		int i = 0;
		while (i < players.m_aKeys.Count())
		{
			CLW_JsonNode value = players.m_aValues[i];
			string code;
			if (value)
				code = Normalize(value.m_sText);

			if (!code.IsEmpty() && code != AUTO)
				s_mOverride.Set(players.m_aKeys[i], code);

			i++;
		}

		Print("CLW: lang loaded " + s_mOverride.Count() + " player overrides from " + LANG_FILE, LogLevel.NORMAL);
	}

	protected static void Save()
	{
		FileIO.MakeDirectory(CLW_CatalogOverrides.FILE_DIR);
		FileHandle handle = FileIO.OpenFile(LANG_FILE, FileMode.WRITE);
		if (!handle)
		{
			Print("CLW: ERROR lang cannot write " + LANG_FILE, LogLevel.ERROR);
			return;
		}

		string body = "{\"players\":{";
		for (int i = 0; i < s_mOverride.Count(); i++)
		{
			if (i > 0)
				body += ",";

			body += "\"" + s_mOverride.GetKey(i) + "\":\"" + s_mOverride.GetElement(i) + "\"";
		}

		body += "}}";
		handle.WriteLine(body);
		handle.Close();
	}
}

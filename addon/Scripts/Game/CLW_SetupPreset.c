// War setup preset from the server file $profile:Clausewitz/overrides.json (top-level "setup", flat object), so a dedicated server
// starts the war unattended with chosen settings instead of waiting for a human in the setup menu (or the scenario's auto start
// with defaults). Same keys and values as the menu / chat "/war start key=value":
//   {"setup":{"autoStartSec":10,"mode":"short","factionA":"RHS_USAF","factionB":"RHS_AFRF","startPositions":"random","battleSize":"large"}}
//   autoStartSec : seconds after game start to start the war with the preset (default 10, minimum 5); -1 = no auto start, the preset
//                  only pre-fills the setup menu. Replaces the scenario's m_iAutoStartSec.
//   mode         : war mode id (missing = scenario default mode)
//   factionA/B   : faction keys of side A / B (missing = scenario sides)
//   any other key: a parameter of that mode (missing = mode default), e.g. aiDifficultyA / aiDifficultyB (easy | normal | hard | veteran;
//                  aiDifficulty sets both), randomSeed (hidden in the setup dialog, a layout seed > 0 for admins; 0 = automatic)
// Details: dev/notes/overrides.md
class CLW_SetupPreset
{
	static const string KEY = "setup";
	static const string KEY_AUTO_START = "autoStartSec";
	static const string KEY_MODE = "mode";
	static const int DEFAULT_DELAY_SEC = 10;
	static const int MIN_DELAY_SEC = 5;

	int m_iAutoStartSec = DEFAULT_DELAY_SEC;	// -1 = menu pre-fill only
	string m_sMode;								// "" = scenario default
	string m_sFactionA;							// "" = scenario side
	string m_sFactionB;
	string m_sValuesJson;						// flat JSON of the mode parameters ("" = defaults)
	string m_sText;								// the raw "setup" object (log)

	// null when the file or its "setup" object is missing or malformed (malformed is logged).
	static CLW_SetupPreset Load()
	{
		if (!FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
			return null;

		string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
		string object = ExtractObject(text, KEY);
		if (object.IsEmpty())
			return null;

		map<string, string> parsed = new map<string, string>();
		string error;
		if (!CLW_ModeParam.ParseFlat(object, parsed, error))
		{
			Print("CLW: ERROR setup preset in overrides.json ignored: " + error + " | " + object, LogLevel.ERROR);
			return null;
		}

		CLW_SetupPreset preset = new CLW_SetupPreset();
		preset.m_sText = object;
		foreach (string key, string value : parsed)
		{
			if (key == KEY_AUTO_START)
			{
				if (!CLW_ModeParam.IsNumber(value))
				{
					Print("CLW: ERROR setup preset in overrides.json ignored: autoStartSec must be a number, got '" + value + "'", LogLevel.ERROR);
					return null;
				}

				preset.m_iAutoStartSec = value.ToInt();
				if (preset.m_iAutoStartSec >= 0 && preset.m_iAutoStartSec < MIN_DELAY_SEC)
					preset.m_iAutoStartSec = MIN_DELAY_SEC;		// let the world, factions and catalogs settle first
			}
			else if (key == KEY_MODE)
				preset.m_sMode = value;
			else if (key == CLW_SetupServer.PARAM_FACTION_A)
				preset.m_sFactionA = value;
			else if (key == CLW_SetupServer.PARAM_FACTION_B)
				preset.m_sFactionB = value;
			else
			{
				// string values like the chat command: SetFromText parses numbers, bools and choices from text
				if (!preset.m_sValuesJson.IsEmpty())
					preset.m_sValuesJson += ",";
				if (key == CLW_AiDifficulty.PARAM_BOTH)		// "aiDifficulty":"hard" = both sides
					preset.m_sValuesJson += "\"" + CLW_AiDifficulty.PARAM_A + "\":\"" + CLW_ModeParam.Escape(value) + "\",\"" + CLW_AiDifficulty.PARAM_B + "\":\"" + CLW_ModeParam.Escape(value) + "\"";
				else
					preset.m_sValuesJson += "\"" + CLW_ModeParam.Escape(key) + "\":\"" + CLW_ModeParam.Escape(value) + "\"";
			}
		}

		if (!preset.m_sValuesJson.IsEmpty())
			preset.m_sValuesJson = "{" + preset.m_sValuesJson + "}";

		Print(string.Format("CLW: setup preset from overrides.json: autoStartSec=%1 mode='%2' factionA='%3' factionB='%4' values=%5", preset.m_iAutoStartSec, preset.m_sMode, preset.m_sFactionA, preset.m_sFactionB, preset.m_sValuesJson), LogLevel.NORMAL);
		return preset;
	}

	bool IsAutoStart()
	{
		return m_iAutoStartSec >= 0;
	}

	// The {...} value of a top-level-ish "key": the first occurrence of "key" followed by ':' and an object. "" when absent.
	// Braces inside strings are skipped. The object is small (a handful of keys), far below the 8191-char Substring cap.
	protected static string ExtractObject(string text, string key)
	{
		string needle = "\"" + key + "\"";
		int at = text.IndexOf(needle);
		if (at < 0)
			return "";

		int n = text.Length();
		int i = at + needle.Length();
		while (i < n && (text.Get(i) == " " || text.Get(i) == "\t" || text.Get(i) == "\r" || text.Get(i) == "\n" || text.Get(i) == ":"))
		{
			i++;
		}

		if (i >= n || text.Get(i) != "{")
		{
			Print("CLW: ERROR setup preset in overrides.json ignored: \"" + key + "\" must be an object", LogLevel.ERROR);
			return "";
		}

		int start = i;
		int depth;
		bool inString;
		while (i < n)
		{
			string c = text.Get(i);
			if (inString)
			{
				if (c == "\\")
					i++;
				else if (c == "\"")
					inString = false;
			}
			else if (c == "\"")
				inString = true;
			else if (c == "{")
				depth++;
			else if (c == "}")
			{
				depth--;
				if (depth == 0)
					return text.Substring(start, i - start + 1);
			}
			i++;
		}

		Print("CLW: ERROR setup preset in overrides.json ignored: unterminated \"" + key + "\" object", LogLevel.ERROR);
		return "";
	}
}

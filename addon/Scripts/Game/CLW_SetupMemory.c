// The last accepted war setup, remembered server side (M4.6): after a win/loss restart (CLW_Rematch) or a server restart the setup screen shows
// again, prefilled with the previous round's values. File $profile:Clausewitz/lastSetup.json, a flat object of string values:
//   {"mode":"short","factionA":"US","factionB":"USSR","battleSize":"large","timeLimitMin":"90",...}
// Same keys and values as the "setup" object of overrides.json / "/war start key=value". Hidden parameters (the random seed) are NEVER stored:
// a restart must not repeat the previous layout by accident. An overrides.json preset that starts the war unattended wins over this record
// (CLW_SetupServer decides). The "Reset to defaults" button of the dialog ignores it (defaults = overrides.json preset or built-in).
// Delete the file to forget the remembered setup. Log prefix: "CLW: setup memory".
class CLW_SetupMemory
{
	static const string FILE_PATH = "$profile:Clausewitz/lastSetup.json";
	static const string KEY_MODE = "mode";
	static const string KEY_SEED = "randomSeed";

	protected static bool s_bLoaded;
	protected static ref map<string, string> s_mValues = new map<string, string>();

	// True when a record exists (memory or file).
	static bool Has()
	{
		Load();
		return s_mValues.Count() > 0;
	}

	// Raw record (key -> value text). Read only.
	static map<string, string> Values()
	{
		Load();
		return s_mValues;
	}

	protected static void Load()
	{
		if (s_bLoaded)
			return;

		s_bLoaded = true;
		s_mValues.Clear();
		if (!FileIO.FileExists(FILE_PATH))
			return;

		string text = SCR_FileIOHelper.GetFileStringContent(FILE_PATH, false);
		map<string, string> parsed = new map<string, string>();
		string error;
		if (text.IsEmpty() || !CLW_ModeParam.ParseFlat(text, parsed, error))
		{
			Print("CLW: setup memory: " + FILE_PATH + " ignored: " + error, LogLevel.WARNING);
			return;
		}

		foreach (string key, string value : parsed)
		{
			if (key == KEY_SEED)
				continue;		// a seed never carries over

			s_mValues.Set(key, value);
		}

		Print("CLW: setup memory: loaded " + s_mValues.Count() + " values from " + FILE_PATH, LogLevel.NORMAL);
	}

	// Remembers an accepted setup: the mode's current (validated) values minus the hidden ones, plus the sides.
	static void Save(CLW_WarMode mode, string factionA, string factionB)
	{
		if (!mode)
			return;

		Load();
		s_mValues.Clear();
		s_mValues.Set(KEY_MODE, mode.GetId());
		s_mValues.Set(CLW_SetupServer.PARAM_FACTION_A, factionA);
		s_mValues.Set(CLW_SetupServer.PARAM_FACTION_B, factionB);
		array<ref CLW_ModeParam> params = {};
		mode.GetParams(params);
		foreach (CLW_ModeParam param : params)
		{
			if (param.m_bHidden)
				continue;

			s_mValues.Set(param.m_sId, param.ValueText());
		}

		string body = "{";
		int n;
		foreach (string key, string value : s_mValues)
		{
			if (n > 0)
				body += ",";

			body += "\"" + CLW_ModeParam.Escape(key) + "\":\"" + CLW_ModeParam.Escape(value) + "\"";
			n++;
		}

		body += "}";
		FileIO.MakeDirectory(CLW_CatalogOverrides.FILE_DIR);
		FileHandle handle = FileIO.OpenFile(FILE_PATH, FileMode.WRITE);
		if (!handle)
		{
			Print("CLW: setup memory: cannot write " + FILE_PATH + " (kept in memory for this server run)", LogLevel.WARNING);
			return;
		}

		handle.WriteLine(body);
		handle.Close();
		Print("CLW: setup memory: saved the accepted setup (" + n + " values, no seed) to " + FILE_PATH, LogLevel.NORMAL);
	}
}

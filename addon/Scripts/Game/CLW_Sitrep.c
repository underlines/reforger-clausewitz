// Faction SITREP data + template (design: dev/notes/sitrep-design.md). Pure data, no engine access besides string helpers.
//
// Template (modelled on the US/NATO SITREP, the SMEAC five-paragraph order and the SALUTE enemy report; kept short):
//   headline   one line: faction, war time, tickets, objectives, unit/contact counts
//   situation  S: where the war stands (objectives held / contested / lost, tickets, time)
//   enemy      E: "Intel" in game, known enemy in words (size, area, grid, age); only what this faction has seen
//   mission    M: HQ intent and main effort
//   execution  X: what our units are doing now / next, and what players can do
//   logistics  L: supplies and income, force cap, weak or wounded units
// Scope: "faction" today. A squad SITREP uses the SAME sections with scope "squad" and unit = squad id (own strength, local
// contacts, current task, next step, ammo), so the shape, the wire format and the journal mapping need no change.
class CLW_SitrepTemplate
{
	static const int SECTION_COUNT = 5;
	static const int MAX_SECTION_CHARS = 420;
	static const int MAX_HEADLINE_CHARS = 140;

	static string Id(int index)
	{
		switch (index)
		{
			case 0: return "situation";
			case 1: return "enemy";
			case 2: return "mission";
			case 3: return "execution";
			case 4: return "logistics";
		}

		return "";
	}

	static string Title(int index)
	{
		switch (index)
		{
			case 0: return "Situation";
			case 1: return "Intel";
			case 2: return "Mission";
			case 3: return "Execution";
			case 4: return "Logistics";
		}

		return "";
	}

	static int IndexOf(string id)
	{
		for (int i = 0; i < SECTION_COUNT; i++)
		{
			if (Id(i) == id)
				return i;
		}

		return -1;
	}
}

class CLW_Sitrep
{
	static const string SOURCE_RULES = "rules";
	static const string SOURCE_LLM = "llm";

	string m_sScope = "faction";	// "faction" | "squad" (later)
	string m_sUnit;					// faction key, or squad id for a squad SITREP
	string m_sSource = "rules";
	float m_fWarTime;				// game time (s) when generated
	string m_sHeadline;
	ref array<string> m_aBodies = {};	// one per CLW_SitrepTemplate section index

	void CLW_Sitrep()
	{
		for (int i = 0; i < CLW_SitrepTemplate.SECTION_COUNT; i++)
		{
			m_aBodies.Insert("");
		}
	}

	void SetBody(int index, string body)
	{
		if (index < 0 || index >= m_aBodies.Count())
			return;

		string text = Clean(body, CLW_SitrepTemplate.MAX_SECTION_CHARS);
		m_aBodies.Set(index, text);
	}

	string GetBody(int index)
	{
		if (index < 0 || index >= m_aBodies.Count())
			return "";

		return m_aBodies[index];
	}

	void SetHeadline(string text)
	{
		m_sHeadline = Clean(text, CLW_SitrepTemplate.MAX_HEADLINE_CHARS);
	}

	bool IsEmpty()
	{
		foreach (string body : m_aBodies)
		{
			if (!body.IsEmpty())
				return false;
		}

		return m_sHeadline.IsEmpty();
	}

	// Content signature without the time stamp: changes only when a section changes (decides if players get a new copy).
	string Signature()
	{
		string text = m_sSource;
		foreach (string body : m_aBodies)
		{
			text += "|" + body;
		}

		return text;
	}

	// Plain text (dashboard "text" field): headline + "TITLE: body" lines.
	string ToText()
	{
		string text = m_sHeadline;
		for (int i = 0; i < m_aBodies.Count(); i++)
		{
			if (m_aBodies[i].IsEmpty())
				continue;

			if (!text.IsEmpty())
				text += "\n";
			text += CLW_SitrepTemplate.Title(i) + ": " + m_aBodies[i];
		}

		return text;
	}

	// JSON object posted to the sidecar (rules SITREP): {"scope","unit","source","warTimeSec","headline","situation",...}
	string ToJson()
	{
		string json = "{\"scope\":\"" + CLW_Situation.Escape(m_sScope) + "\",\"unit\":\"" + CLW_Situation.Escape(m_sUnit) + "\",\"source\":\"" + CLW_Situation.Escape(m_sSource) + "\"";
		int warTime = Math.Round(m_fWarTime);
		json += ",\"warTimeSec\":" + warTime.ToString() + ",\"headline\":\"" + EscapeKeepLines(m_sHeadline) + "\"";
		for (int i = 0; i < m_aBodies.Count(); i++)
		{
			json += ",\"" + CLW_SitrepTemplate.Id(i) + "\":\"" + EscapeKeepLines(m_aBodies[i]) + "\"";
		}

		json += "}";
		return json;
	}

	// JSON string escape that keeps line breaks (as \n).
	protected static string EscapeKeepLines(string text)
	{
		string r = text;
		r.Replace("\\", "\\\\");
		r.Replace("\"", "\\\"");
		r.Replace("\r", "");
		r.Replace("\n", "\\n");
		r.Replace("\t", " ");
		return r;
	}

	// Server -> client wire (see CLW_SitrepStore): tab separated fields, one record per line.
	//   H  faction  source  warTimeSec  version  headline
	//   S  id  title  body                    (body line breaks as <br/>, no tabs)
	string ToWire(string factionKey, int version)
	{
		int warTime = Math.Round(m_fWarTime);
		string wire = "H\t" + factionKey + "\t" + m_sSource + "\t" + warTime.ToString() + "\t" + version.ToString() + "\t" + WireText(m_sHeadline, false);
		for (int i = 0; i < m_aBodies.Count(); i++)
		{
			wire += "\nS\t" + CLW_SitrepTemplate.Id(i) + "\t" + CLW_SitrepTemplate.Title(i) + "\t" + WireText(m_aBodies[i], true);
		}

		return wire;
	}

	protected static string WireText(string text, bool lineBreaks)
	{
		string r = text;
		r.Replace("\t", " ");
		r.Replace("\r", "");
		if (lineBreaks)
			r.Replace("\n", "<br/>");
		else
			r.Replace("\n", " ");
		return r;
	}

	// Normalises text from any source (rules, LLM): no tags / control characters / tabs, trimmed, length-limited. Line breaks stay "\n".
	static string Clean(string text, int maxLength)
	{
		string r = text;
		r.Replace("\r", "");
		r.Replace("\t", " ");
		r.Replace("<", "(");
		r.Replace(">", ")");
		r.Replace("\\n", "\n");	// a model may send a literal backslash-n
		r.Trim();
		if (r.Length() > maxLength)
			r = r.Substring(0, maxLength - 3) + "...";

		return r;
	}

	// Client display text: rich text safe ("%" would be read as a format token by the localisation layer).
	static string DisplaySafe(string text)
	{
		string r = text;
		r.Replace("%", " pct");
		return r;
	}

	// Parses the wire text (client). Returns null when it has no header line.
	static CLW_Sitrep FromWire(string wire, out string factionKey, out int version)
	{
		array<string> lines = {};
		wire.Split("\n", lines, true);
		CLW_Sitrep sitrep;
		foreach (string line : lines)
		{
			array<string> f = {};
			line.Split("\t", f, false);
			if (f.IsEmpty())
				continue;

			if (f[0] == "H" && f.Count() >= 6)
			{
				sitrep = new CLW_Sitrep();
				factionKey = f[1];
				sitrep.m_sUnit = f[1];
				sitrep.m_sSource = f[2];
				sitrep.m_fWarTime = f[3].ToFloat();
				version = f[4].ToInt();
				sitrep.m_sHeadline = f[5];
			}
			else if (f[0] == "S" && f.Count() >= 4 && sitrep)
			{
				int index = CLW_SitrepTemplate.IndexOf(f[1]);
				if (index >= 0)
					sitrep.m_aBodies.Set(index, f[3]);
			}
		}

		return sitrep;
	}

	static string FormatClock(float seconds)
	{
		int total = Math.Max(0, Math.Round(seconds));
		int minutes = total / 60;
		int rest = total - minutes * 60;
		string restText = rest.ToString();
		if (rest < 10)
			restText = "0" + restText;

		return minutes.ToString() + ":" + restText;
	}
}

// JSON shape of the "sitrep" object in the sidecar reply (SCR_JsonLoadContext fills the fields by name).
class CLW_SitrepDto
{
	string headline;
	string situation;
	string enemy;
	string mission;
	string execution;
	string logistics;

	// Returns null when nothing usable arrived.
	CLW_Sitrep ToSitrep(string factionKey, float warTime)
	{
		CLW_Sitrep sitrep = new CLW_Sitrep();
		sitrep.m_sUnit = factionKey;
		sitrep.m_sSource = CLW_Sitrep.SOURCE_LLM;
		sitrep.m_fWarTime = warTime;
		sitrep.SetHeadline(headline);
		sitrep.SetBody(0, situation);
		sitrep.SetBody(1, enemy);
		sitrep.SetBody(2, mission);
		sitrep.SetBody(3, execution);
		sitrep.SetBody(4, logistics);
		if (sitrep.IsEmpty())
			return null;

		return sitrep;
	}
}

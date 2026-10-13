// Localisation of mod text (design: dev/notes/i18n-design.md, how-to: dev/notes/i18n-stringtable-howto.md).
//
// Player-facing text is never sent finished from the server (the server has no client language): the server sends a KEY plus
// PARAMS as one string, the "wire" of a CLW_LocMsg, and the client formats it with the stringtable (Language/CLW.st, keys
// "#CLW-<Area>_<Name>", "%1..%9" are the params). Any existing string RPC / log / popup channel can carry a wire, because
//   * a wire starts with '#', plain text never does,
//   * CLW_Loc.Resolve(text) (used by CLW_Debug.Show, the popup, the hint and the journal code) renders wires and leaves plain
//     text (LLM prose, debug lines) untouched.
//
// Wire format:  #CLW-Key ^ param1 ^ param2 ...   (separator '^'; inside a param '~' '^' and '\n' are escaped as ~t ~c ~n)
// A param that starts with '#' is a key or a nested wire and is translated too (faction names "#AR-Faction_US", a place display
// name, a sentence part): the same rule CLW_Places uses for display names.
// Several messages in one text: one wire per line ("\n"); a line that is not a wire stays as it is.
//
// Never localised: logs / Print, debug chat (CLW_Debug.Say), command names, conf ids, action manifest, LLM text.

// One message: stringtable key + params. Encode() gives the wire for any string RPC; Decode() reads it back.
class CLW_LocMsg
{
	string key;
	ref array<string> params = {};

	static CLW_LocMsg Make(string msgKey, array<string> msgParams = null)
	{
		CLW_LocMsg msg = new CLW_LocMsg();
		msg.key = msgKey;
		if (msgParams)
		{
			foreach (string one : msgParams)
			{
				msg.params.Insert(one);
			}
		}

		return msg;
	}

	string Encode()
	{
		return CLW_Loc.WireOf(key, params);
	}

	// Null when the text is not a wire (plain text).
	static CLW_LocMsg Decode(string wire)
	{
		if (!CLW_Loc.IsWire(wire))
			return null;

		array<string> parts = {};
		wire.Split(CLW_Loc.SEP, parts, false);
		CLW_LocMsg msg = new CLW_LocMsg();
		msg.key = parts[0];
		for (int i = 1; i < parts.Count(); i++)
		{
			msg.params.Insert(CLW_Loc.Unescape(parts[i]));
		}

		return msg;
	}

	// Text in the language of the machine this runs on.
	string Fmt()
	{
		return CLW_Loc.Fmt(key, params);
	}
}

class CLW_Loc
{
	static const string SEP = "^";

	// ---- format (client; on the server the same call gives the server language, English: sidecar / dashboard text) ----

	// Translated + formatted text of a key. Params that start with '#' are rendered first (key or nested wire).
	static string Fmt(string key, array<string> params = null)
	{
		array<string> p = {};
		if (params)
		{
			foreach (string raw : params)
			{
				p.Insert(Resolve(raw));
			}
		}

		while (p.Count() < 9)
		{
			p.Insert("");
		}

		return WidgetManager.Translate(key, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
	}

	// Renders every line of a text: a wire line becomes its translated text, any other line stays. "" stays "".
	static string Resolve(string text)
	{
		if (text.IsEmpty())
			return text;

		if (text.IndexOf("\n") < 0)
		{
			if (!IsWire(text))
				return text;

			return ResolveLine(text);
		}

		array<string> lines = {};
		text.Split("\n", lines, false);
		string result = "";
		for (int i = 0; i < lines.Count(); i++)
		{
			if (i > 0)
				result += "\n";
			result += ResolveLine(lines[i]);
		}

		return result;
	}

	protected static string ResolveLine(string line)
	{
		CLW_LocMsg msg = CLW_LocMsg.Decode(line);
		if (!msg)
			return line;

		return Fmt(msg.key, msg.params);
	}

	// A wire (or a bare key) starts with '#'; plain text from the server, the LLM or the player never does.
	static bool IsWire(string text)
	{
		if (text.Length() < 2 || text.Substring(0, 1) != "#")
			return false;

		string head = text;
		int sep = text.IndexOf(SEP);
		if (sep >= 0)
			head = text.Substring(0, sep);

		return head.IndexOf(" ") < 0;	// "#Heading text" (markdown from an LLM) is not a key
	}

	// ---- build wires (server or client) ----

	// "#CLW-Key" + params. Trailing empty params are dropped. Up to 6 params; use WireOf for more.
	static string W(string key, string p1 = "", string p2 = "", string p3 = "", string p4 = "", string p5 = "", string p6 = "")
	{
		array<string> items = {};
		items.Insert(p1);
		items.Insert(p2);
		items.Insert(p3);
		items.Insert(p4);
		items.Insert(p5);
		items.Insert(p6);
		return WireOf(key, items);
	}

	// Wire built and rendered at once (client code that formats a message of its own): "Rejected: %1".
	static string F(string key, string p1 = "", string p2 = "", string p3 = "", string p4 = "", string p5 = "", string p6 = "")
	{
		return Resolve(W(key, p1, p2, p3, p4, p5, p6));
	}

	static string WireOf(string key, array<string> params)
	{
		int count = params.Count();
		while (count > 0 && params[count - 1].IsEmpty())
		{
			count--;
		}

		string wire = key;
		for (int i = 0; i < count; i++)
		{
			wire += SEP + Escape(params[i]);
		}

		return wire;
	}

	// Several wires / plain lines as one text.
	static string Lines(string a, string b = "", string c = "", string d = "", string e = "")
	{
		string text = a;
		if (!b.IsEmpty())
			text += "\n" + b;
		if (!c.IsEmpty())
			text += "\n" + c;
		if (!d.IsEmpty())
			text += "\n" + d;
		if (!e.IsEmpty())
			text += "\n" + e;
		return text;
	}

	// Display text of a value that is also an id (a choice of a setup parameter): "#CLW-Choice_<param>_<value>" when the table has
	// it, else the value itself. A value that is a key (faction "#AR-Faction_US") is translated.
	static string Choice(string paramId, string value)
	{
		if (IsWire(value) || paramId.StartsWith("faction"))	// faction keys ("US") are shown as they are
			return Resolve(value);

		string key = "#CLW-Choice_" + paramId + "_" + value;
		string text = WidgetManager.Translate(key);
		if (text.IsEmpty() || text == key)
			return value;

		return text;
	}

	// Faction display name as a param: the faction's name key (#AR-Faction_US, a pack string) that the client translates; the key
	// itself when the faction is unknown. Content names always come from the content, never from this mod's table.
	static string FactionName(string factionKey)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (manager)
		{
			Faction faction = manager.GetFactionByKey(factionKey);
			if (faction)
			{
				string name = faction.GetFactionName();
				if (!name.IsEmpty())
					return name;
			}
		}

		return factionKey;
	}

	// Vanilla task text (SCR_Task replicates key + params, every client formats it in its own language). The params of a task
	// text must be flat (plain text or numbers): the task UI does not render nested wires. Plain text (LLM prose) is set as it is.
	static void SetTaskName(SCR_Task task, string text)
	{
		if (!task)
			return;

		CLW_LocMsg msg = CLW_LocMsg.Decode(text);
		if (msg)
			task.SetTaskName(msg.key, msg.params);
		else
			task.SetTaskName(text);
	}

	static void SetTaskDescription(SCR_Task task, string text)
	{
		if (!task)
			return;

		CLW_LocMsg msg = CLW_LocMsg.Decode(text);
		if (msg)
			task.SetTaskDescription(msg.key, msg.params);
		else
			task.SetTaskDescription(text);
	}

	// Number as a param (the stringtable never formats numbers).
	static string N(int value)
	{
		return value.ToString();
	}

	// ---- escaping of params inside a wire ----

	static string Escape(string text)
	{
		string r = text;
		r.Replace("~", "~t");
		r.Replace(SEP, "~c");
		r.Replace("\n", "~n");
		return r;
	}

	static string Unescape(string text)
	{
		string r = text;
		r.Replace("~n", "\n");
		r.Replace("~c", SEP);
		r.Replace("~t", "~");
		return r;
	}
}

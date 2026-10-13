// Wire format v1 of the friendly-units map picture (server encodes, client decodes). See dev/notes/map-friendly-design.md section 3.1.
//   F1|<version>|<factionKey>|<active 0/1>|<switchMask>|<identity>|<count>#<entry>;<entry>;...
//   entry = <t>,<key>,<x5>,<z5>,<h10>,<kind>,<n>,<fl>,<label>
// Free text is sanitised (Clean): the separators | ; , # and control characters become spaces.
class CLW_FriendlyEntry
{
	string m_sType;		// m member | s squad | v vehicle
	string m_sKey;
	int m_iX5;			// world X in 5 m units
	int m_iZ5;			// world Z in 5 m units
	int m_iH10;			// heading in 10 degree steps 0..35
	string m_sKind;
	int m_iCount;
	int m_iFlags;
	string m_sLabel;
}

class CLW_FriendlyPic
{
	static const int VERSION = 1;
	static const int FLAG_HUMAN = 1;
	static const int FLAG_OWN_GROUP = 2;
	static const int FLAG_DAMAGED = 4;
	static const int MAX_LABEL = 24;
	static const int MAX_KEY = 16;
	static const int MAX_KIND = 16;

	string m_sFaction;
	int m_iVersion = VERSION;
	bool m_bActive;
	int m_iMask;
	int m_iIdentity;
	ref array<ref CLW_FriendlyEntry> m_aEntries = {};

	// Sanitised, length-limited text for the wire.
	static string Clean(string text, int maxChars)
	{
		string r = text;
		r.Replace("|", " ");
		r.Replace(";", " ");
		r.Replace(",", " ");
		r.Replace("#", " ");
		r.Replace("\n", " ");
		r.Replace("\r", " ");
		r.Replace("\t", " ");
		r.Trim();
		if (maxChars > 0 && r.Length() > maxChars)
			r = r.Substring(0, maxChars);

		return r;
	}

	protected int ActiveInt()
	{
		if (m_bActive)
			return 1;

		return 0;
	}

	string ToWire()
	{
		string head = string.Format("F1|%1|%2|%3|%4|%5|%6#", m_iVersion, Clean(m_sFaction, MAX_KEY), ActiveInt(), m_iMask, m_iIdentity, m_aEntries.Count());
		string body = "";
		foreach (int i, CLW_FriendlyEntry e : m_aEntries)
		{
			if (!e)
				continue;

			if (i > 0)
				body += ";";

			body += Clean(e.m_sType, 1) + "," + Clean(e.m_sKey, MAX_KEY) + "," + e.m_iX5.ToString() + "," + e.m_iZ5.ToString() + "," + e.m_iH10.ToString() + "," + Clean(e.m_sKind, MAX_KIND) + "," + e.m_iCount.ToString() + "," + e.m_iFlags.ToString() + "," + Clean(e.m_sLabel, MAX_LABEL);
		}

		return head + body;
	}

	// Null when the text is unreadable or has an unknown version.
	static CLW_FriendlyPic FromWire(string wire)
	{
		if (wire.IsEmpty())
			return null;

		int hash = wire.IndexOf("#");
		if (hash < 0)
			return null;

		string head = wire.Substring(0, hash);
		string body = wire.Substring(hash + 1, wire.Length() - hash - 1);
		array<string> h = {};
		head.Split("|", h, false);
		if (h.Count() < 7 || h[0] != "F1")
			return null;

		if (h[1].ToInt() != VERSION)
			return null;

		CLW_FriendlyPic pic = new CLW_FriendlyPic();
		pic.m_iVersion = h[1].ToInt();
		pic.m_sFaction = h[2];
		pic.m_bActive = h[3].ToInt() != 0;
		pic.m_iMask = h[4].ToInt();
		pic.m_iIdentity = h[5].ToInt();
		if (body.IsEmpty())
			return pic;

		array<string> parts = {};
		body.Split(";", parts, true);
		foreach (string part : parts)
		{
			array<string> f = {};
			part.Split(",", f, false);
			if (f.Count() < 8)
				continue;

			CLW_FriendlyEntry e = new CLW_FriendlyEntry();
			e.m_sType = f[0];
			e.m_sKey = f[1];
			e.m_iX5 = f[2].ToInt();
			e.m_iZ5 = f[3].ToInt();
			e.m_iH10 = f[4].ToInt();
			e.m_sKind = f[5];
			e.m_iCount = f[6].ToInt();
			e.m_iFlags = f[7].ToInt();
			if (f.Count() > 8)
				e.m_sLabel = f[8];

			pic.m_aEntries.Insert(e);
		}

		return pic;
	}
}

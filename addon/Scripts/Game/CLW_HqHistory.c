// HQ chat memory: the asking player's last few completed /hq exchanges (his text + HQ's final reply), so the HQ agent can answer follow-ups
// ("yes, that one", "again"). VERY small on purpose: KEEP (default 2) exchanges per player, hard cap MAX_EXCHANGES (3), each text clipped to MAX_TEXT.
// Server only, in memory, never persisted. Cleared on war rematch / new war (CLW_HqChat.Reset) and for players who left (Prune, from the HQ tick).
// Only the asker's own exchanges ride on his hq[] message ("history":[{"p","hq"}], oldest first, current message excluded); never other players' text.
class CLW_HqHistory
{
	static const int KEEP = 2;				// exchanges kept / sent per player (default)
	static const int MAX_EXCHANGES = 3;		// hard cap (also enforced by the sidecar)
	static const int MAX_TEXT = 200;		// chars per text

	// player id -> flat list [p0, hq0, p1, hq1, ...], oldest first
	protected static ref map<int, ref array<string>> s_mByPlayer = new map<int, ref array<string>>();

	static void Reset()
	{
		s_mByPlayer.Clear();
	}

	protected static string Clip(string text)
	{
		string clean = text;
		clean = clean.Trim();
		clean.Replace("\n", " ");
		clean.Replace("\r", " ");
		clean.Replace("\t", " ");
		if (clean.Length() > MAX_TEXT)
			clean = clean.Substring(0, MAX_TEXT);

		return clean;
	}

	// A completed exchange: the player's /hq text and HQ's final reply text. Empty texts are skipped.
	static void Record(int playerId, string playerText, string reply)
	{
		string p = Clip(playerText);
		string hq = Clip(reply);
		if (p.IsEmpty() || hq.IsEmpty())
			return;

		array<string> list;
		if (!s_mByPlayer.Find(playerId, list))
		{
			list = new array<string>();
			s_mByPlayer.Set(playerId, list);
		}

		list.Insert(p);
		list.Insert(hq);
		int maxEntries = Math.Min(KEEP, MAX_EXCHANGES) * 2;
		while (list.Count() > maxEntries)
		{
			list.RemoveOrdered(0);
			list.RemoveOrdered(0);
		}
	}

	// JSON array '[{"p":"..","hq":".."},...]' of this player's own exchanges, "" when none.
	static string GetJson(int playerId)
	{
		array<string> list;
		if (!s_mByPlayer.Find(playerId, list) || list.Count() < 2)
			return "";

		string text = "";
		for (int i = 0; i + 1 < list.Count(); i += 2)
		{
			if (i > 0)
				text += ",";
			text += "{\"p\":\"" + CLW_Situation.Escape(list[i]) + "\",\"hq\":\"" + CLW_Situation.Escape(list[i + 1]) + "\"}";
		}

		return "[" + text + "]";
	}

	// Forget players who are no longer connected (called from the HQ tick).
	static void Prune()
	{
		PlayerManager pm = GetGame().GetPlayerManager();
		if (!pm)
			return;

		array<int> gone = {};
		foreach (int playerId, array<string> list : s_mByPlayer)
		{
			if (!pm.IsPlayerConnected(playerId))
				gone.Insert(playerId);
		}

		foreach (int id : gone)
			s_mByPlayer.Remove(id);
	}
}

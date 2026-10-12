// String helpers that are safe for long strings: string.Substring returns at most 8191 characters
// (verified on the dedicated server, game 1.8.0.13), so a long JSON body must never go through one Substring call.
class CLW_Str
{
	static const int CHUNK = 8000;

	// Substring that works for any length (copies in chunks below the engine limit).
	static string Sub(string s, int start, int len)
	{
		string r = "";
		int done = 0;
		while (done < len)
		{
			int n = Math.Min(CHUNK, len - done);
			r += s.Substring(start + done, n);
			done += n;
		}
		return r;
	}

	// s without its last character (e.g. the closing brace of a JSON object before appending fields).
	static string DropLast(string s)
	{
		if (s.Length() < 1)
			return s;
		return Sub(s, 0, s.Length() - 1);
	}
}

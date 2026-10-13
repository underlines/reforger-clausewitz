// Protocol v4 (i18n): the LLM SITREP of a sidecar reply may carry sitrep.text_by_lang = {"en":{headline,situation,...},"de":{...}} (canonical
// English plus one translation per recipient language, the request's "langs" asked for them). The keys are language codes, so the reply
// DTO cannot name them: this class cuts the per-language objects out of the raw JSON and reads each one as a CLW_SitrepDto.
// Absent / malformed text_by_lang -> null (the single-language v3 behaviour). Server only. Log prefix "CLW: sitrep".
class CLW_SitrepLangs
{
	static const int MAX_LANGS = 12;

	// language code -> SITREP (the LLM one, translated). Null when the reply has no usable text_by_lang.
	static map<string, ref CLW_Sitrep> Parse(string data, string factionKey, float warTime)
	{
		int at = data.IndexOf("\"text_by_lang\"");
		if (at < 0)
			return null;

		int open = data.IndexOfFrom(at + 14, "{");
		if (open < 0)
			return null;

		int close = MatchBrace(data, open);
		if (close < 0)
		{
			Print(string.Format("CLW: sitrep %1 text_by_lang is not valid JSON, ignored", factionKey), LogLevel.WARNING);
			return null;
		}

		map<string, ref CLW_Sitrep> result = new map<string, ref CLW_Sitrep>();
		int i = open + 1;
		while (i < close && result.Count() < MAX_LANGS)
		{
			// next key: "xx"
			int keyStart = data.IndexOfFrom(i, "\"");
			if (keyStart < 0 || keyStart >= close)
				break;

			int keyEnd = data.IndexOfFrom(keyStart + 1, "\"");
			if (keyEnd < 0 || keyEnd >= close)
				break;

			string code = data.Substring(keyStart + 1, keyEnd - keyStart - 1);
			int objStart = data.IndexOfFrom(keyEnd + 1, "{");
			if (objStart < 0 || objStart >= close)
				break;

			int objEnd = MatchBrace(data, objStart);
			if (objEnd < 0 || objEnd > close)
				break;

			i = objEnd + 1;
			code.ToLower();
			if (!CLW_PlayerLang.IsValidCode(code))
				continue;

			SCR_JsonLoadContext loader = new SCR_JsonLoadContext();
			CLW_SitrepDto dto = new CLW_SitrepDto();
			string objText = data.Substring(objStart, objEnd - objStart + 1);
			if (!loader.ImportFromString(objText) || !loader.ReadValue("", dto))
				continue;

			CLW_Sitrep sitrep = dto.ToSitrep(factionKey, warTime);
			if (sitrep)
				result.Set(code, sitrep);
		}

		if (result.IsEmpty())
			return null;

		return result;
	}

	// Index of the "}" that closes the "{" at start; strings (with \" escapes) are skipped. -1 when unbalanced.
	protected static int MatchBrace(string text, int start)
	{
		int depth;
		bool inString;
		int length = text.Length();
		for (int i = start; i < length; i++)
		{
			string ch = text.Get(i);
			if (inString)
			{
				if (ch == "\\")
					i++;	// skip the escaped character
				else if (ch == "\"")
					inString = false;

				continue;
			}

			if (ch == "\"")
			{
				inString = true;
			}
			else if (ch == "{")
			{
				depth++;
			}
			else if (ch == "}")
			{
				depth--;
				if (depth == 0)
					return i;
			}
		}

		return -1;
	}
}

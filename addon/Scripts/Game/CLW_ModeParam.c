// One configurable parameter of a war mode (generic, UI-renderable). Values travel as compact JSON:
// definitions -> ToJson(), chosen values <- flat object {"timeLimitMin":60,"battleSize":"large","ticketBleed":true}.
class CLW_ModeParam
{
	static const int TYPE_INT = 0;
	static const int TYPE_FLOAT = 1;
	static const int TYPE_BOOL = 2;
	static const int TYPE_CHOICE = 3;

	string m_sId;
	string m_sLabel;
	string m_sDescription;
	int m_eType;
	float m_fMin;
	float m_fMax;
	float m_fStep = 1;
	float m_fValue;				// INT/FLOAT value; BOOL 0/1; CHOICE index into m_aChoices
	ref array<string> m_aChoices = {};

	static CLW_ModeParam CreateNumber(string id, string label, string description, int type, float min, float max, float step, float value)
	{
		CLW_ModeParam param = new CLW_ModeParam();
		param.m_sId = id;
		param.m_sLabel = label;
		param.m_sDescription = description;
		param.m_eType = type;
		param.m_fMin = min;
		param.m_fMax = max;
		param.m_fStep = step;
		param.m_fValue = value;
		return param;
	}

	static CLW_ModeParam CreateBool(string id, string label, string description, bool value)
	{
		CLW_ModeParam param = new CLW_ModeParam();
		param.m_sId = id;
		param.m_sLabel = label;
		param.m_sDescription = description;
		param.m_eType = TYPE_BOOL;
		param.m_fMax = 1;
		if (value)
			param.m_fValue = 1;
		return param;
	}

	static CLW_ModeParam CreateChoice(string id, string label, string description, notnull array<string> choices, string value)
	{
		CLW_ModeParam param = new CLW_ModeParam();
		param.m_sId = id;
		param.m_sLabel = label;
		param.m_sDescription = description;
		param.m_eType = TYPE_CHOICE;
		param.m_aChoices.Copy(choices);
		int index = choices.Find(value);
		if (index < 0)
			index = 0;
		param.m_fValue = index;
		param.m_fMax = choices.Count() - 1;
		return param;
	}

	int GetInt()
	{
		return Math.Round(m_fValue);
	}

	bool GetBool()
	{
		return m_fValue >= 0.5;
	}

	string GetChoice()
	{
		int index = GetInt();
		if (index < 0 || index >= m_aChoices.Count())
			return "";

		return m_aChoices[index];
	}

	// Human-readable value for logs.
	string ValueText()
	{
		if (m_eType == TYPE_BOOL)
			return BoolText(GetBool());

		if (m_eType == TYPE_CHOICE)
			return GetChoice();

		if (m_eType == TYPE_INT)
			return GetInt().ToString();

		return m_fValue.ToString();
	}

	// Set from text (JSON value without quotes). Validates type and range. Returns false and fills error when rejected.
	bool SetFromText(string text, out string error)
	{
		if (m_eType == TYPE_BOOL)
		{
			if (text == "true" || text == "1")
				m_fValue = 1;
			else if (text == "false" || text == "0")
				m_fValue = 0;
			else
			{
				error = m_sId + ": expected true/false, got '" + text + "'";
				return false;
			}
			return true;
		}

		if (m_eType == TYPE_CHOICE)
		{
			int index = m_aChoices.Find(text);
			if (index < 0 && IsNumber(text))
				index = text.ToInt();
			if (index < 0 || index >= m_aChoices.Count())
			{
				error = m_sId + ": '" + text + "' is not one of the choices";
				return false;
			}
			m_fValue = index;
			return true;
		}

		if (!IsNumber(text))
		{
			error = m_sId + ": not a number '" + text + "'";
			return false;
		}

		float value = text.ToFloat();
		if (value < m_fMin || value > m_fMax)
		{
			error = string.Format("%1: %2 out of range %3..%4", m_sId, text, m_fMin, m_fMax);
			return false;
		}

		if (m_eType == TYPE_INT)
			value = Math.Round(value);
		m_fValue = value;
		return true;
	}

	CLW_ModeParam Copy()
	{
		CLW_ModeParam copy = new CLW_ModeParam();
		copy.m_sId = m_sId;
		copy.m_sLabel = m_sLabel;
		copy.m_sDescription = m_sDescription;
		copy.m_eType = m_eType;
		copy.m_fMin = m_fMin;
		copy.m_fMax = m_fMax;
		copy.m_fStep = m_fStep;
		copy.m_fValue = m_fValue;
		copy.m_aChoices.Copy(m_aChoices);
		return copy;
	}

	// {"id":"..","label":"..","description":"..","type":"int|float|bool|choice","min":..,"max":..,"step":..,"value":..,"choices":[..]}
	string ToJson()
	{
		string typeName = "float";
		if (m_eType == TYPE_INT)
			typeName = "int";
		else if (m_eType == TYPE_BOOL)
			typeName = "bool";
		else if (m_eType == TYPE_CHOICE)
			typeName = "choice";

		string json = "{\"id\":\"" + Escape(m_sId) + "\",\"label\":\"" + Escape(CLW_Loc.Resolve(m_sLabel)) + "\",\"description\":\"" + Escape(CLW_Loc.Resolve(m_sDescription)) + "\",\"type\":\"" + typeName + "\"";
		json += ",\"min\":" + m_fMin.ToString() + ",\"max\":" + m_fMax.ToString() + ",\"step\":" + m_fStep.ToString();
		if (m_eType == TYPE_BOOL)
			json += ",\"value\":" + BoolText(GetBool());
		else if (m_eType == TYPE_CHOICE)
			json += ",\"value\":\"" + Escape(GetChoice()) + "\"";
		else
			json += ",\"value\":" + ValueText();

		json += ",\"choices\":[";
		foreach (int i, string choice : m_aChoices)
		{
			if (i > 0)
				json += ",";
			json += "\"" + Escape(choice) + "\"";
		}
		json += "]}";
		return json;
	}

	// Values only: {"timeLimitMin":60,...}
	static string ValuesToJson(notnull array<ref CLW_ModeParam> params)
	{
		string json = "{";
		foreach (int i, CLW_ModeParam param : params)
		{
			if (i > 0)
				json += ",";
			if (param.m_eType == TYPE_CHOICE)
				json += "\"" + Escape(param.m_sId) + "\":\"" + Escape(param.GetChoice()) + "\"";
			else
				json += "\"" + Escape(param.m_sId) + "\":" + param.ValueText();
		}
		return json + "}";
	}

	static string BoolText(bool value)
	{
		if (value)
			return "true";

		return "false";
	}

	static string Escape(string text)
	{
		text.Replace("\\", "\\\\");
		text.Replace("\"", "\\\"");
		text.Replace("\n", " ");
		text.Replace("\t", " ");
		return text;
	}

	static bool IsNumber(string text)
	{
		if (text.IsEmpty())
			return false;

		bool digit;
		for (int i = 0; i < text.Length(); i++)
		{
			string c = text.Get(i);
			if ("0123456789".Contains(c))
				digit = true;
			else if (c != "-" && c != "." && c != "e" && c != "E" && c != "+")
				return false;
		}

		return digit;
	}

	// Parse a flat JSON object into key -> raw value text (strings unquoted). False + error on malformed input.
	static bool ParseFlat(string json, notnull map<string, string> values, out string error)
	{
		int n = json.Length();
		int i = 0;
		i = SkipWs(json, i);
		if (i >= n || json.Get(i) != "{")
		{
			error = "params json must be an object";
			return false;
		}
		i++;

		while (true)
		{
			i = SkipWs(json, i);
			if (i >= n)
			{
				error = "params json: unexpected end";
				return false;
			}

			if (json.Get(i) == "}")
				return true;

			if (json.Get(i) == ",")
			{
				i++;
				continue;
			}

			if (json.Get(i) != "\"")
			{
				error = "params json: key expected at " + i;
				return false;
			}

			string key = ReadString(json, i);
			i = i + 2 + key.Length();	// no escapes expected in keys
			i = SkipWs(json, i);
			if (i >= n || json.Get(i) != ":")
			{
				error = "params json: ':' expected after " + key;
				return false;
			}
			i++;
			i = SkipWs(json, i);
			if (i >= n)
			{
				error = "params json: value expected for " + key;
				return false;
			}

			string value;
			if (json.Get(i) == "\"")
			{
				value = ReadString(json, i);
				i = i + 2 + value.Length();
			}
			else
			{
				int start = i;
				while (i < n && json.Get(i) != "," && json.Get(i) != "}")
				{
					i++;
				}
				value = json.Substring(start, i - start);
				value = value.Trim();
			}

			values.Set(key, value);
		}

		return true;
	}

	protected static int SkipWs(string text, int i)
	{
		while (i < text.Length())
		{
			string c = text.Get(i);
			if (c != " " && c != "\t" && c != "\n" && c != "\r")
				break;
			i++;
		}

		return i;
	}

	// Content of the quoted string starting at index start (the opening quote); stops at the next unescaped quote.
	protected static string ReadString(string text, int start)
	{
		string result;
		for (int i = start + 1; i < text.Length(); i++)
		{
			string c = text.Get(i);
			if (c == "\"")
				break;
			result += c;
		}

		return result;
	}
}

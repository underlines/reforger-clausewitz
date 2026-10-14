// Weather and time of day (design: dev/notes/hq-comms-env-traits-design.md section 2, API notes: dev/research/time-and-weather-api.md).
// Setup params (AddParams, called by the war mode constructor), applied at war start on the server (OnWarStart): date, start time, day speed,
// then six weather slots that split the war time limit into equal parts; at each slot border the weather transitions (about 10 % of the
// slot) to the next slot's state, after slot 6 it stays. weatherMode "world" leaves the weather to the world's own automatic weather.
// HQ gets war events at weather changes and at dawn / dusk; the situation block `env` is GetJson(), the rules SITREP line is SitrepLine().
// Everything is server side (TimeAndWeatherManagerEntity replicates date, time, day duration and weather to clients itself).
// Weather state names come from the world's weather manager (GetWeatherStatesList); FALLBACK_STATES only when discovery fails.
class CLW_Environment
{
	static const string KEY_DAY_SPEED = "daySpeed";
	static const string KEY_START_HOUR = "startHour";
	static const string KEY_START_YEAR = "startYear";
	static const string KEY_START_MONTH = "startMonth";
	static const string KEY_START_DAY = "startDay";
	static const string KEY_WEATHER_MODE = "weatherMode";
	static const string KEY_WEATHER = "weather";			// weather1 .. weather6
	static const string RANDOM = "random";
	static const string MODE_SLOTS = "slots";
	static const string MODE_WORLD = "world";
	static const int SLOTS = 6;
	static const int POLL_MS = 10000;
	static const int DEFAULT_LIMIT_MIN = 60;
	static const float TRANSITION_FRACTION = 0.1;			// of one slot
	static const float DAY_SECONDS = 86400;
	static const float TWILIGHT_HOURS = 0.5;				// dawn / dusk: this long before and after sunrise / sunset
	static const float DEFAULT_SUNRISE = 5;
	static const float DEFAULT_SUNSET = 19;

	protected static ref array<string> s_aPlan;			// weather state per slot ("" entries never happen once active)
	protected static ref array<string> s_aStateNames;		// weather states of the world (or the fallback)
	protected static ref array<string> s_aStateLabels;		// parallel: localized name (a key the client translates) or the state name
	protected static bool s_bActive;
	protected static bool s_bWorldWeather;
	protected static float s_fSlotSec;
	protected static int s_iSlot;							// slot whose weather is already (being) applied
	protected static string s_sDaylight;
	protected static float s_fDaySpeed;

	// ---- setup params ----

	// Declares the env params on a war mode (call from the mode constructor).
	static void AddParams(notnull array<ref CLW_ModeParam> params)
	{
		array<string> speeds = {"0x", "1x", "4x", "8x", "16x", "24x", "48x", "96x"};
		params.Insert(CLW_ModeParam.CreateChoice(KEY_DAY_SPEED, "#CLW-Param_daySpeed", "#CLW-Param_daySpeed_Desc", speeds, "4x"));
		params.Insert(CLW_ModeParam.CreateNumber(KEY_START_HOUR, "#CLW-Param_startHour", "#CLW-Param_startHour_Desc", CLW_ModeParam.TYPE_FLOAT, 0, 23.5, 0.5, 8));

		int year = 1989;
		int month = 6;
		int day = 21;
		TimeAndWeatherManagerEntity manager = GetManager();
		if (manager)
			manager.GetDate(year, month, day);
		year = Math.Clamp(year, 1960, 2040);
		params.Insert(CLW_ModeParam.CreateNumber(KEY_START_YEAR, "#CLW-Param_startYear", "#CLW-Param_startYear_Desc", CLW_ModeParam.TYPE_INT, 1960, 2040, 1, year));
		params.Insert(CLW_ModeParam.CreateNumber(KEY_START_MONTH, "#CLW-Param_startMonth", "#CLW-Param_startMonth_Desc", CLW_ModeParam.TYPE_INT, 1, 12, 1, Math.Clamp(month, 1, 12)));
		params.Insert(CLW_ModeParam.CreateNumber(KEY_START_DAY, "#CLW-Param_startDay", "#CLW-Param_startDay_Desc", CLW_ModeParam.TYPE_INT, 1, 31, 1, Math.Clamp(day, 1, 31)));

		array<string> modes = {MODE_SLOTS, MODE_WORLD};
		params.Insert(CLW_ModeParam.CreateChoice(KEY_WEATHER_MODE, "#CLW-Param_weatherMode", "#CLW-Param_weatherMode_Desc", modes, MODE_SLOTS));

		array<string> names = {};
		array<string> labels = {};
		DiscoverStates(manager, names, labels);
		array<string> choices = {RANDOM};
		foreach (string name : names)
		{
			choices.Insert(name);
		}

		for (int slot = 1; slot <= SLOTS; slot++)
		{
			params.Insert(CLW_ModeParam.CreateChoice(KEY_WEATHER + slot, "#CLW-Param_weather" + slot, "#CLW-Param_weather" + slot + "_Desc", choices, RANDOM));
		}
	}

	// ---- lifecycle ----

	// War start (server): apply date, time, day speed; roll and apply the weather plan; start the poll.
	static void OnWarStart()
	{
		Stop();
		if (!Replication.IsServer())
			return;

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.GetMode())
			return;

		TimeAndWeatherManagerEntity manager = GetManager();
		if (!manager)
		{
			Print("CLW: env no TimeAndWeatherManagerEntity in this world, weather and time left alone", LogLevel.WARNING);
			return;
		}

		CLW_WarMode mode = war.GetMode();
		ApplyTime(manager, mode);

		s_aPlan = new array<string>();
		s_aStateNames = new array<string>();
		s_aStateLabels = new array<string>();
		DiscoverStates(manager, s_aStateNames, s_aStateLabels);
		s_bWorldWeather = mode.GetChoiceParam(KEY_WEATHER_MODE) == MODE_WORLD;
		int limitMin = mode.GetIntParam("timeLimitMin", DEFAULT_LIMIT_MIN);
		if (limitMin <= 0)
			limitMin = DEFAULT_LIMIT_MIN;
		s_fSlotSec = limitMin * 60 / SLOTS;
		s_iSlot = 0;

		if (s_bWorldWeather)
			Print("CLW: env weather mode world: the world's own automatic weather stays in charge", LogLevel.NORMAL);
		else
		{
			RollPlan(mode);
			Print("CLW: env weather plan (" + limitMin + " min, slot " + Math.Round(s_fSlotSec) + " s): " + PlanText(), LogLevel.NORMAL);
			ForceWeather(manager, s_aPlan[0], 0);
		}

		s_sDaylight = Daylight(manager);
		s_bActive = true;
		Print("CLW: env active " + TimeText(manager.GetTimeOfTheDay()) + " " + s_sDaylight + " weather " + CurrentName(manager), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(Poll, POLL_MS, true);
	}

	// War end / rematch: stop the slot changes (the weather the war left behind stays).
	static void Stop()
	{
		GetGame().GetCallqueue().Remove(Poll);
		s_bActive = false;
		s_iSlot = 0;
	}

	// ---- outputs ----

	// Situation block `env` (object) or "".
	static string GetJson()
	{
		TimeAndWeatherManagerEntity manager = Live();
		if (!manager)
			return "";

		string current = CurrentName(manager);
		string nextName;
		int nextMin;
		NextChange(manager, nextName, nextMin);

		float sunrise;
		float sunset;
		Sun(manager, sunrise, sunset);
		int year;
		int month;
		int day;
		manager.GetDate(year, month, day);

		string json = "{\"time\":\"" + TimeText(manager.GetTimeOfTheDay()) + "\",\"date\":\"" + year + "-" + Pad2(month) + "-" + Pad2(day) + "\"";
		int speedInt = Math.Round(s_fDaySpeed);
		json += ",\"daySpeed\":" + speedInt + ",\"daylight\":\"" + Daylight(manager) + "\"";
		json += ",\"sunrise\":\"" + TimeText(sunrise) + "\",\"sunset\":\"" + TimeText(sunset) + "\"";
		json += ",\"weather\":\"" + CLW_ModeParam.Escape(current) + "\",\"next\":\"" + CLW_ModeParam.Escape(nextName) + "\",\"nextInMin\":" + nextMin;
		json += ",\"plan\":[";
		if (s_aPlan)
		{
			foreach (int i, string state : s_aPlan)
			{
				if (i > 0)
					json += ",";
				json += "\"" + CLW_ModeParam.Escape(state) + "\"";
			}
		}
		return json + "]}";
	}

	// Rules SITREP line as a CLW_Loc wire, or "" (no manager / not active).
	static string SitrepLine()
	{
		TimeAndWeatherManagerEntity manager = Live();
		if (!manager)
			return "";

		string dayWord = "#CLW-Sit_Env_Day";
		string daylight = Daylight(manager);
		if (daylight == "dusk")
			dayWord = "#CLW-Sit_Env_Dusk";
		else if (daylight == "night")
			dayWord = "#CLW-Sit_Env_Night";
		else if (daylight == "dawn")
			dayWord = "#CLW-Sit_Env_Dawn";

		string weather = LabelOf(CurrentName(manager));
		string nextName;
		int nextMin;
		NextChange(manager, nextName, nextMin);
		string time = TimeText(manager.GetTimeOfTheDay());
		if (nextName.IsEmpty())
			return CLW_Loc.W("#CLW-Sit_Env_Line", time, dayWord, weather);

		if (nextMin < 1)
			nextMin = 1;
		return CLW_Loc.W("#CLW-Sit_Env_LineNext", time, dayWord, weather, LabelOf(nextName), CLW_Loc.N(nextMin));
	}

	// ---- apply ----

	protected static void ApplyTime(TimeAndWeatherManagerEntity manager, CLW_WarMode mode)
	{
		int year = mode.GetIntParam(KEY_START_YEAR, 1989);
		int month = mode.GetIntParam(KEY_START_MONTH, 6);
		int day = mode.GetIntParam(KEY_START_DAY, 21);
		while (day > 28 && !manager.CheckValidDate(year, month, day))
		{
			day--;
		}

		if (!manager.SetDate(year, month, day))
			Print(string.Format("CLW: env SetDate %1-%2-%3 refused, the world's date stays", year, month, day), LogLevel.WARNING);

		float hour = StartHour(mode);
		manager.SetTimeOfTheDay(hour);

		s_fDaySpeed = SpeedOf(mode.GetChoiceParam(KEY_DAY_SPEED));
		if (s_fDaySpeed <= 0)
			manager.SetIsDayAutoAdvanced(false);
		else
		{
			manager.SetIsDayAutoAdvanced(true);
			manager.SetDayDuration(DAY_SECONDS / s_fDaySpeed);
		}

		Print(string.Format("CLW: env date %1-%2-%3 start %4 daySpeed %5x (day duration %6 s)", year, Pad2(month), Pad2(day), TimeText(hour), s_fDaySpeed, manager.GetDayDuration()), LogLevel.NORMAL);
	}

	// startHour is a FLOAT param but GetIntParam rounds: read the raw value through the param list.
	protected static float StartHour(CLW_WarMode mode)
	{
		array<ref CLW_ModeParam> list = {};
		mode.GetParams(list);
		foreach (CLW_ModeParam param : list)
		{
			if (param.m_sId == KEY_START_HOUR)
				return Math.Clamp(param.m_fValue, 0, 23.5);
		}

		return 8;
	}

	protected static void RollPlan(CLW_WarMode mode)
	{
		s_aPlan.Clear();
		string previous;
		for (int slot = 1; slot <= SLOTS; slot++)
		{
			string wanted = mode.GetChoiceParam(KEY_WEATHER + slot);
			if (wanted.IsEmpty() || wanted == RANDOM || !s_aStateNames.Contains(wanted))
			{
				// random: a state different from the previous slot (when the world has more than one)
				array<string> pool = {};
				foreach (string candidate : s_aStateNames)
				{
					if (candidate != previous || s_aStateNames.Count() == 1)
						pool.Insert(candidate);
				}

				wanted = pool.GetRandomElement();
			}

			s_aPlan.Insert(wanted);
			previous = wanted;
		}
	}

	// Server: hold this weather state; transitionSec real seconds of blending (0 = at once).
	protected static void ForceWeather(TimeAndWeatherManagerEntity manager, string state, float transitionSec)
	{
		float transitionHours = 0;
		float stateHours = 1;
		if (s_fDaySpeed > 0)
		{
			transitionHours = transitionSec * s_fDaySpeed / 3600;
			stateHours = s_fSlotSec * s_fDaySpeed / 3600;
			if (stateHours < 0.01)
				stateHours = 0.01;
		}

		manager.ForceWeatherTo(true, state, transitionHours, stateHours);
		Print(string.Format("CLW: env weather -> %1 (transition %2 s = %3 game h)", state, Math.Round(transitionSec), transitionHours), LogLevel.NORMAL);
	}

	// ---- poll ----

	protected static void Poll()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!s_bActive || !war || !war.IsRunning())
			return;

		TimeAndWeatherManagerEntity manager = GetManager();
		if (!manager)
			return;

		if (!s_bWorldWeather)
		{
			int slotSec = Math.Round(s_fSlotSec);
			if (slotSec < 1)
				slotSec = 1;
			int slot = war.GetElapsedSec() / slotSec;
			if (slot > SLOTS - 1)
				slot = SLOTS - 1;
			while (s_iSlot < slot)
			{
				s_iSlot++;
				string state = s_aPlan[s_iSlot];
				ForceWeather(manager, state, s_fSlotSec * TRANSITION_FRACTION);
				PushAll(war, "Weather changing to " + state);
			}
		}

		string daylight = Daylight(manager);
		if (daylight != s_sDaylight)
		{
			s_sDaylight = daylight;
			PushAll(war, DaylightEvent(manager, daylight));
			Print("CLW: env daylight " + daylight + " at " + TimeText(manager.GetTimeOfTheDay()), LogLevel.NORMAL);
		}
	}

	protected static void PushAll(CLW_WarState war, string text)
	{
		array<string> keys = {};
		war.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			war.PushEvent(key, text);
		}
	}

	protected static string DaylightEvent(TimeAndWeatherManagerEntity manager, string daylight)
	{
		string time = TimeText(manager.GetTimeOfTheDay());
		if (daylight == "dusk")
			return "Dusk, light is fading (" + time + "), night follows soon";

		if (daylight == "night")
			return "Night has fallen (" + time + ")";

		if (daylight == "dawn")
			return "Dawn is breaking (" + time + ")";

		return "Full daylight (" + time + ")";
	}

	// ---- queries ----

	protected static TimeAndWeatherManagerEntity GetManager()
	{
		ChimeraWorld world = ChimeraWorld.CastFrom(GetGame().GetWorld());
		if (!world)
			return null;

		return world.GetTimeAndWeatherManager();
	}

	// The manager while the env is active, else null.
	protected static TimeAndWeatherManagerEntity Live()
	{
		if (!s_bActive)
			return null;

		return GetManager();
	}

	protected static void DiscoverStates(TimeAndWeatherManagerEntity manager, notnull array<string> names, notnull array<string> labels)
	{
		if (manager)
		{
			array<ref WeatherState> states = {};
			manager.GetWeatherStatesList(states);
			foreach (WeatherState state : states)
			{
				string name = state.GetStateName();
				if (name.IsEmpty() || names.Contains(name))
					continue;

				names.Insert(name);
				string label = state.GetLocalizedName();
				if (label.IsEmpty())
					label = name;
				labels.Insert(label);
			}
		}

		if (names.IsEmpty())
		{
			Print("CLW: env weather state discovery failed, using the fallback list", LogLevel.WARNING);
			array<string> fallback = {"Clear", "Cloudy", "Overcast", "Rainy"};
			foreach (string fallbackName : fallback)
			{
				names.Insert(fallbackName);
				labels.Insert(fallbackName);
			}
		}
	}

	protected static string CurrentName(TimeAndWeatherManagerEntity manager)
	{
		WeatherState state = manager.GetCurrentWeatherState();
		if (!state)
			return "";

		return state.GetStateName();
	}

	// Display text of a state (the localized-name key when the world has one, else the name).
	protected static string LabelOf(string stateName)
	{
		if (s_aStateNames)
		{
			int index = s_aStateNames.Find(stateName);
			if (index >= 0 && index < s_aStateLabels.Count())
				return s_aStateLabels[index];
		}

		return stateName;
	}

	// Next weather change: state name and real minutes until it starts ("" / 0 = none known).
	protected static void NextChange(TimeAndWeatherManagerEntity manager, out string nextName, out int minutes)
	{
		nextName = "";
		minutes = 0;
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!s_bWorldWeather && s_aPlan && war)
		{
			// first later slot with another state (equal neighbours change nothing)
			for (int later = s_iSlot + 1; later < s_aPlan.Count(); later++)
			{
				if (s_aPlan[later] == s_aPlan[s_iSlot])
					continue;

				float untilSec = later * s_fSlotSec - war.GetElapsedSec();
				nextName = s_aPlan[later];
				minutes = Math.Ceil(untilSec / 60);
				if (minutes < 0)
					minutes = 0;
				return;
			}

			return;
		}

		// world weather: ask the transition manager (game hours -> real minutes through the day duration)
		BaseWeatherStateTransitionManager transitions = manager.GetTransitionManager();
		if (!transitions || !transitions.GetNextState() || !transitions.GetCurrentState() || s_fDaySpeed <= 0)
			return;

		string upcoming = transitions.GetNextState().GetStateName();
		if (upcoming == transitions.GetCurrentState().GetStateName())
			return;

		nextName = upcoming;
		minutes = Math.Ceil(transitions.GetTimeLeftUntilNextState() * 3600 / s_fDaySpeed / 60);
		if (minutes < 0)
			minutes = 0;
	}

	protected static void Sun(TimeAndWeatherManagerEntity manager, out float sunrise, out float sunset)
	{
		sunrise = DEFAULT_SUNRISE;
		sunset = DEFAULT_SUNSET;
		float value;
		if (manager.GetSunriseHour(value))
			sunrise = value;
		if (manager.GetSunsetHour(value))
			sunset = value;
	}

	// day | dusk | night | dawn from the current game time (twilight windows around sunrise / sunset).
	protected static string Daylight(TimeAndWeatherManagerEntity manager)
	{
		float sunrise;
		float sunset;
		Sun(manager, sunrise, sunset);
		float t = manager.GetTimeOfTheDay();
		if (t >= sunrise - TWILIGHT_HOURS && t < sunrise + TWILIGHT_HOURS)
			return "dawn";

		if (t >= sunset - TWILIGHT_HOURS && t < sunset + TWILIGHT_HOURS)
			return "dusk";

		if (t >= sunrise + TWILIGHT_HOURS && t < sunset - TWILIGHT_HOURS)
			return "day";

		return "night";
	}

	// ---- text helpers ----

	protected static string PlanText()
	{
		string text;
		foreach (int i, string state : s_aPlan)
		{
			if (i > 0)
				text += ", ";
			text += state;
		}

		return text;
	}

	// "4x" -> 4, "0x" -> 0.
	protected static float SpeedOf(string choice)
	{
		if (choice.IsEmpty())
			return 1;

		string number = choice;
		number.Replace("x", "");
		return number.ToFloat();
	}

	// 21.66 -> "21:40"
	protected static string TimeText(float hour24)
	{
		int total = Math.Round(hour24 * 60);
		total = total % 1440;
		if (total < 0)
			total += 1440;

		return Pad2(total / 60) + ":" + Pad2(total % 60);
	}

	protected static string Pad2(int value)
	{
		if (value < 10)
			return "0" + value;

		return value.ToString();
	}
}

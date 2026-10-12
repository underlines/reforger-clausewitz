// Ticket counter HUD, client half: a small panel at the top centre (layout UI/layouts/CLW/WarStatusHud.layout) showing both
// sides (name in the side colour, tickets, held objectives), the time left and the setup / end state. Fed by the status line
// of CLW_WarStatus. Created lazily on the first message; hidden while the map is open. Faction names are localised here.
class CLW_WarHud
{
	static const ResourceName LAYOUT = "{7D3A91C45E30B6A1}UI/layouts/CLW/WarStatusHud.layout";
	static const float LOW_TICKETS_RATIO = 0.25;

	protected static Widget s_wRoot;
	protected static bool s_bMapHooked;
	protected static bool s_bMapOpen;
	protected static bool s_bFailed;

	// Parses a CLW_WarStatus line and refreshes the panel.
	static void Update(string wire)
	{
		if (s_bFailed || !Create())
			return;

		array<string> parts = {};
		wire.Split("|", parts, false);
		if (parts.Count() < 5)
			return;

		int phase = parts[0].ToInt();
		int timeLeft = parts[1].ToInt();
		string winner = parts[2];

		string nameA, infoA, nameB, infoB;
		int colorA, colorB;
		bool lowA, lowB;
		FillSide(parts[3], 0, phase, nameA, infoA, colorA, lowA);
		FillSide(parts[4], 1, phase, nameB, infoB, colorB, lowB);
		int warn = ARGB(255, 255, 90, 60);
		int normal = ARGB(255, 235, 235, 235);
		int infoColorA = normal;
		int infoColorB = normal;
		if (lowA)
			infoColorA = warn;
		if (lowB)
			infoColorB = warn;
		SetText("A_Name", nameA, colorA);
		SetText("B_Name", nameB, colorB);
		SetText("A_Info", infoA, infoColorA);
		SetText("B_Info", infoB, infoColorB);

		string status = "";
		string clock = "";
		if (phase == 0)
		{
			status = "Setup...";
		}
		else if (phase == 2)
		{
			status = "War over";
			clock = "Draw";
			if (!winner.IsEmpty())
				clock = FactionName(winner) + " wins";
		}
		else if (timeLeft >= 0)
		{
			clock = FormatTime(timeLeft);
		}

		SetText("Status", status, normal);
		SetText("Clock", clock, normal);
		if (s_wRoot)
			s_wRoot.SetVisible(!s_bMapOpen);
	}

	// side = key,identity,tickets,maxTickets,heldCount,heldValue,supplies (supplies shown for the local side only)
	protected static void FillSide(string side, int index, int phase, out string name, out string info, out int color, out bool low)
	{
		array<string> f = {};
		side.Split(",", f, false);
		if (f.Count() < 6)
		{
			color = ARGB(255, 180, 180, 180);
			return;
		}

		name = FactionName(f[0]);
		color = SideColor(f[1], index);
		if (phase == 0)
			return;

		int tickets = f[2].ToInt();
		int maxTickets = f[3].ToInt();
		string ticketText = "-";
		if (tickets >= 0)
		{
			ticketText = tickets.ToString();
			if (maxTickets > 0 && tickets < maxTickets * LOW_TICKETS_RATIO)
				low = true;
		}

		info = ticketText + "  |  " + f[4] + " (" + f[5] + ")";
		if (f.Count() >= 7 && IsLocalSide(f[0]))
		{
			int supplies = f[6].ToInt();
			if (supplies >= 0)
				info += "  |  Supplies " + supplies.ToString();
		}
	}

	// True when the key is the faction of the local player (the HUD shows supplies of the own side only).
	protected static bool IsLocalSide(string key)
	{
		Faction local = SCR_FactionManager.SGetLocalPlayerFaction();
		return local && local.GetFactionKey() == key;
	}

	// Side colours come from the scenario's map identity; unknown identities fall back to side A blue / side B red.
	static int SideColor(string identity, int index)
	{
		if (identity == "BLUFOR")
			return ARGB(255, 95, 150, 255);

		if (identity == "OPFOR")
			return ARGB(255, 255, 90, 90);

		if (identity == "INDFOR")
			return ARGB(255, 100, 215, 100);

		if (index == 0)
			return ARGB(255, 95, 150, 255);

		if (index == 1)
			return ARGB(255, 255, 90, 90);

		return ARGB(255, 180, 180, 180);
	}

	protected static string FactionName(string key)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (manager)
		{
			SCR_Faction faction = SCR_Faction.Cast(manager.GetFactionByKey(key));
			if (faction)
				return WidgetManager.Translate(faction.GetFactionName());
		}

		return key;
	}

	static string FormatTime(int seconds)
	{
		seconds = Math.Max(0, seconds);
		int minutes = seconds / 60;
		int rest = seconds - minutes * 60;
		string restText = rest.ToString();
		if (rest < 10)
			restText = "0" + restText;

		return minutes.ToString() + ":" + restText;
	}

	protected static void SetText(string widgetName, string text, int color)
	{
		if (!s_wRoot)
			return;

		TextWidget widget = TextWidget.Cast(s_wRoot.FindAnyWidget(widgetName));
		if (!widget)
			return;

		widget.SetText(text);
		widget.SetColorInt(color);
	}

	protected static bool Create()
	{
		if (s_wRoot)
			return true;

		WorkspaceWidget workspace = GetGame().GetWorkspace();
		if (!workspace)
			return false;

		s_wRoot = workspace.CreateWidgets(LAYOUT);
		if (!s_wRoot)
		{
			s_bFailed = true;
			Print("CLW: war status HUD layout could not be created: " + LAYOUT, LogLevel.ERROR);
			return false;
		}

		if (!s_bMapHooked)
		{
			s_bMapHooked = true;
			SCR_MapEntity.GetOnMapOpen().Insert(OnMapOpen);		// [verify] static handler on ScriptInvokerBase
			SCR_MapEntity.GetOnMapClose().Insert(OnMapClose);
		}

		Print("CLW: war status HUD created", LogLevel.NORMAL);
		return true;
	}

	protected static void OnMapOpen(MapConfiguration config)
	{
		s_bMapOpen = true;
		if (s_wRoot)
			s_wRoot.SetVisible(false);
	}

	protected static void OnMapClose(MapConfiguration config)
	{
		s_bMapOpen = false;
		if (s_wRoot)
			s_wRoot.SetVisible(true);
	}
}

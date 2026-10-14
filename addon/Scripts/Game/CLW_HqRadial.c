// HQ radial menu (WP2, design: dev/notes/hq-comms-env-traits-design.md section 1b). Client only, quick access to the most used HQ functions;
// the full dialog (CLW_HqMenuDialog, /hqmenu) stays and is one entry of the circle.
//
// Mechanism: the vanilla GLOBAL radial menu (SCR_RadialMenu.GlobalRadialMenu, the one the commanding menu uses) driven by our own
// SCR_RadialMenuController subclass (CLW_HqRadialController) with the input action CLW_HqRadial (default key U, hold; Configs/System/chimeraInputCommon.conf
// adds it to VONContext). Levels are SCR_SelectionMenuCategoryEntry (CLW_HqRadialCategory), built when they are entered (Fill).
// Dynamic data (role, blocked reasons, vehicle types) comes from the HQ menu payload: the radial sends the action "refresh" when it opens and
// CLW_HqMenuClient.Receive hands every parsed payload to OnPayload. Every server call is the existing RPC CLW_HqMenuAction(action, arg, Zero):
//   req <kind>|<type>   report <count> <kind> <dist> <bearing>   report_cancel   backup   mission   status
// SITREP and the full menu are local (CLW_SitrepClient, CLW_HqMenuOpen).
// Log prefix: "CLW: hqradial".

class CLW_HqRadial
{
	static const string ACTION_OPEN = "CLW_HqRadial";
	static const ResourceName ENTRY_LAYOUT = "{A87A2A3CA0018423}UI/layouts/Common/RadialMenu/SelectionMenuEntryCommand.layout";	// vanilla commanding entry: icon + text
	static const ResourceName ICONS = "{A4A692135635424E}UI/Imagesets/Commanding/Commanding_Icons.imageset";
	static const string VIEW_MARK = "{view}";		// in an action arg: replaced by the camera heading when the entry is performed

	protected static ref CLW_HqRadialController s_Controller;
	protected static ref CLW_HqMenuData s_Data;		// last HQ menu payload (null until the first answer)
	protected static bool s_bChatRegistered;

	// ---- setup ----

	// Creates the controller (its constructor listens to the open action). Idempotent, client only.
	static void Init()
	{
		if (s_Controller || System.IsConsoleApp())
			return;

		s_Controller = new CLW_HqRadialController();
		Print("CLW: hqradial ready: open action '" + ACTION_OPEN + "' (default key U, hold), chat fallback /hqr", LogLevel.NORMAL);
	}

	// Chat fallback /hqr (when the input action cannot be bound): opens the circle without a held key.
	static void RegisterChatCommand()
	{
		if (s_bChatRegistered)
			return;

		SCR_ChatPanelManager chat = SCR_ChatPanelManager.GetInstance();
		if (!chat)
			return;

		ChatCommandInvoker invoker = chat.GetCommandInvoker("hqr");
		if (!invoker)
			return;

		invoker.Insert(OnChatCommand);
		s_bChatRegistered = true;
		Print("CLW: hqradial chat command /hqr registered", LogLevel.NORMAL);
	}

	static void OnChatCommand(SCR_ChatPanel panel, string data)
	{
		Print("CLW: hqradial chat /hqr", LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(OpenFromChat, 400, false);	// let the chat panel close first
	}

	protected static void OpenFromChat()
	{
		Init();
		if (s_Controller)
			s_Controller.OpenFromCommand();
	}

	// ---- payload ----

	// Every parsed HQ menu payload (CLW_HqMenuClient.Receive), also when the dialog is not wanted.
	static void OnPayload(notnull CLW_HqMenuData data)
	{
		s_Data = data;
		Print(string.Format("CLW: hqradial payload cached role=%1 running=%2 requests=%3", data.m_sRole, data.m_bRunning, data.m_aRequests.Count()), LogLevel.NORMAL);
		if (s_Controller)
			s_Controller.Rebuild();
	}

	// Asks the server for a fresh payload (answer: OnPayload).
	static void RequestRefresh()
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (controller)
			controller.CLW_HqMenuAction("refresh", "", vector.Zero);
	}

	static bool HasData()
	{
		return s_Data != null;
	}

	protected static CLW_HqRequest FindRequest(string id)
	{
		if (!s_Data)
			return null;

		foreach (CLW_HqRequest request : s_Data.m_aRequests)
		{
			if (request.m_sId == id)
				return request;
		}

		return null;
	}

	// ---- perform ----

	static void Perform(string action, string arg)
	{
		string sendArg = arg;
		if (sendArg.IndexOf(VIEW_MARK) >= 0)
			sendArg.Replace(VIEW_MARK, ViewBearing().ToString());

		Print(string.Format("CLW: hqradial perform action=%1 arg='%2'", action, sendArg), LogLevel.NORMAL);
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		if (action == "sitrep")
		{
			CLW_SitrepClient.ShowHint(true);	// same as the dialog button: show the last one, ask the server for a fresh one
			controller.CLW_SitrepRequest();
			return;
		}

		if (action == "menu")
		{
			controller.CLW_HqMenuOpen(vector.Zero);
			return;
		}

		controller.CLW_HqMenuAction(action, sendArg, vector.Zero);
	}

	// Heading of the camera in degrees 0-359 (0 = north, 90 = east), the player's "ahead".
	static int ViewBearing()
	{
		vector mat[4];
		float heading;
		CameraManager cameraManager = GetGame().GetCameraManager();
		PlayerCamera camera;
		if (cameraManager)
			camera = PlayerCamera.Cast(cameraManager.CurrentCamera());

		if (camera)
		{
			camera.GetTransform(mat);
			heading = Math.Atan2(mat[2][0], mat[2][2]) * Math.RAD2DEG;
		}
		else
		{
			IEntity entity = SCR_PlayerController.GetLocalControlledEntity();
			if (entity)
				heading = entity.GetYawPitchRoll()[0];
		}

		heading = Math.Repeat(heading, 360);
		int bearing = Math.Round(heading);
		return bearing % 360;
	}

	// ---- entries ----

	protected static CLW_HqRadialEntry MakeLeaf(string name, string shortText, string icon, string action, string arg, string desc, bool enabled)
	{
		CLW_HqRadialEntry entry = new CLW_HqRadialEntry();
		entry.SetName(name);
		entry.SetCommandText(shortText);
		entry.SetDescription(desc);
		entry.SetIcon(ICONS, icon);
		entry.m_sAction = action;
		entry.m_sArg = arg;
		entry.Enable(enabled);
		return entry;
	}

	protected static CLW_HqRadialCategory MakeCategory(string name, string shortText, string icon, string level, string desc, bool enabled)
	{
		CLW_HqRadialCategory category = new CLW_HqRadialCategory();
		category.SetName(name);
		category.SetCommandText(shortText);
		category.SetDescription(desc);
		category.SetIcon(ICONS, icon);
		category.m_sLevel = level;
		category.Enable(enabled);
		return category;
	}

	// The first circle. Static entries, enabled state from the cached payload (a first open without data shows everything enabled; the server decides).
	static void BuildRoot(notnull array<ref SCR_SelectionMenuEntry> entries)
	{
		bool running = true;
		bool reportOk = true;
		string offWhy;
		string reportWhy;
		if (s_Data)
		{
			running = s_Data.m_bRunning;
			reportOk = s_Data.m_bReport;
			reportWhy = CLW_Loc.Resolve(s_Data.m_sReportReason);
		}

		if (!running)
			offWhy = CLW_Loc.F("#CLW-HqMenu_NeedWar");
		if (running && !reportOk)
			offWhy = reportWhy;

		entries.Insert(MakeCategory(CLW_Loc.F("#CLW-HqRadial_Report"), CLW_Loc.F("#CLW-HqRadial_ReportS"), "search-destroy", "report", offWhy, running && reportOk));

		string runWhy;
		if (!running)
			runWhy = CLW_Loc.F("#CLW-HqMenu_NeedWar");

		entries.Insert(MakeCategory(CLW_Loc.F("#CLW-HqRadial_Requests"), CLW_Loc.F("#CLW-HqRadial_RequestsS"), "recruit", "requests", runWhy, running));
		entries.Insert(MakeLeaf(CLW_Loc.F("#CLW-HqRadial_Backup"), CLW_Loc.F("#CLW-HqRadial_BackupS"), "fire-at-will", "backup", "", runWhy, running));
		entries.Insert(MakeLeaf(CLW_Loc.F("#CLW-HqRadial_Cancel"), CLW_Loc.F("#CLW-HqRadial_CancelS"), "halt", "report_cancel", "", runWhy, running));
		entries.Insert(MakeLeaf(CLW_Loc.F("#CLW-HqRadial_Sitrep"), CLW_Loc.F("#CLW-HqRadial_SitrepS"), "look-at", "sitrep", "", "", true));
		entries.Insert(MakeLeaf(CLW_Loc.F("#CLW-HqRadial_Menu"), CLW_Loc.F("#CLW-HqRadial_MenuS"), "custom", "menu", "", "", true));
	}

	// Builds the children of a level when it is entered (CLW_HqRadialCategory.OnPerform).
	static void Fill(notnull CLW_HqRadialCategory category)
	{
		if (category.m_sLevel == "report")
			FillKinds(category);
		else if (category.m_sLevel == "count")
			FillCounts(category);
		else if (category.m_sLevel == "dist")
			FillDistances(category);
		else if (category.m_sLevel == "dir")
			FillDirections(category);
		else if (category.m_sLevel == "requests")
			FillRequests(category);
		else if (category.m_sLevel == "vehicle")
			FillVehicles(category);
		else
			Print("CLW: hqradial unknown level '" + category.m_sLevel + "'", LogLevel.WARNING);
	}

	protected static string KindName(string kind)
	{
		return CLW_Loc.F("#CLW-HqRadial_Kind_" + kind);
	}

	// Report level 1: what.
	protected static void FillKinds(notnull CLW_HqRadialCategory category)
	{
		array<string> kinds = {"inf", "veh", "armor", "air", "unknown"};
		array<string> icons = {"follow", "mount-passenger", "supress", "look-at", "custom"};
		for (int i = 0; i < kinds.Count(); i++)
		{
			string name = KindName(kinds[i]);
			CLW_HqRadialCategory next = MakeCategory(name, name, icons[i], "count", CLW_Loc.F("#CLW-HqRadial_Sum", name), true);
			next.m_sKind = kinds[i];
			category.AddEntry(next);
		}
	}

	// Report level 2: how many.
	protected static void FillCounts(notnull CLW_HqRadialCategory category)
	{
		array<int> counts = {1, 2, 3, 4, 6, 8, 12, 20};
		string kindName = KindName(category.m_sKind);
		foreach (int count : counts)
		{
			string shortText = CLW_Loc.F("#CLW-HqRadial_Count", count.ToString());
			string chain = shortText + " " + kindName;
			CLW_HqRadialCategory next = MakeCategory(chain, shortText, "hud-circle", "dist", CLW_Loc.F("#CLW-HqRadial_Sum", chain), true);
			next.m_sKind = category.m_sKind;
			next.m_iCount = count;
			category.AddEntry(next);
		}
	}

	// Report level 3: how far.
	protected static void FillDistances(notnull CLW_HqRadialCategory category)
	{
		array<int> distances = {50, 100, 200, 300, 500, 800, 1200};
		string head = CLW_Loc.F("#CLW-HqRadial_Count", category.m_iCount.ToString()) + " " + KindName(category.m_sKind);
		foreach (int dist : distances)
		{
			string shortText = CLW_Loc.F("#CLW-HqRadial_Dist", dist.ToString());
			string chain = head + ", " + shortText;
			CLW_HqRadialCategory next = MakeCategory(shortText, shortText, "hud-circle", "dir", CLW_Loc.F("#CLW-HqRadial_Sum", chain), true);
			next.m_sKind = category.m_sKind;
			next.m_iCount = category.m_iCount;
			next.m_iDist = dist;
			category.AddEntry(next);
		}
	}

	// Report level 4: which way, then the entry sends the report. "Ahead" is the camera heading as a number.
	protected static void FillDirections(notnull CLW_HqRadialCategory category)
	{
		array<string> keys = {"ahead", "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
		string head = CLW_Loc.F("#CLW-HqRadial_Count", category.m_iCount.ToString()) + " " + KindName(category.m_sKind) + ", " + CLW_Loc.F("#CLW-HqRadial_Dist", category.m_iDist.ToString());
		string prefix = category.m_iCount.ToString() + " " + category.m_sKind + " " + category.m_iDist.ToString() + " ";
		for (int i = 0; i < keys.Count(); i++)
		{
			string name = CLW_Loc.F("#CLW-HqRadial_Dir_" + keys[i]);
			string bearing = VIEW_MARK;
			string icon = "look-at";
			if (i > 0)
			{
				int degrees = (i - 1) * 45;
				bearing = degrees.ToString();
				icon = "hud-arrow";
			}

			category.AddEntry(MakeLeaf(name, name, icon, "report", prefix + bearing, CLW_Loc.F("#CLW-HqRadial_Sum", head + ", " + name), true));
		}
	}

	// Requests: vehicle (a level), resupply, squad, reinforce, mission, status.
	protected static void FillRequests(notnull CLW_HqRadialCategory category)
	{
		CLW_HqRequest row = FindRequest("vehicle");
		string blocked;
		if (row)
			blocked = CLW_Loc.Resolve(row.m_sBlocked);
		string vehicleName = CLW_Loc.F("#CLW-HqRadial_Vehicle");
		category.AddEntry(MakeCategory(vehicleName, vehicleName, "mount-passenger", "vehicle", blocked, blocked.IsEmpty()));

		AddRequestLeaf(category, "resupply", "#CLW-HqRadial_Resupply", "heal");
		AddRequestLeaf(category, "squad", "#CLW-HqRadial_Squad", "follow");
		AddRequestLeaf(category, "reinforce", "#CLW-HqRadial_Reinforce", "defend");

		// not chat request kinds: own HQ menu actions (the server side is WP1 / lead, see the report)
		string missionName = CLW_Loc.F("#CLW-HqRadial_Mission");
		category.AddEntry(MakeLeaf(missionName, missionName, "search-destroy", "mission", "", "", true));
		string statusName = CLW_Loc.F("#CLW-HqRadial_Status");
		category.AddEntry(MakeLeaf(statusName, statusName, "hud-circle", "status", "", "", true));
	}

	// One plain request (type chosen by the server): enabled unless the payload row says why not.
	protected static void AddRequestLeaf(notnull CLW_HqRadialCategory category, string id, string nameKey, string icon)
	{
		string name = CLW_Loc.F(nameKey);
		string kind = id;
		string blocked;
		CLW_HqRequest row = FindRequest(id);
		if (row)
		{
			kind = row.m_sKind;
			blocked = CLW_Loc.Resolve(row.m_sBlocked);
		}

		category.AddEntry(MakeLeaf(name, name, icon, "req", kind + "|", blocked, blocked.IsEmpty()));
	}

	// Requests -> Vehicle: the types of the payload (the default pick first).
	protected static void FillVehicles(notnull CLW_HqRadialCategory category)
	{
		CLW_HqRequest row = FindRequest("vehicle");
		if (!row || row.m_aOptions.IsEmpty())
		{
			string loading = CLW_Loc.F("#CLW-HqRadial_Loading");
			category.AddEntry(MakeLeaf(loading, loading, "custom", "refresh", "", "", false));
			return;
		}

		string blocked = CLW_Loc.Resolve(row.m_sBlocked);
		foreach (CLW_HqOption option : row.m_aOptions)
		{
			string typeArg = option.m_sArg;
			if (typeArg == "-")
				typeArg = "";

			string label = CLW_Loc.Resolve(option.m_sLabel);
			category.AddEntry(MakeLeaf(label, label, "mount-passenger", "req", row.m_sKind + "|" + typeArg, blocked, blocked.IsEmpty()));
		}
	}
}

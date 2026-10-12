// HQ menu, client side data and entry points (M4 WP4). Wire format: see CLW_HqMenuServer.c. The dialog is CLW_HqMenuDialog
// (preset Configs/CLW/UI/HqMenuDialog.conf, content layout UI/layouts/CLW/HqMenuContent.layout), same approach as the setup menu.
// Log prefix: "CLW: hqmenu".

class CLW_HqOption
{
	string m_sArg;
	string m_sLabel;
}

class CLW_HqRequest
{
	string m_sId;
	string m_sKind;
	string m_sTitle;
	string m_sBlocked;
	int m_iSel;
	ref array<ref CLW_HqOption> m_aOptions = {};
	Managed m_Spin;					// spin box of the row (owned by the widget)
}

// One vehicle row of the depot menu (wire line W).
class CLW_HqVehicle
{
	string m_sType;
	string m_sLabel;
	int m_iCost;
	string m_sKind;
}

class CLW_HqSquad
{
	string m_sId;
	string m_sName;
	int m_iHumans;
	int m_iMembers;
	bool m_bMine;
	bool m_bFull;
}

class CLW_HqMenuData
{
	string m_sFaction;
	string m_sRole;
	string m_sSupplies;
	string m_sBase;
	bool m_bRunning;
	string m_sStatus;
	bool m_bReport;
	string m_sReportReason;
	ref array<ref CLW_HqRequest> m_aRequests = {};
	ref array<ref CLW_HqSquad> m_aSquads = {};
	ref array<string> m_aLines = {};

	// depot mode (wire line V): a paged vehicle list instead of the vehicle request row
	bool m_bDepot;
	string m_sCat = "all";
	int m_iPage;
	int m_iPages = 1;
	int m_iTotal;
	string m_sVBlocked;
	ref array<ref CLW_HqVehicle> m_aVehicles = {};

	// Parses the payload. Null when it has no header.
	static CLW_HqMenuData Parse(string wire)
	{
		CLW_HqMenuData data = new CLW_HqMenuData();
		bool header;
		array<string> lines = {};
		wire.Split("\n", lines, true);
		foreach (string line : lines)
		{
			array<string> f = {};
			line.Split("\t", f, false);
			if (f.IsEmpty())
				continue;

			string type = f[0];
			if (type == "H" && f.Count() >= 5)
			{
				header = true;
				data.m_sFaction = f[1];
				data.m_sRole = f[2];
				data.m_sSupplies = f[3];
				data.m_sBase = f[4];
				data.m_bRunning = f.Count() >= 6 && f[5] == "1";
			}
			else if (type == "S" && f.Count() >= 2)
			{
				data.m_sStatus = f[1];
			}
			else if (type == "R" && f.Count() >= 4)
			{
				CLW_HqRequest request = new CLW_HqRequest();
				request.m_sId = f[1];
				request.m_sKind = f[2];
				request.m_sTitle = f[3];
				if (f.Count() >= 5)
					request.m_sBlocked = f[4];
				data.m_aRequests.Insert(request);
			}
			else if (type == "O" && f.Count() >= 3 && !data.m_aRequests.IsEmpty())
			{
				CLW_HqOption option = new CLW_HqOption();
				option.m_sArg = f[1];
				option.m_sLabel = f[2];
				data.m_aRequests[data.m_aRequests.Count() - 1].m_aOptions.Insert(option);
			}
			else if (type == "Q" && f.Count() >= 6)
			{
				CLW_HqSquad squad = new CLW_HqSquad();
				squad.m_sId = f[1];
				squad.m_sName = f[2];
				squad.m_iHumans = f[3].ToInt();
				squad.m_iMembers = f[4].ToInt();
				squad.m_bMine = f[5] == "1";
				squad.m_bFull = f.Count() >= 7 && f[6] == "1";
				data.m_aSquads.Insert(squad);
			}
			else if (type == "V" && f.Count() >= 5)
			{
				data.m_bDepot = true;
				data.m_sCat = f[1];
				data.m_iPage = f[2].ToInt();
				data.m_iPages = Math.Max(f[3].ToInt(), 1);
				data.m_iTotal = f[4].ToInt();
				if (f.Count() >= 6)
					data.m_sVBlocked = f[5];
			}
			else if (type == "W" && f.Count() >= 4)
			{
				CLW_HqVehicle vehicle = new CLW_HqVehicle();
				vehicle.m_sType = f[1];
				vehicle.m_sLabel = f[2];
				vehicle.m_iCost = f[3].ToInt();
				if (f.Count() >= 5)
					vehicle.m_sKind = f[4];
				data.m_aVehicles.Insert(vehicle);
			}
			else if (type == "L" && f.Count() >= 2)
			{
				data.m_aLines.Insert(f[1]);
			}
			else if (type == "K" && f.Count() >= 2)
			{
				data.m_bReport = f[1] == "1";
				if (f.Count() >= 3)
					data.m_sReportReason = f[2];
			}
		}

		if (!header)
			return null;

		return data;
	}
}

class CLW_HqMenuClient
{
	static const ResourceName PRESET_CONFIG = "{5A97D0E4C1B2683F}Configs/CLW/UI/HqMenuDialog.conf";
	static const string PRESET_TAG = "clw_hq";

	protected static ref CLW_HqMenuDialog s_Dialog;
	protected static vector s_vTerminal;		// terminal the menu was opened at (Zero = chat command)
	protected static bool s_bWanted;			// the player asked for the menu and did not close it (late payloads are ignored otherwise)

	// The player asked for the menu (terminal action or /hqmenu). Called by the controller before it asks the server.
	static void SetTerminal(vector terminalPos)
	{
		s_vTerminal = terminalPos;
		s_bWanted = true;
	}

	static vector GetTerminal()
	{
		return s_vTerminal;
	}

	static void OnClosed()
	{
		s_bWanted = false;
		s_Dialog = null;
	}

	// Payload from the server: open the dialog, or refresh it when it is open.
	static void Receive(string wire)
	{
		CLW_HqMenuData data = CLW_HqMenuData.Parse(wire);
		if (!data)
		{
			Print("CLW: hqmenu payload without a header, ignored", LogLevel.ERROR);
			return;
		}

		if (s_Dialog && s_Dialog.IsOpen())
		{
			s_Dialog.Update(data);
			return;
		}

		if (!s_bWanted)
			return;

		Print(string.Format("CLW: hqmenu opening: faction=%1 role=%2 requests=%3 squads=%4 lines=%5", data.m_sFaction, data.m_sRole, data.m_aRequests.Count(), data.m_aSquads.Count(), data.m_aLines.Count()), LogLevel.NORMAL);
		CLW_HqMenuDialog dialog = new CLW_HqMenuDialog();
		dialog.m_Data = data;
		SCR_ConfigurableDialogUi created = SCR_ConfigurableDialogUi.CreateFromPreset(PRESET_CONFIG, PRESET_TAG, dialog);
		if (!created)
		{
			s_bWanted = false;
			Print("CLW: hqmenu could not be created (preset / layout problem), use the /request and /report chat commands", LogLevel.ERROR);
			CLW_Debug.Show("HQ menu failed to open, use /request and /report");
			return;
		}

		s_Dialog = dialog;
	}

	// Menu button -> server (it runs the same code as the chat commands).
	static void Act(string action, string arg)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print(string.Format("CLW: hqmenu button action=%1 arg='%2'", action, arg), LogLevel.NORMAL);
		controller.CLW_HqMenuAction(action, arg, s_vTerminal);
	}

	// ---- chat command /hqmenu ----

	static void RegisterChatCommand()
	{
		SCR_ChatPanelManager chat = SCR_ChatPanelManager.GetInstance();
		if (!chat)
			return;

		ChatCommandInvoker invoker = chat.GetCommandInvoker("hqmenu");
		if (!invoker)
			return;

		invoker.Insert(OnChatCommand);
		SCR_PlayerController.CLW_MarkHqMenuChatRegistered();
		Print("CLW: hqmenu chat command /hqmenu registered", LogLevel.NORMAL);
	}

	static void OnChatCommand(SCR_ChatPanel panel, string data)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print("CLW: hqmenu chat /hqmenu", LogLevel.NORMAL);
		controller.CLW_HqMenuOpen(vector.Zero);
	}
}

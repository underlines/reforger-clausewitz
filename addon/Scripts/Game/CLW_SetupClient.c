// Client entry points of the war setup menu (called from CLW_SetupPlayerController RPCs, or directly on the listen-server host).
class CLW_SetupClient
{
	static const ResourceName PRESET_CONFIG = "{7D3A91C45E20B6F9}Configs/CLW/UI/SetupDialog.conf";
	static const string PRESET_TAG = "clw_setup";

	protected static ref CLW_SetupDialog s_Dialog;

	// Server asks this player to set up the war: parse the payload and open the dialog (ignored while it is already open).
	static void Open(string wire)
	{
		if (s_Dialog && s_Dialog.IsOpen())
		{
			Print("CLW: setup menu already open, ignoring the new payload", LogLevel.NORMAL);
			return;
		}

		CLW_SetupData data = CLW_SetupData.Parse(wire);
		if (!data)
		{
			Print("CLW: setup menu payload has no modes, not opening", LogLevel.ERROR);
			return;
		}

		Print(string.Format("CLW: setup menu opening: %1 modes, %2 battle params, default '%3'", data.m_aModes.Count(), data.m_aBattle.Count(), data.m_sDefaultMode), LogLevel.NORMAL);
		CLW_SetupDialog dialog = new CLW_SetupDialog();
		dialog.m_Data = data;
		SCR_ConfigurableDialogUi created = SCR_ConfigurableDialogUi.CreateFromPreset(PRESET_CONFIG, PRESET_TAG, dialog);
		if (!created)
		{
			Print("CLW: setup menu could not be created (preset / layout problem), use /war start", LogLevel.ERROR);
			CLW_Debug.Show("setup menu failed to open, use /war start [mode] [key=value ...]");
			return;
		}

		s_Dialog = dialog;
	}

	// Another player is the setup owner.
	static void Wait(string ownerName)
	{
		string text = "Waiting for " + ownerName + " to set up the war";
		Print("CLW: " + text, LogLevel.NORMAL);
		CLW_Debug.Show(text);
		SCR_HintManagerComponent.ShowCustomHint(text, "Clausewitz", 10);
	}

	// Server verdict on a submit or a chat command.
	static void Result(bool ok, string text)
	{
		Print(string.Format("CLW: setup result ok=%1 '%2'", ok, text), LogLevel.NORMAL);
		if (s_Dialog && s_Dialog.IsOpen())
		{
			s_Dialog.OnServerResult(ok, text);
			return;
		}

		if (!ok)
			CLW_Debug.Show("setup: " + text);
	}

	// The war started (by anyone, any way): close the menu and show the announcement.
	static void Closed(string text)
	{
		Print("CLW: setup closed: " + text, LogLevel.NORMAL);
		if (s_Dialog && s_Dialog.IsOpen())
			s_Dialog.Close();
		s_Dialog = null;

		CLW_Debug.Show(text);
		SCR_HintManagerComponent.ShowCustomHint(text, "Clausewitz", 12);
	}

	// ---- chat fallback: /war setup | /war start [mode] [key=value ...] ----

	static void RegisterChatCommand()
	{
		SCR_ChatPanelManager chat = SCR_ChatPanelManager.GetInstance();
		if (!chat)
			return;

		ChatCommandInvoker invoker = chat.GetCommandInvoker("war");
		if (!invoker)
			return;

		invoker.Insert(OnChatCommand);
		SCR_PlayerController.CLW_MarkChatRegistered();
		Print("CLW: chat command /war registered", LogLevel.NORMAL);
	}

	static void OnChatCommand(SCR_ChatPanel panel, string data)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print("CLW: chat /war " + data, LogLevel.NORMAL);
		controller.CLW_SendWarCommand(data);
	}
}

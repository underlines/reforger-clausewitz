// Client entry points of the war setup menu (called from CLW_SetupPlayerController RPCs, or directly on the listen-server host).
// Players have no character during SETUP, so the /war chat command is registered from Open / Wait as well (not only on the
// first controlled entity), and Open tells the server it is showing the menu (CLW_SendSetupAck) so the server stops re-sending.
class CLW_SetupClient
{
	static const ResourceName PRESET_CONFIG = "{7D3A91C45E20B6F9}Configs/CLW/UI/SetupDialog.conf";
	static const string PRESET_TAG = "clw_setup";

	protected static ref CLW_SetupDialog s_Dialog;
	protected static bool s_bPlaceholderHidden;

	// Server asks this player to set up the war: parse the payload and open the dialog (ignored while it is already open).
	static void Open(string wire)
	{
		RegisterChatCommand();
		HideLoadingPlaceholder();
		if (s_Dialog && s_Dialog.IsOpen())
		{
			Print("CLW: setup menu already open, ignoring the new payload", LogLevel.NORMAL);
			SendAck();
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
			CLW_Debug.Show("#CLW-Setup_Failed");
			return;
		}

		s_Dialog = dialog;
		SendAck();
	}

	// During SETUP the player has no character and the server withholds NotifyReadyForSpawn_S, so vanilla
	// SCR_PlayerDeployMenuHandlerComponent.SetReady() never calls DestroyLoadingPlaceholder(): the full-screen splash
	// (created once in SCR_RespawnSystemComponent.OnInit, never re-created) would cover the setup menu. Remove it here.
	protected static void HideLoadingPlaceholder()
	{
		SCR_RespawnSystemComponent respawn = SCR_RespawnSystemComponent.GetInstance();
		if (!respawn)
			return;

		respawn.DestroyLoadingPlaceholder();
		if (s_bPlaceholderHidden)
			return;

		s_bPlaceholderHidden = true;
		Print("CLW: setup hid the respawn loading placeholder", LogLevel.NORMAL);
	}

	protected static void SendAck()
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (controller)
			controller.CLW_SendSetupAck();
	}

	// Another player is the setup owner.
	static void Wait(string ownerName)
	{
		RegisterChatCommand();
		HideLoadingPlaceholder();
		string text = CLW_Loc.F("#CLW-Setup_Waiting", ownerName);
		Print("CLW: " + text, LogLevel.NORMAL);
		CLW_Debug.Show(text);
		SCR_HintManagerComponent.ShowCustomHint(text, "Clausewitz", 10);
	}

	// Server verdict on a submit or a chat command.
	static void Result(bool ok, string text)
	{
		Print(string.Format("CLW: setup result ok=%1 '%2'", ok, CLW_Loc.Resolve(text)), LogLevel.NORMAL);
		if (s_Dialog && s_Dialog.IsOpen())
		{
			s_Dialog.OnServerResult(ok, text);
			return;
		}

		if (!ok)
			CLW_Debug.Show(CLW_Loc.W("#CLW-Setup_ErrorLine", text));
	}

	// The war started (by anyone, any way): close the menu and show the announcement.
	static void Closed(string text)
	{
		string shown = CLW_Loc.Resolve(text);	// the server sends a CLW_Loc wire
		Print("CLW: setup closed: " + shown, LogLevel.NORMAL);
		if (s_Dialog && s_Dialog.IsOpen())
			s_Dialog.Close();
		s_Dialog = null;

		CLW_Debug.Show(shown);
		SCR_HintManagerComponent.ShowCustomHint(shown, "Clausewitz", 12);
	}

	// ---- chat fallback: /war setup | /war start [mode] [key=value ...] ----

	static void RegisterChatCommand()
	{
		if (SCR_PlayerController.CLW_IsChatRegistered())
			return;

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

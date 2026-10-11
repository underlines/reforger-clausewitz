// In-game debug text. Say() runs on the server, logs the line and shows it in the chat panel of
// every player (delivered by an owner RPC on SCR_PlayerController, see CLW_DebugPlayerController.c).
class CLW_Debug
{
	static const bool ENABLED = true;

	static void Say(string msg)
	{
		Print("CLW: " + msg, LogLevel.NORMAL);
		if (!ENABLED || !Replication.IsServer())
			return;

		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int id : ids)
		{
			if (id == SCR_PlayerController.GetLocalPlayerId())
			{
				CLW_Debug.Show(msg);	// listen server host: no RPC needed
				continue;
			}

			SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(id));
			if (controller)
				controller.CLW_RpcShow(msg);
		}
	}

	// Local display; also called by the client RPC.
	static void Show(string msg)
	{
		SCR_ChatPanelManager chat = SCR_ChatPanelManager.GetInstance();
		if (chat)
			chat.ShowHelpMessage("CLW: " + msg);
	}
}

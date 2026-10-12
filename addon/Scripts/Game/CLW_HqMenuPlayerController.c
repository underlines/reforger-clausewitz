// RPC glue of the HQ menu (M4 WP4), same pattern as CLW_SetupPlayerController.c / CLW_RequestPlayerController.c.
// Client -> server: open (terminal position or Zero for the chat command), button action. Server -> owner: the menu payload.
// The listen-server host never uses the RPCs (CLW_HqMenuServer calls CLW_HqMenuClient directly).
modded class SCR_PlayerController
{
	protected static bool s_bClwHqMenuChatRegistered;

	// ---- client -> server ----

	void CLW_HqMenuOpen(vector terminalPos)
	{
		CLW_HqMenuClient.SetTerminal(terminalPos);
		if (Replication.IsServer())
		{
			CLW_HqMenuServer.Open(GetPlayerId(), terminalPos);
			return;
		}

		Rpc(CLW_RpcAsk_HqMenuOpen, terminalPos);
	}

	void CLW_HqMenuAction(string action, string arg, vector terminalPos)
	{
		if (Replication.IsServer())
		{
			CLW_HqMenuServer.Action(GetPlayerId(), action, arg, terminalPos);
			return;
		}

		Rpc(CLW_RpcAsk_HqMenuAction, action, arg, terminalPos);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_RpcAsk_HqMenuOpen(vector terminalPos)
	{
		CLW_HqMenuServer.Open(GetPlayerId(), terminalPos);	// the sender: never a player id from the client
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_RpcAsk_HqMenuAction(string action, string arg, vector terminalPos)
	{
		CLW_HqMenuServer.Action(GetPlayerId(), action, arg, terminalPos);
	}

	// ---- server -> owning client ----

	void CLW_HqMenuSend(string wire)
	{
		Rpc(CLW_RpcAsk_HqMenuData, wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_HqMenuData(string wire)
	{
		CLW_HqMenuClient.Receive(wire);
	}

	// ---- client hook: register /hqmenu once the local controller has an entity ----

	static void CLW_MarkHqMenuChatRegistered()
	{
		s_bClwHqMenuChatRegistered = true;
	}

	override void OnControlledEntityChanged(IEntity from, IEntity to)
	{
		super.OnControlledEntityChanged(from, to);

		if (this != GetGame().GetPlayerController())
			return;

		if (!s_bClwHqMenuChatRegistered)
			CLW_HqMenuClient.RegisterChatCommand();
	}
}

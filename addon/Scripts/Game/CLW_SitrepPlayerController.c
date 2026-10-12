// RPC glue of the faction SITREP: server -> owning client (the SITREP of the player's own faction only), client -> server (resend request).
// Same pattern as CLW_MissionPlayerController.c. The listen-server host never uses the RPCs (CLW_SitrepStore calls the client directly).
modded class SCR_PlayerController
{
	protected static bool s_bClwSitrepChatRegistered;

	// Server side: deliver the wire text to this controller's owner.
	void CLW_SitrepSend(string wire)
	{
		Rpc(CLW_SitrepRpcAsk_Show, wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_SitrepRpcAsk_Show(string wire)
	{
		CLW_SitrepClient.Receive(wire);
	}

	// Client side: ask the server to send the SITREP again.
	void CLW_SitrepRequest()
	{
		if (Replication.IsServer())
		{
			CLW_SitrepStore.Resend(GetPlayerId());
			return;
		}

		Rpc(CLW_SitrepRpcAsk_Request);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_SitrepRpcAsk_Request()
	{
		CLW_SitrepStore.Resend(GetPlayerId());	// the sender: never a player id from the client
	}

	static void CLW_MarkSitrepChatRegistered()
	{
		s_bClwSitrepChatRegistered = true;
	}

	override void OnControlledEntityChanged(IEntity from, IEntity to)
	{
		super.OnControlledEntityChanged(from, to);

		if (this != GetGame().GetPlayerController())
			return;

		if (!s_bClwSitrepChatRegistered)
			CLW_SitrepClient.RegisterChatCommand();

		if (to)
			CLW_SitrepClient.OnLocalSpawn();
	}
}

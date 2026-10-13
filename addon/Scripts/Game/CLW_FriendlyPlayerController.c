// RPC glue of the friendly-units map picture: server -> owning client (the picture of the player's own faction only),
// client -> server (resend request). Same pattern as CLW_SitrepPlayerController.c; the listen-server host never uses the RPCs.
modded class SCR_PlayerController
{
	// Server side: deliver the wire text to this controller's owner.
	void CLW_FriendlySend(string wire)
	{
		Rpc(CLW_FriendlyRpcAsk_Show, wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_FriendlyRpcAsk_Show(string wire)
	{
		CLW_FriendlyMapClient.Receive(wire);
	}

	// Client side: ask the server to send the picture again.
	void CLW_FriendlyRequest()
	{
		if (Replication.IsServer())
		{
			CLW_FriendlyPicture.Resend(GetPlayerId());
			return;
		}

		Rpc(CLW_FriendlyRpcAsk_Request);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_FriendlyRpcAsk_Request()
	{
		CLW_FriendlyPicture.Resend(GetPlayerId());	// the sender: never a player id from the client
	}
}

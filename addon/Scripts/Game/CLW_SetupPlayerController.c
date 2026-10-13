// RPC glue of the war setup menu (phase P5), same pattern as CLW_DebugPlayerController.c.
// Server -> owner: open menu / waiting hint / result / war started. Client -> server: submit, chat command, menu ack.
// The listen-server host never uses RPCs: CLW_SetupServer / the methods below call the local code directly.
modded class SCR_PlayerController
{
	protected static bool s_bClwChatRegistered;

	// ---- server -> owning client (public wrappers, called from CLW_SetupServer) ----

	void CLW_RpcOpenSetup(string wire)
	{
		Rpc(CLW_RpcAsk_OpenSetup, wire);
	}

	void CLW_RpcSetupWait(string ownerName)
	{
		Rpc(CLW_RpcAsk_SetupWait, ownerName);
	}

	void CLW_RpcSetupResult(bool ok, string text)
	{
		Rpc(CLW_RpcAsk_SetupResult, ok, text);
	}

	void CLW_RpcSetupClosed(string text)
	{
		Rpc(CLW_RpcAsk_SetupClosed, text);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_OpenSetup(string wire)
	{
		CLW_SetupClient.Open(wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_SetupWait(string ownerName)
	{
		CLW_SetupClient.Wait(ownerName);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_SetupResult(bool ok, string text)
	{
		CLW_SetupClient.Result(ok, text);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_SetupClosed(string text)
	{
		CLW_SetupClient.Closed(text);
	}

	// ---- client -> server ----

	// Menu submit. battleJson / modeValuesJson: flat JSON objects (CLW_ModeParam.ValuesToJson format).
	void CLW_SubmitSetup(string battleJson, string modeId, string modeValuesJson)
	{
		Print("CLW: setup submit from the menu: mode=" + modeId + " values=" + modeValuesJson + " battle=" + battleJson, LogLevel.NORMAL);
		if (Replication.IsServer())
		{
			string error = CLW_SetupServer.Submit(GetPlayerId(), battleJson, modeId, modeValuesJson);
			CLW_SetupClient.Result(error.IsEmpty(), error);
			return;
		}

		Rpc(CLW_RpcAsk_SubmitSetup, battleJson, modeId, modeValuesJson);
	}

	// The menu is open on this client (server stops re-sending). The sender id is GetPlayerId(), never a parameter.
	void CLW_SendSetupAck()
	{
		if (Replication.IsServer())
		{
			CLW_SetupServer.OnMenuAck(GetPlayerId());
			return;
		}

		Rpc(CLW_RpcAsk_SetupAck);
	}

	// Chat command text after "/war" (see CLW_SetupClient.OnChatCommand).
	void CLW_SendWarCommand(string args)
	{
		if (Replication.IsServer())
		{
			string reply = CLW_SetupServer.ChatCommand(GetPlayerId(), args);
			if (!reply.IsEmpty())
				CLW_Debug.Show(reply);
			return;
		}

		Rpc(CLW_RpcAsk_WarCommand, args);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_RpcAsk_SubmitSetup(string battleJson, string modeId, string modeValuesJson)
	{
		int playerId = GetPlayerId();	// the sender: never trust a player id from the client
		string error = CLW_SetupServer.Submit(playerId, battleJson, modeId, modeValuesJson);
		CLW_RpcSetupResult(error.IsEmpty(), error);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_RpcAsk_SetupAck()
	{
		CLW_SetupServer.OnMenuAck(GetPlayerId());
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_RpcAsk_WarCommand(string args)
	{
		string reply = CLW_SetupServer.ChatCommand(GetPlayerId(), args);
		if (!reply.IsEmpty())
			CLW_RpcShow(reply);
	}

	// ---- client hook: register the /war chat command (also done by CLW_SetupClient.Open / Wait for players without a character) ----

	override void OnControlledEntityChanged(IEntity from, IEntity to)
	{
		super.OnControlledEntityChanged(from, to);

		if (!s_bClwChatRegistered && this == GetGame().GetPlayerController())
			CLW_SetupClient.RegisterChatCommand();
	}

	static bool CLW_IsChatRegistered()
	{
		return s_bClwChatRegistered;
	}

	static void CLW_MarkChatRegistered()
	{
		s_bClwChatRegistered = true;
	}
}

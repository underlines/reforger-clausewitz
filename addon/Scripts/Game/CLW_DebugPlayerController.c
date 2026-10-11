// Server -> owning client delivery of CLW_Debug.Say text.
modded class SCR_PlayerController
{
	// Server side: send msg to this controller's owner.
	void CLW_RpcShow(string msg)
	{
		Rpc(CLW_RpcAsk_Show, msg);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_Show(string msg)
	{
		CLW_Debug.Show(msg);
	}
}

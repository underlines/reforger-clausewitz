// RPC glue of the group mirror (CLW_SquadGroups): server -> owning client "register / unregister this HQ squad locally".
// The listen-server host never receives it (CLW_SquadGroups.MirrorTo skips the local player).
modded class SCR_PlayerController
{
	void CLW_GrpMirror(RplId groupRplId, int groupId, bool registered)
	{
		Rpc(CLW_GrpRpcAsk_Mirror, groupRplId, groupId, registered);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_GrpRpcAsk_Mirror(RplId groupRplId, int groupId, bool registered)
	{
		CLW_SquadGroups.MirrorLocal(groupRplId, groupId, registered, 0);
	}
}

// War start on other factions than the setup sides (e.g. RHS instead of US/USSR): the players already in the game must change faction
// through the vanilla owner path (RequestFaction -> server -> response), otherwise their running client keeps the old faction and its
// deploy menu shows the old loadouts with every spawn point disabled (DS e2e 2026-10-12; a rejoining client was fine).
modded class SCR_PlayerController
{
	// Server: ask this player's client to request the faction itself.
	void CLW_SwitchFaction_S(int factionIndex)
	{
		RplComponent rpl = RplComponent.Cast(FindComponent(RplComponent));
		if (!rpl || rpl.IsOwner())
		{
			CLW_RequestOwnFaction(factionIndex);	// listen server / Workbench: the server owns this controller
			return;
		}

		Rpc(CLW_RpcSwitchFaction_O, factionIndex);
	}

	// Server: this player's client applies the war sides (playable/hostile) locally. Sent before CLW_SwitchFaction_S (same reliable channel).
	void CLW_SyncSides_S(string keyA, string keyB)
	{
		RplComponent rpl = RplComponent.Cast(FindComponent(RplComponent));
		if (!rpl || rpl.IsOwner())
			return;	// listen server / Workbench: the server already holds the state

		Rpc(CLW_RpcSyncSides_O, keyA, keyB);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcSyncSides_O(string keyA, string keyB)
	{
		CLW_Sides.ApplySidesLocal(keyA, keyB);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcSwitchFaction_O(int factionIndex)
	{
		CLW_RequestOwnFaction(factionIndex);
	}

	protected void CLW_RequestOwnFaction(int factionIndex)
	{
		FactionManager factionManager = GetGame().GetFactionManager();
		SCR_PlayerFactionAffiliationComponent affiliation = SCR_PlayerFactionAffiliationComponent.Cast(FindComponent(SCR_PlayerFactionAffiliationComponent));
		if (!factionManager || !affiliation)
		{
			Print("CLW: sides faction switch: no faction manager / affiliation component on this controller", LogLevel.WARNING);
			return;
		}

		Faction faction = factionManager.GetFactionByIndex(factionIndex);
		bool sent = affiliation.RequestFaction(faction);
		Print(string.Format("CLW: sides faction switch requested by the client: %1 (sent %2)", faction, sent), LogLevel.NORMAL);
	}
}

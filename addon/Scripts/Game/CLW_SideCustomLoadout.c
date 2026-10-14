// Spawn-screen entry "my saved loadout" (loadouts plan R1 entry, R3): one per war side in CLW_LoadoutManager.et. Subclass of the vanilla
// player-arsenal loadout (NOT a modded one, see reforger-script-dev) whose faction comes from the side at runtime, so it covers pack factions
// (RHS_USAF, RHS_AFRF ...) without an entry per faction and satisfies mods that look for a SCR_PlayerArsenalLoadout of the player's faction.
//
// Behaviour is the vanilla one: the entry is available when the player has a saved arsenal loadout (SCR_ArsenalManagerComponent), the spawn
// applies it in SCR_PlayerArsenalLoadout.OnLoadoutSpawned (it deletes a saved kit of another faction). The vanilla saved slot is kept in sync
// with the custom record by CLW_ArsenalSaveHook / CLW_CustomLoadouts (G2). Only the faction, the base prefab and the hidden state differ:
// both come from CLW_LoadoutSlots (slot 0 of the side), which the server fills at war start and sends to the clients.
[BaseContainerProps(configRoot: true), BaseContainerCustomTitleField("m_sLoadoutName")]
class CLW_SideCustomLoadout : SCR_PlayerArsenalLoadout
{
	[Attribute("0", UIWidgets.EditBox, "War side this entry belongs to: 0 = side A, 1 = side B")]
	int m_iSide;

	// Both machines: list the entry for its side's faction, or hide it.
	void Bind()
	{
		if (CLW_LoadoutSlots.IsVisible(m_iSide, 0))
			m_sAffiliatedFaction = CLW_LoadoutSlots.SideKey(m_iSide);
		else
			m_sAffiliatedFaction = "";
	}

	override ResourceName GetLoadoutResource()
	{
		return CLW_LoadoutSlots.Prefab(m_iSide, 0);
	}

	override ResourceName GetDefaultLoadoutResource()
	{
		return CLW_LoadoutSlots.Prefab(m_iSide, 0);
	}

	override bool IsLoadoutAvailable(int playerId)
	{
		if (!CLW_LoadoutSlots.IsVisible(m_iSide, 0))
			return false;

		return super.IsLoadoutAvailable(playerId);
	}

	override bool IsLoadoutAvailableClient()
	{
		if (!CLW_LoadoutSlots.IsVisible(m_iSide, 0))
			return false;

		return super.IsLoadoutAvailableClient();
	}
}

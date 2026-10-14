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

	// Vanilla applies the kit strictly to this entry's base prefab: a kit captured on another soldier (squad leader saved, rifleman base) fails on
	// a storage id, and vanilla then DELETES the saved slot, so the player spawned with the default gear and "Load my loadout" went grey (DS e2e
	// 2026-10-15, RHS_USAF). When the vanilla slot is gone after super, the player's record is applied with the tolerant CLW_Kits.Apply and put
	// back into the vanilla slot.
	override void OnLoadoutSpawned(GenericEntity pOwner, int playerId)
	{
		super.OnLoadoutSpawned(pOwner, playerId);
		if (!pOwner || !Replication.IsServer() || !CLW_ArsenalSaveHook.VanillaKit(playerId).IsEmpty())
			return;

		string factionKey = CLW_LoadoutSlots.SideKey(m_iSide);
		CLW_CustomLoadoutRec rec = CLW_CustomLoadouts.Get(CLW_CustomLoadouts.IdentityOf(playerId), factionKey);
		if (!rec)
			return;

		array<string> missing = {};
		bool applied = CLW_Kits.Apply(pOwner, rec.loadout, "custom spawn player " + playerId, missing);
		SCR_ArsenalManagerComponent manager;
		bool restored;
		if (SCR_ArsenalManagerComponent.GetArsenalManager(manager))
			restored = manager.CLW_RestoreLoadout(playerId, factionKey, rec.loadout);

		Print(string.Format("CLW: loadouts custom spawn player %1 %2: vanilla apply dropped the kit, re-applied=%3 missing=%4 slot restored=%5", playerId, factionKey, applied, missing.Count(), restored), LogLevel.NORMAL);
	}
}

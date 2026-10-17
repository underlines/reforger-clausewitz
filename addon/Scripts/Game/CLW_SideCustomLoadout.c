// Spawn-screen entry "my saved loadout" (loadouts plan R1 entry, R3): one per war side in CLW_LoadoutManager.et. Subclass of the vanilla
// player-arsenal loadout (NOT a modded one, see reforger-script-dev) whose faction comes from the side at runtime, so it covers pack factions
// (RHS_USAF, RHS_AFRF ...) without an entry per faction and satisfies mods that look for a SCR_PlayerArsenalLoadout of the player's faction.
//
// Availability does NOT use vanilla's saved-loadout state (Workshop loadout editors replace it): the server checks CLW_CustomLoadouts, the
// client the set of faction keys the server sent (CLW_LoadoutClient). The spawn always applies our record (OnLoadoutSpawned). The vanilla
// saved slot is only kept in sync as a compatibility nicety (CLW_ArsenalSaveHook). Faction, base prefab and hidden state come from
// CLW_LoadoutSlots (slot 0 of the side), which the server fills at war start and sends to the clients. No WCS symbols are referenced, so the
// class works whether or not a loadout-editor mod added fields (m_sWCSSlotId ...) to SCR_PlayerArsenalLoadout.
[BaseContainerProps(configRoot: true), BaseContainerCustomTitleField("m_sLoadoutName")]
class CLW_SideCustomLoadout : SCR_PlayerArsenalLoadout
{
	[Attribute("0", UIWidgets.EditBox, "War side this entry belongs to: 0 = side A, 1 = side B")]
	int m_iSide;

	// Both machines: list the entry for its side's faction, or hide it.
	// The base prefab also goes into the vanilla field (see CLW_ServerLoadout.Bind).
	void Bind()
	{
		m_sLoadoutResource = CLW_LoadoutSlots.Prefab(m_iSide, 0);
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

	// Server: our own record decides (never the vanilla slot, which loadout-editor mods replace).
	override bool IsLoadoutAvailable(int playerId)
	{
		if (!CLW_LoadoutSlots.IsVisible(m_iSide, 0))
			return false;

		return CLW_CustomLoadouts.Get(CLW_CustomLoadouts.IdentityOf(playerId), CLW_LoadoutSlots.SideKey(m_iSide)) != null;
	}

	// Client: the server-sent set of faction keys (CLW_LoadoutClient), not SCR_ArsenalManagerComponent.GetLocalPlayerLoadoutAvailable().
	override bool IsLoadoutAvailableClient()
	{
		if (!CLW_LoadoutSlots.IsVisible(m_iSide, 0))
			return false;

		return CLW_LoadoutClient.Has(CLW_LoadoutSlots.SideKey(m_iSide));
	}

	// super sets the faction affiliation (and vanilla / WCS may try their own apply, strictly to this entry's base prefab: a kit captured on
	// another soldier fails on a storage id and vanilla then DELETES its saved slot, DS e2e 2026-10-15). Whatever happened there, our record is
	// then applied with the tolerant CLW_Kits.Apply (whole gear replaced). The vanilla slot is only re-synced as a compatibility nicety; our
	// record in CLW_CustomLoadouts is never touched by a vanilla failure path.
	override void OnLoadoutSpawned(GenericEntity pOwner, int playerId)
	{
		super.OnLoadoutSpawned(pOwner, playerId);
		if (!pOwner || !Replication.IsServer())
			return;

		string factionKey = CLW_LoadoutSlots.SideKey(m_iSide);
		CLW_CustomLoadoutRec rec = CLW_CustomLoadouts.Get(CLW_CustomLoadouts.IdentityOf(playerId), factionKey);
		if (!rec)
		{
			Print(string.Format("CLW: loadouts custom spawn player %1 %2: no record, nothing applied", playerId, factionKey), LogLevel.NORMAL);
			return;
		}

		array<string> missing = {};
		bool applied = CLW_Kits.Apply(pOwner, rec.loadout, "custom spawn player " + playerId, missing);
		SCR_ArsenalManagerComponent manager;
		bool restored;
		if (SCR_ArsenalManagerComponent.GetArsenalManager(manager))
			restored = manager.CLW_RestoreLoadout(playerId, factionKey, rec.loadout);

		Print(string.Format("CLW: loadouts custom spawn player %1 %2: applied=%3 missing=%4 vanilla slot synced=%5", playerId, factionKey, applied, missing.Count(), restored), LogLevel.NORMAL);
		CLW_LoadoutClient.Push(playerId);
	}
}

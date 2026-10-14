// Spawn-screen entry "server loadout" (loadouts plan R3): 7 fixed slots per war side in CLW_LoadoutManager.et. Subclass of the vanilla
// faction loadout, NOT a modded one (a modded [BaseContainerProps] class that prefabs reference becomes "Unknown class", see reforger-script-dev).
//
// Faction, name and base prefab are not in the prefab: they come from CLW_LoadoutSlots, which the server fills at war start (library entry
// of the faction with order == slot, else the R2 role prefab for the default role order, else hidden) and sends to the clients. The entry is
// listed by the deploy menu for the side's faction because Bind() writes m_sAffiliatedFaction (what SCR_LoadoutManager filters by); a hidden
// slot, and every slot before the war, has an empty faction.
//
// Spawn: the base prefab spawns; OnLoadoutSpawned (server, SCR_BaseGameMode.OnPlayerSpawnFinalize_S) applies the library kit string with
// CLW_Kits.Apply. Strings never leave the server. With the setup toggle serverKitCost on (default) the kit costs supplies like an arsenal kit
// (CLW_Kits.Cost, charged at the side's HQ stock); when the side cannot pay the player keeps the plain base prefab. AI never uses this class.
[BaseContainerProps(configRoot: true), BaseContainerCustomTitleField("m_sLoadoutName")]
class CLW_ServerLoadout : SCR_FactionPlayerLoadout
{
	[Attribute("0", UIWidgets.EditBox, "War side this slot belongs to: 0 = side A, 1 = side B")]
	int m_iSide;

	[Attribute("1", UIWidgets.EditBox, "Spawn slot 1..7 (the library entry with this order fills it)")]
	int m_iSlot;

	// Both machines: list the slot for its side's faction, or hide it.
	void Bind()
	{
		if (CLW_LoadoutSlots.IsVisible(m_iSide, m_iSlot))
			m_sAffiliatedFaction = CLW_LoadoutSlots.SideKey(m_iSide);
		else
			m_sAffiliatedFaction = "";
	}

	override string GetLoadoutName()
	{
		string name = CLW_LoadoutSlots.Name(m_iSide, m_iSlot);
		if (name.IsEmpty())
			return super.GetLoadoutName();

		return name;
	}

	override ResourceName GetLoadoutResource()
	{
		return CLW_LoadoutSlots.Prefab(m_iSide, m_iSlot);
	}

	override ResourceName GetDefaultLoadoutResource()
	{
		return CLW_LoadoutSlots.Prefab(m_iSide, m_iSlot);
	}

	override bool IsLoadoutAvailable(int playerId)
	{
		return CLW_LoadoutSlots.IsVisible(m_iSide, m_iSlot);
	}

	override bool IsLoadoutAvailableClient()
	{
		return CLW_LoadoutSlots.IsVisible(m_iSide, m_iSlot);
	}

	override void OnLoadoutSpawned(GenericEntity pOwner, int playerId)
	{
		super.OnLoadoutSpawned(pOwner, playerId);	// affiliates the character with the side's faction
		if (pOwner && Replication.IsServer())
			ApplyKit(pOwner, playerId);
	}

	// Server: the library kit of this slot onto the freshly spawned character (library slots only; role slots are the plain prefab).
	protected void ApplyKit(IEntity character, int playerId)
	{
		string factionKey = CLW_LoadoutSlots.SideKey(m_iSide);
		string slotText = CLW_LoadoutSlots.Source(m_iSide, m_iSlot);
		if (slotText != CLW_LoadoutSlots.SRC_LIBRARY)
		{
			Print("CLW: loadouts spawn player " + playerId.ToString() + " side " + m_iSide.ToString() + " slot " + m_iSlot.ToString() + " faction=" + factionKey + " source=" + slotText + ": base prefab only", LogLevel.NORMAL);
			return;
		}

		CLW_LibraryEntry entry = CLW_LoadoutLibrary.Find(CLW_LoadoutSlots.EntryId(m_iSide, m_iSlot));
		if (!entry || entry.loadout.IsEmpty())
		{
			Print("CLW: loadouts spawn player " + playerId.ToString() + " slot " + m_iSlot.ToString() + " faction=" + factionKey + ": library entry gone, base prefab only", LogLevel.WARNING);
			return;
		}

		int cost;
		if (playerId > 0 && CLW_Capabilities.Has("serverKitCost"))
		{
			cost = Math.Max(0, CLW_Kits.Cost(entry.loadout, factionKey));	// -1 = unknown = free
			if (cost > 0 && !CLW_SupplyStore.TrySpend(factionKey, CLW_Services.BASE_HQ, cost))
			{
				int available = CLW_SupplyStore.Get(factionKey, CLW_Services.BASE_HQ);
				Print("CLW: loadouts spawn player " + playerId.ToString() + " slot " + m_iSlot.ToString() + " '" + entry.name + "': not enough supplies (" + cost.ToString() + " needed, " + available.ToString() + " available), base prefab only", LogLevel.NORMAL);
				CLW_LoadoutSync.Tell(playerId, CLW_Loc.W("#CLW-Loadouts_NoSupplies", entry.name, CLW_Loc.N(cost), CLW_Loc.N(available)));
				return;
			}
		}

		array<string> missing = {};
		string tag = "player " + playerId.ToString() + " slot " + m_iSlot.ToString();
		bool applied = CLW_Kits.Apply(character, entry.loadout, tag, missing);
		if (!applied && cost > 0)
			CLW_SupplyStore.Refund(factionKey, CLW_Services.BASE_HQ, cost);

		Print("CLW: loadouts spawn player " + playerId.ToString() + " side " + m_iSide.ToString() + " slot " + m_iSlot.ToString() + " faction=" + factionKey + " kit='" + entry.name + "' applied=" + applied.ToString() + " cost=" + cost.ToString() + " missing=" + missing.Count().ToString(), LogLevel.NORMAL);
		if (applied && cost > 0)
			CLW_LoadoutSync.Tell(playerId, CLW_Loc.W("#CLW-Loadouts_KitCost", entry.name, CLW_Loc.N(cost)));
	}
}

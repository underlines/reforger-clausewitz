// Kit core (loadouts work package G2, contract: dev/notes/loadouts-impl.md). A "kit" is the vanilla arsenal loadout string:
// {"faction":"<key>","arsenalLoadout":{"prefab":<character>,"storages":[{"id":..,"slots":{"<slot>":{"prefab":..,"storages":[..]}}}],labels,activeWeapon}}
// exactly what SCR_ArsenalManagerComponent.SetPlayerArsenalLoadout stores and SCR_PlayerArsenalLoadout.OnLoadoutSpawned applies, so a kit
// captured here is a valid vanilla saved loadout and the other way round.
//   Capture   vanilla SCR_PlayerArsenalLoadout.ReadLoadoutString of a living character (player or AI).
//   Apply     vanilla SCR_PlayerArsenalLoadout.ApplyLoadoutString when every item prefab loads; a tolerant copy of the vanilla walk when some
//             do not (those items are skipped, their nested data consumed, the slot stays empty, 'missing' lists them). Server only.
//             Old gear is removed by the inventory manager's TryDeleteItem (deleted, not dropped), like the vanilla spawn does.
//   Items     every item prefab of the kit, found by scanning the string for "prefab" keys (no JSON depth limit, never throws).
//   Validate  R6: ok | degraded | invalid. An item is "missing" when its prefab does not load or it is not in the faction's current arsenal
//             pool (SCR_EntityCatalogManagerComponent.GetEntryWithPrefabFromFactionCatalog ITEM, which CLW_ArsenalPool extends with the kin
//             factions while a war runs; call it during a war, else only the faction's own catalog counts). No exemption for items that come
//             with the character prefab: vanilla refuses to save a loadout with any item outside the faction catalogs (CanSaveLoadout), so a
//             saved kit passes. "Primary weapon survives" = at least one surviving item whose arsenal data has item mode WEAPON /
//             WEAPON_VARIANTS (approximation: a pistol-only remainder also counts).
//   Cost      supply cost of the kit, vanilla per-item cost (SCR_PlayerArsenalLoadout.ComputeSuppliesCost), -1 when unknown.
// Log prefix "CLW: loadouts".

// Counters of one tolerant apply (internal).
class CLW_KitStats
{
	int m_iApplied;
	int m_iSkipped;
	ref array<string> m_aFailed = {};
}

class CLW_Kits
{
	static const string STATUS_OK = "ok";
	static const string STATUS_DEGRADED = "degraded";
	// Items of the last Validate call that are not in the faction's arsenal pool (log only).
	static int s_iLastOutsidePool;
	static const string STATUS_INVALID = "invalid";
	static const int MAX_PREFAB_CHARS = 1024;

	// ---- capture ----

	// Vanilla loadout string of a living character; "" on failure.
	static string Capture(IEntity character)
	{
		if (!character)
			return "";

		EnsureComponentsToCheck();
		string factionKey = SCR_PlayerArsenalLoadout.ARSENALLOADOUT_FACTIONKEY_NONE;
		SCR_ChimeraCharacter chimera = SCR_ChimeraCharacter.Cast(character);
		if (chimera)
		{
			Faction charFaction = chimera.GetFaction();
			if (charFaction)
				factionKey = charFaction.GetFactionKey();
		}

		JsonSaveContext context = new JsonSaveContext();
		if (!context.WriteValue(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_FACTION_KEY, factionKey))
			return "";

		if (!SCR_PlayerArsenalLoadout.ReadLoadoutString(character, context))
			return "";

		return context.SaveToString();
	}

	// The storage component list vanilla fills in SCR_ArsenalManagerComponent.OnPostInit; a game mode without that component leaves it null.
	protected static void EnsureComponentsToCheck()
	{
		if (!SCR_PlayerArsenalLoadout.ARSENALLOADOUT_COMPONENTS_TO_CHECK || SCR_PlayerArsenalLoadout.ARSENALLOADOUT_COMPONENTS_TO_CHECK.IsEmpty())
			SCR_ArsenalManagerComponent.GetArsenalLoadoutComponentsToCheck(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_COMPONENTS_TO_CHECK);
	}

	// ---- string scan (no JSON parse: the kit nests deeper than the tiny reader allows, and garbage must not matter) ----

	// Every value of a "prefab" key in order of appearance (the first one is the character prefab).
	protected static void ScanPrefabs(string loadout, notnull array<string> all)
	{
		int len = loadout.Length();
		int pos = 0;
		while (pos < len)
		{
			int at = loadout.IndexOfFrom(pos, "\"prefab\"");
			if (at < 0)
				return;

			int idx = at + 8;
			pos = idx;
			while (idx < len && (loadout.Get(idx) == " " || loadout.Get(idx) == "\t" || loadout.Get(idx) == "\n" || loadout.Get(idx) == "\r"))
				idx++;

			if (idx >= len || loadout.Get(idx) != ":")
				continue;

			idx++;
			while (idx < len && (loadout.Get(idx) == " " || loadout.Get(idx) == "\t" || loadout.Get(idx) == "\n" || loadout.Get(idx) == "\r"))
				idx++;

			if (idx >= len || loadout.Get(idx) != "\"")
				continue;

			int endQuote = loadout.IndexOfFrom(idx + 1, "\"");
			if (endQuote < 0)
				return;

			int valueLen = endQuote - idx - 1;
			pos = endQuote + 1;
			if (valueLen <= 0 || valueLen > MAX_PREFAB_CHARS)
				continue;

			// vanilla writes bare GUIDs ("FAE8B9F7CEA49696", DS test 2026-10-14); Resource.Load needs "{GUID}"
			string value = loadout.Substring(idx + 1, valueLen);
			if (value.IndexOf("{") < 0)
				value = "{" + value + "}";

			all.Insert(value);
		}
	}

	// The character prefab the kit was saved from ("" when the string has none).
	static string BasePrefab(string loadout)
	{
		array<string> all = {};
		ScanPrefabs(loadout, all);
		if (all.IsEmpty())
			return "";

		return all[0];
	}

	// The faction key stored in the kit ("" when unreadable).
	static string FactionOf(string loadout)
	{
		if (loadout.IsEmpty())
			return "";

		JsonLoadContext context = new JsonLoadContext(false);
		if (!context.LoadFromString(loadout))
			return "";

		string factionKey;
		if (!context.ReadValue(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_FACTION_KEY, factionKey))
			return "";

		return factionKey;
	}

	// Item prefabs referenced by a kit, unique, in kit order (the character prefab itself is not an item).
	static void Items(string loadout, notnull array<string> items)
	{
		array<string> all = {};
		ScanPrefabs(loadout, all);
		for (int i = 1; i < all.Count(); i++)
		{
			if (items.Find(all[i]) < 0)
				items.Insert(all[i]);
		}
	}

	// Same as Items, plus how often each prefab occurs in the kit (counts[i] belongs to items[i]); for the dashboard item details.
	static void ItemsCounted(string loadout, notnull array<string> items, notnull array<int> counts)
	{
		array<string> all = {};
		ScanPrefabs(loadout, all);
		for (int i = 1; i < all.Count(); i++)
		{
			int at = items.Find(all[i]);
			if (at < 0)
			{
				items.Insert(all[i]);
				counts.Insert(1);
			}
			else
			{
				counts[at] = counts[at] + 1;
			}
		}
	}

	// ---- validation (R6) ----

	// "ok" | "degraded" | "invalid"; missing = prefabs that do not load or are outside the faction's current arsenal pool.
	static string Validate(string loadout, string factionKey, notnull array<string> missing)
	{
		missing.Clear();
		s_iLastOutsidePool = 0;
		if (loadout.IsEmpty())
			return STATUS_INVALID;

		JsonLoadContext context = new JsonLoadContext(false);
		if (!context.LoadFromString(loadout))
			return STATUS_INVALID;

		string storedFaction;
		if (!context.ReadValue(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_FACTION_KEY, storedFaction))
			return STATUS_INVALID;

		// vanilla drops a saved loadout of another faction at spawn (SCR_PlayerArsenalLoadout.OnLoadoutSpawned)
		if (storedFaction != factionKey)
			return STATUS_INVALID;

		if (!context.StartObject(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_KEY))
			return STATUS_INVALID;

		array<string> items = {};
		Items(loadout, items);
		if (items.IsEmpty() || BasePrefab(loadout).IsEmpty())
			return STATUS_INVALID;

		FactionManager factionManager = GetGame().GetFactionManager();
		if (!factionManager)
			return STATUS_INVALID;

		SCR_Faction faction = SCR_Faction.Cast(factionManager.GetFactionByKey(factionKey));
		if (!faction)
			return STATUS_INVALID;

		SCR_EntityCatalogManagerComponent catalogs = SCR_EntityCatalogManagerComponent.GetInstance();
		bool weaponSurvives;
		int surviving;
		foreach (string item : items)
		{
			ResourceName prefab = item;
			Resource resource = Resource.Load(prefab);
			if (!resource || !resource.IsValid())
			{
				missing.Insert(item);
				continue;
			}

			surviving++;
			if (!catalogs)
				continue;

			// Pool membership is informational only (DS test 2026-10-14): a kit also lists nested parts (magazines in weapons, attachments,
			// uniform parts) and the character's spawn gear, which are often no ITEM catalog entries, so a normal kit failed with 5-51
			// "unavailable" items. Missing = the prefab no longer loads (mod pack changed).
			SCR_EntityCatalogEntry entry = catalogs.GetEntryWithPrefabFromFactionCatalog(EEntityCatalogType.ITEM, prefab, faction);
			if (!entry)
			{
				s_iLastOutsidePool++;
				continue;
			}

			SCR_ArsenalItem data = SCR_ArsenalItem.Cast(entry.GetEntityDataOfType(SCR_ArsenalItem));
			if (data)
			{
				SCR_EArsenalItemMode mode = data.GetItemMode();
				if (mode == SCR_EArsenalItemMode.WEAPON || mode == SCR_EArsenalItemMode.WEAPON_VARIANTS)
					weaponSurvives = true;
			}
		}

		if (missing.IsEmpty())
			return STATUS_OK;

		if (surviving > 0)
			return STATUS_DEGRADED;

		return STATUS_INVALID;
	}

	// ---- cost ----

	// Supply cost of the kit with the default cost type, -1 if unknown.
	static int Cost(string loadout, string factionKey)
	{
		return CostAt(loadout, factionKey, SCR_EArsenalSupplyCostType.DEFAULT);
	}

	// Same with the cost type of a specific arsenal (SCR_ArsenalComponent.GetSupplyCostType).
	static int CostAt(string loadout, string factionKey, SCR_EArsenalSupplyCostType costType)
	{
		if (loadout.IsEmpty())
			return -1;

		FactionManager factionManager = GetGame().GetFactionManager();
		if (!factionManager)
			return -1;

		SCR_Faction faction = SCR_Faction.Cast(factionManager.GetFactionByKey(factionKey));
		if (!faction)
			return -1;

		JsonLoadContext context = new JsonLoadContext(false);
		if (!context.LoadFromString(loadout))
			return -1;

		if (!context.StartObject(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_KEY))
			return -1;

		SCR_ArsenalPlayerLoadout scratch = new SCR_ArsenalPlayerLoadout();
		SCR_PlayerArsenalLoadout.ComputeSuppliesCost(context, faction, scratch, costType);
		return Math.Round(scratch.suppliesCost);
	}

	// ---- apply ----

	// Replaces the whole gear of a living character with the kit. Server only. tag = log context ("ai US-3 slot 2", "player 5 load").
	static bool Apply(IEntity character, string loadout, string tag, notnull array<string> missing)
	{
		if (!character || loadout.IsEmpty())
		{
			Print("CLW: loadouts apply " + tag + ": nothing to do (no character or empty kit)", LogLevel.WARNING);
			return false;
		}

		if (!Replication.IsServer())
		{
			Print("CLW: loadouts apply " + tag + ": refused, not the server", LogLevel.WARNING);
			return false;
		}

		InventoryStorageManagerComponent manager = InventoryStorageManagerComponent.Cast(character.FindComponent(InventoryStorageManagerComponent));
		if (!manager)
		{
			Print("CLW: loadouts apply " + tag + ": FAILED, the character has no inventory storage manager", LogLevel.WARNING);
			return false;
		}

		EnsureComponentsToCheck();
		array<string> items = {};
		Items(loadout, items);
		map<string, bool> skip = new map<string, bool>();
		foreach (string item : items)
		{
			ResourceName prefab = item;
			Resource resource = Resource.Load(prefab);
			if (!resource || !resource.IsValid())
			{
				missing.Insert(item);
				skip.Set(item, true);
			}
		}

		bool ok;
		string how = "vanilla";
		CLW_KitStats stats = new CLW_KitStats();
		if (skip.IsEmpty())
		{
			ok = ApplyVanilla(character, loadout);
			stats.m_iApplied = items.Count();
			if (!ok)
			{
				Print("CLW: loadouts apply " + tag + ": the vanilla apply failed, retrying with the tolerant walk", LogLevel.WARNING);
				how = "tolerant (after vanilla failure)";
				stats = new CLW_KitStats();
				ok = ApplyTolerant(character, loadout, manager, skip, stats);
			}
		}
		else
		{
			how = "tolerant";
			ok = ApplyTolerant(character, loadout, manager, skip, stats);
		}

		foreach (string failed : stats.m_aFailed)
		{
			if (missing.Find(failed) < 0)
				missing.Insert(failed);
		}

		string primary = PrimaryWeaponName(character);
		Print(string.Format("CLW: loadouts apply %1: ok=%2 via=%3 items=%4 applied=%5 skipped=%6 missing=%7 primary=%8", tag, ok, how, items.Count(), stats.m_iApplied, stats.m_iSkipped, missing.Count(), primary), LogLevel.NORMAL);
		foreach (string gone : missing)
		{
			Print("CLW: loadouts apply " + tag + ": missing " + gone, LogLevel.NORMAL);
		}

		return ok;
	}

	// Prefab of the weapon in weapon slot 0 (the primary), "none" when the character has none.
	protected static string PrimaryWeaponName(IEntity character)
	{
		BaseWeaponManagerComponent weapons = BaseWeaponManagerComponent.Cast(character.FindComponent(BaseWeaponManagerComponent));
		if (!weapons)
			return "none";

		array<WeaponSlotComponent> slots = {};
		weapons.GetWeaponsSlots(slots);
		foreach (WeaponSlotComponent slot : slots)
		{
			if (slot.GetWeaponSlotIndex() != 0)
				continue;

			IEntity weapon = slot.GetWeaponEntity();
			if (weapon)
				return SCR_ResourceNameUtils.GetPrefabName(weapon);
		}

		return "none";
	}

	protected static bool ApplyVanilla(IEntity character, string loadout)
	{
		JsonLoadContext context = new JsonLoadContext();
		if (!context.LoadFromString(loadout))
			return false;

		string storedFaction;
		if (!context.ReadValue(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_FACTION_KEY, storedFaction))
			return false;

		return SCR_PlayerArsenalLoadout.ApplyLoadoutString(character, context);
	}

	protected static bool ApplyTolerant(IEntity character, string loadout, InventoryStorageManagerComponent manager, map<string, bool> skip, CLW_KitStats stats)
	{
		JsonLoadContext context = new JsonLoadContext();
		if (!context.LoadFromString(loadout))
			return false;

		string storedFaction;
		if (!context.ReadValue(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_FACTION_KEY, storedFaction))
			return false;

		if (!context.StartObject(SCR_PlayerArsenalLoadout.ARSENALLOADOUT_KEY))
			return false;

		if (!ApplyEntity(character, context, manager, null, -1, skip, stats))
			return false;

		if (!ApplyCharacterData(character, context))
			return false;

		return context.EndObject();
	}

	// Copy of SCR_PlayerArsenalLoadout.ApplyEntityLoadoutString, tolerant: an unavailable item (skip) or one the storage refuses leaves its
	// slot empty and its nested data is consumed, instead of aborting the whole kit.
	protected static bool ApplyEntity(IEntity owner, LoadContext context, InventoryStorageManagerComponent manager, BaseInventoryStorageComponent parentStorage, int slotId, map<string, bool> skip, CLW_KitStats stats)
	{
		if (parentStorage && slotId != -1)
		{
			ResourceName prefab;
			if (!context.Read(prefab))
				return false;

			if (skip.Contains(prefab))
			{
				if (owner)
					manager.TryDeleteItem(owner);

				stats.m_iSkipped++;
				return SkipNested(context);
			}

			ResourceName currentPrefab = SCR_ResourceNameUtils.GetPrefabName(owner);
			if (prefab != currentPrefab)
			{
				if (owner && !manager.TryDeleteItem(owner))
				{
					stats.m_aFailed.Insert(prefab);
					return SkipNested(context);
				}

				if (!manager.TrySpawnPrefabToStorage(prefab, parentStorage, slotId))
				{
					stats.m_aFailed.Insert(prefab);
					return SkipNested(context);
				}

				owner = parentStorage.Get(slotId);
				if (!owner)
				{
					stats.m_aFailed.Insert(prefab);
					return SkipNested(context);
				}

				stats.m_iApplied++;
			}
		}

		return ApplyStorages(owner, context, manager, skip, stats);
	}

	// Copy of SCR_PlayerArsenalLoadout.ApplyEntityStorageString (tolerant, see ApplyEntity).
	protected static bool ApplyStorages(IEntity owner, LoadContext context, InventoryStorageManagerComponent manager, map<string, bool> skip, CLW_KitStats stats)
	{
		int storageCount;
		context.StartArray("storages", storageCount);
		if (storageCount == 0)
			return true;

		set<BaseInventoryStorageComponent> storageCandidates();
		SCR_PlayerArsenalLoadout.FindStorageComponents(owner, storageCandidates);

		for (int nStorage = 0; nStorage < storageCount; ++nStorage)
		{
			if (!context.StartObject())
				return false;

			string id;
			if (!context.Read(id))
				return false;

			BaseInventoryStorageComponent storage;
			foreach (BaseInventoryStorageComponent candidate : storageCandidates)
			{
				if (ComponentId(owner, candidate) == id)
				{
					storage = candidate;
					break;
				}
			}

			if (!storage)
			{
				// the character prefab lacks this storage: consume its data
				if (!SkipSlots(context))
					return false;

				if (!context.EndObject())
					return false;

				continue;
			}

			map<int, IEntity> slottedItems = new map<int, IEntity>();
			array<InventoryItemComponent> ownedItems = {};
			storage.GetOwnedItems(ownedItems, false);
			foreach (InventoryItemComponent ownedItem : ownedItems)
			{
				slottedItems.Insert(ownedItem.GetParentSlot().GetID(), ownedItem.GetOwner());
			}

			int slotCount = 0;
			if (!context.StartMap("slots", slotCount))
				return false;

			for (int i = 0; i < slotCount; ++i)
			{
				string idxStr;
				if (!context.ReadMapKey(i, idxStr))
					return false;

				const int childSlotId = idxStr.ToInt(-1);
				if (childSlotId == -1)
					return false;

				IEntity existing;
				slottedItems.Take(childSlotId, existing);

				if (!context.StartObject(idxStr))
					return false;

				if (!ApplyEntity(existing, context, manager, storage, childSlotId, skip, stats))
					return false;

				if (!context.EndObject())
					return false;
			}

			if (!context.EndMap())
				return false;

			// whatever the kit does not mention in this storage is deleted (not dropped)
			foreach (int slotKey, IEntity leftover : slottedItems)
			{
				if (leftover && !manager.TryDeleteItem(leftover))
					stats.m_aFailed.Insert("(old item not removable) " + SCR_ResourceNameUtils.GetPrefabName(leftover));
			}

			if (!context.EndObject())
				return false;
		}

		return context.EndArray();
	}

	// Same id the vanilla writer uses for a storage component.
	protected static string ComponentId(IEntity owner, GenericComponent component)
	{
		BaseContainer source = component.GetComponentSource(owner);
		if (!source)
			return "";

		return component.ClassName() + ":" + SCR_ResourceNameUtils.GetPrefabGUID(source.GetResourceName());
	}

	// The "storages" block of an entity whose prefab was already read: consumed, nothing applied (mirror of the vanilla cost walk).
	protected static bool SkipNested(LoadContext context)
	{
		int storageCount;
		context.StartArray("storages", storageCount);
		if (storageCount == 0)
			return true;

		for (int nStorage = 0; nStorage < storageCount; ++nStorage)
		{
			if (!context.StartObject())
				return false;

			string id;
			if (!context.Read(id))
				return false;

			if (!SkipSlots(context))
				return false;

			if (!context.EndObject())
				return false;
		}

		return context.EndArray();
	}

	// The "slots" map of a storage: consumed, nothing applied.
	protected static bool SkipSlots(LoadContext context)
	{
		int slotCount = 0;
		if (!context.StartMap("slots", slotCount))
			return false;

		for (int i = 0; i < slotCount; ++i)
		{
			string idxStr;
			if (!context.ReadMapKey(i, idxStr))
				return false;

			if (!context.StartObject(idxStr))
				return false;

			ResourceName childPrefab;
			if (!context.Read(childPrefab))
				return false;

			if (!SkipNested(context))
				return false;

			if (!context.EndObject())
				return false;
		}

		return context.EndMap();
	}

	// Copy of SCR_PlayerArsenalLoadout.ApplyCharacterDataLoadoutString (character labels + the weapon in hand).
	protected static bool ApplyCharacterData(IEntity owner, LoadContext context)
	{
		SCR_EditableCharacterComponent editableCharacter = SCR_EditableCharacterComponent.Cast(owner.FindComponent(SCR_EditableCharacterComponent));
		if (editableCharacter)
		{
			array<EEditableEntityLabel> loadoutLabels = {};
			if (!context.Read(loadoutLabels))
				return false;

			editableCharacter.SetCustomCharacterLabels_S(loadoutLabels);
		}

		int activeWeaponIdx;
		context.ReadDefault(activeWeaponIdx, -1);
		BaseWeaponManagerComponent weaponManager = BaseWeaponManagerComponent.Cast(owner.FindComponent(BaseWeaponManagerComponent));
		CharacterControllerComponent charController = CharacterControllerComponent.Cast(owner.FindComponent(CharacterControllerComponent));
		if (!weaponManager || !charController)
			return true;

		if (activeWeaponIdx != -1)
		{
			array<WeaponSlotComponent> outSlots = {};
			if (weaponManager.GetWeaponsSlots(outSlots))
			{
				WeaponSlotComponent desiredSlot, fallbackSlot;
				foreach (WeaponSlotComponent slot : outSlots)
				{
					int slotIdx = slot.GetWeaponSlotIndex();
					if (slotIdx == activeWeaponIdx)
					{
						desiredSlot = slot;
						break;
					}

					if (!fallbackSlot && slotIdx < 3 && slot.GetWeaponEntity())
						fallbackSlot = slot;
				}

				IEntity weaponEntity;
				if (desiredSlot)
					weaponEntity = desiredSlot.GetWeaponEntity();
				else if (fallbackSlot)
					weaponEntity = fallbackSlot.GetWeaponEntity();

				charController.TryEquipRightHandItem(weaponEntity, EEquipItemType.EEquipTypeWeapon, true);
			}
		}
		else
		{
			charController.TryEquipRightHandItem(null, EEquipItemType.EEquipTypeUnarmedDeliberate, true);
		}

		return true;
	}
}

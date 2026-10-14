// Item details of a saved kit for the sidecar dashboard (loadouts item info, WP7). Resolves every item prefab of a custom loadout record
// to {id, name, key, path, type, cost, count} and the kit total, all at runtime from the game's own data (no hard-wired items, RHS/WCS
// packs work):
//   name  the arsenal catalog entry's UI info name (SCR_EntityCatalogEntry.GetEntityUiInfo / GetEntityName), else the editable-entity UI info of
//         the prefab; translated with WidgetManager.Translate. 'key' carries the raw localization key when the translation is not usable
//         (empty or still a "#..." key); the dashboard then falls back to key, path and finally the GUID.
//   type  the arsenal data of the faction's ITEM catalog entry (the kin pool included, see CLW_ArsenalPool): item mode AMMUNITION ->
//         "magazine", ATTACHMENT -> "attachment", else SCR_EArsenalItemType -> rifle | pistol | machinegun | sniper | launcher | throwable |
//         explosive | medical | headwear | torso | vest | legs | footwear | handwear | backpack | equipment | vehicle. Items without an
//         entry (nested parts, character spawn gear) are classified from the prefab components: MagazineComponent -> "magazine",
//         WeaponComponent -> "weapon", BaseLoadoutClothComponent -> "clothing", else "other".
//   cost  SCR_ArsenalItem.GetSupplyCost(DEFAULT, false) of that item alone (attachments and ammo are listed as their own items, so they
//         are not counted twice), -1 when the item has no arsenal data. totalCost = CLW_Kits.Cost (vanilla kit cost), -1 when unknown.
//   count how often the prefab occurs in the kit (CLW_Kits.ItemsCounted).
// The result is a JSON fragment appended to the record JSON ("totalCost":N,"itemInfo":[...]); it is NOT part of the player-loadouts.json
// file (the file DTO stays as it was) and is cached per record. '+' only (strings can get long). Log prefix "CLW: loadouts".

class CLW_ItemInfo
{
	protected static ref map<string, string> s_mCache = new map<string, string>();

	static void ClearCache()
	{
		s_mCache.Clear();
	}

	// ",\"totalCost\":N,\"itemInfo\":[...]" for a record ("" when the game data is not ready yet, so the next call tries again).
	static string JsonFor(notnull CLW_CustomLoadoutRec rec)
	{
		string cacheKey = rec.identityId + "|" + rec.faction + "|" + rec.savedAt.ToString() + "|" + rec.loadout.Length().ToString();
		string cached;
		if (s_mCache.Find(cacheKey, cached))
			return cached;

		string json = Build(rec);
		if (!json.IsEmpty())
			s_mCache.Set(cacheKey, json);

		return json;
	}

	protected static string Build(notnull CLW_CustomLoadoutRec rec)
	{
		if (rec.loadout.IsEmpty())
			return "";

		FactionManager factionManager = GetGame().GetFactionManager();
		SCR_EntityCatalogManagerComponent catalogs = SCR_EntityCatalogManagerComponent.GetInstance();
		if (!factionManager || !catalogs)
			return "";

		SCR_Faction faction = SCR_Faction.Cast(factionManager.GetFactionByKey(rec.faction));
		if (!faction)
			return "";

		array<string> items = {};
		array<int> counts = {};
		CLW_Kits.ItemsCounted(rec.loadout, items, counts);
		if (items.IsEmpty())
			return "";

		string list = "[";
		for (int i = 0; i < items.Count(); i++)
		{
			if (i > 0)
				list += ",";

			list += ItemJson(items[i], counts[i], catalogs, faction);
		}

		list += "]";
		return ",\"totalCost\":" + CLW_Kits.Cost(rec.loadout, rec.faction).ToString() + ",\"itemInfo\":" + list;
	}

	protected static string ItemJson(string id, int count, SCR_EntityCatalogManagerComponent catalogs, SCR_Faction faction)
	{
		ResourceName prefab = id;
		SCR_EntityCatalogEntry entry = catalogs.GetEntryWithPrefabFromFactionCatalog(EEntityCatalogType.ITEM, prefab, faction);

		SCR_ArsenalItem data;
		if (entry)
			data = SCR_ArsenalItem.Cast(entry.GetEntityDataOfType(SCR_ArsenalItem));

		int cost = -1;
		string type;
		if (data)
		{
			cost = data.GetSupplyCost(SCR_EArsenalSupplyCostType.DEFAULT, false);
			type = TypeOfData(data);
		}
		else
		{
			type = TypeOfPrefab(prefab);
		}

		string key = NameKey(entry, prefab);
		string name = "";
		if (!key.IsEmpty())
		{
			name = WidgetManager.Translate(key);
			if (name.IsEmpty() || name == key || name.IndexOf("#") == 0)
				name = "";
		}

		string json = "{\"id\":\"" + CLW_CustomLoadouts.Esc(id) + "\",\"name\":\"" + CLW_CustomLoadouts.Esc(name) + "\"";
		if (name.IsEmpty() && !key.IsEmpty())
			json += ",\"key\":\"" + CLW_CustomLoadouts.Esc(key) + "\"";

		string path = prefab.GetPath();
		if (!path.IsEmpty())
			json += ",\"path\":\"" + CLW_CustomLoadouts.Esc(path) + "\"";

		return json + ",\"type\":\"" + type + "\",\"cost\":" + cost.ToString() + ",\"count\":" + count.ToString() + "}";
	}

	// Localization key (or plain text) of the item name; "" when the game knows none.
	protected static string NameKey(SCR_EntityCatalogEntry entry, ResourceName prefab)
	{
		string key;
		if (entry)
		{
			SCR_UIInfo info = entry.GetEntityUiInfo();
			if (info)
				key = info.GetName();

			if (key.IsEmpty())
				key = entry.GetEntityName();
		}

		if (key.IsEmpty())
		{
			SCR_EditableEntityUIInfo editable = SCR_EditableEntityUIInfo.ExtractEditableUIInfoFromPrefab(prefab);
			if (editable)
				key = editable.GetName();
		}

		return key;
	}

	// Type key of an arsenal data record (see the header).
	static string TypeOfData(notnull SCR_ArsenalItem data)
	{
		SCR_EArsenalItemMode mode = data.GetItemMode();
		if (mode == SCR_EArsenalItemMode.AMMUNITION)
			return "magazine";

		if (mode == SCR_EArsenalItemMode.ATTACHMENT)
			return "attachment";

		SCR_EArsenalItemType type = data.GetItemType();
		if (type == SCR_EArsenalItemType.RIFLE)
			return "rifle";
		if (type == SCR_EArsenalItemType.PISTOL)
			return "pistol";
		if (type == SCR_EArsenalItemType.MACHINE_GUN)
			return "machinegun";
		if (type == SCR_EArsenalItemType.SNIPER_RIFLE)
			return "sniper";
		if (type == SCR_EArsenalItemType.ROCKET_LAUNCHER || type == SCR_EArsenalItemType.MORTARS)
			return "launcher";
		if (type == SCR_EArsenalItemType.LETHAL_THROWABLE || type == SCR_EArsenalItemType.NON_LETHAL_THROWABLE)
			return "throwable";
		if (type == SCR_EArsenalItemType.EXPLOSIVES)
			return "explosive";
		if (type == SCR_EArsenalItemType.HEAL)
			return "medical";
		if (type == SCR_EArsenalItemType.HEADWEAR)
			return "headwear";
		if (type == SCR_EArsenalItemType.TORSO)
			return "torso";
		if (type == SCR_EArsenalItemType.VEST_AND_WAIST)
			return "vest";
		if (type == SCR_EArsenalItemType.LEGS)
			return "legs";
		if (type == SCR_EArsenalItemType.FOOTWEAR)
			return "footwear";
		if (type == SCR_EArsenalItemType.HANDWEAR)
			return "handwear";
		if (type == SCR_EArsenalItemType.BACKPACK || type == SCR_EArsenalItemType.RADIO_BACKPACK)
			return "backpack";
		if (type == SCR_EArsenalItemType.EQUIPMENT)
			return "equipment";
		if (type == SCR_EArsenalItemType.WEAPON_ATTACHMENT)
			return "attachment";
		if (type == SCR_EArsenalItemType.HELICOPTER || type == SCR_EArsenalItemType.VEHICLE)
			return "vehicle";

		return "other";
	}

	// Type key of an item without arsenal data, from the components of the prefab ("other" when nothing matches or the prefab does not load).
	static string TypeOfPrefab(ResourceName prefab)
	{
		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
			return "other";

		IEntitySource source = SCR_BaseContainerTools.FindEntitySource(resource);
		if (!source)
			return "other";

		array<IEntityComponentSource> found = {};
		if (SCR_BaseContainerTools.FindComponentSourcesOfClass(source, MagazineComponent, true, found) > 0)
			return "magazine";

		found.Clear();
		if (SCR_BaseContainerTools.FindComponentSourcesOfClass(source, WeaponComponent, true, found) > 0)
			return "weapon";

		found.Clear();
		if (SCR_BaseContainerTools.FindComponentSourcesOfClass(source, BaseLoadoutClothComponent, true, found) > 0)
			return "clothing";

		return "other";
	}
}

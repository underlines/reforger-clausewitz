// Default arsenal prices (WP10): pack items (RHS, WCS, GRS ...) often ship an SCR_ArsenalItem with supply cost 0 and are then free in the
// vanilla arsenal. While the Clausewitz game mode runs, an item whose OWN cost (m_iSupplyCost) is <= 0 gets a default price from a data table
// (Configs/CLW/ArsenalPrices.conf, CLW_ArsenalPriceTable): path-substring overrides first, then rules by item type / item mode, then a
// fallback. Items that already have a price are never touched.
// ONE choke point: `modded SCR_ArsenalItem` overrides the supply-cost getters (GetSupplyCost, GetMilitarySupplyAllocationCost,
// GetRefundAmountValue), the only places where vanilla reads m_iSupplyCost. Everything reads those: the vanilla arsenal UI and purchase
// (SCR_ArsenalComponent / SCR_ArsenalInventoryStorageManagerComponent), the loadout cost (SCR_PlayerArsenalLoadout.ComputeSuppliesCost, so
// SCR_ArsenalManagerComponent, WCS_LoadoutEditor, Bacon), weapon attachment sums and CLW_Kits.Cost / CostAt / CLW_ItemInfo.
// Deterministic: a pure function of the item's own data (type, mode, prefab path, own cost) and the table file, so server and clients agree.
// Active only while CLW_ArsenalPool.IsActive() (our game mode or a taken-over one) and CLW_ArsenalPriceTable.m_bEnabled.
// Decision on free items: the table keeps clothing (torso, legs, footwear, handwear) and plain equipment (map, compass, watch) at 0, as
// vanilla does (all vanilla torso/legs/footwear/handwear and 8 of 9 plain equipment items cost 0); a matched rule with cost 0 means "stays free".
// Log prefix: "CLW: services arsenal price" (once per faction and machine, called from the arsenal pool log).
// Notes: dev/notes/arsenal-prices.md

// A per-path override ("_scope" -> 15). First match wins, checked before the type rules. Cost 0 = free.
[BaseContainerProps(), BaseContainerCustomTitleField("m_sPathContains")]
class CLW_ArsenalPriceOverride
{
	[Attribute("", UIWidgets.EditBox, "Text (case-insensitive) that the item's prefab path must contain")]
	string m_sPathContains;

	[Attribute("10", UIWidgets.EditBox, "Default supply cost (0 = free)", params: "0 inf 1")]
	int m_iCost;
}

// One type/mode rule. First matching rule wins. Type 0 = any type; mode 0 = any mode; mode WEAPON also matches WEAPON_VARIANTS.
[BaseContainerProps(), BaseContainerCustomTitleField("m_sName")]
class CLW_ArsenalPriceRule
{
	[Attribute("", UIWidgets.EditBox, "Label (documentation only)")]
	string m_sName;

	[Attribute("0", UIWidgets.SearchComboBox, "Item type (0 = any)", enums: ParamEnumArray.FromEnum(SCR_EArsenalItemType))]
	SCR_EArsenalItemType m_eItemType;

	[Attribute("0", UIWidgets.SearchComboBox, "Item mode (0 = any)", enums: ParamEnumArray.FromEnum(SCR_EArsenalItemMode))]
	SCR_EArsenalItemMode m_eItemMode;

	[Attribute("10", UIWidgets.EditBox, "Default supply cost (0 = stays free)", params: "0 inf 1")]
	int m_iCost;

	bool Matches(int type, int mode)
	{
		int wantedType = m_eItemType;
		if (wantedType != 0 && wantedType != type)
			return false;

		int wantedMode = m_eItemMode;
		if (wantedMode == 0)
			return true;

		if (wantedMode == SCR_EArsenalItemMode.WEAPON)
			return mode == SCR_EArsenalItemMode.WEAPON || mode == SCR_EArsenalItemMode.WEAPON_VARIANTS;

		return wantedMode == mode;
	}
}

// Multiplier of the default price per arsenal cost type (SCR_ArsenalComponent.GetSupplyCostType). Missing = 1.
[BaseContainerProps()]
class CLW_ArsenalPriceMultiplier
{
	[Attribute("0", UIWidgets.SearchComboBox, "Arsenal cost type", enums: ParamEnumArray.FromEnum(SCR_EArsenalSupplyCostType))]
	SCR_EArsenalSupplyCostType m_eCostType;

	[Attribute("1", UIWidgets.EditBox, "Factor on the default price (0 = free for this cost type)", params: "0 inf 0.01")]
	float m_fMultiplier;
}

[BaseContainerProps(configRoot: true)]
class CLW_ArsenalPriceTable
{
	[Attribute("1", UIWidgets.CheckBox, "Master switch. Off = vanilla prices only")]
	bool m_bEnabled;

	[Attribute("1", UIWidgets.EditBox, "Smallest default price after the multiplier (only when the default is above 0)", params: "0 inf 1")]
	int m_iMinimum;

	[Attribute("2", UIWidgets.EditBox, "Default price of an item that no override and no rule matches", params: "0 inf 1")]
	int m_iFallbackCost;

	[Attribute(desc: "Multiplier per arsenal cost type")]
	ref array<ref CLW_ArsenalPriceMultiplier> m_aMultipliers;

	[Attribute(desc: "Path-substring overrides, first match wins, before the rules")]
	ref array<ref CLW_ArsenalPriceOverride> m_aOverrides;

	[Attribute(desc: "Type/mode rules, first match wins")]
	ref array<ref CLW_ArsenalPriceRule> m_aRules;

	void Normalize()
	{
		if (!m_aOverrides)
			return;

		foreach (CLW_ArsenalPriceOverride entry : m_aOverrides)
		{
			if (!entry)
				continue;

			string lowered = entry.m_sPathContains;
			lowered.ToLower();
			entry.m_sPathContains = lowered;
		}
	}

	// Default price of an item (cost type DEFAULT, no multiplier). 'lowerPath' is the lower-case prefab path.
	int BaseCost(int type, int mode, string lowerPath)
	{
		if (m_aOverrides && !lowerPath.IsEmpty())
		{
			foreach (CLW_ArsenalPriceOverride entry : m_aOverrides)
			{
				if (entry && !entry.m_sPathContains.IsEmpty() && lowerPath.Contains(entry.m_sPathContains))
					return entry.m_iCost;
			}
		}

		if (m_aRules)
		{
			foreach (CLW_ArsenalPriceRule rule : m_aRules)
			{
				if (rule && rule.Matches(type, mode))
					return rule.m_iCost;
			}
		}

		return m_iFallbackCost;
	}

	// The base price scaled for an arsenal cost type, never below the minimum unless it is free.
	int Scale(int baseCost, SCR_EArsenalSupplyCostType costType)
	{
		if (baseCost <= 0)
			return 0;

		float factor = 1;
		if (m_aMultipliers)
		{
			foreach (CLW_ArsenalPriceMultiplier entry : m_aMultipliers)
			{
				if (entry && entry.m_eCostType == costType)
					factor = entry.m_fMultiplier;
			}
		}

		if (factor <= 0)
			return 0;

		int scaled = Math.Round(baseCost * factor);
		return Math.Max(scaled, m_iMinimum);
	}
}

class CLW_ArsenalPrices
{
	static const ResourceName TABLE_PATH = "Configs/CLW/ArsenalPrices.conf";
	static const int LOG_EXAMPLES = 4;

	protected static ref CLW_ArsenalPriceTable s_Table;
	protected static bool s_bTableLoaded;
	protected static ref array<string> s_aLogged = {};

	// The price table while prices are active (war running, table present and enabled), else null.
	static CLW_ArsenalPriceTable GetActiveTable()
	{
		if (!CLW_ArsenalPool.IsActive())
			return null;

		if (!s_bTableLoaded)
		{
			s_bTableLoaded = true;
			s_Table = SCR_ConfigHelperT<CLW_ArsenalPriceTable>.GetConfigObject(TABLE_PATH);
			if (s_Table)
			{
				s_Table.Normalize();
				Print("CLW: services arsenal price table loaded (enabled=" + s_Table.m_bEnabled.ToString() + ")", LogLevel.NORMAL);
			}
			else
			{
				Print("CLW: services arsenal price table missing (" + TABLE_PATH + "), vanilla prices only", LogLevel.WARNING);
			}
		}

		if (!s_Table || !s_Table.m_bEnabled)
			return null;

		return s_Table;
	}

	// Short label of an item for the log.
	static string TypeLabel(notnull SCR_ArsenalItem item)
	{
		SCR_EArsenalItemMode mode = item.GetItemMode();
		if (mode == SCR_EArsenalItemMode.AMMUNITION)
			return "magazine";

		if (mode == SCR_EArsenalItemMode.ATTACHMENT)
			return "attachment";

		SCR_EArsenalItemType type = item.GetItemType();
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
		if (type == SCR_EArsenalItemType.TORSO || type == SCR_EArsenalItemType.LEGS || type == SCR_EArsenalItemType.FOOTWEAR || type == SCR_EArsenalItemType.HANDWEAR)
			return "clothing";
		if (type == SCR_EArsenalItemType.VEST_AND_WAIST)
			return "vest";
		if (type == SCR_EArsenalItemType.BACKPACK || type == SCR_EArsenalItemType.RADIO_BACKPACK)
			return "backpack";
		if (type == SCR_EArsenalItemType.EQUIPMENT)
			return "equipment";
		if (type == SCR_EArsenalItemType.WEAPON_ATTACHMENT)
			return "attachment";

		return "other";
	}

	// Logs once per faction and machine how many of the faction's pool items got a default price. 'items' = the unfiltered pool.
	static void LogFaction(string factionKey, notnull array<SCR_ArsenalItem> items)
	{
		if (s_aLogged.Contains(factionKey))
			return;

		if (!GetActiveTable())
			return;

		s_aLogged.Insert(factionKey);
		map<string, int> byType = new map<string, int>();
		array<string> typeOrder = {};
		array<string> examples = {};
		int priced;
		foreach (SCR_ArsenalItem item : items)
		{
			if (!item)
				continue;

			int price = item.CLW_GetDefaultPrice(SCR_EArsenalSupplyCostType.DEFAULT);
			if (price <= 0)
				continue;

			priced++;
			string label = TypeLabel(item);
			int known;
			if (byType.Find(label, known))
			{
				byType.Set(label, known + 1);
			}
			else
			{
				byType.Set(label, 1);
				typeOrder.Insert(label);
			}

			if (examples.Count() < LOG_EXAMPLES)
				examples.Insert(item.GetItemResourceName() + "=" + price.ToString());
		}

		string detail;
		foreach (string typeKey : typeOrder)
		{
			if (!detail.IsEmpty())
				detail += ", ";

			detail += typeKey + " " + byType.Get(typeKey).ToString();
		}

		string sample;
		foreach (string example : examples)
			sample += " " + example;

		Print("CLW: services arsenal price " + factionKey + ": " + priced.ToString() + " items priced by default (" + detail + ") e.g.:" + sample, LogLevel.NORMAL);
	}
}

modded class SCR_ArsenalItem
{
	// Cached table price (cost type DEFAULT, before the multiplier); -1 = not computed yet. Only filled while prices are active.
	protected int m_iCLWDefaultBase = -1;

	// Default price this item gets from the Clausewitz price table for the cost type; 0 when it has its own price, prices are
	// inactive, or the table says free.
	int CLW_GetDefaultPrice(SCR_EArsenalSupplyCostType costType)
	{
		if (m_iSupplyCost > 0)
			return 0;

		CLW_ArsenalPriceTable table = CLW_ArsenalPrices.GetActiveTable();
		if (!table)
			return 0;

		if (m_iCLWDefaultBase < 0)
		{
			string lowerPath = GetItemResourceName();
			lowerPath.ToLower();
			m_iCLWDefaultBase = table.BaseCost(m_eItemType, m_eItemMode, lowerPath);
		}

		return table.Scale(m_iCLWDefaultBase, costType);
	}

	override int GetSupplyCost(SCR_EArsenalSupplyCostType supplyCostType, bool addAdditionalCosts = true)
	{
		int cost = super.GetSupplyCost(supplyCostType, addAdditionalCosts);
		if (m_iSupplyCost > 0)
			return cost;

		// An alternative cost of this cost type (SCR_ArsenalAlternativeCostData) above 0 is the item's own price
		if (super.GetSupplyCost(supplyCostType, false) > 0)
			return cost;

		return cost + CLW_GetDefaultPrice(supplyCostType);
	}

	override int GetMilitarySupplyAllocationCost(SCR_EArsenalSupplyCostType supplyCostType, bool addAdditionalCosts = true)
	{
		int cost = super.GetMilitarySupplyAllocationCost(supplyCostType, addAdditionalCosts);
		if (m_iSupplyCost > 0 || !m_bUseMilitarySupplyAllocation)
			return cost;

		if (super.GetMilitarySupplyAllocationCost(supplyCostType, false) > 0)
			return cost;

		return cost + CLW_GetDefaultPrice(supplyCostType);
	}

	// Refund base of vanilla: the (default) price, so a defaulted item refunds what it costs.
	override protected int GetRefundAmountValue()
	{
		int value = super.GetRefundAmountValue();
		if (value > 0)
			return value;

		return CLW_GetDefaultPrice(SCR_EArsenalSupplyCostType.DEFAULT);
	}
}

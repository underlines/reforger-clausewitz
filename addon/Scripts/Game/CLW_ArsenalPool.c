// Arsenal item pool (M3 phase B = loadouts-arsenal-plan L2 layers 1 + 3, "kin pool"): while the Clausewitz game mode runs, every faction's
// arsenal shows its own ITEM catalog plus the ITEM catalogs of the factions friendly to it in the faction data (vanilla friendly lists:
// a pack faction such as RHS_USAF lists the vanilla faction it is built on), minus the enemy side. Own items win on duplicates.
// On top of that, data rules (Configs/CLW/ArsenalPackRules.conf, CLW_ArsenalPackRules) can add "items of catalog X (optionally only prefab
// paths containing Y) also go to faction Z" for packs whose items sit in a faction catalog that is not on the friendly list, and can block
// prefab paths everywhere. Rules never pull the enemy side (server: the scenario's other side; clients know no scenario, so "*" sources
// are limited to the friendly factions there).
// Two choke points on SCR_EntityCatalogManagerComponent: GetFactionArsenalItems (what every arsenal and loadout editor lists) and
// GetEntryWithPrefabFromFactionCatalog for ITEM (what the arsenal request / WCS Loadout Editor checks and prices an item by).
// Clients run the same code (the arsenal contents are computed on every machine), so the rule only uses data that exists on all machines:
// the faction manager's friendly lists, the rules file and the game mode type. The server additionally drops the enemy side's faction (scenario).
// Log prefix: "CLW: services arsenal pool" (once per faction and machine, with the unfiltered counts, as soon as the catalogs are filled).
// How the packs reach the factions (WCS_Arsenal, GRS, ...): dev/research/arsenal-packs.md

// One rule: items of the source catalogs join the arsenals of the target factions.
[BaseContainerProps()]
class CLW_ArsenalPackRule
{
	[Attribute("", UIWidgets.EditBox, "Rule name (log only)")]
	string m_sName;

	[Attribute("1", UIWidgets.CheckBox, "Rule is active")]
	bool m_bEnabled;

	[Attribute(desc: "Faction keys whose arsenal gets the items. Empty or \"*\" = every faction")]
	ref array<string> m_aTargetFactions;

	[Attribute(desc: "Faction keys whose ITEM catalog is the source. \"*\" = every faction but the enemy (clients: only friendly factions)")]
	ref array<string> m_aSourceFactions;

	[Attribute(desc: "Only items whose prefab path contains one of these texts (case-insensitive, e.g. a pack folder or name prefix). Empty = all items of the source")]
	ref array<string> m_aPathContains;

	[Attribute(desc: "Never items whose prefab path contains one of these texts")]
	ref array<string> m_aPathExcludes;

	void Normalize()
	{
		CLW_ArsenalPool.LowerAll(m_aPathContains);
		CLW_ArsenalPool.LowerAll(m_aPathExcludes);
	}

	bool AppliesTo(string factionKey)
	{
		if (!m_bEnabled)
			return false;

		if (!m_aTargetFactions || m_aTargetFactions.IsEmpty() || m_aTargetFactions.Contains("*"))
			return true;

		return m_aTargetFactions.Contains(factionKey);
	}

	string GetLabel()
	{
		if (m_sName.IsEmpty())
			return "rule";

		return m_sName;
	}
}

[BaseContainerProps(configRoot: true)]
class CLW_ArsenalPackRules
{
	[Attribute()]
	ref array<ref CLW_ArsenalPackRule> m_aRules;

	[Attribute(desc: "Prefab path texts (case-insensitive) that never appear in any arsenal during a war, own items included (safety valve for broken or unwanted pack items)")]
	ref array<string> m_aBlockedPathContains;
}

class CLW_ArsenalPool
{
	static const ResourceName RULES_PATH = "Configs/CLW/ArsenalPackRules.conf";
	static const int MAX_EMPTY_TRIES = 40;

	protected static ref array<string> s_aLogged = {};
	protected static ref map<string, int> s_mEmptyTries = new map<string, int>();
	protected static ref CLW_ArsenalPackRules s_Rules;
	protected static bool s_bRulesLoaded;

	static void Reset()
	{
		s_aLogged.Clear();
		s_mEmptyTries.Clear();
		s_bRulesLoaded = false;
		s_Rules = null;
	}

	static bool IsActive()
	{
		return CLW_GameMode.Cast(GetGame().GetGameMode()) != null;
	}

	static void LowerAll(array<string> list)
	{
		if (!list)
			return;

		for (int i = 0; i < list.Count(); i++)
		{
			string text = list[i];
			text.ToLower();
			list.Set(i, text);
		}
	}

	// The rules file (null when missing: the pool is then kin only). Loaded once per session on each machine.
	static CLW_ArsenalPackRules GetRules()
	{
		if (s_bRulesLoaded)
			return s_Rules;

		s_bRulesLoaded = true;
		s_Rules = SCR_ConfigHelperT<CLW_ArsenalPackRules>.GetConfigObject(RULES_PATH);
		if (!s_Rules)
		{
			Print("CLW: services arsenal pool rules missing (" + RULES_PATH + "), kin pool only", LogLevel.WARNING);
			return null;
		}

		int active;
		if (s_Rules.m_aRules)
		{
			foreach (CLW_ArsenalPackRule rule : s_Rules.m_aRules)
			{
				if (!rule)
					continue;

				rule.Normalize();
				if (rule.m_bEnabled)
					active++;
			}
		}

		LowerAll(s_Rules.m_aBlockedPathContains);
		int blocked = 0;
		if (s_Rules.m_aBlockedPathContains)
			blocked = s_Rules.m_aBlockedPathContains.Count();

		Print(string.Format("CLW: services arsenal pool rules: %1 active, %2 blocked path texts", active, blocked), LogLevel.NORMAL);
		return s_Rules;
	}

	// True when the lower-case path contains one of the (already lower-case) texts.
	static bool PathHas(string lowerPath, array<string> texts)
	{
		if (!texts)
			return false;

		foreach (string text : texts)
		{
			if (!text.IsEmpty() && lowerPath.Contains(text))
				return true;
		}

		return false;
	}

	// Key of the enemy side as far as this machine knows it (server: scenario; client: empty).
	static string GetEnemyKey(notnull SCR_Faction faction)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario && scenario.IsFaction(faction.GetFactionKey()))
			return scenario.GetEnemyKey(faction.GetFactionKey());

		return "";
	}

	// Friendly factions whose items join the pool of 'faction' (never the faction itself, never the enemy side).
	static void GetKin(notnull SCR_Faction faction, notnull array<SCR_Faction> kin)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (!manager)
			return;

		string enemyKey = GetEnemyKey(faction);

		array<Faction> all = {};
		manager.GetFactionsList(all);
		foreach (Faction other : all)
		{
			SCR_Faction candidate = SCR_Faction.Cast(other);
			if (!candidate || candidate == faction)
				continue;

			if (!enemyKey.IsEmpty() && candidate.GetFactionKey() == enemyKey)
				continue;

			if (faction.DoCheckIfFactionFriendly(candidate) || candidate.DoCheckIfFactionFriendly(faction))
				kin.Insert(candidate);
		}
	}

	// Source factions of a rule for 'faction' (never itself, never the enemy side, no duplicates).
	static void GetRuleSources(notnull SCR_Faction faction, notnull CLW_ArsenalPackRule rule, notnull array<SCR_Faction> kin, notnull array<SCR_Faction> sources)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (!manager || !rule.m_aSourceFactions)
			return;

		string enemyKey = GetEnemyKey(faction);
		bool knowsEnemy = CLW_ScenarioConfig.Get() != null;

		foreach (string key : rule.m_aSourceFactions)
		{
			if (key == "*")
			{
				if (!knowsEnemy)
				{
					// client: the enemy is unknown here, so only the friendly factions can be taken for sure
					foreach (SCR_Faction friend : kin)
						AddSource(faction, friend, enemyKey, sources);

					continue;
				}

				array<Faction> all = {};
				manager.GetFactionsList(all);
				foreach (Faction other : all)
					AddSource(faction, SCR_Faction.Cast(other), enemyKey, sources);

				continue;
			}

			AddSource(faction, SCR_Faction.Cast(manager.GetFactionByKey(key)), enemyKey, sources);
		}
	}

	protected static void AddSource(notnull SCR_Faction faction, SCR_Faction candidate, string enemyKey, notnull array<SCR_Faction> sources)
	{
		if (!candidate || candidate == faction || sources.Contains(candidate))
			return;

		if (!enemyKey.IsEmpty() && candidate.GetFactionKey() == enemyKey)
			return;

		sources.Insert(candidate);
	}

	// A faction that never got items from its own catalog is retried silently a few times (catalogs fill after the first calls).
	static bool ShouldLog(string factionKey)
	{
		return !s_aLogged.Contains(factionKey);
	}

	// Count a call that found no own items; true when the retries are used up (log the empty result once).
	static bool CountEmptyTry(string factionKey)
	{
		int tries = 0;
		s_mEmptyTries.Find(factionKey, tries);
		tries++;
		s_mEmptyTries.Set(factionKey, tries);
		return tries >= MAX_EMPTY_TRIES;
	}

	static void LogOnce(string factionKey, string text)
	{
		if (s_aLogged.Contains(factionKey))
			return;

		s_aLogged.Insert(factionKey);
		Print("CLW: services arsenal pool " + factionKey + ": " + text, LogLevel.NORMAL);
	}
}

modded class SCR_EntityCatalogManagerComponent
{
	override bool GetFactionArsenalItems(out array<SCR_ArsenalItem> arsenalItems, SCR_Faction faction, SCR_EArsenalItemType typeFilter = -1, SCR_EArsenalItemMode modeFilter = -1, SCR_EArsenalGameModeType arsenalGameModeType = -1, EArsenalItemDisplayType requiresDisplayType = -1)
	{
		bool result = super.GetFactionArsenalItems(arsenalItems, faction, typeFilter, modeFilter, arsenalGameModeType, requiresDisplayType);
		if (!faction || !CLW_ArsenalPool.IsActive())
			return result;

		string factionKey = faction.GetFactionKey();
		string detail;
		int kinAdded, ruleAdded, blockedCount;
		bool changed = CLW_ExtendPool(faction, arsenalItems, typeFilter, modeFilter, arsenalGameModeType, requiresDisplayType, detail, kinAdded, ruleAdded, blockedCount);

		if (CLW_ArsenalPool.ShouldLog(factionKey))
		{
			// The counts are taken over the whole catalog (no filters), so the line is the same whatever combination is asked first, and
			// only once the faction's own catalog has items (the first calls come before the catalogs are filled).
			array<SCR_ArsenalItem> everything = {};
			super.GetFactionArsenalItems(everything, faction);
			int ownCount = everything.Count();
			if (ownCount > 0 || CLW_ArsenalPool.CountEmptyTry(factionKey))
			{
				string fullDetail;
				int fullKin, fullRule, fullBlocked;
				CLW_ExtendPool(faction, everything, -1, -1, -1, -1, fullDetail, fullKin, fullRule, fullBlocked);
				CLW_ArsenalPool.LogOnce(factionKey, string.Format("own=%1 kin=%2 rules=%3 blocked=%4 total=%5 |%6", ownCount, fullKin, fullRule, fullBlocked, everything.Count(), fullDetail));
			}
		}

		if (!changed)
			return result;

		return !arsenalItems.IsEmpty();
	}

	// The same pool for the per-item lookup: vanilla RpcAsk_ArsenalRequestItem_ and the WCS Loadout Editor accept an arsenal request (and
	// price it) only when the item has an entry in the arsenal faction's ITEM catalog. Without this, kin / rule items were listed but
	// could not be taken and showed cost 0. Order: own catalog, kin, rule sources; blocked paths never resolve.
	override SCR_EntityCatalogEntry GetEntryWithPrefabFromFactionCatalog(EEntityCatalogType catalogType, ResourceName prefabToFind, notnull SCR_Faction faction)
	{
		SCR_EntityCatalogEntry entry = super.GetEntryWithPrefabFromFactionCatalog(catalogType, prefabToFind, faction);
		if (catalogType != EEntityCatalogType.ITEM || prefabToFind.IsEmpty() || !CLW_ArsenalPool.IsActive())
			return entry;

		CLW_ArsenalPackRules rules = CLW_ArsenalPool.GetRules();
		string lowerPath = prefabToFind;
		lowerPath.ToLower();
		if (rules && CLW_ArsenalPool.PathHas(lowerPath, rules.m_aBlockedPathContains))
			return null;

		if (entry)
			return entry;

		array<SCR_Faction> kin = {};
		CLW_ArsenalPool.GetKin(faction, kin);
		foreach (SCR_Faction kinFaction : kin)
		{
			entry = super.GetEntryWithPrefabFromFactionCatalog(catalogType, prefabToFind, kinFaction);
			if (entry)
				return entry;
		}

		if (!rules || !rules.m_aRules)
			return null;

		string factionKey = faction.GetFactionKey();
		foreach (CLW_ArsenalPackRule rule : rules.m_aRules)
		{
			if (!rule || !rule.AppliesTo(factionKey))
				continue;

			if (rule.m_aPathContains && !rule.m_aPathContains.IsEmpty() && !CLW_ArsenalPool.PathHas(lowerPath, rule.m_aPathContains))
				continue;

			if (CLW_ArsenalPool.PathHas(lowerPath, rule.m_aPathExcludes))
				continue;

			array<SCR_Faction> sources = {};
			CLW_ArsenalPool.GetRuleSources(faction, rule, kin, sources);
			foreach (SCR_Faction source : sources)
			{
				entry = super.GetEntryWithPrefabFromFactionCatalog(catalogType, prefabToFind, source);
				if (entry)
					return entry;
			}
		}

		return null;
	}

	// Adds kin and rule items to 'items' (own items already in), then removes blocked paths. Returns true when anything was added or removed.
	protected bool CLW_ExtendPool(notnull SCR_Faction faction, notnull array<SCR_ArsenalItem> items, SCR_EArsenalItemType typeFilter, SCR_EArsenalItemMode modeFilter, SCR_EArsenalGameModeType arsenalGameModeType, EArsenalItemDisplayType requiresDisplayType, out string detail, out int kinAdded, out int ruleAdded, out int blockedCount)
	{
		kinAdded = 0;
		ruleAdded = 0;
		blockedCount = 0;
		detail = "";

		array<SCR_Faction> kin = {};
		CLW_ArsenalPool.GetKin(faction, kin);
		CLW_ArsenalPackRules rules = CLW_ArsenalPool.GetRules();

		bool hasRules = rules && rules.m_aRules && !rules.m_aRules.IsEmpty();
		bool hasBlocked = rules && rules.m_aBlockedPathContains && !rules.m_aBlockedPathContains.IsEmpty();
		if (kin.IsEmpty() && !hasRules && !hasBlocked)
			return false;

		map<string, bool> seen = new map<string, bool>();
		foreach (SCR_ArsenalItem own : items)
		{
			if (own)
				seen.Set(own.GetItemResourceName(), true);
		}

		foreach (SCR_Faction kinFaction : kin)
		{
			int added = CLW_AddCatalogItems(kinFaction, items, seen, null, null, typeFilter, modeFilter, arsenalGameModeType, requiresDisplayType);
			kinAdded += added;
			detail += " " + kinFaction.GetFactionKey() + "=" + added;
		}

		if (hasRules)
		{
			string factionKey = faction.GetFactionKey();
			foreach (CLW_ArsenalPackRule rule : rules.m_aRules)
			{
				if (!rule || !rule.AppliesTo(factionKey))
					continue;

				array<SCR_Faction> sources = {};
				CLW_ArsenalPool.GetRuleSources(faction, rule, kin, sources);
				foreach (SCR_Faction source : sources)
				{
					int ruleItems = CLW_AddCatalogItems(source, items, seen, rule.m_aPathContains, rule.m_aPathExcludes, typeFilter, modeFilter, arsenalGameModeType, requiresDisplayType);
					ruleAdded += ruleItems;
					detail += " [" + rule.GetLabel() + ":" + source.GetFactionKey() + "=" + ruleItems + "]";
				}
			}
		}

		if (hasBlocked)
		{
			for (int i = items.Count() - 1; i >= 0; i--)
			{
				SCR_ArsenalItem item = items[i];
				if (!item)
					continue;

				string path = item.GetItemResourceName();
				path.ToLower();
				if (CLW_ArsenalPool.PathHas(path, rules.m_aBlockedPathContains))
				{
					items.Remove(i);
					blockedCount++;
				}
			}
		}

		return kinAdded > 0 || ruleAdded > 0 || blockedCount > 0;
	}

	// Items of 'source's ITEM catalog that are not in 'seen' yet (and pass the optional path filters); returns the number added.
	protected int CLW_AddCatalogItems(notnull SCR_Faction source, notnull array<SCR_ArsenalItem> items, notnull map<string, bool> seen, array<string> pathContains, array<string> pathExcludes, SCR_EArsenalItemType typeFilter, SCR_EArsenalItemMode modeFilter, SCR_EArsenalGameModeType arsenalGameModeType, EArsenalItemDisplayType requiresDisplayType)
	{
		SCR_EntityCatalog itemCatalog = GetFactionEntityCatalogOfType(EEntityCatalogType.ITEM, source, false);
		if (!itemCatalog)
			return 0;

		bool filterPaths = (pathContains && !pathContains.IsEmpty()) || (pathExcludes && !pathExcludes.IsEmpty());
		array<SCR_ArsenalItem> extra = {};
		GetArsenalItems(extra, itemCatalog, typeFilter, modeFilter, arsenalGameModeType, requiresDisplayType);
		int added = 0;
		foreach (SCR_ArsenalItem item : extra)
		{
			if (!item)
				continue;

			string itemName = item.GetItemResourceName();
			if (seen.Contains(itemName))
				continue;

			if (filterPaths)
			{
				string lowerPath = itemName;
				lowerPath.ToLower();
				if (pathContains && !pathContains.IsEmpty() && !CLW_ArsenalPool.PathHas(lowerPath, pathContains))
					continue;

				if (CLW_ArsenalPool.PathHas(lowerPath, pathExcludes))
					continue;
			}

			seen.Set(itemName, true);
			items.Insert(item);
			added++;
		}

		return added;
	}
}

// Arsenal item pool (M3 phase B = loadouts-arsenal-plan L2 layers 1 + 3, "kin pool"): while the Clausewitz game mode runs, every faction's
// arsenal shows its own ITEM catalog plus the ITEM catalogs of the factions friendly to it in the faction data (vanilla friendly lists:
// a pack faction such as RHS_USAF lists the vanilla faction it is built on), minus the enemy side. Own items win on duplicates.
// One choke point: SCR_EntityCatalogManagerComponent.GetFactionArsenalItems, which every arsenal and loadout editor ends up in.
// Clients run the same code (the arsenal contents are computed on every machine), so the rule only uses data that exists on all machines:
// the faction manager's friendly lists and the game mode type. The server additionally drops the enemy side's faction (scenario).
// Log prefix: "CLW: services arsenal pool" (once per faction and machine).
class CLW_ArsenalPool
{
	protected static ref array<string> s_aLogged = {};

	static void Reset()
	{
		s_aLogged.Clear();
	}

	static bool IsActive()
	{
		return CLW_GameMode.Cast(GetGame().GetGameMode()) != null;
	}

	// Friendly factions whose items join the pool of 'faction' (never the faction itself, never the enemy side).
	static void GetKin(notnull SCR_Faction faction, notnull array<SCR_Faction> kin)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (!manager)
			return;

		string enemyKey;
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario && scenario.IsFaction(faction.GetFactionKey()))
			enemyKey = scenario.GetEnemyKey(faction.GetFactionKey());

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

		array<SCR_Faction> kin = {};
		CLW_ArsenalPool.GetKin(faction, kin);
		if (kin.IsEmpty())
			return result;

		map<string, bool> seen = new map<string, bool>();
		foreach (SCR_ArsenalItem own : arsenalItems)
		{
			if (own)
				seen.Set(own.GetItemResourceName(), true);
		}

		int ownCount = arsenalItems.Count();
		string detail;
		foreach (SCR_Faction kinFaction : kin)
		{
			SCR_EntityCatalog itemCatalog = GetFactionEntityCatalogOfType(EEntityCatalogType.ITEM, kinFaction, false);
			if (!itemCatalog)
				continue;

			array<SCR_ArsenalItem> extra = {};
			GetArsenalItems(extra, itemCatalog, typeFilter, modeFilter, arsenalGameModeType, requiresDisplayType);
			int added = 0;
			foreach (SCR_ArsenalItem item : extra)
			{
				if (!item || seen.Contains(item.GetItemResourceName()))
					continue;

				seen.Set(item.GetItemResourceName(), true);
				arsenalItems.Insert(item);
				added++;
			}

			detail += " " + kinFaction.GetFactionKey() + "=" + added;
		}

		// one line per faction and machine (the first filter combination asked)
		CLW_ArsenalPool.LogOnce(faction.GetFactionKey(), string.Format("own=%1 kin:%2 total=%3", ownCount, detail, arsenalItems.Count()));

		return !arsenalItems.IsEmpty();
	}
}

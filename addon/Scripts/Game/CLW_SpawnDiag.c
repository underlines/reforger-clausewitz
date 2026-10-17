// Spawn diagnostics (M4.6 custom loadout bug: the deploy menu shows the generic "spawn point not available", which vanilla gives for any
// refusal without its own reason text). Server only, one line per change of the outcome per player, so the 1 s deploy-menu poll does not
// flood the log. Log: "CLW: spawn check player <id>: <result> loadout=<class> '<name>' faction=<key> prefab=<..>" and
// "CLW: spawn loadout assign player <id> index <i>: <ok|refused> loadout=<class> faction=<key> player faction=<key>".
class CLW_SpawnDiag
{
	protected static ref map<int, string> s_mLast = new map<int, string>();

	static void Note(int playerId, string line)
	{
		string last;
		if (s_mLast.Find(playerId, last) && last == line)
			return;

		s_mLast.Set(playerId, line);
		Print("" + line, LogLevel.NORMAL);	// a bare variable prints as "string line = '...'"
	}

	static string Describe(SCR_BasePlayerLoadout loadout)
	{
		if (!loadout)
			return "none";

		string faction = "-";
		SCR_FactionPlayerLoadout factionLoadout = SCR_FactionPlayerLoadout.Cast(loadout);
		if (factionLoadout)
			faction = factionLoadout.GetFactionKey();

		return loadout.Type().ToString() + " '" + loadout.GetLoadoutName() + "' faction=" + faction;
	}
}

modded class SCR_SpawnHandlerComponent
{
	override SCR_ESpawnResult CanHandleRequest_S(SCR_SpawnRequestComponent requestComponent, SCR_SpawnData data)
	{
		SCR_ESpawnResult result = super.CanHandleRequest_S(requestComponent, data);
		if (!requestComponent)
			return result;

		int playerId = requestComponent.GetPlayerId();
		SCR_BasePlayerLoadout loadout;
		PlayerController controller = requestComponent.GetPlayerController();
		if (controller)
		{
			SCR_PlayerLoadoutComponent loadoutComp = SCR_PlayerLoadoutComponent.Cast(controller.FindComponent(SCR_PlayerLoadoutComponent));
			if (loadoutComp)
				loadout = loadoutComp.GetLoadout();
		}

		string prefab;
		if (data)
			prefab = data.GetPrefab();

		CLW_SpawnDiag.Note(playerId, "CLW: spawn check player " + playerId.ToString() + " (" + Type().ToString() + "): " + typename.EnumToString(SCR_ESpawnResult, result) + " loadout=" + CLW_SpawnDiag.Describe(loadout) + " prefab=" + prefab);
		return result;
	}
}

// Which part of the validation refuses: vanilla's own conditions (spawn point exists, player spawn points allowed) vs the whole modded chain.
modded class SCR_SpawnPointSpawnHandlerComponent
{
	protected override bool ValidateData_S(SCR_SpawnRequestComponent requestComponent, SCR_SpawnData data)
	{
		bool ok = super.ValidateData_S(requestComponent, data);
		if (ok || !requestComponent)
			return ok;

		SCR_SpawnPointSpawnData spawnPointData = SCR_SpawnPointSpawnData.Cast(data);
		string point = "no spawn point data";
		if (spawnPointData && spawnPointData.GetSpawnPoint())
			point = spawnPointData.GetSpawnPoint().Type().ToString() + " '" + spawnPointData.GetSpawnPoint().GetSpawnPointName() + "'";
		else if (spawnPointData)
			point = "spawn point not found";

		CLW_SpawnDiag.Note(-requestComponent.GetPlayerId(), "CLW: spawn validate player " + requestComponent.GetPlayerId().ToString() + " refused: " + point + " prefab=" + data.GetPrefab());
		return ok;
	}
}

modded class SCR_PlayerLoadoutComponent
{
	protected override bool CanAssignLoadout_S(int loadoutIndex)
	{
		bool ok = super.CanAssignLoadout_S(loadoutIndex);
		SCR_LoadoutManager manager = GetGame().GetLoadoutManager();
		SCR_BasePlayerLoadout loadout;
		if (manager)
			loadout = manager.GetLoadoutByIndex(loadoutIndex);

		string playerFaction = "-";
		SCR_PlayerFactionAffiliationComponent affiliation = GetPlayerFactionAffiliationComponent();
		if (affiliation && affiliation.GetAffiliatedFaction())
			playerFaction = affiliation.GetAffiliatedFaction().GetFactionKey();

		string verdict = "refused";
		if (ok)
			verdict = "ok";

		Print("CLW: spawn loadout assign player " + GetPlayerId().ToString() + " index " + loadoutIndex.ToString() + ": " + verdict + " loadout=" + CLW_SpawnDiag.Describe(loadout) + " player faction=" + playerFaction, LogLevel.NORMAL);
		return ok;
	}
}

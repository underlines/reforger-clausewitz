// Clausewitz game mode: AI High Command war with human players on both sides.
// Inherits the whole player flow (spawn, groups, tasks, markers) from GameMode_Base.et.
//
// The war itself runs in CLW_WarRuntime (a holder any SCR_BaseGameMode entity can host: M4 takeover of Game Master worlds); the hooks
// that forward the game mode events to it are the modded SCR_BaseGameMode in CLW_GameModeHooks.c. This entity class only carries the
// prefab attributes and keeps the static API (GetBases, GetWarState, ...) the rest of the mod uses.

class CLW_GameModeClass : SCR_BaseGameModeClass
{
}

class CLW_GameMode : SCR_BaseGameMode
{
	[Attribute("", UIWidgets.ResourceNamePicker, "Scenario config: factions, bases, objectives, economy (spec 0a). Empty = Configs/CLW/Scenarios/<world file name>.conf, a scenario listing the world, else the auto scenario (CLW_AutoScenario)", "conf")]
	protected ResourceName m_ScenarioConfig;

	// The prefab's scenario attribute (read by the war runtime at the game start).
	ResourceName GetScenarioAttribute()
	{
		return m_ScenarioConfig;
	}

	// The game mode entity that hosts the war: this one, or the taken-over one (SCR_GameModeEditor ...). Null before the game start.
	static SCR_BaseGameMode GetInstance()
	{
		return CLW_WarRuntime.GetOwner();
	}

	static CLW_ScenarioConfig GetScenario()
	{
		return CLW_ScenarioConfig.Get();
	}

	static CLW_Bases GetBases()
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime)
			return null;

		return runtime.GetBases();
	}

	static CLW_ControlPoints GetControlPoints()
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime)
			return null;

		return runtime.GetControlPoints();
	}

	static CLW_WarState GetWarState()
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime)
			return null;

		return runtime.GetWarState();
	}

	static CLW_SquadRegistry GetRegistry()
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime)
			return null;

		return runtime.GetRegistry();
	}

	static CLW_ForceCatalog GetForceCatalog()
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime)
			return null;

		return runtime.GetForceCatalog();
	}

	static CLW_ForceSpawner GetForceSpawner()
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime)
			return null;

		return runtime.GetForceSpawner();
	}

	// The faction's commander (LLM bridge wrapping the rule commander), null for an unknown key.
	static CLW_RemoteCommander GetCommander(string factionKey)
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime)
			return null;

		return runtime.GetCommander(factionKey);
	}

	static CLW_VehicleManager GetVehicleManager()
	{
		CLW_WarRuntime runtime = CLW_WarRuntime.Get();
		if (!runtime)
			return null;

		return runtime.GetVehicleManager();
	}
}

// Clausewitz game mode: AI High Command war with human players on both sides.
// Inherits the whole player flow (spawn, groups, tasks, markers) from GameMode_Base.et.

class CLW_GameModeClass : SCR_BaseGameModeClass
{
}

class CLW_GameMode : SCR_BaseGameMode
{
	protected override void OnGameStart()
	{
		super.OnGameStart();
		Print("CLW: game mode started", LogLevel.NORMAL);
	}
}

// Spawn logic of the Clausewitz game mode (set as m_SpawnLogic in CLW_GameMode.et). Own subclass of the vanilla menu logic: a modded
// SCR_*SpawnLogic made the class unknown to the prefab loader ("Unknown class", 2026-10-12).
//
// The behaviour itself (setup-phase autospawn, forced faction, HQ-squad wait before the deploy menu) lives in the modded SCR_MenuSpawnLogic
// (CLW_SpawnFlow.c), so a taken-over Game Master game mode, whose prefab keeps the vanilla logic, gets the same flow. This subclass only
// switches the flow on for our own prefab.
[BaseContainerProps(category: "Respawn")]
class CLW_SpawnLogic : SCR_MenuSpawnLogic
{
	override bool CLW_SpawnFlowActive()
	{
		return true;
	}
}

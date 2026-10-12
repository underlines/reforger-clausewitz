// Runtime swap of the spawn logic of the vanilla respawn system (M4 takeover).
//
// The spawn logic is a [BaseContainerProps] object held in the prefab attribute m_SpawnLogic of SCR_RespawnSystemComponent. Modding the
// logic class itself is NOT possible (the prefab loader then reports "Unknown class 'SCR_MenuSpawnLogic'" in every vanilla prefab that uses
// it and the component loses its logic), so the takeover of a Game Master game mode replaces the instance at runtime instead. This is a
// modded COMPONENT class (not a BaseContainerProps class), which only adds one method.
//
// Double handling: after the swap the respawn system calls only the new logic (m_SpawnLogic is the single dispatch point of every
// OnPlayer*_S event). The only thing the old logic registered itself is the per-player invokers of OnPlayerRegistered_S; for players that
// are already registered they are removed from the old logic and added for the new one. The swap runs at the game start, normally before
// any player exists.

modded class SCR_RespawnSystemComponent
{
	// Replaces m_SpawnLogic by 'logic' and initialises it. Returns false (and changes nothing) when it cannot.
	bool CLW_ReplaceSpawnLogic(SCR_SpawnLogic logic, out string previousName, out string reason)
	{
		previousName = "none";
		reason = "";
		if (!logic)
		{
			reason = "no logic given";
			return false;
		}

		SCR_SpawnLogic previous = m_SpawnLogic;
		if (previous)
			previousName = previous.ClassName();

		array<int> playerIds = {};
		GetGame().GetPlayerManager().GetPlayers(playerIds);
		foreach (int oldPlayerId : playerIds)
		{
			if (!previous)
				break;

			SCR_RespawnComponent oldRespawn = previous.GetPlayerRespawnComponent_S(oldPlayerId);
			if (oldRespawn)
			{
				oldRespawn.GetOnRespawnRequestInvoker_S().Remove(previous.OnPlayerSpawnRequest_S);
				oldRespawn.GetOnRespawnResponseInvoker_S().Remove(previous.OnPlayerSpawnResponse_S);
			}
		}

		m_SpawnLogic = logic;
		m_SpawnLogic.OnInit(this);
		foreach (int newPlayerId : playerIds)
		{
			m_SpawnLogic.OnPlayerRegistered_S(newPlayerId);
		}

		return true;
	}
}

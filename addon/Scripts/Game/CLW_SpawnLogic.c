// Spawn logic of the Clausewitz game mode (set as m_SpawnLogic in CLW_GameMode.et). Own subclass of the vanilla menu logic: a modded
// SCR_*SpawnLogic made the class unknown to the prefab loader ("Unknown class", 2026-10-12; confirmed again for SCR_MenuSpawnLogic in the
// M4 takeover e2e: NEVER mod a vanilla [BaseContainerProps] class that vanilla prefabs reference, the prefab loses the object).
// Used by CLW_GameMode.et, and by the takeover of a Game Master game mode: CLW_WarRuntime swaps a new instance into the vanilla respawn
// system at runtime (CLW_RespawnSwap.c).
//
//   SETUP phase : no character, no menu. The player is parked (polled until the war starts) so nobody spawns or sees the deploy menu
//                 before the sides exist; the setup dialog of the owner runs without a body. The menu opens when the war starts.
//   Otherwise   : the vanilla deploy menu (faction tiles = the two playable sides, loadouts, base spawn points, HQ squad list).
//                 It waits until at least one HQ squad is joinable (else the group selector has nothing and Deploy stays disabled).
// Group choice from the menu ends in CLW_SquadJoin / CLW_PlayerSquad like every other entry point.
[BaseContainerProps(category: "Respawn")]
class CLW_SpawnLogic : SCR_MenuSpawnLogic
{
	protected ref map<int, int> m_mSquadWaits = new map<int, int>();

	// True while the war is still being set up (no war state without a scenario: vanilla menu behaviour).
	protected bool IsSetup()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		return war && !war.IsRunning() && !war.IsEnded();
	}

	// Poll interval of a parked player waiting for the war to leave setup.
	static const int SETUP_PARK_POLL_MS = 1000;

	protected ref map<int, bool> m_mParked = new map<int, bool>();

	protected override void DoSpawn_S(int playerId)
	{
		if (!GetGame().GetPlayerManager().GetPlayerController(playerId))
		{
			m_mParked.Remove(playerId);
			return;	// disconnected while waiting
		}

		if (IsSetup())
		{
			if (!m_mParked.Contains(playerId))	// one pending poll per player
			{
				m_mParked.Set(playerId, true);
				Print(string.Format("CLW: spawn logic player %1 parked until the war starts (setup phase)", playerId), LogLevel.NORMAL);
				GetGame().GetCallqueue().CallLater(ParkPoll_S, SETUP_PARK_POLL_MS, false, playerId);
			}
			return;
		}

		if (!SquadsReady(playerId))
			return;

		Print(string.Format("CLW: spawn logic player %1: deploy menu", playerId), LogLevel.NORMAL);
		super.DoSpawn_S(playerId);
	}

	protected void ParkPoll_S(int playerId)
	{
		if (!GetGame().GetPlayerManager().GetPlayerController(playerId))
		{
			m_mParked.Remove(playerId);
			return;
		}

		if (IsSetup())
		{
			GetGame().GetCallqueue().CallLater(ParkPoll_S, SETUP_PARK_POLL_MS, false, playerId);
			return;
		}

		m_mParked.Remove(playerId);
		Print(string.Format("CLW: spawn logic player %1 released: war started", playerId), LogLevel.NORMAL);
		DoSpawn_S(playerId);
	}

	// Death / entity lost: setup = park (no menu, no character), else the vanilla menu.
	override void OnPlayerEntityLost_S(int playerId)
	{
		if (IsSetup())
		{
			DoSpawn_S(playerId);	// parks; skips SCR_MenuSpawnLogic's NotifyReadyForSpawn_S, so the client never opens the menu
			return;
		}

		super.OnPlayerEntityLost_S(playerId);
	}

	// Persistence reconnect hands over via NotifyPlayerReadyForSpawn (SCR_SpawnLogic.OnControlledCharacterAvailable): no menu in setup.
	protected override void NotifyPlayerReadyForSpawn(int playerId)
	{
		if (IsSetup())
		{
			DoSpawn_S(playerId);
			return;
		}

		super.NotifyPlayerReadyForSpawn(playerId);
	}

	// Menu waits for HQ squads (start forces are spawned a few seconds after the war start). Gives up after DEPLOY_WAIT_SQUADS_MAX tries.
	protected bool SquadsReady(int playerId)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!registry || registry.CountJoinable() > 0)
		{
			m_mSquadWaits.Remove(playerId);
			return true;
		}

		int tries;
		m_mSquadWaits.Find(playerId, tries);
		tries++;
		if (tries > CLW_WarConfig.DEPLOY_WAIT_SQUADS_MAX)
		{
			m_mSquadWaits.Remove(playerId);
			Print(string.Format("CLW: spawn logic player %1: still no joinable squad, opening the deploy menu anyway", playerId), LogLevel.WARNING);
			return true;
		}

		m_mSquadWaits.Set(playerId, tries);
		if (tries == 1)
			Print(string.Format("CLW: spawn logic player %1 waits for HQ squads before the deploy menu opens", playerId), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(DoSpawn_S, CLW_WarConfig.DEPLOY_WAIT_SQUADS_MS, false, playerId);
		return false;
	}
}

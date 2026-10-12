// Spawn logic of the Clausewitz game mode (set as m_SpawnLogic in CLW_GameMode.et). Own subclass of the vanilla menu logic: a modded
// SCR_*SpawnLogic made the class unknown to the prefab loader ("Unknown class", 2026-10-12; confirmed again for SCR_MenuSpawnLogic in the
// M4 takeover e2e: NEVER mod a vanilla [BaseContainerProps] class that vanilla prefabs reference, the prefab loses the object).
// Used by CLW_GameMode.et, and by the takeover of a Game Master game mode: CLW_WarRuntime swaps a new instance into the vanilla respawn
// system at runtime (CLW_RespawnSwap.c).
//
//   SETUP phase : no menu. The player is auto-spawned on the default side at its base (forced faction + a spawnable loadout, never the
//                 saved-arsenal one), so the war setup dialog of the owner works and nobody sees the deploy menu before the sides exist.
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

	protected override bool GetForcedFaction(out Faction faction)
	{
		if (IsSetup() && CLW_Sides.GetPlayerFaction(faction))
			return true;

		return super.GetForcedFaction(faction);
	}

	protected override void DoSpawn_S(int playerId)
	{
		if (!GetGame().GetPlayerManager().GetPlayerController(playerId))
			return;	// disconnected while waiting

		if (IsSetup())
		{
			AutoSpawn_S(playerId);
			return;
		}

		if (!SquadsReady(playerId))
			return;

		Print(string.Format("CLW: spawn logic player %1: deploy menu", playerId), LogLevel.NORMAL);
		super.DoSpawn_S(playerId);
	}

	// Death / entity lost: setup = respawn at once without a menu, else the vanilla menu.
	override void OnPlayerEntityLost_S(int playerId)
	{
		if (IsSetup())
		{
			DoSpawn_S(playerId);	// skips SCR_MenuSpawnLogic's NotifyReadyForSpawn_S, so the client never opens the menu
			return;
		}

		super.OnPlayerEntityLost_S(playerId);
	}

	override void OnPlayerSpawnFailed_S(int playerId)
	{
		super.OnPlayerSpawnFailed_S(playerId);
		if (IsSetup())
		{
			const int delay = Math.RandomFloat(900, 1100);
			GetGame().GetCallqueue().CallLater(DoSpawn_S, delay, false, playerId);
		}
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

	// SCR_AutoSpawnLogic.DoSpawn_S with the scenario's default side and a spawnable (non-arsenal) loadout.
	protected void AutoSpawn_S(int playerId)
	{
		Faction target;
		if (!CLW_Sides.GetPlayerFaction(target))
		{
			Print("CLW: spawn logic setup: no default player faction in the scenario, retrying", LogLevel.WARNING);
			OnPlayerSpawnFailed_S(playerId);
			return;
		}

		GetPlayerFactionComponent_S(playerId).RequestFaction(target);

		SCR_BasePlayerLoadout targetLoadout;
		array<SCR_BasePlayerLoadout> loadouts = {};
		if (CLW_Sides.GetSpawnLoadouts(target.GetFactionKey(), loadouts) > 0)
			targetLoadout = loadouts.GetRandomElement();
		else
			targetLoadout = GetGame().GetLoadoutManager().GetRandomFactionLoadout(target);

		GetPlayerLoadoutComponent_S(playerId).RequestLoadout(targetLoadout);

		Faction faction = GetPlayerFactionComponent_S(playerId).GetAffiliatedFaction();
		if (!faction)
		{
			OnPlayerSpawnFailed_S(playerId);
			return;
		}

		SCR_BasePlayerLoadout loadout = GetPlayerLoadoutComponent_S(playerId).GetLoadout();
		if (!loadout)
		{
			OnPlayerSpawnFailed_S(playerId);
			return;
		}

		SCR_SpawnPoint point = SCR_SpawnPoint.GetRandomSpawnPointForFaction(faction.GetFactionKey());
		if (!point)
		{
			OnPlayerSpawnFailed_S(playerId);
			return;
		}

		SCR_SpawnPointSpawnData data = new SCR_SpawnPointSpawnData(loadout.GetLoadoutResource(), point.GetRplId());
		if (!GetPlayerRespawnComponent_S(playerId).CanSpawn(data))
		{
			OnPlayerSpawnFailed_S(playerId);
			return;
		}

		GetPlayerRespawnComponent_S(playerId).RequestSpawn(data);
	}
}

// Clausewitz spawn flow on the vanilla menu spawn logic (M4 takeover, research option (a)).
//
// The spawn logic is a prefab attribute (m_SpawnLogic of SCR_RespawnSystemComponent), so a taken-over Game Master game mode keeps the
// vanilla SCR_MenuSpawnLogic and its prefab cannot be given CLW_SpawnLogic. The behaviour therefore lives here, in the modded vanilla class,
// switched on per instance by CLW_SpawnFlowActive():
//   - CLW_SpawnLogic (our prefab) overrides it to true: the flow always runs, as before;
//   - every other menu spawn logic runs it only while the war runtime is a takeover (CLW_WarRuntime.IsTakeover, server only).
// With the flow off every override is a plain super call (vanilla behaviour). History: the flow used to be the body of CLW_SpawnLogic; a
// modded SCR_*SpawnLogic named BY ITS OWN NAME in a prefab made the class unknown to the prefab loader (2026-10-12), which is why our prefab
// still names the subclass CLW_SpawnLogic. Here we only mod the vanilla class the vanilla prefabs already use [verify on the takeover run].
//
//   SETUP phase : no menu. The player is auto-spawned on the default side at its base (forced faction + a spawnable loadout, never the
//                 saved-arsenal one), so the war setup dialog of the owner works and nobody sees the deploy menu before the sides exist.
//   Otherwise   : the vanilla deploy menu (faction tiles = the two playable sides, loadouts, base spawn points, HQ squad list).
//                 It waits until at least one HQ squad is joinable (else the group selector has nothing and Deploy stays disabled).
// Group choice from the menu ends in CLW_SquadJoin / CLW_PlayerSquad like every other entry point.

modded class SCR_MenuSpawnLogic
{
	// Squads-ready wait counters per player (created on first use: field initializers are avoided in modded classes).
	protected ref map<int, int> m_mClwSquadWaits;

	// True when this spawn logic instance runs the Clausewitz flow.
	bool CLW_SpawnFlowActive()
	{
		return CLW_WarRuntime.IsTakeover();
	}

	// True while the war is still being set up (no war state without a scenario: vanilla menu behaviour).
	protected bool CLW_IsSetup()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		return war && !war.IsRunning() && !war.IsEnded();
	}

	protected override bool GetForcedFaction(out Faction faction)
	{
		if (CLW_SpawnFlowActive() && CLW_IsSetup() && CLW_Sides.GetPlayerFaction(faction))
			return true;

		return super.GetForcedFaction(faction);
	}

	protected override void DoSpawn_S(int playerId)
	{
		if (!CLW_SpawnFlowActive())
		{
			super.DoSpawn_S(playerId);
			return;
		}

		if (!GetGame().GetPlayerManager().GetPlayerController(playerId))
			return;	// disconnected while waiting

		if (CLW_IsSetup())
		{
			CLW_AutoSpawn_S(playerId);
			return;
		}

		if (!CLW_SquadsReady(playerId))
			return;

		Print(string.Format("CLW: spawn logic player %1: deploy menu", playerId), LogLevel.NORMAL);
		super.DoSpawn_S(playerId);
	}

	// Death / entity lost: setup = respawn at once without a menu, else the vanilla menu.
	override void OnPlayerEntityLost_S(int playerId)
	{
		if (CLW_SpawnFlowActive() && CLW_IsSetup())
		{
			DoSpawn_S(playerId);	// skips SCR_MenuSpawnLogic's NotifyReadyForSpawn_S, so the client never opens the menu
			return;
		}

		super.OnPlayerEntityLost_S(playerId);
	}

	override void OnPlayerSpawnFailed_S(int playerId)
	{
		super.OnPlayerSpawnFailed_S(playerId);
		if (CLW_SpawnFlowActive() && CLW_IsSetup())
		{
			const int delay = Math.RandomFloat(900, 1100);
			GetGame().GetCallqueue().CallLater(DoSpawn_S, delay, false, playerId);
		}
	}

	// Menu waits for HQ squads (start forces are spawned a few seconds after the war start). Gives up after DEPLOY_WAIT_SQUADS_MAX tries.
	protected bool CLW_SquadsReady(int playerId)
	{
		if (!m_mClwSquadWaits)
			m_mClwSquadWaits = new map<int, int>();

		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!registry || registry.CountJoinable() > 0)
		{
			m_mClwSquadWaits.Remove(playerId);
			return true;
		}

		int tries;
		m_mClwSquadWaits.Find(playerId, tries);
		tries++;
		if (tries > CLW_WarConfig.DEPLOY_WAIT_SQUADS_MAX)
		{
			m_mClwSquadWaits.Remove(playerId);
			Print(string.Format("CLW: spawn logic player %1: still no joinable squad, opening the deploy menu anyway", playerId), LogLevel.WARNING);
			return true;
		}

		m_mClwSquadWaits.Set(playerId, tries);
		if (tries == 1)
			Print(string.Format("CLW: spawn logic player %1 waits for HQ squads before the deploy menu opens", playerId), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(DoSpawn_S, CLW_WarConfig.DEPLOY_WAIT_SQUADS_MS, false, playerId);
		return false;
	}

	// SCR_AutoSpawnLogic.DoSpawn_S with the scenario's default side and a spawnable (non-arsenal) loadout.
	protected void CLW_AutoSpawn_S(int playerId)
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

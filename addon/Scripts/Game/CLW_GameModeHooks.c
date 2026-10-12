// Thin hooks of Clausewitz on EVERY game mode entity (M4 takeover, option C): CLW_GameMode (our prefab) and, with the takeover switch on,
// a foreign one such as the vanilla SCR_GameModeEditor of a Game Master world. Each hook calls super first, then forwards to the war
// runtime ONLY when this very entity hosts it (CLW_WarRuntime.Of), so a game mode Clausewitz did not take over behaves like vanilla.
// The war status RPC lives here (not on CLW_GameMode) because an RPC of a modded class works on every subclass instance.

modded class SCR_BaseGameMode
{
	protected override void OnGameStart()
	{
		super.OnGameStart();
		CLW_WarRuntime.OnGameModeStart(this);
	}

	// Respawn gate: a faction without tickets (or a finished war) cannot spawn players.
	override bool CanPlayerSpawn_S(SCR_SpawnRequestComponent requestComponent, SCR_SpawnHandlerComponent handlerComponent, SCR_SpawnData data, out SCR_ESpawnResult result = SCR_ESpawnResult.SPAWN_NOT_ALLOWED)
	{
		if (!super.CanPlayerSpawn_S(requestComponent, handlerComponent, data, result))
			return false;

		CLW_WarRuntime runtime = CLW_WarRuntime.Of(this);
		if (!runtime)
			return true;

		return runtime.CanPlayerSpawn(requestComponent, result);
	}

	// Kill accounting: every destroyed controllable (AI, player, vehicle) costs its faction tickets.
	protected override void OnControllableDestroyedEx(notnull SCR_InstigatorContextData instigatorContextData)
	{
		super.OnControllableDestroyedEx(instigatorContextData);
		CLW_WarRuntime runtime = CLW_WarRuntime.Of(this);
		if (runtime)
			runtime.OnEntityDestroyed(instigatorContextData);
	}

	// Players: the war state de-duplicates against OnControllableDestroyedEx by entity.
	protected override void OnPlayerKilledEx(notnull SCR_InstigatorContextData instigatorContextData)
	{
		super.OnPlayerKilledEx(instigatorContextData);
		CLW_WarRuntime runtime = CLW_WarRuntime.Of(this);
		if (runtime)
			runtime.OnEntityDestroyed(instigatorContextData);
	}

	// Player flow: after the spawn the player is in an HQ squad (menu choice) or gets the fallback auto-join.
	override void OnPlayerSpawnFinalize_S(SCR_SpawnRequestComponent requestComponent, SCR_SpawnHandlerComponent handlerComponent, SCR_SpawnData data, IEntity entity)
	{
		super.OnPlayerSpawnFinalize_S(requestComponent, handlerComponent, data, entity);
		CLW_WarRuntime runtime = CLW_WarRuntime.Of(this);
		if (runtime)
			runtime.OnPlayerSpawnFinalize(entity);
	}

	// Ticket counter HUD: server -> every client (also run locally for the listen-server host; the broadcast does not loop back).
	void CLW_BroadcastWarStatus(string wire)
	{
		if (!Replication.IsServer())
			return;

		Rpc(CLW_RpcAsk_WarStatus, wire);
		if (SCR_PlayerController.GetLocalPlayerId() > 0)
			CLW_WarHud.Update(wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Broadcast)]
	protected void CLW_RpcAsk_WarStatus(string wire)
	{
		CLW_WarRuntime.OnClientStatus();
		CLW_WarHud.Update(wire);
	}
}

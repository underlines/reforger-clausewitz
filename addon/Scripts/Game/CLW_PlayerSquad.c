// CLW_PlayerSquad: the ONLY sanctioned way to put a player in command of a scene-placed AI squad.
//
// PATTERN (never break it): do NOT call SCR_GroupsManagerComponent.AddPlayerToGroup / AssignGroupID / group.AddPlayer
// directly for player squads. Always call CLW_PlayerSquad.Join(squad, playerId). Reason (vanilla, verified 2026-10-12):
// the radial commanding menu looks up FindGroup(controller.GetGroupID()); the controller ID is only set by
// SCR_PlayerControllerGroupComponent.RPC_AskJoinGroup, and the orders are applied to the SLAVE group of the player's
// group, so the AI must live in the slave group and the squad must carry a real group ID.
//
// Join does, server side, in a retried and idempotent order:
//   1. RegisterGroup + AssignGroupID (real ID)       2. controller.RPC_AskJoinGroup(id) (sets controller group ID, adds player)
//   3. player becomes squad leader                    4. slave group (Slave_Group.et) spawned + linked via RequestSetGroupSlave
//   5. all living AI moved from the squad into the slave group with AddAIToSlaveGroup (after a replication delay)
class CLW_PlayerSquad
{
	protected static const int MAX_ATTEMPTS = 12;
	protected static const int RETRY_MS = 1000;
	protected static const int SLAVE_REPLICATION_MS = 2500;
	protected static const int VERIFY_MS = 4000;
	protected static const int RECONCILE_MS = 3000;
	protected static const int RECONCILE_ROUNDS = 40;

	// Raise the per-player AI limit of the vanilla commanding manager (default 8, our squads have 9 AI). Call on every machine.
	static void ApplyCommandingLimit()
	{
		SCR_CommandingManagerComponent commanding = SCR_CommandingManagerComponent.GetInstance();
		if (!commanding)
		{
			Print("CLW: PlayerSquad no SCR_CommandingManagerComponent on the game mode (commanding menu will be empty)", LogLevel.WARNING);
			return;
		}

		if (commanding.GetMaxAIPerGroup() != -1)
		{
			Print(string.Format("CLW: PlayerSquad max AI per group %1 -> -1 (unlimited)", commanding.GetMaxAIPerGroup()), LogLevel.NORMAL);
			commanding.SetMaxAIPerGroup(-1);
		}
	}

	// Put playerId in command of squad (server only). Safe to call again (respawn, second player).
	static void Join(SCR_AIGroup squad, int playerId)
	{
		if (!Replication.IsServer())
			return;

		if (!squad)
		{
			Print("CLW: PlayerSquad Join: no squad", LogLevel.WARNING);
			return;
		}

		Print(string.Format("CLW: PlayerSquad Join squad=%1 player=%2", squad, playerId), LogLevel.NORMAL);
		ApplyCommandingLimit();
		Step(squad, playerId, 1);
	}

	protected static void Retry(SCR_AIGroup squad, int playerId, int attempt, string reason, int delayMs)
	{
		if (attempt >= MAX_ATTEMPTS)
		{
			Print(string.Format("CLW: PlayerSquad GIVING UP after %1 attempts (player=%2): %3", attempt, playerId, reason), LogLevel.ERROR);
			return;
		}

		Print(string.Format("CLW: PlayerSquad attempt %1/%2 waiting (player=%3): %4", attempt, MAX_ATTEMPTS, playerId, reason), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(Step, delayMs, false, squad, playerId, attempt + 1);
	}

	protected static void Step(SCR_AIGroup squad, int playerId, int attempt)
	{
		if (!squad)
			return;

		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		SCR_PlayerControllerGroupComponent controller = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
		if (!groups || !controller)
		{
			Retry(squad, playerId, attempt, "no groups manager / player controller group component yet", RETRY_MS);
			return;
		}

		Faction playerFaction = SCR_FactionManager.SGetPlayerFaction(playerId);
		if (!playerFaction || squad.GetFaction() != playerFaction)
		{
			Retry(squad, playerId, attempt, "player faction not yet equal to squad faction", RETRY_MS);
			return;
		}

		// 1. registered group with a real ID (both idempotent)
		groups.RegisterGroup(squad);
		groups.AssignGroupID(squad);
		squad.SetDeleteWhenEmpty(false);
		squad.SetCanDeleteIfNoPlayer(false);
		int squadId = squad.GetGroupID();
		if (squadId < 0)
		{
			Retry(squad, playerId, attempt, "squad has no group ID yet", RETRY_MS);
			return;
		}

		// 2. join through the controller so controller.m_iGroupID is set (and replicated) = what the commanding menu reads
		if (controller.GetGroupID() != squadId || !squad.IsPlayerInGroup(playerId))
		{
			controller.RPC_AskJoinGroup(squadId);
			Print(string.Format("CLW: PlayerSquad joined via controller: player=%1 controllerGroupId=%2 squadId=%3 inGroup=%4", playerId, controller.GetGroupID(), squadId, squad.IsPlayerInGroup(playerId)), LogLevel.NORMAL);
		}
		if (controller.GetGroupID() != squadId || !squad.IsPlayerInGroup(playerId))
		{
			Retry(squad, playerId, attempt, string.Format("join not applied (controllerGroupId=%1 squadId=%2)", controller.GetGroupID(), squadId), RETRY_MS);
			return;
		}

		// 3. leader: the first human of the squad leads it
		if (squad.GetLeaderID() != playerId && squad.GetPlayerCount() <= 1)
		{
			squad.SetGroupLeader(playerId);
			Print(string.Format("CLW: PlayerSquad SetGroupLeader player=%1 -> leaderId=%2 isLeader=%3", playerId, squad.GetLeaderID(), squad.IsPlayerLeader(playerId)), LogLevel.NORMAL);
		}

		// 4. slave group, created the way SCR_GroupsManagerComponent.CreateNewPlayableGroup does
		SCR_AIGroup slave = squad.GetSlave();
		if (!slave)
		{
			if (!CreateSlave(squad, groups))
			{
				Retry(squad, playerId, attempt, "slave group could not be created / linked", RETRY_MS);
				return;
			}

			// vanilla re-sends the link to clients after 2 s; give the slave time to replicate before AI are added
			Retry(squad, playerId, attempt, "slave group created, waiting for replication", SLAVE_REPLICATION_MS);
			return;
		}

		// 5. move the AI
		int moved;
		int skipped;
		MoveAI(squad, slave, controller, moved, skipped);
		Print(string.Format("CLW: PlayerSquad player=%1 controllerGroupId=%2 squadId=%3 slaveExists=true slaveAgents=%4 movedNow=%5 skipped=%6 squadAgentsLeft=%7 leaderId=%8", playerId, controller.GetGroupID(), squadId, slave.GetAgentsCount(), moved, skipped, squad.GetAgentsCount(), squad.GetLeaderID()), LogLevel.NORMAL);
		if (skipped > 0)
		{
			Retry(squad, playerId, attempt, string.Format("%1 AI could not be moved yet", skipped), RETRY_MS);
			return;
		}

		Print(string.Format("CLW: PlayerSquad READY player=%1 squadId=%2 controllerGroupId=%3 slaveAgents=%4 leaderEntity=%5", playerId, squadId, controller.GetGroupID(), slave.GetAgentsCount(), squad.GetLeaderEntity()), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(Verify, VERIFY_MS, false, squad, playerId);
		GetGame().GetCallqueue().CallLater(Reconcile, RECONCILE_MS, false, squad, playerId, 1);
	}

	// Group members spawn in waves, so AI that appear in the scene group after the join are moved to the slave group too.
	protected static void Reconcile(SCR_AIGroup squad, int playerId, int round)
	{
		if (!squad || round > RECONCILE_ROUNDS)
			return;

		SCR_AIGroup slave = squad.GetSlave();
		SCR_PlayerControllerGroupComponent controller = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
		if (slave && controller)
		{
			int moved;
			int skipped;
			MoveAI(squad, slave, controller, moved, skipped);
			if (moved > 0)
				Print(string.Format("CLW: PlayerSquad reconcile round %1: moved %2 late AI, slaveAgents=%3", round, moved, slave.GetAgentsCount()), LogLevel.NORMAL);
		}

		GetGame().GetCallqueue().CallLater(Reconcile, RECONCILE_MS, false, squad, playerId, round + 1);
	}

	// Spawn the slave group prefab and link it to the master. RequestSetGroupSlave calls RPC_DoSetGroupSlave locally first (SetSlave takes effect
	// on the server at once) and re-broadcasts to clients after 2 s.
	protected static bool CreateSlave(SCR_AIGroup squad, SCR_GroupsManagerComponent groups)
	{
		SCR_CommandingManagerComponent commanding = SCR_CommandingManagerComponent.GetInstance();
		if (!commanding)
		{
			Print("CLW: PlayerSquad no SCR_CommandingManagerComponent, cannot create slave group", LogLevel.ERROR);
			return false;
		}

		IEntity slaveEntity = GetGame().SpawnEntityPrefab(Resource.Load(commanding.GetGroupPrefab()));
		SCR_AIGroup slave = SCR_AIGroup.Cast(slaveEntity);
		if (!slave)
		{
			Print("CLW: PlayerSquad slave prefab did not spawn as SCR_AIGroup", LogLevel.ERROR);
			return false;
		}

		slave.DeactivateAI();	// activated by AddAIToSlaveGroup when the first AI arrives
		slave.SetImportance(SCR_EAISpawnImportance.CRITICAL);
		slave.SetDeleteWhenEmpty(false);

		RplComponent slaveRpl = RplComponent.Cast(slave.FindComponent(RplComponent));
		RplComponent masterRpl = RplComponent.Cast(squad.FindComponent(RplComponent));
		if (!slaveRpl || !masterRpl)
		{
			Print("CLW: PlayerSquad master/slave has no RplComponent", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(slave);
			return false;
		}

		groups.RequestSetGroupSlave(masterRpl.Id(), slaveRpl.Id());
		if (!squad.GetSlave())
		{
			Print("CLW: PlayerSquad RequestSetGroupSlave did not link the slave", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(slave);
			return false;
		}

		Print(string.Format("CLW: PlayerSquad slave group created %1 linked to squad %2", slave, squad), LogLevel.NORMAL);
		return true;
	}

	// Move every living AI soldier of the squad (not players) into the slave group: remove from the scene group first, then vanilla AddAIToSlaveGroup.
	protected static void MoveAI(SCR_AIGroup squad, SCR_AIGroup slave, SCR_PlayerControllerGroupComponent controller, out int moved, out int skipped)
	{
		moved = 0;
		skipped = 0;
		array<AIAgent> agents = {};
		squad.GetAgents(agents);

		array<SCR_ChimeraCharacter> toMove = {};
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(agent.GetControlledEntity());
			if (!character)
				continue;

			if (GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(character) > 0)
				continue;

			CharacterControllerComponent charController = CharacterControllerComponent.Cast(character.FindComponent(CharacterControllerComponent));
			if (charController && charController.GetLifeState() == ECharacterLifeState.DEAD)
				continue;

			toMove.Insert(character);
		}

		int limit = -1;
		SCR_CommandingManagerComponent commanding = SCR_CommandingManagerComponent.GetInstance();
		if (commanding)
			limit = commanding.GetMaxAIPerGroup();

		foreach (SCR_ChimeraCharacter ai : toMove)
		{
			if (limit != -1 && slave.GetAgentsCount() >= limit)
			{
				Print(string.Format("CLW: PlayerSquad AI limit %1 reached, %2 not moved", limit, ai), LogLevel.WARNING);
				skipped++;
				continue;
			}

			squad.RemoveAgentFromControlledEntity(ai);
			controller.AddAIToSlaveGroup(ai, squad);
			moved++;
		}
	}

	protected static void Verify(SCR_AIGroup squad, int playerId)
	{
		if (!squad)
			return;

		SCR_PlayerControllerGroupComponent controller = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
		SCR_AIGroup slave = squad.GetSlave();
		int controllerGroupId = -2;
		if (controller)
			controllerGroupId = controller.GetGroupID();
		int slaveAgents = -1;
		AIWaypoint slaveWaypoint;
		if (slave)
		{
			slaveAgents = slave.GetAgentsCount();
			slaveWaypoint = slave.GetCurrentWaypoint();
		}

		Print(string.Format("CLW: PlayerSquad VERIFY player=%1 controllerGroupId=%2 squadId=%3 slaveExists=%4 slaveAgents=%5 slaveWaypoint=%6 squadLeaderId=%7", playerId, controllerGroupId, squad.GetGroupID(), slave != null, slaveAgents, slaveWaypoint, squad.GetLeaderID()), LogLevel.NORMAL);
	}
}

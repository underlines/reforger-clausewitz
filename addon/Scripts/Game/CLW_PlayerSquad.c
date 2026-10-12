// CLW_PlayerSquad: the ONLY sanctioned way to put a player in command of a scene-placed AI squad.
//
// PATTERN (never break it): do NOT call SCR_GroupsManagerComponent.AddPlayerToGroup / AssignGroupID / group.AddPlayer
// directly for player squads. Always go through this service. Reason (vanilla, verified 2026-10-12):
// the radial commanding menu looks up FindGroup(controller.GetGroupID()); the controller ID is only set by
// SCR_PlayerControllerGroupComponent.RPC_AskJoinGroup, and the orders are applied to the SLAVE group of the player's
// group, so the AI must live in the slave group and the squad must carry a real group ID.
//
// This class is the SERVICE behind every entry point (deploy menu, groups menu, command post, auto-join fallback):
//   Join(squad, pid)       = join through the controller (RPC_AskJoinGroup) + Establish. Programmatic entry (auto-join, command post).
//   Establish(squad, pid)  = the vanilla membership already exists (a menu did it): make the squad commandable. Called by the
//                            server hooks in CLW_SquadJoin and by the spawn flow. Idempotent: a running chain is not doubled.
//   Release(squad)         = the last human left: AI back from the slave to the master group, HQ commands the squad again.
//
// Establish does, server side, in a retried and idempotent order:
//   1. RegisterGroup + AssignGroupID (real ID)       2. controller.RPC_AskJoinGroup(id) (Join only; sets controller group ID, adds player)
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

	protected static ref array<string> s_aActive = {};				// running chains ("player/squad")
	protected static ref array<SCR_AIGroup> s_aReconciling = {};	// squads with a running reconcile loop (weak)

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

	// Put playerId in command of squad (server only): vanilla join through the controller, then Establish.
	// Safe to call again (respawn, second player).
	static void Join(SCR_AIGroup squad, int playerId)
	{
		Begin(squad, playerId, true);
	}

	// The player is (becoming) a member of the squad through the vanilla group code: make the squad commandable (server only).
	static void Establish(SCR_AIGroup squad, int playerId)
	{
		Begin(squad, playerId, false);
	}

	protected static string KeyOf(SCR_AIGroup squad, int playerId)
	{
		return string.Format("%1/%2", playerId, squad);
	}

	protected static string Label(bool askJoin)
	{
		if (askJoin)
			return "Join";

		return "Establish";
	}

	protected static void Begin(SCR_AIGroup squad, int playerId, bool askJoin)
	{
		if (!Replication.IsServer())
			return;

		if (!squad)
		{
			Print("CLW: PlayerSquad Join/Establish: no squad", LogLevel.WARNING);
			return;
		}

		string key = KeyOf(squad, playerId);
		if (s_aActive.Contains(key))
		{
			Print(string.Format("CLW: PlayerSquad %1 squad=%2 player=%3: already in progress", Label(askJoin), squad, playerId), LogLevel.NORMAL);
			return;
		}

		s_aActive.Insert(key);
		Print(string.Format("CLW: PlayerSquad %1 squad=%2 player=%3", Label(askJoin), squad, playerId), LogLevel.NORMAL);
		ApplyCommandingLimit();
		Step(squad, playerId, 1, askJoin);
	}

	protected static void Finish(SCR_AIGroup squad, int playerId)
	{
		s_aActive.RemoveItem(KeyOf(squad, playerId));
	}

	protected static void Retry(SCR_AIGroup squad, int playerId, int attempt, string reason, int delayMs, bool askJoin)
	{
		if (attempt >= MAX_ATTEMPTS)
		{
			Print(string.Format("CLW: PlayerSquad GIVING UP after %1 attempts (player=%2): %3", attempt, playerId, reason), LogLevel.ERROR);
			Finish(squad, playerId);
			return;
		}

		Print(string.Format("CLW: PlayerSquad attempt %1/%2 waiting (player=%3): %4", attempt, MAX_ATTEMPTS, playerId, reason), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(Step, delayMs, false, squad, playerId, attempt + 1, askJoin);
	}

	protected static void Step(SCR_AIGroup squad, int playerId, int attempt, bool askJoin)
	{
		if (!squad)
			return;

		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		SCR_PlayerControllerGroupComponent controller = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
		if (!groups || !controller)
		{
			Retry(squad, playerId, attempt, "no groups manager / player controller group component yet", RETRY_MS, askJoin);
			return;
		}

		Faction playerFaction = SCR_FactionManager.SGetPlayerFaction(playerId);
		if (!playerFaction || squad.GetFaction() != playerFaction)
		{
			Retry(squad, playerId, attempt, "player faction not yet equal to squad faction", RETRY_MS, askJoin);
			return;
		}

		// 1. registered group with a real ID, max members, name (all idempotent; mirrors to the clients)
		CLW_SquadGroups.EnsureRegistered(squad);
		groups.RegisterGroup(squad);
		groups.AssignGroupID(squad);
		squad.SetDeleteWhenEmpty(false);
		squad.SetCanDeleteIfNoPlayer(false);
		int squadId = squad.GetGroupID();
		if (squadId < 0)
		{
			Retry(squad, playerId, attempt, "squad has no group ID yet", RETRY_MS, askJoin);
			return;
		}

		// 2. join through the controller so controller.m_iGroupID is set (and replicated) = what the commanding menu reads.
		// Establish: a menu did the join already, only wait until the vanilla join is complete.
		bool joined = controller.GetGroupID() == squadId && squad.IsPlayerInGroup(playerId);
		if (!joined && !askJoin)
		{
			Retry(squad, playerId, attempt, string.Format("vanilla join not complete yet (controllerGroupId=%1 squadId=%2)", controller.GetGroupID(), squadId), RETRY_MS, askJoin);
			return;
		}

		if (!joined)
		{
			controller.RPC_AskJoinGroup(squadId);
			Print(string.Format("CLW: PlayerSquad joined via controller: player=%1 controllerGroupId=%2 squadId=%3 inGroup=%4", playerId, controller.GetGroupID(), squadId, squad.IsPlayerInGroup(playerId)), LogLevel.NORMAL);
		}
		if (controller.GetGroupID() != squadId || !squad.IsPlayerInGroup(playerId))
		{
			Retry(squad, playerId, attempt, string.Format("join not applied (controllerGroupId=%1 squadId=%2)", controller.GetGroupID(), squadId), RETRY_MS, askJoin);
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
				Retry(squad, playerId, attempt, "slave group could not be created / linked", RETRY_MS, askJoin);
				return;
			}

			// vanilla re-sends the link to clients after 2 s; give the slave time to replicate before AI are added
			Retry(squad, playerId, attempt, "slave group created, waiting for replication", SLAVE_REPLICATION_MS, askJoin);
			return;
		}

		// 5. move the AI
		int moved;
		int skipped;
		MoveAI(squad, slave, controller, moved, skipped);
		Print(string.Format("CLW: PlayerSquad player=%1 controllerGroupId=%2 squadId=%3 slaveExists=true slaveAgents=%4 movedNow=%5 skipped=%6 squadAgentsLeft=%7 leaderId=%8", playerId, controller.GetGroupID(), squadId, slave.GetAgentsCount(), moved, skipped, squad.GetAgentsCount(), squad.GetLeaderID()), LogLevel.NORMAL);
		if (skipped > 0)
		{
			Retry(squad, playerId, attempt, string.Format("%1 AI could not be moved yet", skipped), RETRY_MS, askJoin);
			return;
		}

		Print(string.Format("CLW: PlayerSquad READY player=%1 squadId=%2 controllerGroupId=%3 slaveAgents=%4 leaderEntity=%5", playerId, squadId, controller.GetGroupID(), slave.GetAgentsCount(), squad.GetLeaderEntity()), LogLevel.NORMAL);
		Finish(squad, playerId);
		GetGame().GetCallqueue().CallLater(Verify, VERIFY_MS, false, squad, playerId);
		if (!s_aReconciling.Contains(squad))
		{
			s_aReconciling.Insert(squad);
			GetGame().GetCallqueue().CallLater(Reconcile, RECONCILE_MS, false, squad, playerId, 1);
		}
	}

	// ---- release ----

	// The last human left the squad (switch, disconnect, faction change): the AI go back from the slave group to the master group,
	// the slave stops, and HQ (rule commander / LLM) commands the squad again. Idempotent; a no-op while a human is in the squad.
	// [verify] mirrors the inverse of AddAIToSlaveGroup; check in play that the AI keep moving and get new orders.
	static void Release(SCR_AIGroup squad, string reason)
	{
		if (!Replication.IsServer() || !squad || squad.GetPlayerCount() > 0)
			return;

		SCR_AIGroup slave = squad.GetSlave();
		if (!slave)
			return;

		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		RplComponent slaveRpl = RplComponent.Cast(slave.FindComponent(RplComponent));
		if (!groups || !slaveRpl)
			return;

		array<AIAgent> agents = {};
		slave.GetAgents(agents);
		array<SCR_ChimeraCharacter> back = {};
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(agent.GetControlledEntity());
			if (character && GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(character) <= 0)
				back.Insert(character);
		}

		int moved;
		foreach (SCR_ChimeraCharacter ai : back)
		{
			RplComponent aiRpl = ai.GetRplComponent();
			slave.RemoveAgentFromControlledEntity(ai);
			if (aiRpl)
				groups.AskRemoveAiMemberFromGroup(slaveRpl.Id(), aiRpl.Id());	// clears the recruited flag on every machine
			squad.AddAgentFromControlledEntity(ai);
			moved++;
		}

		CLW_Waypoints.Clear(slave);
		slave.Deactivate();
		CLW_Waypoints.Clear(squad);	// leftover player orders; HQ issues fresh ones
		if (!squad.IsAIActivated())
			squad.ActivateAI();

		Print(string.Format("CLW: PlayerSquad RELEASE squad=%1 reason=%2 aiBack=%3 masterAgents=%4 slaveAgents=%5", squad, reason, moved, squad.GetAgentsCount(), slave.GetAgentsCount()), LogLevel.NORMAL);
		CLW_SquadJoin.HandBackToCommander(squad);
	}

	// Group members spawn in waves, so AI that appear in the scene group after the join are moved to the slave group too.
	protected static void Reconcile(SCR_AIGroup squad, int playerId, int round)
	{
		if (!squad || round > RECONCILE_ROUNDS || squad.GetPlayerCount() == 0)
		{
			s_aReconciling.RemoveItem(squad);
			return;
		}

		if (!squad.IsPlayerInGroup(playerId))
			playerId = squad.GetPlayerIDs()[0];	// the player left: any member's controller can move the AI

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

// CLW_SquadJoin: server hooks on the VANILLA group events. Every entry point (deploy menu, groups menu, command post, auto-join)
// changes membership through vanilla code; these hooks turn that into the commandable squad (CLW_PlayerSquad) or give it back.
// Only squads adopted by the registry are handled, other groups are none of our business.
//
//   SCR_AIGroup.GetOnPlayerAdded(group, pid)          -> (100 ms later) CLW_PlayerSquad.Establish + far-squad hint
//   SCR_AIGroup.GetOnPlayerRemoved(group, pid)        -> (100 ms later) CLW_PlayerSquad.Release when no human is left
//   SCR_AIGroup.GetOnPlayerLeaderChanged(groupId, pid)-> name re-applied, mission board re-evaluates
// The 100 ms: vanilla fires the events inside AddPlayer / RemovePlayer, before the controller's group ID is updated.
class CLW_SquadJoin
{
	protected static bool s_bInit;
	protected static const int LEADER_TICK_MS = 2000;
	protected static const int LEADER_SPAWN_DELAY_MS = 3000;

	// Server, once (game start).
	static void Init()
	{
		if (!Replication.IsServer() || s_bInit)
			return;

		s_bInit = true;
		SCR_AIGroup.GetOnPlayerAdded().Insert(OnPlayerAdded);	// [verify] static invoker (SCR_AIGroup group, int playerID)
		SCR_AIGroup.GetOnPlayerRemoved().Insert(OnPlayerRemoved);
		SCR_AIGroup.GetOnPlayerLeaderChanged().Insert(OnLeaderChanged);	// (int groupId, int playerId)
		GetGame().GetCallqueue().CallLater(LeaderTick, LEADER_TICK_MS, true);	// death / respawn / incapacitated: polled, the vanilla events are too indirect
		SCR_BaseGameMode mode = SCR_BaseGameMode.Cast(GetGame().GetGameMode());
		if (mode)
			mode.GetOnPlayerSpawned().Insert(OnPlayerSpawned);
		Print("CLW: squad join hooks installed", LogLevel.NORMAL);
	}

	// ---- human squad leader (M4.6, rules in CLW_PlayerSquad.EnsureHumanLeader) ----

	protected static void LeaderTick()
	{
		array<SCR_AIGroup> squads = {};
		CLW_SquadGroups.GetRegistered(squads);
		foreach (SCR_AIGroup squad : squads)
		{
			if (squad.GetPlayerCount() > 0)
				CLW_PlayerSquad.EnsureHumanLeader(squad);
		}
	}

	// A player (re)spawned: once his character is an agent of the squad (vanilla adds it after the gadgets are initialised) he leads again.
	protected static void OnPlayerSpawned(int playerId, IEntity entity)
	{
		if (!Replication.IsServer())
			return;

		GetGame().GetCallqueue().CallLater(DoSpawned, LEADER_SPAWN_DELAY_MS, false, playerId);
	}

	protected static void DoSpawned(int playerId)
	{
		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		SCR_AIGroup group;
		if (groups)
			group = groups.GetPlayerGroup(playerId);
		if (!IsOurs(group))
			return;

		CLW_PlayerSquad.EnsureHumanLeader(group);
		CLW_MissionBoard.RequestTick();
	}

	protected static bool IsOurs(SCR_AIGroup group)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		return registry && group && registry.IsAdopted(group);
	}

	protected static void OnPlayerAdded(SCR_AIGroup group, int playerId)
	{
		if (!Replication.IsServer() || !IsOurs(group))
			return;

		Print(string.Format("CLW: squad join: player %1 joined squad %2 (group id %3) through the vanilla group code", playerId, group, group.GetGroupID()), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(DoAdded, CLW_WarConfig.SQUAD_JOIN_DEFER_MS, false, group, playerId);
	}

	protected static void OnPlayerRemoved(SCR_AIGroup group, int playerId)
	{
		if (!Replication.IsServer() || !IsOurs(group))
			return;

		Print(string.Format("CLW: squad join: player %1 left squad %2 (players left %3)", playerId, group, group.GetPlayerCount()), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(DoRemoved, CLW_WarConfig.SQUAD_JOIN_DEFER_MS, false, group, playerId);
	}

	protected static void OnLeaderChanged(int groupId, int playerId)
	{
		if (!Replication.IsServer())
			return;

		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		if (!groups)
			return;

		SCR_AIGroup group = groups.FindGroup(groupId);
		if (!IsOurs(group))
			return;

		Print(string.Format("CLW: squad join: squad %1 leader is now player %2", group, playerId), LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(DoLeader, CLW_WarConfig.SQUAD_JOIN_DEFER_MS, false, group);
	}

	protected static void DoAdded(SCR_AIGroup group, int playerId)
	{
		if (!group || !group.IsPlayerInGroup(playerId))
			return;

		CLW_PlayerSquad.Establish(group, playerId);
		HintIfFar(group, playerId);
		CLW_MissionBoard.RequestTick();	// the new member gets the squad's mission tasks
	}

	protected static void DoRemoved(SCR_AIGroup group, int playerId)
	{
		if (!group)
			return;

		if (group.GetPlayerCount() > 0)
		{
			// another human stays: vanilla picked m_aPlayerIDs[0] as leader (maybe dead), our rule picks an able one
			CLW_PlayerSquad.EnsureHumanLeader(group);
			CLW_MissionBoard.RequestTick();	// the leaver loses the squad's mission tasks
			return;
		}

		CLW_PlayerSquad.Release(group, "last human left (player " + playerId + ")");
		CLW_MissionBoard.RequestTick();
	}

	protected static void DoLeader(SCR_AIGroup group)
	{
		if (!group)
			return;

		CLW_SquadGroups.ApplyName(group);	// SetGroupLeader wiped the name
		CLW_PlayerSquad.EnsureHumanLeader(group);	// vanilla may have handed the lead to a dead human or an AI
		CLW_MissionBoard.RequestTick();	// missions follow the human leader (the board moves offers / accepted mission to him)
	}

	// The squad is leaving the registry (wiped, faction left the war): unregister from the vanilla lists and free its humans.
	static void OnSquadReleased(SCR_AIGroup group)
	{
		if (!Replication.IsServer() || !group)
			return;

		array<int> players = {};
		foreach (int playerId : group.GetPlayerIDs())
		{
			players.Insert(playerId);
		}

		CLW_SquadGroups.Unregister(group);
		foreach (int memberId : players)
		{
			group.RemovePlayer(memberId);
			SCR_PlayerControllerGroupComponent controller = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(memberId);
			if (controller)
				controller.ResetGroupIDs_S();	// [verify] clears the controller group ID so the player can pick another squad
			Print(string.Format("CLW: squad join: player %1 removed from released squad %2", memberId, group), LogLevel.NORMAL);
		}
	}

	// HQ takes the squad back: forget the old order of the human-led time and let the commander plan again.
	static void HandBackToCommander(SCR_AIGroup squad)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!registry || !squad)
			return;

		string id = registry.GetId(squad);
		string key = CLW_SquadRegistry.GetFactionKey(squad);
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(key);
		if (commander && commander.GetRules() && commander.GetRules().GetOrders())
			commander.GetRules().GetOrders().ForgetOrder(id);

		Print(string.Format("CLW: squad join: squad %1 (%2) handed back to the %3 commander", id, squad, key), LogLevel.NORMAL);
	}

	// ---- mid-field switch policy ----
	// A player who picks a squad in the groups menu while alive is NOT teleported: when the squad is far away he gets a
	// popup with distance and direction, the squad follows his orders from where it is.

	protected static void HintIfFar(SCR_AIGroup group, int playerId)
	{
		IEntity body = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
		if (!body)
			return;	// deploying: he spawns at the base

		vector center;
		int count;
		array<IEntity> members = CLW_SquadInfo.Members(group);
		foreach (IEntity member : members)
		{
			if (!member || GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(member) > 0)
				continue;

			center += member.GetOrigin();
			count++;
		}

		if (count == 0)
			return;

		center = center / count;
		vector delta = center - body.GetOrigin();
		delta[1] = 0;
		float distance = delta.Length();
		if (distance <= CLW_WarConfig.SQUAD_FAR_HINT_M)
			return;

		string text = CLW_Loc.W("#CLW-Squad_Far", CLW_SquadGroups.DisplayName(group), DistanceText(distance), CompassText(delta));
		Print("CLW: squad join: player " + playerId + " joined a far squad: " + CLW_Loc.Resolve(text), LogLevel.NORMAL);
		CLW_MissionBoard.SendPopup(playerId, CLW_Loc.W("#CLW-Squad_Title"), text, "");
	}

	static string DistanceText(float meters)
	{
		if (meters < 1000)
			return Math.Round(meters).ToString() + " m";

		return (Math.Round(meters / 100) / 10).ToString() + " km";
	}

	// 8-point compass of a world delta (Enforce: +Z = north, +X = east).
	static string CompassText(vector delta)
	{
		float angle = Math.Atan2(delta[0], delta[2]) * Math.RAD2DEG;
		if (angle < 0)
			angle += 360;

		array<string> names = {"#CLW-Compass_N", "#CLW-Compass_NE", "#CLW-Compass_E", "#CLW-Compass_SE", "#CLW-Compass_S", "#CLW-Compass_SW", "#CLW-Compass_W", "#CLW-Compass_NW"};
		int index = Math.Round(angle / 45);	// Math.Round returns float: no % on floats in Enforce
		index = index % 8;
		return names[index];
	}
}

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

	// Server, once (game start).
	static void Init()
	{
		if (!Replication.IsServer() || s_bInit)
			return;

		s_bInit = true;
		SCR_AIGroup.GetOnPlayerAdded().Insert(OnPlayerAdded);	// [verify] static invoker (SCR_AIGroup group, int playerID)
		SCR_AIGroup.GetOnPlayerRemoved().Insert(OnPlayerRemoved);
		SCR_AIGroup.GetOnPlayerLeaderChanged().Insert(OnLeaderChanged);	// (int groupId, int playerId)
		Print("CLW: squad join hooks installed", LogLevel.NORMAL);
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
	}

	protected static void DoRemoved(SCR_AIGroup group, int playerId)
	{
		if (!group || group.GetPlayerCount() > 0)
			return;

		CLW_PlayerSquad.Release(group, "last human left (player " + playerId + ")");
		CLW_MissionBoard.RequestTick();
	}

	protected static void DoLeader(SCR_AIGroup group)
	{
		if (!group)
			return;

		CLW_SquadGroups.ApplyName(group);	// SetGroupLeader wiped the name
		CLW_MissionBoard.RequestTick();	// missions follow the human leader
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

		string text = string.Format("%1 is %2 %3 of you", CLW_SquadGroups.DisplayName(group), DistanceText(distance), CompassText(delta));
		Print("CLW: squad join: player " + playerId + " joined a far squad: " + text, LogLevel.NORMAL);
		CLW_MissionBoard.SendPopup(playerId, "Squad", text, "");
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

		array<string> names = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
		int index = Math.Round(angle / 45) % 8;
		return names[index];
	}
}

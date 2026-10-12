// CLW_SquadGroups: HQ squads in the VANILLA group lists (deploy menu, groups menu). Server registers, clients mirror.
//
// Why a mirror: SCR_AIGroup.m_iGroupID, the playable registration and the faction list entry travel only in RplSave/RplLoad
// (initial state), and pack squad prefabs have m_bPlayable 0. A group registered AFTER it was replicated is invisible to the
// client menus, so the server tells every client (RPC in CLW_GroupMirrorController.c) which groups to register locally.
// Idempotent on both sides; resent to a player on connect and on every spawn.
//
// Joinable = adopted by the registry, infantry kind, no vehicle link, not hidden, alive, on a side (decided by
// CLW_SquadRegistry.SyncJoinable). Vehicle crews / linked cargo are never registered.
class CLW_SquadGroups
{
	protected static const int MIRROR_RETRY_MS = 500;
	protected static const int MIRROR_RETRY_MAX = 20;

	// Server: groups registered by us (weak references, nulls are purged on use).
	protected static ref array<SCR_AIGroup> s_aRegistered = {};

	static bool IsRegistered(SCR_AIGroup group)
	{
		return group && s_aRegistered.Contains(group);
	}

	static int Count(string factionKey = "")
	{
		int count;
		for (int i = s_aRegistered.Count() - 1; i >= 0; i--)
		{
			SCR_AIGroup group = s_aRegistered[i];
			if (!group)
			{
				s_aRegistered.Remove(i);
				continue;
			}

			if (factionKey.IsEmpty() || CLW_SquadRegistry.GetFactionKey(group) == factionKey)
				count++;
		}

		return count;
	}

	// "<id> - <display type>", e.g. "CLW_Squad_US_1 - Rifle squad".
	static string DisplayName(notnull SCR_AIGroup group)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		string id;
		if (registry)
			id = registry.GetId(group);
		string type = CLW_SquadInfo.TypeName(group);
		if (id.IsEmpty())
			return type;

		return id + " - " + type;
	}

	// ---- server ----

	// Make the squad pickable in the vanilla menus. Idempotent. False when it could not be registered.
	static bool Register(SCR_AIGroup group)
	{
		if (!Replication.IsServer() || !group)
			return false;

		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		Faction faction = group.GetFaction();
		if (!groups || !faction)
			return false;

		bool first = !s_aRegistered.Contains(group);
		groups.RegisterGroup(group);
		groups.AssignGroupID(group);
		if (group.GetGroupID() < 0)
		{
			Print(string.Format("CLW: squad groups %1 got no group ID", group), LogLevel.WARNING);
			return false;
		}

		if (group.GetRadioFrequency() <= 0)
		{
			int frequency = groups.GetFreeFrequency(faction);
			if (frequency > 0)
			{
				groups.ClaimFrequency(frequency, faction);
				group.SetRadioFrequency(frequency);
			}
			else
			{
				Print("CLW: squad groups out of group radio frequencies", LogLevel.WARNING);
			}
		}

		if (group.GetGroupRole() != SCR_EGroupRole.NONE)
			group.SetGroupRole(SCR_EGroupRole.NONE);	// no role presets: no loadout / rank restriction in the menus
		group.SetMaxMembers(CLW_WarConfig.SQUAD_MAX_HUMANS);	// the prefab default is 1 human; IsFull() ignores AI
		group.SetCanDeleteIfNoPlayer(false);	// else the groups manager deletes it when the last human leaves
		group.SetDeleteWhenEmpty(false);
		ApplyName(group);

		if (first)
		{
			s_aRegistered.Insert(group);
			groups.OnGroupCreated(group);
			Print(string.Format("CLW: squad groups registered %1 id=%2 name='%3' freq=%4 maxHumans=%5", group, group.GetGroupID(), DisplayName(group), group.GetRadioFrequency(), group.GetMaxMembers()), LogLevel.NORMAL);
		}

		MirrorToAll(group, true);
		return true;
	}

	// Make sure a squad that is about to get a player (programmatic CLW_PlayerSquad.Join) is registered.
	static void EnsureRegistered(SCR_AIGroup group)
	{
		if (!IsRegistered(group))
			Register(group);
	}

	// (Re)apply the custom name. Needed after every leader change: SetGroupLeader wipes a name whose author is not the new leader.
	static void ApplyName(SCR_AIGroup group)
	{
		if (!Replication.IsServer() || !group)
			return;

		int author = group.GetLeaderID();
		if (author <= 0)
			author = CLW_WarConfig.SQUAD_NAME_AUTHOR;
		group.SetCustomName(DisplayName(group), author);	// [verify] author id accepted; text passes the async profanity filter
	}

	static void Unregister(SCR_AIGroup group)
	{
		if (!Replication.IsServer() || !group || !s_aRegistered.Contains(group))
			return;

		s_aRegistered.RemoveItem(group);
		MirrorToAll(group, false);
		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		if (groups)
			groups.UnregisterGroup(group);

		Print(string.Format("CLW: squad groups unregistered %1 id=%2", group, group.GetGroupID()), LogLevel.NORMAL);
	}

	// Players cannot create their own groups: HQ squads are the only ones (hides "new group" in the deploy and groups menus).
	static void LockNewGroups()
	{
		if (!Replication.IsServer())
			return;

		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		if (groups && groups.GetNewGroupsAllowed())
		{
			groups.SetNewGroupsAllowed(false);
			Print("CLW: squad groups new player groups disabled", LogLevel.NORMAL);
		}
	}

	// Delete vanilla playable groups nobody is in and that are not HQ squads: the predefined preset groups
	// (SCR_GroupsManagerComponent.CreatePredefinedGroups at game start) and empty auto-created ones would clutter the lists.
	static void PruneVanillaGroups(notnull CLW_SquadRegistry registry)
	{
		if (!Replication.IsServer())
			return;

		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		if (!groups)
			return;

		array<SCR_AIGroup> all = {};
		groups.GetAllPlayableGroups(all);
		foreach (SCR_AIGroup group : all)
		{
			if (!group || registry.IsAdopted(group) || s_aRegistered.Contains(group))
				continue;

			if (group.GetPlayerCount() > 0 || group.GetAgentsCount() > 0 || group.IsSlave())
				continue;

			Print(string.Format("CLW: squad groups removing vanilla empty group %1 id=%2 preset='%3'", group, group.GetGroupID(), group.GetPresetResource()), LogLevel.NORMAL);
			groups.DeleteAndUnregisterGroup(group);
		}
	}

	// ---- mirror, server side ----

	static void MirrorToAll(SCR_AIGroup group, bool registered)
	{
		array<int> ids = {};
		GetGame().GetPlayerManager().GetPlayers(ids);
		foreach (int playerId : ids)
		{
			MirrorTo(group, playerId, registered);
		}
	}

	// Send every registered squad to one player (connect, spawn). The listen-server host shares the server state: nothing to send.
	static void MirrorAll(int playerId)
	{
		if (!Replication.IsServer())
			return;

		int sent;
		for (int i = s_aRegistered.Count() - 1; i >= 0; i--)
		{
			SCR_AIGroup group = s_aRegistered[i];
			if (!group)
			{
				s_aRegistered.Remove(i);
				continue;
			}

			if (MirrorTo(group, playerId, true))
				sent++;
		}

		if (sent > 0)
			Print(string.Format("CLW: squad groups mirrored %1 squads to player %2", sent, playerId), LogLevel.NORMAL);
	}

	protected static bool MirrorTo(SCR_AIGroup group, int playerId, bool registered)
	{
		if (playerId <= 0 || playerId == SCR_PlayerController.GetLocalPlayerId())
			return false;

		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerManager().GetPlayerController(playerId));
		RplComponent rpl = RplComponent.Cast(group.FindComponent(RplComponent));
		if (!controller || !rpl)
			return false;

		controller.CLW_GrpMirror(rpl.Id(), group.GetGroupID(), registered);
		return true;
	}

	// ---- mirror, client side ----

	static void MirrorLocal(RplId groupRplId, int groupId, bool registered, int attempt)
	{
		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		RplComponent rpl = RplComponent.Cast(Replication.FindItem(groupRplId));
		SCR_AIGroup group;
		if (rpl)
			group = SCR_AIGroup.Cast(rpl.GetEntity());

		if (!groups || !group || (registered && !group.GetFaction()))
		{
			if (registered && attempt < MIRROR_RETRY_MAX)
				GetGame().GetCallqueue().CallLater(MirrorLocal, MIRROR_RETRY_MS, false, groupRplId, groupId, registered, attempt + 1);
			return;
		}

		if (!registered)
		{
			groups.UnregisterGroup(group);
			groups.GetOnPlayableGroupRemoved().Invoke(group);
			return;
		}

		bool known = groups.FindGroup(groupId) == group;
		if (group.GetGroupID() != groupId)
			group.SetGroupID(groupId);
		groups.RegisterGroup(group);
		if (!known)
		{
			groups.OnGroupCreated(group);	// refreshes an open deploy / groups menu
			Print(string.Format("CLW: squad groups client registered group id=%1 %2", groupId, group), LogLevel.NORMAL);
		}
	}
}

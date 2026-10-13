// Server-side builder of the friendly-units map picture. Every TICK_MS it builds, per faction that has connected recipients, the squad and
// vehicle lists once and, per recipient, the picture allowed by the war's map switches (CLW_FriendlyMapSettings). A picture is sent only
// when its wire text changed (positions are quantised to 5 m, heading to 10 degrees). Only the recipient's OWN faction data is ever sent.
// Design: dev/notes/map-friendly-design.md. Log lines are prefixed "CLW: friendly".
class CLW_FriendlyItem
{
	ref CLW_FriendlyEntry m_Entry;
	SCR_AIGroup m_Group;	// squads: the group (own-group dedupe)
	IEntity m_Vehicle;		// vehicles: the vehicle entity
	IEntity m_Leader;		// squads: position reference entity
}

class CLW_FriendlyPicture
{
	static const int TICK_MS = 4000;
	static const int MAX_ENTRIES = 120;
	static const int MAX_CHARS = 4000;
	static const int QUIET_LOG_EVERY = 10;

	protected static ref map<int, string> s_mSent;		// player id -> last wire sent
	protected static int s_iTicks;
	protected static bool s_bStarted;

	protected static void EnsureInit()
	{
		if (!s_mSent)
			s_mSent = new map<int, string>();
	}

	static void Start()
	{
		if (!Replication.IsServer())
			return;

		ScriptCallQueue queue = GetGame().GetCallqueue();
		if (!queue)
			return;

		EnsureInit();
		queue.Remove(Tick);	// a second Start (new war) must not double the timer
		queue.CallLater(Tick, TICK_MS, true);
		s_bStarted = true;
		Print("CLW: friendly picture started (tick " + TICK_MS.ToString() + " ms)", LogLevel.NORMAL);
	}

	// ---- tick ----

	static void Tick()
	{
		if (!Replication.IsServer())
			return;

		if (!CLW_GameMode.GetWarState())
			return;

		EnsureInit();
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		int startMs = System.GetTickCount();
		int mask = CLW_FriendlyMapSettings.Mask();
		array<int> ids = {};
		players.GetPlayers(ids);

		// recipients grouped by faction
		ref map<string, ref array<int>> byFaction = new map<string, ref array<int>>();
		foreach (int id : ids)
		{
			if (id <= 0)
				continue;

			Faction faction = SCR_FactionManager.SGetPlayerFaction(id);
			if (!faction)
			{
				s_mSent.Remove(id);
				continue;
			}

			string key = faction.GetFactionKey();
			if (!CLW_ScenarioConfig.IsClwFaction(key))
				continue;

			array<int> list;
			if (!byFaction.Find(key, list))
			{
				list = {};
				byFaction.Set(key, list);
			}

			list.Insert(id);
		}

		// forget players that left
		array<int> gone = {};
		foreach (int known, string text : s_mSent)
		{
			if (!ids.Contains(known))
				gone.Insert(known);
		}

		foreach (int goneId : gone)
		{
			s_mSent.Remove(goneId);
		}

		s_iTicks++;
		foreach (string factionKey, array<int> recipients : byFaction)
		{
			TickFaction(factionKey, recipients, mask, startMs);
		}
	}

	protected static void TickFaction(string factionKey, notnull array<int> recipients, int mask, int startMs)
	{
		array<ref CLW_FriendlyItem> vehicles = {};
		if ((mask & CLW_FriendlyMapSettings.BIT_VEHICLES) != 0)
			BuildVehicles(factionKey, recipients, vehicles);

		array<ref CLW_FriendlyItem> squads = {};
		if ((mask & CLW_FriendlyMapSettings.BIT_SQUADS) != 0)
			BuildSquads(factionKey, squads);

		int identity = CLW_ScenarioConfig.GetIdentity(factionKey);
		int sent = 0;
		int bytes = 0;
		foreach (int playerId : recipients)
		{
			CLW_FriendlyPic pic = BuildFor(playerId, factionKey, mask, identity, squads, vehicles);
			string wire = pic.ToWire();
			string last;
			if (s_mSent.Find(playerId, last) && last == wire)
				continue;

			if (Deliver(playerId, wire))
			{
				s_mSent.Set(playerId, wire);
				sent++;
				bytes += wire.Length();
			}
		}

		if (sent > 0 || s_iTicks % QUIET_LOG_EVERY == 0)
			Print(string.Format("CLW: friendly tick %1 squads=%2 vehicles=%3 recipients=%4 sent=%5 bytes=%6 ms=%7", factionKey, squads.Count(), vehicles.Count(), recipients.Count(), sent, bytes, System.GetTickCount() - startMs), LogLevel.NORMAL);
	}

	// Picture of one recipient: squads, then vehicles, then own-group members (cap order), all of the recipient's own faction.
	protected static CLW_FriendlyPic BuildFor(int playerId, string factionKey, int mask, int identity, notnull array<ref CLW_FriendlyItem> squads, notnull array<ref CLW_FriendlyItem> vehicles)
	{
		CLW_FriendlyPic pic = new CLW_FriendlyPic();
		pic.m_sFaction = factionKey;
		pic.m_bActive = true;
		pic.m_iMask = mask;
		pic.m_iIdentity = identity;

		SCR_AIGroup own = CLW_RequestService.GroupOf(playerId);
		bool wantMembers = (mask & CLW_FriendlyMapSettings.BIT_SQUADMATES) != 0;
		bool wantVehicles = (mask & CLW_FriendlyMapSettings.BIT_VEHICLES) != 0;

		array<IEntity> drawnVehicles = {};
		if (wantVehicles)
		{
			foreach (CLW_FriendlyItem v : vehicles)
			{
				if (v && v.m_Vehicle)
					drawnVehicles.Insert(v.m_Vehicle);
			}
		}

		foreach (CLW_FriendlyItem s : squads)
		{
			if (!s || !s.m_Entry)
				continue;

			if (wantMembers && own && s.m_Group == own)
				continue;	// drawn as members

			if (wantVehicles && s.m_Leader)
			{
				IEntity riding = VehicleOf(s.m_Leader);
				if (riding && drawnVehicles.Contains(riding))
					continue;	// rides a drawn vehicle
			}

			pic.m_aEntries.Insert(s.m_Entry);
		}

		if (wantVehicles)
		{
			foreach (CLW_FriendlyItem v2 : vehicles)
			{
				if (v2 && v2.m_Entry)
					pic.m_aEntries.Insert(v2.m_Entry);
			}
		}

		if (wantMembers && own)
			AddMembers(playerId, own, pic.m_aEntries);

		if (pic.m_aEntries.Count() > MAX_ENTRIES)
		{
			Print(string.Format("CLW: friendly picture capped %1 -> %2 entries for player %3", pic.m_aEntries.Count(), MAX_ENTRIES, playerId), LogLevel.WARNING);
			pic.m_aEntries.Resize(MAX_ENTRIES);
		}

		while (pic.m_aEntries.Count() > 0 && pic.ToWire().Length() > MAX_CHARS)
		{
			pic.m_aEntries.Remove(pic.m_aEntries.Count() - 1);
		}

		return pic;
	}

	// ---- shared lists ----

	protected static void BuildSquads(string factionKey, notnull array<ref CLW_FriendlyItem> outItems)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!players)
			return;

		if (registry)
		{
			array<string> ids = {};
			registry.GetIds(factionKey, ids);
			foreach (string id : ids)
			{
				if (registry.IsHidden(id))
					continue;

				SCR_AIGroup group = registry.Find(id);
				if (!group)
					continue;

				int alive = CLW_SquadInfo.AliveCount(group);
				int humans = group.GetPlayerCount();
				if (alive <= 0 && humans <= 0)
					continue;

				string kind = registry.GetKind(id);
				if (kind.IsEmpty())
					kind = CLW_ForceKind.INFANTRY;

				CLW_FriendlyItem item = MakeSquad(group, id, registry.GetName(id), kind, alive, players);
				if (item)
					outItems.Insert(item);
			}
		}

		// human-only groups (vanilla groups without AI, not adopted by the registry)
		FactionManager factions = GetGame().GetFactionManager();
		SCR_GroupsManagerComponent groups = SCR_GroupsManagerComponent.GetInstance();
		if (!factions || !groups)
			return;

		Faction faction = factions.GetFactionByKey(factionKey);
		if (!faction)
			return;

		array<SCR_AIGroup> playable = groups.GetPlayableGroupsByFaction(faction);
		if (!playable)
			return;

		foreach (SCR_AIGroup g : playable)
		{
			if (!g || g.GetPlayerCount() <= 0)
				continue;

			if (registry && registry.IsAdopted(g))
				continue;

			CLW_FriendlyItem humanItem = MakeSquad(g, "g" + g.GetGroupID().ToString(), "", CLW_ForceKind.INFANTRY, g.GetPlayerCount(), players);
			if (humanItem)
				outItems.Insert(humanItem);
		}
	}

	protected static CLW_FriendlyItem MakeSquad(notnull SCR_AIGroup group, string id, string name, string kind, int alive, notnull PlayerManager players)
	{
		IEntity leader = CLW_SquadInfo.LeaderEntity(group);
		if (!leader)
			return null;

		int flags = 0;
		string label = CLW_FriendlyPic.Clean(name, 24);
		if (group.GetPlayerCount() > 0)
		{
			flags |= CLW_FriendlyPic.FLAG_HUMAN;
			IEntity human = CLW_SquadInfo.HumanLeaderEntity(group);
			string playerName;
			if (human)
				playerName = players.GetPlayerName(players.GetPlayerIdFromControlledEntity(human));

			playerName = CLW_FriendlyPic.Clean(playerName, 11);
			if (!playerName.IsEmpty())
			{
				if (label.IsEmpty())
					label = playerName;
				else
					label = CLW_FriendlyPic.Clean(name, 10) + " (" + playerName + ")";
			}
		}

		CLW_FriendlyItem item = new CLW_FriendlyItem();
		item.m_Group = group;
		item.m_Leader = leader;
		item.m_Entry = MakeEntry("s", id, leader, kind, Math.Max(alive, 1), flags, label);
		return item;
	}

	protected static void BuildVehicles(string factionKey, notnull array<int> recipients, notnull array<ref CLW_FriendlyItem> outItems)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		// vehicles with a human of this faction inside: root vehicle -> player id
		array<IEntity> playerVehicles = {};
		array<int> playerOf = {};
		foreach (int pid : recipients)
		{
			IEntity vehicle = VehicleOf(players.GetPlayerControlledEntity(pid));
			if (!vehicle || playerVehicles.Contains(vehicle))
				continue;

			playerVehicles.Insert(vehicle);
			playerOf.Insert(pid);
		}

		array<IEntity> done = {};
		CLW_VehicleManager manager = CLW_GameMode.GetVehicleManager();
		if (manager)
		{
			array<CLW_VehicleUnit> units = {};
			manager.GetUnits(factionKey, units);
			foreach (CLW_VehicleUnit unit : units)
			{
				if (!unit)
					continue;

				IEntity ent = unit.GetVehicle();
				if (!ent || done.Contains(ent))
					continue;

				done.Insert(ent);
				int flags = 0;
				string label = CLW_FriendlyPic.Clean(unit.GetVehicleTypeName(), 24);
				int index = playerVehicles.Find(ent);
				if (index >= 0)
				{
					flags |= CLW_FriendlyPic.FLAG_HUMAN;
					string playerName = CLW_FriendlyPic.Clean(players.GetPlayerName(playerOf[index]), 11);
					if (!playerName.IsEmpty())
						label = CLW_FriendlyPic.Clean(unit.GetVehicleTypeName(), 11) + " (" + playerName + ")";
				}

				string kind = unit.GetKind();
				if (kind.IsEmpty())
					kind = CLW_FriendlyKinds.OfVehicle(ent);

				CLW_FriendlyItem item = new CLW_FriendlyItem();
				item.m_Vehicle = ent;
				item.m_Entry = MakeEntry("v", unit.GetId(), ent, kind, Math.Max(unit.GetCrewAlive(), 1), flags, label);
				outItems.Insert(item);
			}
		}

		// vehicles of players that no unit owns
		foreach (int i, IEntity pv : playerVehicles)
		{
			if (done.Contains(pv))
				continue;

			string playerLabel = CLW_FriendlyPic.Clean(players.GetPlayerName(playerOf[i]), 24);
			CLW_FriendlyItem pItem = new CLW_FriendlyItem();
			pItem.m_Vehicle = pv;
			pItem.m_Entry = MakeEntry("v", "p" + playerOf[i].ToString(), pv, CLW_FriendlyKinds.OfVehicle(pv), 1, CLW_FriendlyPic.FLAG_HUMAN, playerLabel);
			outItems.Insert(pItem);
		}
	}

	// Members of the recipient's own group (AI incl. slave group, plus other human members); alive only, never the recipient.
	protected static void AddMembers(int playerId, notnull SCR_AIGroup group, notnull array<ref CLW_FriendlyEntry> outEntries)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		string groupId;
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (registry)
			groupId = registry.GetId(group);

		if (groupId.IsEmpty())
			groupId = "g" + group.GetGroupID().ToString();

		IEntity self = players.GetPlayerControlledEntity(playerId);
		array<IEntity> members = CLW_SquadInfo.Members(group);
		if (!members)
			members = {};

		array<int> memberIds = {};
		memberIds.Copy(group.GetPlayerIDs());
		foreach (int mid : memberIds)
		{
			IEntity humanEnt = players.GetPlayerControlledEntity(mid);
			if (humanEnt && !members.Contains(humanEnt))
				members.Insert(humanEnt);
		}

		int index = 0;
		foreach (IEntity member : members)
		{
			if (!member || member == self)
				continue;

			CharacterControllerComponent controller = CharacterControllerComponent.Cast(member.FindComponent(CharacterControllerComponent));
			if (!controller || controller.GetLifeState() != ECharacterLifeState.ALIVE)
				continue;

			int flags = CLW_FriendlyPic.FLAG_OWN_GROUP;
			string label = "";
			int memberPlayer = players.GetPlayerIdFromControlledEntity(member);
			if (memberPlayer > 0)
			{
				flags |= CLW_FriendlyPic.FLAG_HUMAN;
				label = players.GetPlayerName(memberPlayer);
			}

			index++;
			outEntries.Insert(MakeEntry("m", groupId + "." + index.ToString(), member, CLW_ForceKind.INFANTRY, 1, flags, label));
		}
	}

	// ---- helpers ----

	protected static CLW_FriendlyEntry MakeEntry(string type, string key, notnull IEntity ent, string kind, int count, int flags, string label)
	{
		vector pos = ent.GetOrigin();
		CLW_FriendlyEntry e = new CLW_FriendlyEntry();
		e.m_sType = type;
		e.m_sKey = CLW_FriendlyPic.Clean(key, CLW_FriendlyPic.MAX_KEY);
		e.m_iX5 = Math.Floor(pos[0] / 5);
		e.m_iZ5 = Math.Floor(pos[2] / 5);
		int h = Math.Round(ent.GetYawPitchRoll()[0] / 10);
		while (h < 0)
		{
			h += 36;
		}

		while (h >= 36)
		{
			h -= 36;
		}

		e.m_iH10 = h;
		e.m_sKind = kind;
		e.m_iCount = count;
		e.m_iFlags = flags;
		e.m_sLabel = CLW_FriendlyPic.Clean(label, CLW_FriendlyPic.MAX_LABEL);
		return e;
	}

	// Root vehicle a character sits in, else null.
	protected static IEntity VehicleOf(IEntity person)
	{
		if (!person)
			return null;

		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(person.FindComponent(SCR_CompartmentAccessComponent));
		if (!access || !access.IsInCompartment())
			return null;

		IEntity inside = access.GetVehicle();
		if (!inside)
			return null;

		return inside.GetRootParent();
	}

	// ---- delivery ----

	// Returns false when the controller was not ready (the next tick retries).
	protected static bool Deliver(int playerId, string wire)
	{
		if (playerId == SCR_PlayerController.GetLocalPlayerId())
		{
			CLW_FriendlyMapClient.Receive(wire);	// listen-server host: no RPC
			return true;
		}

		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return false;

		SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
		if (!controller)
			return false;

		controller.CLW_FriendlySend(wire);
		return true;
	}

	// A client asked for its picture again: forget what was sent, deliver now.
	static void Resend(int playerId)
	{
		EnsureInit();
		s_mSent.Remove(playerId);
		if (s_bStarted)
			Tick();
	}

	// Rematch / war end: inactive header to everyone with a faction, caches cleared (the client drops its markers, vanilla returns).
	static void RematchReset()
	{
		if (!Replication.IsServer())
			return;

		PlayerManager players = GetGame().GetPlayerManager();
		if (players)
		{
			array<int> ids = {};
			players.GetPlayers(ids);
			foreach (int id : ids)
			{
				if (id <= 0)
					continue;

				Faction faction = SCR_FactionManager.SGetPlayerFaction(id);
				if (!faction)
					continue;

				CLW_FriendlyPic pic = new CLW_FriendlyPic();
				pic.m_sFaction = faction.GetFactionKey();
				pic.m_bActive = false;
				Deliver(id, pic.ToWire());
			}
		}

		s_mSent = null;
		s_iTicks = 0;
		Print("CLW: friendly picture reset (inactive header sent)", LogLevel.NORMAL);
	}
}

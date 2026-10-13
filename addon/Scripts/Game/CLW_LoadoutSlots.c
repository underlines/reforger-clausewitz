// Spawn-screen slot state (loadouts plan R3 / R1 entry, "client determinism"): per war side 1 custom entry (slot 0) and 7 server loadout slots
// (1..7). The deploy menu lists the loadouts of CLW_LoadoutManager.et, which exists on every machine, so the entries are FIXED objects
// (CLW_ServerLoadout, CLW_SideCustomLoadout) that read their faction, name and base prefab from this class:
//   server  CLW_LoadoutSync fills it at war start (Rebuild: library entry -> R2 role prefab -> hidden) and Publish sends it to every client;
//   client  OnWire stores it. Kit strings NEVER replicate: the wire holds only side faction keys, slot name, base prefab and source.
// An entry is listed for its side's faction by writing SCR_FactionPlayerLoadout.m_sAffiliatedFaction (what SCR_LoadoutManager filters by);
// a hidden slot (and every slot before the war) has an empty faction and is listed for nobody. Joiners get the wire from the server when
// their controller registers (CLW_LoadoutPlayerHooks). Log: "CLW: loadouts slot <side> <idx> faction=<key> name=<..> source=<library|role|hidden>".

// Wire shapes (JSON keys = field names).
class CLW_LoadoutSlotDto
{
	int s;			// side 0 / 1
	int i;			// slot 0 = custom, 1..7
	string n;		// name
	string p;		// base prefab
	string src;		// library | role
}

class CLW_LoadoutSlotsDto
{
	int v;
	ref array<string> sides = {};
	ref array<ref CLW_LoadoutSlotDto> slots = {};
}

class CLW_LoadoutSlots
{
	static const int SLOT_COUNT = 7;
	static const int SIDE_COUNT = 2;
	static const int STRIDE = 8;						// array index = side * STRIDE + slot (slot 0 = custom)
	static const string SRC_LIBRARY = "library";
	static const string SRC_ROLE = "role";
	static const string SRC_HIDDEN = "hidden";
	static const string CUSTOM_BASE_ROLE = "ROLE_RIFLEMAN";	// the custom entry spawns this role's prefab, the saved kit replaces its gear

	protected static ref array<string> s_aSideKeys = {};
	protected static ref array<string> s_aNames = {};
	protected static ref array<string> s_aPrefabs = {};
	protected static ref array<string> s_aSources = {};
	protected static ref array<string> s_aEntryIds = {};	// server only: library entry behind a slot
	protected static string s_sLastWire;

	// ---- state ----

	protected static void EnsureInit()
	{
		while (s_aSideKeys.Count() < SIDE_COUNT)
		{
			s_aSideKeys.Insert("");
		}

		while (s_aNames.Count() < SIDE_COUNT * STRIDE)
		{
			s_aNames.Insert("");
			s_aPrefabs.Insert("");
			s_aSources.Insert("");
			s_aEntryIds.Insert("");
		}
	}

	protected static int Index(int side, int slot)
	{
		return side * STRIDE + slot;
	}

	protected static bool InRange(int side, int slot)
	{
		return side >= 0 && side < SIDE_COUNT && slot >= 0 && slot <= SLOT_COUNT;
	}

	// Everything hidden, no sides.
	static void Clear()
	{
		EnsureInit();
		for (int side = 0; side < SIDE_COUNT; side++)
		{
			s_aSideKeys[side] = "";
		}

		for (int i = 0; i < s_aNames.Count(); i++)
		{
			s_aNames[i] = "";
			s_aPrefabs[i] = "";
			s_aSources[i] = "";
			s_aEntryIds[i] = "";
		}
	}

	static string SideKey(int side)
	{
		EnsureInit();
		if (side < 0 || side >= SIDE_COUNT)
			return "";

		return s_aSideKeys[side];
	}

	static string Name(int side, int slot)
	{
		EnsureInit();
		if (!InRange(side, slot))
			return "";

		return s_aNames[Index(side, slot)];
	}

	static ResourceName Prefab(int side, int slot)
	{
		EnsureInit();
		if (!InRange(side, slot))
			return ResourceName.Empty;

		return s_aPrefabs[Index(side, slot)];
	}

	// library | role | hidden
	static string Source(int side, int slot)
	{
		EnsureInit();
		if (!InRange(side, slot))
			return SRC_HIDDEN;

		string source = s_aSources[Index(side, slot)];
		if (source.IsEmpty())
			return SRC_HIDDEN;

		return source;
	}

	// Server: library entry id behind a library slot.
	static string EntryId(int side, int slot)
	{
		EnsureInit();
		if (!InRange(side, slot))
			return "";

		return s_aEntryIds[Index(side, slot)];
	}

	// The slot is listed for its side's faction (side known, content not hidden).
	static bool IsVisible(int side, int slot)
	{
		return !SideKey(side).IsEmpty() && Source(side, slot) != SRC_HIDDEN && !Prefab(side, slot).IsEmpty();
	}

	protected static void Set(int side, int slot, string name, ResourceName prefab, string source, string entryId)
	{
		int index = Index(side, slot);
		s_aNames[index] = name;
		s_aPrefabs[index] = prefab;
		s_aSources[index] = source;
		s_aEntryIds[index] = entryId;
	}

	protected static string SideLetter(int side)
	{
		if (side == 0)
			return "A";

		return "B";
	}

	// ---- server: content of the slots ----

	protected static bool CanLoad(ResourceName prefab)
	{
		if (prefab.IsEmpty())
			return false;

		Resource resource = Resource.Load(prefab);
		return resource && resource.IsValid();
	}

	// Slots from the library (active entry with order == slot, status ok / degraded), else the R2 role prefab for the default role order, else
	// hidden. keys = the war's faction keys (side A, side B).
	static void Rebuild(notnull array<string> keys)
	{
		Clear();
		for (int side = 0; side < SIDE_COUNT; side++)
		{
			if (side >= keys.Count() || keys[side].IsEmpty())
				continue;

			string key = keys[side];
			s_aSideKeys[side] = key;
			array<string> roles = {};
			CLW_RolePicker.Roles(key, roles);
			for (int slot = 1; slot <= SLOT_COUNT; slot++)
			{
				BuildSlot(side, slot, key, roles);
			}

			BuildCustom(side, key, roles);
		}
	}

	protected static void BuildSlot(int side, int slot, string key, notnull array<string> roles)
	{
		string defaultRole;
		if (slot - 1 < roles.Count())
			defaultRole = roles[slot - 1];

		CLW_LibraryEntry entry = CLW_LoadoutLibrary.SlotEntry(key, slot);
		if (entry)
		{
			ResourceName basePrefab = entry.basePrefab;
			if (!CanLoad(basePrefab))
			{
				basePrefab = CLW_RolePicker.RolePrefab(key, entry.role);
				if (!CanLoad(basePrefab) && !defaultRole.IsEmpty())
					basePrefab = CLW_RolePicker.RolePrefab(key, defaultRole);

				Print("CLW: loadouts slot " + SideLetter(side) + " " + slot.ToString() + " library entry " + entry.id + ": base prefab '" + entry.basePrefab + "' does not load, using " + basePrefab, LogLevel.WARNING);
			}

			if (CanLoad(basePrefab))
			{
				Set(side, slot, entry.name, basePrefab, SRC_LIBRARY, entry.id);
				return;
			}
		}

		if (defaultRole.IsEmpty())
			return;

		ResourceName rolePrefab = CLW_RolePicker.RolePrefab(key, defaultRole);
		if (!CanLoad(rolePrefab))
			return;

		string roleName = CLW_RolePicker.RoleName(key, defaultRole);
		if (roleName.IsEmpty())
			roleName = defaultRole;

		Set(side, slot, roleName, rolePrefab, SRC_ROLE, "");
	}

	// Slot 0: the player's own saved kit. Base prefab = the rifleman role prefab (any role of the faction when it has no rifleman).
	protected static void BuildCustom(int side, string key, notnull array<string> roles)
	{
		ResourceName basePrefab = CLW_RolePicker.RolePrefab(key, CUSTOM_BASE_ROLE);
		if (!CanLoad(basePrefab))
		{
			foreach (string role : roles)
			{
				basePrefab = CLW_RolePicker.RolePrefab(key, role);
				if (CanLoad(basePrefab))
					break;
			}
		}

		if (!CanLoad(basePrefab))
		{
			Print("CLW: loadouts slot " + SideLetter(side) + " 0 faction=" + key + ": no base prefab for the custom loadout, entry stays hidden", LogLevel.WARNING);
			return;
		}

		Set(side, 0, "", basePrefab, SRC_ROLE, "");
	}

	// ---- both machines: entries ----

	// Writes the faction of every listed entry (hidden = empty faction). Both machines, whenever the state changes.
	static void BindEntries()
	{
		SCR_LoadoutManager manager = GetGame().GetLoadoutManager();
		if (!manager)
			return;

		array<SCR_BasePlayerLoadout> all = {};
		manager.GetPlayerLoadouts(all);
		foreach (SCR_BasePlayerLoadout loadout : all)
		{
			CLW_ServerLoadout server = CLW_ServerLoadout.Cast(loadout);
			if (server)
			{
				server.Bind();
				continue;
			}

			CLW_SideCustomLoadout custom = CLW_SideCustomLoadout.Cast(loadout);
			if (custom)
				custom.Bind();
		}
	}

	protected static void LogSlots(string where)
	{
		for (int side = 0; side < SIDE_COUNT; side++)
		{
			for (int slot = 0; slot <= SLOT_COUNT; slot++)
			{
				string slotName = Name(side, slot);
				if (slot == 0)
					slotName = "custom";

				Print("CLW: loadouts slot " + SideLetter(side) + " " + slot.ToString() + " faction=" + SideKey(side) + " name=" + slotName + " source=" + Source(side, slot) + " [" + where + "]", LogLevel.NORMAL);
			}
		}
	}

	// ---- server: wire ----

	protected static string BuildWire()
	{
		EnsureInit();
		string wire = "{\"v\":1,\"sides\":[\"" + CLW_Situation.Escape(s_aSideKeys[0]) + "\",\"" + CLW_Situation.Escape(s_aSideKeys[1]) + "\"],\"slots\":[";
		bool first = true;
		for (int side = 0; side < SIDE_COUNT; side++)
		{
			for (int slot = 0; slot <= SLOT_COUNT; slot++)
			{
				if (!IsVisible(side, slot))
					continue;

				if (!first)
					wire += ",";

				first = false;
				wire += "{\"s\":" + side.ToString() + ",\"i\":" + slot.ToString();
				wire += ",\"n\":\"" + CLW_Situation.Escape(Name(side, slot)) + "\"";
				wire += ",\"p\":\"" + CLW_Situation.Escape(Prefab(side, slot)) + "\"";
				wire += ",\"src\":\"" + Source(side, slot) + "\"}";
			}
		}

		wire += "]}";
		return wire;
	}

	// Server: bind the entries here, log, and (changed or force) send the state to every client.
	static void Publish(string reason, bool force)
	{
		if (!Replication.IsServer())
			return;

		string wire = BuildWire();
		bool changed = wire != s_sLastWire;
		s_sLastWire = wire;
		BindEntries();
		if (!changed && !force)
			return;

		Print("CLW: loadouts slots publish (" + reason + "): " + wire.Length().ToString() + " chars", LogLevel.NORMAL);
		LogSlots("server");
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			SendWire(playerId, wire);
		}
	}

	// Server: a joiner (or a client that missed it) gets the current state.
	static void SendTo(int playerId)
	{
		if (!Replication.IsServer() || s_sLastWire.IsEmpty())
			return;

		SendWire(playerId, s_sLastWire);
	}

	protected static void SendWire(int playerId, string wire)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
		if (controller)
			controller.CLW_SendLoadoutSlots_S(wire);
	}

	// ---- client ----

	static void OnWire(string wire)
	{
		if (Replication.IsServer())
			return;	// a listen-server host already holds the state

		CLW_LoadoutSlotsDto dto = new CLW_LoadoutSlotsDto();
		SCR_JsonLoadContext ctx = new SCR_JsonLoadContext();
		if (!ctx.ImportFromString(wire) || !ctx.ReadValue("", dto))
		{
			Print("CLW: loadouts slots: wire is not valid JSON (" + wire.Length().ToString() + " chars), ignored", LogLevel.WARNING);
			return;
		}

		Clear();
		for (int side = 0; side < SIDE_COUNT; side++)
		{
			if (side < dto.sides.Count())
				s_aSideKeys[side] = dto.sides[side];
		}

		foreach (CLW_LoadoutSlotDto slot : dto.slots)
		{
			if (slot && InRange(slot.s, slot.i))
				Set(slot.s, slot.i, slot.n, slot.p, slot.src, "");
		}

		s_sLastWire = wire;
		BindEntries();
		LogSlots("client");
	}
}

// Server -> owning client delivery of the slot state (same owner-RPC pattern as CLW_SyncSides_S).
modded class SCR_PlayerController
{
	void CLW_SendLoadoutSlots_S(string wire)
	{
		RplComponent rpl = RplComponent.Cast(FindComponent(RplComponent));
		if (!rpl || rpl.IsOwner())
			return;	// listen server / Workbench: the server already holds the state

		Rpc(CLW_RpcLoadoutSlots_O, wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcLoadoutSlots_O(string wire)
	{
		CLW_LoadoutSlots.OnWire(wire);
	}
}

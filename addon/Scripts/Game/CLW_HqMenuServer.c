// HQ menu, server side (M4 WP4, VISION 6a light). One dialog (CLW_HqMenuDialog) with the sections Requests, Squad, Base, Contact report.
// Opened from the "Open HQ menu" action of the request terminal (CLW_HqMenuAction) or the chat command /hqmenu. Server authoritative:
// every button runs the SAME code as the chat commands (CLW_RequestService.Handle = /request, CLW_Reports.Chat = /report,
// CLW_PlayerSquad.Join = joining AI squads, CLW_SitrepClient = /sitrep), so rules, cooldowns and logs stay in one place.
// Data to the client is a flat TAB / LF text (same idea as CLW_SetupServer.BuildWire), one line per item, first field = line type:
//   H <faction> <role> <supplies> <base label> <running 0|1>            header (who you are, where the stock comes from)
//   S <text>                                                            status line (answer to the last action)
//   R <id> <kind> <title> <blocked reason or empty>                     one request row, followed by its options
//   O <arg> <label>                                                     an option of the previous R (type to request)
//   Q <squadId> <name> <humans> <members> <mine 0|1> <full 0|1>         a joinable HQ squad of the player's side
//   L <text>                                                            a base line (stock, FOB, convoy)
//   K <allowed 0|1> <reason>                                            contact report available
// Server only. Log prefix: "CLW: hqmenu".

class CLW_HqMenuConfig
{
	static const int SQUADS_MAX = 10;				// squads listed
	static const int OPTIONS_MAX = 10;				// vehicle / squad types listed per request row
	static const int FIELD_MAX = 200;				// characters per text field on the wire
	static const int DELIVERY_MARKER_SEC = 300;		// map marker of a delivered vehicle is removed after this
	static const int DELIVERY_POLL_MS = 2000;		// check "did the requester get in"
}

class CLW_HqMenuServer
{
	// ---- entry points (from the player controller RPCs) ----

	// The player opened the menu (terminal action: terminalPos = terminal; chat: Zero). Answers with the first payload.
	static void Open(int playerId, vector terminalPos)
	{
		if (!Replication.IsServer() || playerId <= 0)
			return;

		Print(string.Format("CLW: hqmenu open player=%1(%2) terminal=%3", playerId, CLW_RequestService.PlayerName(playerId), terminalPos), LogLevel.NORMAL);
		Push(playerId, terminalPos, "");
	}

	// A button of the menu. action: req | join | leave | report | refresh. Runs the shared code, then pushes fresh data.
	static void Action(int playerId, string action, string arg, vector terminalPos)
	{
		if (!Replication.IsServer() || playerId <= 0)
			return;

		Print(string.Format("CLW: hqmenu action player=%1(%2) action=%3 arg='%4' terminal=%5", playerId, CLW_RequestService.PlayerName(playerId), action, arg, terminalPos), LogLevel.NORMAL);
		string status;
		if (action == "req")
		{
			// arg = "<kind>|<type or where>"
			array<string> parts = {};
			arg.Split("|", parts, false);
			string kind;
			string typeArg;
			if (parts.Count() > 0)
				kind = parts[0];
			if (parts.Count() > 1)
				typeArg = parts[1];
			kind.ToLower();
			status = CLW_RequestService.Handle(playerId, kind, typeArg, terminalPos);	// answers chat + hint itself
		}
		else if (action == "report")
		{
			status = "HQ: " + CLW_Reports.Chat(playerId, arg);
		}
		else if (action == "join")
		{
			status = JoinSquad(playerId, arg);
			CLW_RequestService.Answer(playerId, status, true);
		}
		else if (action == "leave")
		{
			status = LeaveSquad(playerId);
			CLW_RequestService.Answer(playerId, status, true);
		}
		else if (action != "refresh")
		{
			Print("CLW: hqmenu unknown action '" + action + "'", LogLevel.WARNING);
			status = "unknown menu action";
		}

		Push(playerId, terminalPos, status);
	}

	protected static void Push(int playerId, vector terminalPos, string status)
	{
		string wire = BuildWire(playerId, terminalPos, status);
		if (playerId == SCR_PlayerController.GetLocalPlayerId())
		{
			CLW_HqMenuClient.Receive(wire);	// listen-server host: no RPC
			return;
		}

		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerManager().GetPlayerController(playerId));
		if (controller)
			controller.CLW_HqMenuSend(wire);
		else
			Print("CLW: hqmenu WARNING no controller for player " + playerId, LogLevel.WARNING);
	}

	// ---- squads (CLW_PlayerSquad is the only sanctioned way in) ----

	protected static string JoinSquad(int playerId, string squadId)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		string faction = CLW_RequestService.FactionKeyOf(playerId);
		vector pos;
		if (!war || !war.IsRunning() || !registry)
			return "HQ: squads are available once the war is running.";
		if (faction.IsEmpty() || !CLW_RequestService.PlayerPos(playerId, pos))
			return "HQ: you must be deployed on a side to join a squad.";

		SCR_AIGroup group = registry.Find(squadId);
		if (!group || !registry.IsJoinable(group) || CLW_SquadRegistry.GetFactionKey(group) != faction)
		{
			Print(string.Format("CLW: hqmenu join player=%1 squad=%2 refused: not a joinable squad of %3", playerId, squadId, faction), LogLevel.NORMAL);
			return "HQ: that squad is not available (it may be gone or belong to the other side).";
		}

		if (group.IsPlayerInGroup(playerId))
			return "HQ: you are already in " + CLW_SquadGroups.DisplayName(group) + ".";

		if (group.IsFull())
			return "HQ: " + CLW_SquadGroups.DisplayName(group) + " has no free slot for a player.";

		Print(string.Format("CLW: hqmenu join player=%1 -> squad %2 (%3) via CLW_PlayerSquad.Join", playerId, squadId, group), LogLevel.NORMAL);
		CLW_PlayerSquad.Join(group, playerId);
		return "HQ: joining " + CLW_SquadGroups.DisplayName(group) + ".";
	}

	// Same calls as CLW_SquadJoin.OnSquadReleased; the vanilla hooks (CLW_SquadJoin) release the squad when no human is left.
	protected static string LeaveSquad(int playerId)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		SCR_AIGroup group = CLW_RequestService.GroupOf(playerId);
		if (!registry || !group || !registry.IsAdopted(group))
			return "HQ: you are not in an HQ squad.";

		string name = CLW_SquadGroups.DisplayName(group);
		group.RemovePlayer(playerId);
		SCR_PlayerControllerGroupComponent controller = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
		if (controller)
			controller.ResetGroupIDs_S();	// [verify] same call as CLW_SquadJoin.OnSquadReleased: frees the player to pick another squad
		Print(string.Format("CLW: hqmenu leave player=%1 left squad %2 (humans left %3)", playerId, name, group.GetPlayerCount()), LogLevel.NORMAL);
		return "HQ: you left " + name + ". Pick another squad in this menu or the groups menu.";
	}

	// ---- wire ----

	// Tabs and line breaks would break the format.
	static string Clean(string text)
	{
		text.Replace("\t", " ");
		text.Replace("\n", " ");
		text.Replace("\r", " ");
		if (text.Length() > CLW_HqMenuConfig.FIELD_MAX)
			text = text.Substring(0, CLW_HqMenuConfig.FIELD_MAX);

		return text;
	}

	protected static string Line(string type, string a = "", string b = "", string c = "", string d = "", string e = "", string f = "")
	{
		string line = type;
		if (!a.IsEmpty() || !b.IsEmpty() || !c.IsEmpty() || !d.IsEmpty() || !e.IsEmpty() || !f.IsEmpty())
			line += "\t" + Clean(a);
		if (!b.IsEmpty() || !c.IsEmpty() || !d.IsEmpty() || !e.IsEmpty() || !f.IsEmpty())
			line += "\t" + Clean(b);
		if (!c.IsEmpty() || !d.IsEmpty() || !e.IsEmpty() || !f.IsEmpty())
			line += "\t" + Clean(c);
		if (!d.IsEmpty() || !e.IsEmpty() || !f.IsEmpty())
			line += "\t" + Clean(d);
		if (!e.IsEmpty() || !f.IsEmpty())
			line += "\t" + Clean(e);
		if (!f.IsEmpty())
			line += "\t" + Clean(f);

		return line + "\n";
	}

	static string BuildWire(int playerId, vector terminalPos, string status)
	{
		string faction = CLW_RequestService.FactionKeyOf(playerId);
		string role = CLW_RequestService.RoleOf(playerId);
		CLW_WarState war = CLW_GameMode.GetWarState();
		bool running = war && war.IsRunning();

		// the base whose stock a terminal request draws from
		string baseId;
		string baseLabel = "HQ";
		if (terminalPos != vector.Zero)
		{
			CLW_TerminalInfo terminal = CLW_RequestTerminals.FindAt(terminalPos);
			if (terminal && terminal.m_sSide == faction)
			{
				baseId = terminal.m_sBase;
				if (!CLW_SupplyStore.IsHq(baseId))
					baseLabel = baseId;
			}
		}

		int supplies;
		if (running && !faction.IsEmpty())
			supplies = CLW_SupplyStore.Get(faction, baseId);

		string wire = Line("H", faction, role, supplies.ToString(), baseLabel, "0");
		if (running)
			wire = Line("H", faction, role, supplies.ToString(), baseLabel, "1");
		if (!status.IsEmpty())
			wire += Line("S", status);

		if (!running || faction.IsEmpty())
		{
			wire += Line("L", "The menu is available once the war is running and you are on a side.");
			wire += Line("K", "0", "the war is not running");
			return wire;
		}

		AppendRequests(wire, playerId, faction, baseId, terminalPos);
		AppendSquads(wire, playerId, faction);
		AppendBase(wire, faction, baseId);

		if (CLW_Capabilities.Has("reports"))
			wire += Line("K", "1", "");
		else
			wire += Line("K", "0", "contact reports are off in this war mode");

		return wire;
	}

	// ---- requests ----

	// Why a request of this kind would be refused right now ("" = looks fine). The executor still decides; this is the hint on the row.
	protected static string Blocked(int playerId, string faction, string kind, string baseId)
	{
		if (!CLW_Capabilities.Has("requests"))
			return "requests are off in this war mode";

		vector pos;
		if (!CLW_RequestService.PlayerPos(playerId, pos))
			return "you are not deployed";

		if (CLW_RequestKind.NeedsLeader(kind) && !CLW_RequestService.IsSquadLeader(playerId))
			return "squad leaders only";

		float left = CLW_RequestService.CooldownLeft(playerId, kind);
		if (left > 0)
			return "cooldown, " + Math.Round(left) + " s left";

		int cost = -1;
		if (kind == CLW_RequestKind.VEHICLE)
		{
			CLW_ForceEntry vehicle = CLW_RequestService.PickVehicle(faction, "");
			if (!vehicle)
				return "no vehicle in the faction's catalog";
			cost = CLW_RequestService.VehicleCost(vehicle);
		}
		else if (kind == CLW_RequestKind.SQUAD)
		{
			CLW_ForceEntry team = CLW_RequestService.PickInfantry(faction, "");
			if (!team)
				return "no AI team in the faction's catalog";
			cost = team.m_iCost;
		}
		else if (kind == CLW_RequestKind.RESUPPLY)
		{
			cost = CLW_RequestConfig.RESUPPLY_COST;
		}

		if (cost > 0)
		{
			int have = CLW_SupplyStore.Get(faction, baseId);
			if (have < cost)
				return string.Format("needs %1 supplies, the base has %2", cost, have);
		}

		return "";
	}

	protected static void AppendRequests(inout string wire, int playerId, string faction, string baseId, vector terminalPos)
	{
		string where;
		if (terminalPos != vector.Zero)
			where = " (at this terminal)";

		string reason = Blocked(playerId, faction, CLW_RequestKind.VEHICLE, baseId);
		wire += Line("R", "vehicle", CLW_RequestKind.VEHICLE, "Vehicle: empty, parked at the depot" + where, reason);
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		array<CLW_ForceEntry> entries = {};
		if (catalog)
			catalog.GetEntries(faction, entries, true);	// players drive: crewless vehicles too

		CLW_ForceEntry defaultVehicle = CLW_RequestService.PickVehicle(faction, "");
		if (defaultVehicle)
			wire += Line("O", "-", "Default: " + defaultVehicle.GetVehicleName() + " (" + CLW_RequestService.VehicleCost(defaultVehicle) + ")");

		int listed;
		foreach (CLW_ForceEntry entry : entries)
		{
			if (!entry || !entry.m_bEnabled || !entry.HasVehicle() || listed >= CLW_HqMenuConfig.OPTIONS_MAX)
				continue;

			wire += Line("O", entry.m_sType, entry.GetVehicleName() + " (" + CLW_RequestService.VehicleCost(entry) + ")");
			listed++;
		}

		reason = Blocked(playerId, faction, CLW_RequestKind.SQUAD, baseId);
		wire += Line("R", "squad", CLW_RequestKind.SQUAD, "Squad: an AI team joins your squad (squad leaders)", reason);
		CLW_ForceEntry defaultTeam = CLW_RequestService.PickInfantry(faction, "");
		if (defaultTeam)
			wire += Line("O", "-", "Default: " + defaultTeam.m_sName + " (" + defaultTeam.m_iCost + ")");

		listed = 0;
		foreach (CLW_ForceEntry team : entries)
		{
			if (!team || !team.m_bEnabled || team.HasVehicle() || team.m_GroupPrefab.IsEmpty() || listed >= CLW_HqMenuConfig.OPTIONS_MAX)
				continue;

			wire += Line("O", team.m_sType, team.m_sName + " (" + team.m_iCost + ")");
			listed++;
		}

		reason = Blocked(playerId, faction, CLW_RequestKind.RESUPPLY, baseId);
		wire += Line("R", "resupply", CLW_RequestKind.RESUPPLY, "Resupply box: ammo at your position, cost " + CLW_RequestConfig.RESUPPLY_COST, reason);

		reason = Blocked(playerId, faction, CLW_RequestKind.REINFORCE, baseId);
		wire += Line("R", "reinforce", CLW_RequestKind.REINFORCE, "Reinforce: HQ sends the nearest free group to you (squad leaders, free)", reason);

		reason = Blocked(playerId, faction, CLW_RequestKind.RECON, baseId);
		wire += Line("R", "recon", CLW_RequestKind.RECON, "Recon: HQ sends a group to look (squad leaders, free)", reason);
		wire += Line("O", "here", "Where: here");
		wire += Line("O", "objective", "Where: nearest objective");
	}

	// ---- squads ----

	protected static void AppendSquads(inout string wire, int playerId, string faction)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!registry)
			return;

		array<string> ids = {};
		registry.GetIds(faction, ids);
		int listed;
		foreach (string id : ids)
		{
			SCR_AIGroup group = registry.Find(id);
			if (!group || !registry.IsJoinable(group) || listed >= CLW_HqMenuConfig.SQUADS_MAX)
				continue;

			string mine = "0";
			if (group.IsPlayerInGroup(playerId))
				mine = "1";
			string full = "0";
			if (group.IsFull())
				full = "1";

			wire += Line("Q", id, CLW_SquadGroups.DisplayName(group), group.GetPlayerCount().ToString(), CLW_SquadInfo.AliveCount(group).ToString(), mine, full);
			listed++;
		}
	}

	// ---- base (side stock, FOBs, convoys) ----

	protected static void AppendBase(inout string wire, string faction, string baseId)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		string line = "HQ supplies: " + war.GetSupplies(faction);
		if (CLW_Capabilities.Has("logistics"))
			line += ", side total " + CLW_SupplyStore.GetTotal(faction);
		wire += Line("L", line);

		if (!CLW_SupplyStore.IsHq(baseId) && CLW_SupplyStore.HasOwnStock(faction, baseId))
			wire += Line("L", "This terminal draws on " + baseId + " stock: " + CLW_SupplyStore.Get(faction, baseId));

		array<CLW_Fob> fobs = {};
		CLW_FobManager.GetFobs(faction, fobs);
		foreach (CLW_Fob fob : fobs)
		{
			string fobLine = fob.m_sId;
			if (!fob.m_sName.IsEmpty())
				fobLine += " " + fob.m_sName;
			fobLine += ", grid " + CLW_Places.Grid(fob.m_vPos) + ", " + fob.StateName();
			if (fob.IsInProgress())
				fobLine += " " + Math.Round(fob.m_fBuild) + "%";
			if (fob.m_bUnderAttack)
				fobLine += ", UNDER ATTACK";
			if (fob.IsActive() && CLW_SupplyStore.HasOwnStock(faction, fob.m_sId))
				fobLine += ", stock " + CLW_SupplyStore.Get(faction, fob.m_sId);
			wire += Line("L", fobLine);
		}

		if (fobs.IsEmpty() && CLW_Capabilities.Has("fob"))
			wire += Line("L", "No FOB yet.");

		if (!CLW_Capabilities.Has("logistics"))
			return;

		int convoys;
		array<string> objects = {};
		string json = CLW_Convoys.ConvoysJson(faction);
		SplitObjects(json, objects);
		foreach (string obj : objects)
		{
			string state = JsonField(obj, "state");
			if (state != "planned" && state != "loading" && state != "en_route")
				continue;

			wire += Line("L", string.Format("Convoy %1: %2 -> %3, %4 supplies, %5, grid %6", JsonField(obj, "id"), JsonField(obj, "from"), JsonField(obj, "to"), JsonField(obj, "cargo"), state, JsonField(obj, "grid")));
			convoys++;
		}

		if (convoys == 0)
			wire += Line("L", "No convoy in flight.");
	}

	// Convoy JSON body "{..},{..}" -> one string per object (flat objects, no nested braces except the pos array).
	protected static void SplitObjects(string json, notnull array<string> outObjects)
	{
		if (json.IsEmpty())
			return;

		int depth;
		int start = -1;
		int i;
		while (i < json.Length())
		{
			string ch = json.Get(i);
			if (ch == "{")
			{
				if (depth == 0)
					start = i;
				depth++;
			}
			else if (ch == "}")
			{
				depth--;
				if (depth == 0 && start >= 0)
				{
					outObjects.Insert(json.Substring(start, i - start + 1));
					start = -1;
				}
			}

			i++;
		}
	}

	// Value of "key" in a flat JSON object (string without quotes, or the number text). "" when absent.
	protected static string JsonField(string obj, string key)
	{
		string needle = "\"" + key + "\":";
		int at = obj.IndexOf(needle);
		if (at < 0)
			return "";

		int from = at + needle.Length();
		if (from >= obj.Length())
			return "";

		string rest = obj.Substring(from, obj.Length() - from);
		if (rest.Get(0) == "\"")
		{
			string quoted = rest.Substring(1, rest.Length() - 1);
			int close = quoted.IndexOf("\"");
			if (close < 0)
				return quoted;

			return quoted.Substring(0, close);
		}

		int end = rest.Length();
		int comma = rest.IndexOf(",");
		if (comma >= 0)
			end = comma;
		int brace = rest.IndexOf("}");
		if (brace >= 0 && brace < end)
			end = brace;

		return rest.Substring(0, end);
	}
}

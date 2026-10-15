// HQ menu, server side (M4 WP4, VISION 6a light). One dialog (CLW_HqMenuDialog) with the sections Requests, Squad, Base, Contact report.
// Opened from the "Open HQ menu" action of the request terminal (CLW_HqMenuAction) or the chat command /hqmenu. Server authoritative:
// every button runs the SAME code as the chat commands (CLW_RequestService.Handle = /request, CLW_Reports.Chat = /report, CLW_Backup.Chat = /backup,
// CLW_PlayerSquad.Join = joining AI squads, CLW_SitrepClient = /sitrep), so rules, cooldowns and logs stay in one place.
// Data to the client is a flat TAB / LF text (same idea as CLW_SetupServer.BuildWire), one line per item, first field = line type:
//   H <faction> <role> <supplies> <base label> <running 0|1>            header (who you are, where the stock comes from)
//   S <text>                                                            status line (answer to the last action)
//   R <id> <kind> <title> <blocked reason or empty>                     one request row, followed by its options
//   O <arg> <label>                                                     an option of the previous R (type to request)
//   Q <squadId> <name> <humans> <members> <mine 0|1> <full 0|1>         a joinable HQ squad of the player's side
//   L <text>                                                            a base line (stock, FOB, convoy)
//   K <allowed 0|1> <reason>                                            contact report available
//   G <language code> <override or empty>                              the player's HQ language (CLW_PlayerLang.Resolve) and the stored override (CLW_PlayerLang.GetOverride, empty = auto)
// i18n: every text field (S, R title / blocked reason, O label, L, K reason) is a CLW_Loc wire (or plain content text such as a vehicle
// name), formatted by the client in its own language (CLW_HqMenuClient / CLW_HqMenuDialog). Wire params are content names or numbers.
// Server only. Log prefix: "CLW: hqmenu".

class CLW_HqMenuConfig
{
	static const int SQUADS_MAX = 10;				// squads listed
	static const int OPTIONS_MAX = 10;				// vehicle / squad types listed per request row
	static const int FIELD_MAX = 400;				// characters per text field on the wire (a wire is longer than its text)
	static const int DELIVERY_MARKER_SEC = 300;		// map marker of a delivered vehicle is removed after this
	static const int DELIVERY_POLL_MS = 2000;		// check "did the requester get in"
	static const int PAGE_SIZE = 12;				// depot menu: vehicles per page

	// Depot menu categories (CLW_ForceKind of the catalog entry): all / light / motorized / mech / armor / logistics.
	static bool IsCategory(string cat)
	{
		return cat == "all" || cat == "light" || cat == "motorized" || cat == "mech" || cat == "armor" || cat == "logistics";
	}

	static bool InCategory(string cat, string kind)
	{
		if (cat == "all")
			return true;
		if (cat == "light")
			return kind == CLW_ForceKind.LIGHT || kind == CLW_ForceKind.RECON;
		if (cat == "armor")
			return kind == CLW_ForceKind.ARMOR || kind == CLW_ForceKind.ARMORED_RECON;
		return cat == kind;
	}
}

class CLW_HqMenuServer
{
	protected static ref map<int, string> s_mView = new map<int, string>();	// WP11 depot menu: player id -> "<category>|<page>"

	// ---- entry points (from the player controller RPCs) ----

	// The player opened the menu (terminal action: terminalPos = terminal; chat: Zero). Answers with the first payload.
	static void Open(int playerId, vector terminalPos)
	{
		if (!Replication.IsServer() || playerId <= 0)
			return;

		Print(string.Format("CLW: hqmenu open player=%1(%2) terminal=%3", playerId, CLW_RequestService.PlayerName(playerId), terminalPos), LogLevel.NORMAL);
		s_mView.Remove(playerId);
		CLW_ServiceInstance depot = CLW_DepotLookup.FindAt(CLW_RequestService.FactionKeyOf(playerId), terminalPos);
		if (depot)
			Print(string.Format("CLW: depot menu %1 opened at %2 %3", CLW_RequestService.PlayerName(playerId), depot.m_sSide, depot.m_sBase), LogLevel.NORMAL);

		Push(playerId, terminalPos, "");
	}

	// A button of the menu. action: req | join | leave | report (arg = /report syntax, direction may be a bearing or ahead) | report_cancel | backup (arg = note) | lang | refresh. Runs the shared code, then pushes fresh data.
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
			CLW_RequestService.HandleWire(playerId, kind, typeArg, terminalPos, status);	// answers chat + hint itself; the status line shows the same translated answer (the returned English text is for logs)
			if (status.IsEmpty())
				status = CLW_Loc.W("#CLW-HqMenu_Answered");
		}
		else if (action == "vpage")
		{
			s_mView.Set(playerId, arg);	// validated when the page is built (AppendDepotVehicles)
		}
		else if (action == "vreq")
		{
			// depot menu: arg = exact catalog type id; the server re-validates side, distance to the depot, cost and caps (Handle)
			string depotResult = CLW_RequestService.HandleWire(playerId, CLW_RequestKind.VEHICLE, arg, terminalPos, status);
			if (status.IsEmpty())
				status = CLW_Loc.W("#CLW-HqMenu_Answered");
			Print(string.Format("CLW: depot request %1 %2 -> %3", CLW_RequestService.PlayerName(playerId), arg, depotResult), LogLevel.NORMAL);
		}
		else if (action == "report")
		{
			CLW_Reports.ChatWire(playerId, arg, status);	// answers chat + hint itself; the status line shows the same translated answer
			if (status.IsEmpty())
				status = CLW_Loc.W("#CLW-HqMenu_Answered");
		}
		else if (action == "report_cancel")
		{
			CLW_Reports.ChatWire(playerId, "cancel", status);	// the same code as /report cancel; answers chat + hint itself
			if (status.IsEmpty())
				status = CLW_Loc.W("#CLW-HqMenu_Answered");
		}
		else if (action == "backup")
		{
			CLW_Backup.ChatWire(playerId, arg, status);	// the same code as /backup <note> (arg = note); answers chat + hint itself
			if (status.IsEmpty())
				status = CLW_Loc.W("#CLW-HqMenu_Answered");
		}
		else if (action == "mission" || action == "status")
		{
			// radial menu: the registry entries REQUEST_MISSION / REQUEST_STATUS as the player (same path as an /hq action)
			string entryName = "REQUEST_STATUS";
			if (action == "mission")
				entryName = "REQUEST_MISSION";
			status = RunPlayerEntry(playerId, entryName);
			CLW_RequestService.Answer(playerId, status, true);
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
		else if (action == "lang")
		{
			// language option (auto | xx): the same code as /hq language, validated again on the server
			if (CLW_HqLang.Apply(playerId, arg, "menu"))
				status = CLW_HqLang.SetAnswer(playerId, CLW_HqLang.Normalize(arg));
			else
				status = CLW_Loc.W("#CLW-Hq_LangBad", CLW_HqText.Param(arg));
			CLW_RequestService.Answer(playerId, status, true);
		}
		else if (action != "refresh")
		{
			Print("CLW: hqmenu unknown action '" + action + "'", LogLevel.WARNING);
			status = CLW_Loc.W("#CLW-HqMenu_UnknownAction");
		}

		Push(playerId, terminalPos, status);
	}

	// One player registry entry without params, run as the player (authority "player"); returns the answer wire.
	protected static string RunPlayerEntry(int playerId, string actionName)
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		CLW_ActionEntry entry = registry.Find(actionName);
		if (!entry || entry.m_sAuthority != CLW_ActionEntry.AUTH_PLAYER)
			return CLW_Loc.W("#CLW-Hq_NotAllowed", actionName);

		CLW_ActionCall call = new CLW_ActionCall();
		call.m_sAction = actionName;
		call.m_sFaction = CLW_RequestService.FactionKeyOf(playerId);
		call.m_sAuthority = CLW_ActionEntry.AUTH_PLAYER;
		call.m_sSource = "player";
		call.m_iPlayerId = playerId;
		call.m_sReason = "hq menu by " + CLW_RequestService.PlayerName(playerId);
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(call.m_sFaction);
		if (commander)
			call.m_Rules = commander.GetRules();

		string result;
		bool granted = registry.Run(call, result);
		Print(string.Format("CLW: hqmenu entry %1 player=%2 -> %3: %4", actionName, playerId, granted, result), LogLevel.NORMAL);
		if (granted)
			return CLW_Loc.W("#CLW-Req_Granted", call.PlayerWire(result));

		return CLW_Loc.W("#CLW-Req_Refused", actionName, call.PlayerWire(result));
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
			return CLW_Loc.W("#CLW-HqMenu_NoWarSquads");
		if (faction.IsEmpty() || !CLW_RequestService.PlayerPos(playerId, pos))
			return CLW_Loc.W("#CLW-HqMenu_NotDeployedSquad");

		SCR_AIGroup group = registry.Find(squadId);
		if (!group || !registry.IsJoinable(group) || CLW_SquadRegistry.GetFactionKey(group) != faction)
		{
			Print(string.Format("CLW: hqmenu join player=%1 squad=%2 refused: not a joinable squad of %3", playerId, squadId, faction), LogLevel.NORMAL);
			return CLW_Loc.W("#CLW-HqMenu_SquadGone");
		}

		if (group.IsPlayerInGroup(playerId))
			return CLW_Loc.W("#CLW-HqMenu_AlreadyIn", CLW_SquadGroups.DisplayName(group));

		if (group.IsFull())
			return CLW_Loc.W("#CLW-HqMenu_SquadNoSlot", CLW_SquadGroups.DisplayName(group));

		Print(string.Format("CLW: hqmenu join player=%1 -> squad %2 (%3) via CLW_PlayerSquad.Join", playerId, squadId, group), LogLevel.NORMAL);
		CLW_PlayerSquad.Join(group, playerId);
		return CLW_Loc.W("#CLW-HqMenu_Joining", CLW_SquadGroups.DisplayName(group));
	}

	// Same calls as CLW_SquadJoin.OnSquadReleased; the vanilla hooks (CLW_SquadJoin) release the squad when no human is left.
	protected static string LeaveSquad(int playerId)
	{
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		SCR_AIGroup group = CLW_RequestService.GroupOf(playerId);
		if (!registry || !group || !registry.IsAdopted(group))
			return CLW_Loc.W("#CLW-HqMenu_NotInSquad");

		string name = CLW_SquadGroups.DisplayName(group);
		group.RemovePlayer(playerId);
		SCR_PlayerControllerGroupComponent controller = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
		if (controller)
			controller.ResetGroupIDs_S();	// [verify] same call as CLW_SquadJoin.OnSquadReleased: frees the player to pick another squad
		Print(string.Format("CLW: hqmenu leave player=%1 left squad %2 (humans left %3)", playerId, name, group.GetPlayerCount()), LogLevel.NORMAL);
		return CLW_Loc.W("#CLW-HqMenu_Left", name);
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
		bool atDepot;
		CLW_ServiceInstance depotInst = CLW_DepotLookup.FindAt(faction, terminalPos);
		if (depotInst)
		{
			atDepot = true;
			baseId = depotInst.m_sBase;
			if (!CLW_SupplyStore.IsHq(baseId))
				baseLabel = baseId;
		}
		else if (terminalPos != vector.Zero)
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
		wire += Line("G", CLW_PlayerLang.Resolve(playerId), CLW_PlayerLang.GetOverride(playerId));

		if (!running || faction.IsEmpty())
		{
			wire += Line("L", CLW_Loc.W("#CLW-HqMenu_NeedWar"));
			wire += Line("K", "0", CLW_Loc.W("#CLW-HqMenu_ReportWarOff"));
			return wire;
		}

		if (atDepot)
			AppendDepotVehicles(wire, playerId, faction, baseId);
		else
			AppendRequests(wire, playerId, faction, baseId, terminalPos);
		AppendSquads(wire, playerId, faction);
		AppendBase(wire, faction, baseId);

		if (CLW_Capabilities.Has("reports"))
			wire += Line("K", "1", "");
		else
			wire += Line("K", "0", CLW_Loc.W("#CLW-HqMenu_ReportOff"));

		return wire;
	}

	// ---- requests ----

	// Why a request of this kind would be refused right now ("" = looks fine), as a wire. The executor still decides; this is the hint on the row.
	protected static string Blocked(int playerId, string faction, string kind, string baseId)
	{
		if (!CLW_Capabilities.Has("requests"))
			return CLW_Loc.W("#CLW-HqMenu_Blk_RequestsOff");

		vector pos;
		if (!CLW_RequestService.PlayerPos(playerId, pos))
			return CLW_Loc.W("#CLW-Req_NotDeployed");

		if (CLW_RequestKind.NeedsLeader(kind) && !CLW_RequestService.IsSquadLeader(playerId))
			return CLW_Loc.W("#CLW-HqMenu_Blk_LeaderOnly");

		float left = CLW_RequestService.CooldownLeft(playerId, kind);
		if (left > 0)
		{
			int leftSec = Math.Round(left);
			return CLW_Loc.W("#CLW-HqMenu_Blk_Cooldown", CLW_Loc.N(leftSec));
		}

		int cost = -1;
		if (kind == CLW_RequestKind.VEHICLE)
		{
			CLW_ForceEntry vehicle = CLW_RequestService.PickVehicle(faction, "");
			if (!vehicle)
				return CLW_Loc.W("#CLW-HqMenu_Blk_NoVehicle");
			cost = CLW_RequestService.VehicleCost(vehicle);
		}
		else if (kind == CLW_RequestKind.SQUAD)
		{
			CLW_ForceEntry team = CLW_RequestService.PickInfantry(faction, "");
			if (!team)
				return CLW_Loc.W("#CLW-HqMenu_Blk_NoTeam");
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
				return CLW_Loc.W("#CLW-HqMenu_Blk_Supplies", CLW_Loc.N(cost), CLW_Loc.N(have));
		}

		return "";
	}

	// WP11 depot menu: header V <category> <page> <pages> <total> <blocked>, then one W <type> <name> <cost> <kind> per vehicle of the page.
	protected static void AppendDepotVehicles(inout string wire, int playerId, string faction, string baseId)
	{
		string cat = "all";
		int page;
		string view;
		if (s_mView.Find(playerId, view))
		{
			array<string> viewParts = {};
			view.Split("|", viewParts, false);
			if (viewParts.Count() > 0)
				cat = viewParts[0];
			if (viewParts.Count() > 1)
				page = viewParts[1].ToInt();
		}

		cat.ToLower();
		if (!CLW_HqMenuConfig.IsCategory(cat))
			cat = "all";

		array<CLW_ForceEntry> entries = {};
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (catalog)
			catalog.GetEntries(faction, entries, true);	// players drive: crewless vehicles too

		array<CLW_ForceEntry> shown = {};
		foreach (CLW_ForceEntry entry : entries)
		{
			if (!entry || !entry.m_bEnabled || !entry.HasVehicle() || !CLW_HqMenuConfig.InCategory(cat, entry.m_sKind))
				continue;

			shown.Insert(entry);
		}

		int pages = Math.Max(1, (shown.Count() + CLW_HqMenuConfig.PAGE_SIZE - 1) / CLW_HqMenuConfig.PAGE_SIZE);
		page = Math.Clamp(page, 0, pages - 1);
		s_mView.Set(playerId, cat + "|" + page.ToString());
		string reason = Blocked(playerId, faction, CLW_RequestKind.VEHICLE, baseId);
		wire += Line("V", cat, page.ToString(), pages.ToString(), shown.Count().ToString(), reason);
		int from = page * CLW_HqMenuConfig.PAGE_SIZE;
		int to = Math.Min(from + CLW_HqMenuConfig.PAGE_SIZE, shown.Count());
		for (int i = from; i < to; i++)
		{
			CLW_ForceEntry listed = shown[i];
			wire += Line("W", listed.m_sType, listed.GetVehicleName(), CLW_RequestService.VehicleCost(listed).ToString(), listed.m_sKind);
		}
	}

	protected static void AppendRequests(inout string wire, int playerId, string faction, string baseId, vector terminalPos)
	{
		string vehicleTitle = CLW_Loc.W("#CLW-HqMenu_RowVehicle");
		if (terminalPos != vector.Zero)
			vehicleTitle = CLW_Loc.W("#CLW-HqMenu_RowVehicleTerm");

		string reason = Blocked(playerId, faction, CLW_RequestKind.VEHICLE, baseId);
		wire += Line("R", "vehicle", CLW_RequestKind.VEHICLE, vehicleTitle, reason);
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		array<CLW_ForceEntry> entries = {};
		if (catalog)
			catalog.GetEntries(faction, entries, true);	// players drive: crewless vehicles too

		CLW_ForceEntry defaultVehicle = CLW_RequestService.PickVehicle(faction, "");
		if (defaultVehicle)
			wire += Line("O", "-", CLW_Loc.W("#CLW-HqMenu_OptDefault", defaultVehicle.GetVehicleName(), CLW_Loc.N(CLW_RequestService.VehicleCost(defaultVehicle))));

		int listed;
		foreach (CLW_ForceEntry entry : entries)
		{
			if (!entry || !entry.m_bEnabled || !entry.HasVehicle() || listed >= CLW_HqMenuConfig.OPTIONS_MAX)
				continue;

			wire += Line("O", entry.m_sType, CLW_Loc.W("#CLW-HqMenu_OptNameCost", entry.GetVehicleName(), CLW_Loc.N(CLW_RequestService.VehicleCost(entry))));
			listed++;
		}

		reason = Blocked(playerId, faction, CLW_RequestKind.SQUAD, baseId);
		wire += Line("R", "squad", CLW_RequestKind.SQUAD, CLW_Loc.W("#CLW-HqMenu_RowSquad"), reason);
		CLW_ForceEntry defaultTeam = CLW_RequestService.PickInfantry(faction, "");
		if (defaultTeam)
			wire += Line("O", "-", CLW_Loc.W("#CLW-HqMenu_OptDefault", defaultTeam.m_sName, CLW_Loc.N(defaultTeam.m_iCost)));

		listed = 0;
		foreach (CLW_ForceEntry team : entries)
		{
			if (!team || !team.m_bEnabled || team.HasVehicle() || team.m_GroupPrefab.IsEmpty() || listed >= CLW_HqMenuConfig.OPTIONS_MAX)
				continue;

			wire += Line("O", team.m_sType, CLW_Loc.W("#CLW-HqMenu_OptNameCost", team.m_sName, CLW_Loc.N(team.m_iCost)));
			listed++;
		}

		reason = Blocked(playerId, faction, CLW_RequestKind.RESUPPLY, baseId);
		wire += Line("R", "resupply", CLW_RequestKind.RESUPPLY, CLW_Loc.W("#CLW-HqMenu_RowResupply", CLW_Loc.N(CLW_RequestConfig.RESUPPLY_COST)), reason);

		reason = Blocked(playerId, faction, CLW_RequestKind.REINFORCE, baseId);
		wire += Line("R", "reinforce", CLW_RequestKind.REINFORCE, CLW_Loc.W("#CLW-HqMenu_RowReinforce"), reason);

		reason = Blocked(playerId, faction, CLW_RequestKind.RECON, baseId);
		wire += Line("R", "recon", CLW_RequestKind.RECON, CLW_Loc.W("#CLW-HqMenu_RowRecon"), reason);
		wire += Line("O", "here", CLW_Loc.W("#CLW-HqMenu_WhereHere"));
		wire += Line("O", "objective", CLW_Loc.W("#CLW-HqMenu_WhereObjective"));
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
		string line = CLW_Loc.W("#CLW-HqMenu_BaseSupplies", CLW_Loc.N(war.GetSupplies(faction)));
		if (CLW_Capabilities.Has("logistics"))
			line = CLW_Loc.W("#CLW-HqMenu_BaseSupplyTotal", CLW_Loc.N(war.GetSupplies(faction)), CLW_Loc.N(CLW_SupplyStore.GetTotal(faction)));
		wire += Line("L", line);

		if (!CLW_SupplyStore.IsHq(baseId) && CLW_SupplyStore.HasOwnStock(faction, baseId))
			wire += Line("L", CLW_Loc.W("#CLW-HqMenu_BaseTerminal", baseId, CLW_Loc.N(CLW_SupplyStore.Get(faction, baseId))));

		array<CLW_Fob> fobs = {};
		CLW_FobManager.GetFobs(faction, fobs);
		foreach (CLW_Fob fob : fobs)
		{
			string fobName = fob.m_sId;
			if (!fob.m_sName.IsEmpty())
				fobName += " " + fob.m_sName;

			string fobProgress;
			if (fob.IsInProgress())
			{
				int buildPct = Math.Round(fob.m_fBuild);
				fobProgress = CLW_Loc.W("#CLW-HqMenu_FobProgress", CLW_Loc.N(buildPct));
			}

			string fobAttack;
			if (fob.m_bUnderAttack)
				fobAttack = CLW_Loc.W("#CLW-HqMenu_FobAttack");

			string fobStock;
			if (fob.IsActive() && CLW_SupplyStore.HasOwnStock(faction, fob.m_sId))
				fobStock = CLW_Loc.W("#CLW-HqMenu_FobStock", CLW_Loc.N(CLW_SupplyStore.Get(faction, fob.m_sId)));

			wire += Line("L", CLW_Loc.W("#CLW-HqMenu_Fob", fobName, CLW_Places.Grid(fob.m_vPos), CLW_Loc.W("#CLW-HqMenu_FobState_" + fob.StateName()), fobProgress, fobAttack, fobStock));
		}

		if (fobs.IsEmpty() && CLW_Capabilities.Has("fob"))
			wire += Line("L", CLW_Loc.W("#CLW-HqMenu_NoFob"));

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

			wire += Line("L", CLW_Loc.W("#CLW-HqMenu_Convoy", JsonField(obj, "id"), JsonField(obj, "from"), JsonField(obj, "to"), JsonField(obj, "cargo"), CLW_Loc.W("#CLW-HqMenu_ConvoyState_" + state), JsonField(obj, "grid")));
			convoys++;
		}

		if (convoys == 0)
			wire += Line("L", CLW_Loc.W("#CLW-HqMenu_NoConvoy"));
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

// Server side of the war setup menu (spec 2a, phase P5). Server only.
// - tracks the setup owner (CLW_WarState owns the id; this class re-assigns it when the owner leaves). The owner is the lowest
//   player id with a player controller: players have NO character during SETUP (CLW_SpawnLogic parks them until the war starts),
// - sends the owner the setup data (wire text, see BuildWire) so the owner's client opens the menu, and re-sends it every RESEND_MS
//   until the client acknowledged it (OnMenuAck, CLW_SetupClient.Open; the menu may be sent before the client UI is ready), at most
//   MAX_SEND_ATTEMPTS times. The others get a "waiting" hint, sent once a player has been seen in two polls,
// - validates the submit (sender == owner or admin, phase == SETUP, mode + values + factions) and calls CLW_WarState.StartWar,
// - announces the started war and closes the menus.
// A rematch (CLW_Rematch.c) calls Start again for the next round: the owner is picked anew and the menu is re-sent.
// The client side lives in CLW_SetupClient.c / CLW_SetupDialog.c, the RPC glue in CLW_SetupPlayerController.c.
class CLW_SetupServer
{
	static const string PARAM_FACTION_A = "factionA";
	static const string PARAM_FACTION_B = "factionB";
	static const int POLL_MS = 1500;
	static const int RESEND_MS = 5000;			// menu re-send interval until the owner acknowledged it
	static const int MAX_SEND_ATTEMPTS = 12;

	protected static ref CLW_SetupServer s_Instance;

	protected CLW_WarState m_War;
	protected CLW_ScenarioConfig m_Scenario;
	protected int m_iOwner = -1;				// owner we last sent the menu to
	protected bool m_bMenuAck;					// the owner's client confirmed the menu is open
	protected int m_iSendAttempts;
	protected int m_iLastSendTick;
	protected bool m_bGaveUp;
	protected bool m_bAnnounced;
	protected string m_sBattleText;
	protected ref array<int> m_aWaitSent = {};
	protected ref array<int> m_aSeen = {};		// players seen in one poll (waiting hint goes out on the second)

	// Called by CLW_WarState.Init (server only).
	static void Start(CLW_WarState war, CLW_ScenarioConfig scenario)
	{
		if (!Replication.IsServer() || !war || !scenario)
			return;

		if (s_Instance)
			GetGame().GetCallqueue().Remove(s_Instance.Poll);	// rematch: the driver of the previous round (normally stopped itself already)

		s_Instance = new CLW_SetupServer();
		s_Instance.m_War = war;
		s_Instance.m_Scenario = scenario;
		s_Instance.m_sBattleText = scenario.GetFactionKey(0) + " vs " + scenario.GetFactionKey(1);
		GetGame().GetCallqueue().CallLater(s_Instance.Poll, POLL_MS, true);

		array<string> keys = {};
		array<string> names = {};
		s_Instance.CollectFactions(keys, names);
		string list;
		foreach (int i, string key : keys)
		{
			list += " " + key + "='" + names[i] + "'";
		}
		Print("CLW: setup server started; factions offered:" + list, LogLevel.NORMAL);
	}

	// ---- public entry points used by the player controller RPCs (server) ----

	// Submit from the menu. Returns "" on success (war started) or the error text for the player.
	static string Submit(int playerId, string battleJson, string modeId, string modeJson)
	{
		if (!s_Instance)
			return CLW_Loc.W("#CLW-Setup_Unavailable");

		return s_Instance.DoSubmit(playerId, battleJson, modeId, modeJson);
	}

	// Chat fallback: args = "start [modeId] [key=value ...]" or "setup". Returns a reply line for the sender.
	static string ChatCommand(int playerId, string args)
	{
		if (!s_Instance)
			return CLW_Loc.W("#CLW-Setup_Unavailable");

		return s_Instance.DoChat(playerId, args);
	}

	// Menu data as JSON: {"phase","default","setupOwner","modes":[...],"battle":[...]} (log / dashboard; the client menu uses BuildWire).
	static string GetSetupJson()
	{
		if (!s_Instance || !s_Instance.m_War)
			return "{}";

		return s_Instance.BuildSetupJson();
	}

	// The owner's client opened (or already had open) the menu. playerId comes from the RPC receiver, never from the client.
	static void OnMenuAck(int playerId)
	{
		if (!s_Instance || playerId != s_Instance.m_iOwner || s_Instance.m_bMenuAck)
			return;

		s_Instance.m_bMenuAck = true;
		Print("CLW: setup menu acknowledged by player " + playerId, LogLevel.NORMAL);
	}

	// ---- polling: owner tracking, menu delivery, waiting hints, start announcement ----

	protected void Poll()
	{
		if (!m_War)
			return;

		if (m_War.GetPhase() != CLW_EWarPhase.SETUP)
		{
			if (!m_bAnnounced)
				Announce();
			GetGame().GetCallqueue().Remove(Poll);
			return;
		}

		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		int owner = m_War.GetSetupOwner();
		if (owner > 0 && !players.IsPlayerConnected(owner))
		{
			Print("CLW: setup owner " + owner + " left, picking the next player", LogLevel.NORMAL);
			m_War.SetSetupOwner(-1);
			owner = -1;
		}

		if (owner < 0)
		{
			owner = PickNewOwner(players);
			if (owner < 0)
				return;

			m_War.SetSetupOwner(owner);
			Print("CLW: setup owner is now player " + owner + " (" + players.GetPlayerName(owner) + ")", LogLevel.NORMAL);
		}

		if (owner != m_iOwner)
		{
			m_iOwner = owner;
			m_bMenuAck = false;
			m_bGaveUp = false;
			m_iSendAttempts = 0;
			m_aWaitSent.Clear();
			m_aSeen.Clear();
		}

		// (a respawn of the owner needs no special case: the open menu survives it and Open ignores a second payload)
		if (!m_bMenuAck && !m_bGaveUp)
		{
			if (m_iSendAttempts >= MAX_SEND_ATTEMPTS)
			{
				if ((System.GetTickCount() - m_iLastSendTick) >= RESEND_MS)
				{
					m_bGaveUp = true;
					Print("CLW: setup menu never acknowledged by player " + owner + " after " + m_iSendAttempts + " attempts, giving up (use /war setup)", LogLevel.WARNING);
				}
			}
			else if (m_iSendAttempts == 0 || (System.GetTickCount() - m_iLastSendTick) >= RESEND_MS)
			{
				m_iSendAttempts++;
				m_iLastSendTick = System.GetTickCount();
				SendOpen(owner, m_iSendAttempts);
			}
		}

		string ownerName = players.GetPlayerName(owner);
		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int id : ids)
		{
			if (id == owner || m_aWaitSent.Contains(id))
				continue;

			if (!m_aSeen.Contains(id))
			{
				m_aSeen.Insert(id);
				continue;
			}

			m_aWaitSent.Insert(id);
			SendWait(id, ownerName);
		}
	}

	protected int PickNewOwner(PlayerManager players)
	{
		array<int> ids = {};
		players.GetPlayers(ids);
		int best = -1;
		foreach (int id : ids)
		{
			if (id <= 0 || !players.GetPlayerController(id))
				continue;

			if (best < 0 || id < best)
				best = id;
		}

		return best;
	}

	protected void Announce()
	{
		m_bAnnounced = true;
		string text = CLW_Loc.W("#CLW-War_Started");
		CLW_WarMode mode = m_War.GetMode();
		if (mode)
			text = CLW_Loc.W("#CLW-War_StartedMode", mode.GetName(), mode.GetId(), mode.ValuesText(), m_sBattleText);

		Print("CLW: setup announce: " + CLW_Loc.Resolve(text), LogLevel.NORMAL);
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int id : ids)
		{
			SendClosed(id, text);
		}
	}

	// ---- sending (local call for the listen-server host, RPC otherwise) ----

	protected static bool IsLocal(int playerId)
	{
		return playerId == SCR_PlayerController.GetLocalPlayerId();
	}

	protected static SCR_PlayerController GetController(int playerId)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return null;

		return SCR_PlayerController.Cast(players.GetPlayerController(playerId));
	}

	protected void SendOpen(int playerId, int attempt)
	{
		string wire = BuildWire();
		Print(string.Format("CLW: setup send menu to player %1 (%2 bytes, attempt %3)", playerId, wire.Length(), attempt), LogLevel.NORMAL);
		if (IsLocal(playerId))
		{
			CLW_SetupClient.Open(wire);
			return;
		}

		SCR_PlayerController controller = GetController(playerId);
		if (controller)
			controller.CLW_RpcOpenSetup(wire);
		else
			Print("CLW: setup no controller for player " + playerId, LogLevel.WARNING);
	}

	protected void SendWait(int playerId, string ownerName)
	{
		if (IsLocal(playerId))
		{
			CLW_SetupClient.Wait(ownerName);
			return;
		}

		SCR_PlayerController controller = GetController(playerId);
		if (controller)
			controller.CLW_RpcSetupWait(ownerName);
	}

	protected void SendClosed(int playerId, string text)
	{
		if (IsLocal(playerId))
		{
			CLW_SetupClient.Closed(text);
			return;
		}

		SCR_PlayerController controller = GetController(playerId);
		if (controller)
			controller.CLW_RpcSetupClosed(text);
	}

	static void SendResult(int playerId, bool ok, string text)
	{
		if (IsLocal(playerId))
		{
			CLW_SetupClient.Result(ok, text);
			return;
		}

		SCR_PlayerController controller = GetController(playerId);
		if (controller)
			controller.CLW_RpcSetupResult(ok, text);
	}

	// Re-send the menu on request (chat "/war setup") if the sender may set up the war.
	static void Resend(int playerId)
	{
		if (!s_Instance || !s_Instance.m_War)
			return;

		if (s_Instance.m_War.GetPhase() != CLW_EWarPhase.SETUP)
		{
			SendResult(playerId, false, CLW_Loc.W("#CLW-Setup_AlreadyStarted"));
			return;
		}

		if (!s_Instance.IsAllowed(playerId))
		{
			SendResult(playerId, false, CLW_Loc.W("#CLW-Setup_OwnerOnlyOpen"));
			return;
		}

		Print("CLW: setup menu requested by player " + playerId, LogLevel.NORMAL);
		s_Instance.SendOpen(playerId, 0);		// manual request: no ack bookkeeping
	}

	// ---- submit ----

	protected bool IsAllowed(int playerId)
	{
		if (playerId <= 0)
			return false;

		if (playerId == m_War.GetSetupOwner())
			return true;

		PlayerManager players = GetGame().GetPlayerManager();
		if (players && players.HasPlayerRole(playerId, EPlayerRole.ADMINISTRATOR))		// [verify] EPlayerRole.ADMINISTRATOR
			return true;

		return false;
	}

	protected string DoSubmit(int playerId, string battleJson, string modeId, string modeJson)
	{
		Print(string.Format("CLW: setup submit from player %1: battle=%2 mode='%3' values=%4", playerId, battleJson, modeId, modeJson), LogLevel.NORMAL);
		string error = Check(playerId, battleJson, modeId, modeJson);
		if (!error.IsEmpty())
		{
			Print("CLW: setup REJECTED: " + CLW_Loc.Resolve(error), LogLevel.WARNING);
			return error;
		}

		string keyA;
		string keyB;
		ParseBattle(battleJson, keyA, keyB, error);
		if (modeId.IsEmpty())
			modeId = m_War.GetDefaultModeId();

		// StartWar validates the sides again and re-keys the whole war (CLW_Sides.Apply) before the mode starts.
		if (!m_War.StartWar(modeId, modeJson, playerId, keyA, keyB))
		{
			Print("CLW: setup REJECTED: StartWar refused", LogLevel.WARNING);
			return CLW_Loc.W("#CLW-Setup_StartFailed");
		}

		m_sBattleText = m_Scenario.GetFactionKey(0) + " vs " + m_Scenario.GetFactionKey(1);

		Print("CLW: setup ACCEPTED from player " + playerId + ", war is starting", LogLevel.NORMAL);
		return "";
	}

	// Everything that can reject a submit without side effects. "" = fine.
	protected string Check(int playerId, string battleJson, string modeId, string modeJson)
	{
		if (m_War.GetPhase() != CLW_EWarPhase.SETUP)
			return CLW_Loc.W("#CLW-Setup_AlreadyStarted");

		if (!IsAllowed(playerId))
			return CLW_Loc.W("#CLW-Setup_OwnerOnlyStart");

		string keyA;
		string keyB;
		string error;
		if (!ParseBattle(battleJson, keyA, keyB, error))
			return error;

		if (modeId.IsEmpty())
			modeId = m_War.GetDefaultModeId();

		CLW_WarMode mode = FindMode(modeId);
		if (!mode)
			return CLW_Loc.W("#CLW-Setup_UnknownMode", modeId);

		if (!mode.ApplyParamsJson(modeJson, error))		// no-op on failure; StartWar applies the same values again
			return error;

		return "";
	}

	protected bool ParseBattle(string battleJson, out string keyA, out string keyB, out string error)
	{
		keyA = m_Scenario.GetFactionKey(0);
		keyB = m_Scenario.GetFactionKey(1);
		battleJson = battleJson.Trim();
		if (!battleJson.IsEmpty())
		{
			map<string, string> parsed = new map<string, string>();
			if (!CLW_ModeParam.ParseFlat(battleJson, parsed, error))
				return false;

			foreach (string key, string value : parsed)
			{
				if (key == PARAM_FACTION_A)
					keyA = value;
				else if (key == PARAM_FACTION_B)
					keyB = value;
				else
				{
					error = CLW_Loc.W("#CLW-Setup_UnknownBattleParam", key);
					return false;
				}
			}
		}

		array<string> keys = {};
		array<string> names = {};
		CollectFactions(keys, names);
		if (!keys.Contains(keyA))
		{
			error = CLW_Loc.W("#CLW-Setup_UnknownFaction", keyA);
			return false;
		}

		if (!keys.Contains(keyB))
		{
			error = CLW_Loc.W("#CLW-Setup_UnknownFaction", keyB);
			return false;
		}

		if (keyA == keyB)
		{
			error = CLW_Loc.W("#CLW-Setup_SidesDiffer");
			return false;
		}

		error = m_Scenario.CheckSides(keyA, keyB);		// SCR_Faction, exists, not on the ignore list
		if (!error.IsEmpty())
			return false;

		return true;
	}

	protected CLW_WarMode FindMode(string id)
	{
		for (int i = 0; i < m_War.GetModeCount(); i++)
		{
			CLW_WarMode mode = m_War.GetModeAt(i);
			if (mode && mode.GetId() == id)
				return mode;
		}

		return null;
	}

	// "/war start [modeId] [key=value ...]" and "/war setup"
	protected string DoChat(int playerId, string args)
	{
		args.TrimInPlace();
		array<string> tokens = {};
		args.Split(" ", tokens, true);
		if (tokens.IsEmpty())
			return CLW_Loc.W("#CLW-Setup_Usage");

		string sub = tokens[0];
		sub.ToLower();
		if (sub == "setup")
		{
			Resend(playerId);
			return "";
		}

		if (sub != "start")
			return CLW_Loc.W("#CLW-Setup_Usage");

		string modeId;
		string battleJson;
		string modeJson;
		for (int i = 1; i < tokens.Count(); i++)
		{
			string token = tokens[i];
			int eq = token.IndexOf("=");
			if (eq < 0)
			{
				modeId = token;
				continue;
			}

			string key = token.Substring(0, eq);
			string value = token.Substring(eq + 1, token.Length() - eq - 1);
			string piece = "\"" + CLW_ModeParam.Escape(key) + "\":\"" + CLW_ModeParam.Escape(value) + "\"";
			if (key == PARAM_FACTION_A || key == PARAM_FACTION_B)
			{
				if (!battleJson.IsEmpty())
					battleJson += ",";
				battleJson += piece;
			}
			else
			{
				if (!modeJson.IsEmpty())
					modeJson += ",";
				modeJson += piece;
			}
		}

		if (!battleJson.IsEmpty())
			battleJson = "{" + battleJson + "}";
		if (!modeJson.IsEmpty())
			modeJson = "{" + modeJson + "}";

		Print("CLW: setup chat start from player " + playerId + ": " + args, LogLevel.NORMAL);
		string error = DoSubmit(playerId, battleJson, modeId, modeJson);
		if (error.IsEmpty())
			return CLW_Loc.W("#CLW-Setup_StartAccepted");

		return CLW_Loc.W("#CLW-Setup_StartRejected", error);
	}

	// ---- factions ----

	// Every SCR_Faction of the world's faction manager except the scenario's ignore list; the scenario's own sides are always included.
	protected void CollectFactions(notnull array<string> keys, notnull array<string> names)
	{
		FactionManager manager = GetGame().GetFactionManager();
		if (manager)
		{
			array<Faction> factions = {};
			manager.GetFactionsList(factions);
			foreach (Faction faction : factions)
			{
				SCR_Faction scrFaction = SCR_Faction.Cast(faction);
				if (!scrFaction)
					continue;

				string key = scrFaction.GetFactionKey();
				if (m_Scenario.m_aIgnoreFactions && m_Scenario.m_aIgnoreFactions.Contains(key))
					continue;

				if (keys.Contains(key))
					continue;

				keys.Insert(key);
				names.Insert(scrFaction.GetFactionName());
			}
		}

		for (int i = 0; i < 2; i++)
		{
			string defaultKey = m_Scenario.GetFactionKey(i);
			if (!defaultKey.IsEmpty() && !keys.Contains(defaultKey))
			{
				keys.Insert(defaultKey);
				names.Insert(defaultKey);
			}
		}
	}

	protected void BuildBattleParams(notnull array<ref CLW_ModeParam> outParams)
	{
		array<string> keys = {};
		array<string> names = {};
		CollectFactions(keys, names);
		string list;
		foreach (int i, string key : keys)
		{
			if (i > 0)
				list += ", ";
			list += key;	// the faction names are in the selector itself (translated there)
		}

		// defaults: the server preset's sides when they exist here (CLW_SetupPreset.c), else the scenario sides
		string defaultA = m_Scenario.GetFactionKey(0);
		string defaultB = m_Scenario.GetFactionKey(1);
		CLW_SetupPreset preset = m_War.GetPreset();
		if (preset && keys.Contains(preset.m_sFactionA))
			defaultA = preset.m_sFactionA;
		if (preset && keys.Contains(preset.m_sFactionB))
			defaultB = preset.m_sFactionB;

		outParams.Insert(CLW_ModeParam.CreateChoice(PARAM_FACTION_A, "#CLW-Param_factionA", CLW_Loc.W("#CLW-Param_factionA_Desc", list), keys, defaultA));
		outParams.Insert(CLW_ModeParam.CreateChoice(PARAM_FACTION_B, "#CLW-Param_factionB", "#CLW-Param_factionB_Desc", keys, defaultB));
	}

	// ---- payloads ----

	protected string BuildSetupJson()
	{
		string json = m_War.GetModesJson();
		array<ref CLW_ModeParam> battle = {};
		BuildBattleParams(battle);
		json = CLW_Str.DropLast(json) + ",\"battle\":[";
		foreach (int i, CLW_ModeParam param : battle)
		{
			if (i > 0)
				json += ",";
			json += param.ToJson();
		}
		return json + "]}";
	}

	protected static string Clean(string text)
	{
		text.Replace("\t", " ");
		text.Replace("\n", " ");	// a CLW_Loc wire has no raw tabs or line breaks (escaped), so wires pass through unchanged
		text.Replace("\r", " ");
		return text;
	}

	protected static string WireParam(string group, CLW_ModeParam param)
	{
		string choices;
		foreach (int i, string choice : param.m_aChoices)
		{
			if (i > 0)
				choices += "|";
			choices += Clean(choice);
		}

		string line = "P\t" + group + "\t" + Clean(param.m_sId) + "\t" + Clean(param.m_sLabel);
		line += "\t" + Clean(param.m_sDescription) + "\t" + param.m_eType.ToString();
		line += "\t" + param.m_fMin.ToString() + "\t" + param.m_fMax.ToString() + "\t" + param.m_fStep.ToString();
		line += "\t" + Clean(param.ValueText()) + "\t" + choices + "\n";
		return line;
	}

	// Line based menu payload (fields separated by TAB, lines by LF), parsed by CLW_SetupClient:
	//   H  default mode id
	//   P  group (B = battle, M:<mode id>), id, label, description, type (0 int,1 float,2 bool,3 choice), min, max, step, value, choices joined by |
	//   M  mode id, name, description
	// The client has no verified reader for mixed-type JSON, so the display data is flat text; the SUBMIT stays JSON (CLW_ModeParam.ValuesToJson).
	protected string BuildWire()
	{
		string wire = "H\t" + Clean(m_War.GetDefaultModeId()) + "\n";
		array<ref CLW_ModeParam> battle = {};
		BuildBattleParams(battle);
		foreach (CLW_ModeParam param : battle)
		{
			wire += WireParam("B", param);
		}

		for (int i = 0; i < m_War.GetModeCount(); i++)
		{
			CLW_WarMode mode = m_War.GetModeAt(i);
			if (!mode)
				continue;

			wire += "M\t" + Clean(mode.GetId()) + "\t" + Clean(mode.GetName()) + "\t" + Clean(mode.GetDescription()) + "\n";
			array<ref CLW_ModeParam> params = {};
			mode.GetParams(params);
			foreach (CLW_ModeParam modeParam : params)
			{
				wire += WireParam("M:" + mode.GetId(), modeParam);
			}
		}

		return wire;
	}
}

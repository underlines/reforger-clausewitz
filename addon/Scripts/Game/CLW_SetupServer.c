// Server side of the war setup menu (spec 2a, phase P5). Server only.
// - tracks the setup owner (CLW_WarState owns the id; this class re-assigns it when the owner leaves),
// - sends the owner the setup data (wire text, see BuildWire) so the owner's client opens the menu, and the others a "waiting" hint,
// - validates the submit (sender == owner or admin, phase == SETUP, mode + values + factions) and calls CLW_WarState.StartWar,
// - announces the started war and closes the menus.
// The client side lives in CLW_SetupClient.c / CLW_SetupDialog.c, the RPC glue in CLW_SetupPlayerController.c.
class CLW_SetupServer
{
	static const string PARAM_FACTION_A = "factionA";
	static const string PARAM_FACTION_B = "factionB";
	static const int POLL_MS = 1500;

	protected static ref CLW_SetupServer s_Instance;

	protected CLW_WarState m_War;
	protected CLW_ScenarioConfig m_Scenario;
	protected int m_iOwner = -1;				// owner we last sent the menu to
	protected IEntity m_OwnerEntity;			// to detect a respawn (re-send)
	protected bool m_bAnnounced;
	protected string m_sBattleText;
	protected ref array<int> m_aWaitSent = {};

	// Called by CLW_WarState.Init (server only).
	static void Start(CLW_WarState war, CLW_ScenarioConfig scenario)
	{
		if (!Replication.IsServer() || !war || !scenario)
			return;

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
			return "setup is not available";

		return s_Instance.DoSubmit(playerId, battleJson, modeId, modeJson);
	}

	// Chat fallback: args = "start [modeId] [key=value ...]" or "setup". Returns a reply line for the sender.
	static string ChatCommand(int playerId, string args)
	{
		if (!s_Instance)
			return "setup is not available";

		return s_Instance.DoChat(playerId, args);
	}

	// Menu data as JSON: {"phase","default","setupOwner","modes":[...],"battle":[...]} (log / dashboard; the client menu uses BuildWire).
	static string GetSetupJson()
	{
		if (!s_Instance || !s_Instance.m_War)
			return "{}";

		return s_Instance.BuildSetupJson();
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

		IEntity entity = players.GetPlayerControlledEntity(owner);
		if (owner != m_iOwner)
		{
			m_iOwner = owner;
			m_OwnerEntity = entity;
			m_aWaitSent.Clear();
			SendOpen(owner);
		}
		else if (entity && entity != m_OwnerEntity)
		{
			m_OwnerEntity = entity;
			Print("CLW: setup owner respawned, re-sending the menu", LogLevel.NORMAL);
			SendOpen(owner);
		}
		else if (!entity)
		{
			m_OwnerEntity = null;
		}

		string ownerName = players.GetPlayerName(owner);
		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int id : ids)
		{
			if (id == owner || m_aWaitSent.Contains(id))
				continue;

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
			if (id <= 0 || !players.GetPlayerControlledEntity(id))
				continue;

			if (best < 0 || id < best)
				best = id;
		}

		return best;
	}

	protected void Announce()
	{
		m_bAnnounced = true;
		string text = "War started";
		CLW_WarMode mode = m_War.GetMode();
		if (mode)
			text = "War started: " + mode.GetName() + " (" + mode.GetId() + ") " + mode.ValuesText() + " | " + m_sBattleText;

		Print("CLW: setup announce: " + text, LogLevel.NORMAL);
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

	protected void SendOpen(int playerId)
	{
		string wire = BuildWire();
		Print(string.Format("CLW: setup send menu to player %1 (%2 bytes)", playerId, wire.Length()), LogLevel.NORMAL);
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
			SendResult(playerId, false, "the war has already started");
			return;
		}

		if (!s_Instance.IsAllowed(playerId))
		{
			SendResult(playerId, false, "only the setup owner may open the setup menu");
			return;
		}

		Print("CLW: setup menu requested by player " + playerId, LogLevel.NORMAL);
		s_Instance.SendOpen(playerId);
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
			Print("CLW: setup REJECTED: " + error, LogLevel.WARNING);
			return error;
		}

		string keyA;
		string keyB;
		ParseBattle(battleJson, keyA, keyB, error);
		if (modeId.IsEmpty())
			modeId = m_Scenario.m_sDefaultMode;

		// StartWar validates the sides again and re-keys the whole war (CLW_Sides.Apply) before the mode starts.
		if (!m_War.StartWar(modeId, modeJson, playerId, keyA, keyB))
		{
			Print("CLW: setup REJECTED: StartWar refused", LogLevel.WARNING);
			return "the war could not be started (see the server log)";
		}

		m_sBattleText = m_Scenario.GetFactionKey(0) + " vs " + m_Scenario.GetFactionKey(1);

		Print("CLW: setup ACCEPTED from player " + playerId + ", war is starting", LogLevel.NORMAL);
		return "";
	}

	// Everything that can reject a submit without side effects. "" = fine.
	protected string Check(int playerId, string battleJson, string modeId, string modeJson)
	{
		if (m_War.GetPhase() != CLW_EWarPhase.SETUP)
			return "the war has already started";

		if (!IsAllowed(playerId))
			return "only the setup owner may start the war";

		string keyA;
		string keyB;
		string error;
		if (!ParseBattle(battleJson, keyA, keyB, error))
			return error;

		if (modeId.IsEmpty())
			modeId = m_Scenario.m_sDefaultMode;

		CLW_WarMode mode = FindMode(modeId);
		if (!mode)
			return "unknown war mode '" + modeId + "'";

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
					error = "unknown battle parameter " + key;
					return false;
				}
			}
		}

		array<string> keys = {};
		array<string> names = {};
		CollectFactions(keys, names);
		if (!keys.Contains(keyA))
		{
			error = "unknown faction '" + keyA + "'";
			return false;
		}

		if (!keys.Contains(keyB))
		{
			error = "unknown faction '" + keyB + "'";
			return false;
		}

		if (keyA == keyB)
		{
			error = "the two sides must be different factions";
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
			return "usage: /war setup | /war start [mode] [key=value ...]";

		string sub = tokens[0];
		sub.ToLower();
		if (sub == "setup")
		{
			Resend(playerId);
			return "";
		}

		if (sub != "start")
			return "usage: /war setup | /war start [mode] [key=value ...]";

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
			return "war start accepted";

		return "war start rejected: " + error;
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
			list += key + " = " + names[i];
		}

		outParams.Insert(CLW_ModeParam.CreateChoice(PARAM_FACTION_A, "Side A", "First faction. Available: " + list, keys, m_Scenario.GetFactionKey(0)));
		outParams.Insert(CLW_ModeParam.CreateChoice(PARAM_FACTION_B, "Side B", "Second faction, must differ from side A.", keys, m_Scenario.GetFactionKey(1)));
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
		text.Replace("\n", " ");
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
		string wire = "H\t" + Clean(m_Scenario.m_sDefaultMode) + "\n";
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

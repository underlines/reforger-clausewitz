// HQ chat (M3 phase A, design section 6): "/hq <free text>" is a player talking to HQ over the radio. With the LLM sidecar online the
// message goes to its agent turn: it rides on the faction's situation POST as hq[] (pending messages), the sidecar answers in a later
// reply with hqActions [{msgId, id, action, args:[{name,value}]}] and hqReplies [{msgId, text, fallback}]. The game then answers the
// player with the reply text and runs each action through the action registry AS THE REQUESTING PLAYER (authority "player"), so the
// request grant rules apply exactly as for /request: the five request kinds use CLW_RequestService.Handle (same history, same answer),
// any other player entry of the registry runs through a generic registry call. Without the LLM (sidecar off / unreachable / engine rules)
// or when no answer arrives within TIMEOUT_SEC, the player gets the /request menu hint instead. Server only. Log lines "CLW: hq ...".

// Shapes of the sidecar reply (filled by SCR_JsonLoadContext, field names = JSON keys), used by CLW_OrdersReply.
class CLW_HqArgDto
{
	string name;
	string value;	// positions as "x,z"
}

class CLW_HqActionDto
{
	int msgId;
	string id;
	string action;
	ref array<ref CLW_HqArgDto> args = {};
}

class CLW_HqReplyDto
{
	int msgId;
	string text;
	bool fallback;	// true = no answer from the model: show the /request menu hint
	string setLanguage;	// protocol v4: the asker wants HQ in this language (ISO 639-1 code or "auto"); "" = unchanged
}

// Language helpers shared by /hq language, the HQ menu and the sidecar reply (setLanguage). Resolution itself is CLW_PlayerLang.
class CLW_HqLang
{
	// Codes the HQ menu offers (besides "auto"); any other ISO 639-1 code works through /hq language <code>.
	static void GetMenuCodes(notnull array<string> outCodes)
	{
		outCodes.Insert("en");
		outCodes.Insert("de");
		outCodes.Insert("fr");
		outCodes.Insert("es");
		outCodes.Insert("ru");
		outCodes.Insert("pl");
		outCodes.Insert("ja");
		outCodes.Insert("zh");
		outCodes.Insert("it");
		outCodes.Insert("pt");
	}

	static bool InMenu(string code)
	{
		array<string> codes = {};
		GetMenuCodes(codes);
		return codes.Contains(code);
	}

	// Param for a wire / widget: the language's stringtable name for "auto" and the codes of the menu, else the code itself.
	static string Label(string code)
	{
		if (code == "auto" || InMenu(code))
			return "#CLW-Lang_" + code;

		return code;
	}

	// The player's setting as a param: the stored code ("de"), or "auto (de)" with the language it resolves to now (a wire part).
	static string Current(int playerId)
	{
		string stored = CLW_PlayerLang.GetOverride(playerId);
		if (!stored.IsEmpty())
			return stored;

		return CLW_Loc.W("#CLW-Lang_AutoOf", CLW_PlayerLang.Resolve(playerId));
	}

	// Lower case, trimmed; "" when it is neither "auto" nor a valid code.
	static string Normalize(string raw)
	{
		string code = raw;
		code = code.Trim();
		code.ToLower();
		if (code == "auto" || CLW_PlayerLang.IsValidCode(code))
			return code;

		return "";
	}

	// Sets ("xx") or clears ("auto") the language override of ONE player; the code is validated here again (it may come from a
	// client or from the LLM). Returns false for an invalid code. Log lines "CLW: lang ...".
	static bool Apply(int playerId, string raw, string source)
	{
		string code = Normalize(raw);
		if (code.IsEmpty())
		{
			Print(string.Format("CLW: lang player=%1(%2) rejected code '%3' (source %4)", playerId, CLW_RequestService.PlayerName(playerId), raw, source), LogLevel.WARNING);
			return false;
		}

		if (code == "auto")
		{
			CLW_PlayerLang.ClearOverride(playerId);
		}
		else if (!CLW_PlayerLang.SetOverride(playerId, code))
		{
			Print(string.Format("CLW: lang player=%1(%2) SetOverride refused '%3' (source %4)", playerId, CLW_RequestService.PlayerName(playerId), code, source), LogLevel.WARNING);
			return false;
		}

		Print(string.Format("CLW: lang player=%1(%2) %3 -> now '%4' (source %5)", playerId, CLW_RequestService.PlayerName(playerId), code, CLW_PlayerLang.Resolve(playerId), source), LogLevel.NORMAL);
		return true;
	}

	// Chat answer to a language change (a wire, shown in the player's game language).
	static string SetAnswer(int playerId, string code)
	{
		if (code == "auto")
			return CLW_Loc.W("#CLW-Hq_LangAuto", Label(CLW_PlayerLang.Resolve(playerId)));

		return CLW_Loc.W("#CLW-Hq_LangSet", Label(code));
	}
}

// Free text (LLM reply, result text) as a wire param: a leading '#' would make it a key.
class CLW_HqText
{
	static string Param(string text)
	{
		if (text.Length() > 0 && text.Substring(0, 1) == "#")
			return " " + text;

		return text;
	}
}

class CLW_HqConfig
{
	static const float TIMEOUT_SEC = 90;			// a message without an answer gets the fallback hint (above CLW_RemoteCommander.TIMEOUT_SECONDS, one slow turn)
	static const int TICK_MS = 5000;
	static const int MAX_TEXT = 240;
	static const int MAX_PENDING = 12;
	static const float PLAYER_COOLDOWN_SEC = 4;		// between two /hq of one player
	static const int SITUATION_MAX = 6;				// pending messages sent per situation
	static const int MAX_ACTIONS_PER_MSG = 3;
}

class CLW_HqMessage
{
	int m_iId;
	int m_iPlayer;
	string m_sName;
	string m_sFaction;
	string m_sRole;
	string m_sSquad;
	vector m_vPos;
	string m_sText;
	string m_sReply;	// the radio reply, shown together with the action result in one hint
	float m_fTime;		// world time (s)
}

class CLW_HqChat
{
	protected static ref array<ref CLW_HqMessage> s_aPending = {};
	protected static ref map<int, float> s_mLastMessage = new map<int, float>();	// player -> world time of the last /hq
	protected static int s_iSeq;
	protected static bool s_bTicking;

	// ---- lifecycle (CLW_RequestService.OnGameStart / OnWarStart) ----

	static void Reset()
	{
		s_aPending.Clear();
		s_mLastMessage.Clear();
		CLW_HqHistory.Reset();	// war rematch / new war: no conversation memory carried over
		s_iSeq = 0;
		if (s_bTicking)
		{
			GetGame().GetCallqueue().Remove(Tick);
			s_bTicking = false;
		}
	}

	static void Start()
	{
		if (!Replication.IsServer() || s_bTicking)
			return;

		s_bTicking = true;
		GetGame().GetCallqueue().CallLater(Tick, CLW_HqConfig.TICK_MS, true);
	}

	// ---- entry point: chat "/hq <text>" (server) ----

	static void Chat(int playerId, string text)
	{
		if (!Replication.IsServer())
			return;

		string clean = text;
		clean = clean.Trim();
		if (clean.IsEmpty())
		{
			CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Hq_SayWhat"), true);
			return;
		}

		if (HandleLanguage(playerId, clean))
			return;

		if (clean.Length() > CLW_HqConfig.MAX_TEXT)
			clean = clean.Substring(0, CLW_HqConfig.MAX_TEXT);

		string faction = CLW_RequestService.FactionKeyOf(playerId);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (faction.IsEmpty() || !war || !war.IsRunning())
		{
			CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Hq_NoWar"), true);
			return;
		}

		float now = CLW_RequestService.Now();
		float last;
		if (s_mLastMessage.Find(playerId, last) && now - last < CLW_HqConfig.PLAYER_COOLDOWN_SEC)
		{
			CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Hq_Wait"), false);
			return;
		}

		foreach (CLW_HqMessage pending : s_aPending)
		{
			if (pending.m_iPlayer == playerId)
			{
				CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Hq_Busy"), false);
				return;
			}
		}

		s_mLastMessage.Set(playerId, now);
		string name = CLW_RequestService.PlayerName(playerId);
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(faction);
		if (!commander || !commander.IsOnline() || !CLW_Capabilities.Has("requests") || s_aPending.Count() >= CLW_HqConfig.MAX_PENDING)
		{
			Print(string.Format("CLW: hq %1 (%2): no LLM available, menu hint: '%3'", name, faction, clean), LogLevel.NORMAL);
			Fallback(playerId);
			return;
		}

		CLW_HqMessage msg = new CLW_HqMessage();
		s_iSeq++;
		msg.m_iId = s_iSeq;
		msg.m_iPlayer = playerId;
		msg.m_sName = name;
		msg.m_sFaction = faction;
		msg.m_sRole = CLW_RequestService.RoleOf(playerId);
		SCR_AIGroup group = CLW_RequestService.GroupOf(playerId);
		if (group)
			msg.m_sSquad = "group " + group.GetGroupID();
		CLW_RequestService.PlayerPos(playerId, msg.m_vPos);
		msg.m_sText = clean;
		msg.m_fTime = now;
		s_aPending.Insert(msg);
		Print(string.Format("CLW: hq #%1 player=%2(%3) faction=%4 role=%5 text='%6' -> sent to the sidecar with the next situation", msg.m_iId, playerId, name, faction, msg.m_sRole, clean), LogLevel.NORMAL);
		CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Hq_Received"), false);
	}

	// "/hq language", "/hq language <code>", "/hq language auto" (works without the LLM). True = handled.
	protected static bool HandleLanguage(int playerId, string text)
	{
		array<string> words = {};
		text.Split(" ", words, true);
		if (words.IsEmpty() || words.Count() > 2)
			return false;

		string first = words[0];
		first.ToLower();
		if (first != "language")
			return false;

		if (words.Count() == 1)
		{
			CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Hq_LangCurrent", CLW_HqLang.Current(playerId)), true);
			return true;
		}

		string code = CLW_HqLang.Normalize(words[1]);
		if (code.IsEmpty() || !CLW_HqLang.Apply(playerId, code, "chat"))
		{
			CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Hq_LangBad", CLW_HqText.Param(words[1])), true);
			return true;
		}

		CLW_RequestService.Answer(playerId, CLW_HqLang.SetAnswer(playerId, code), true);
		return true;
	}

	// No LLM answer: tell the player to use the fixed requests.
	static void Fallback(int playerId)
	{
		CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Hq_NoOperator"), true);
		CLW_RequestService.ShowMenu(playerId);
	}

	// ---- situation (protocol v3 "hq") ----

	// JSON array of the faction's pending messages, "" when none: [{"id","player","role","squad","grid","pos":[x,z],"lang","text","ageSec"}] (protocol v4: lang = CLW_PlayerLang.Resolve of the asker).
	static string GetJson(string factionKey)
	{
		string text = "";
		int written;
		float now = CLW_RequestService.Now();
		foreach (CLW_HqMessage msg : s_aPending)
		{
			if (msg.m_sFaction != factionKey || written >= CLW_HqConfig.SITUATION_MAX)
				continue;

			if (written > 0)
				text += ",";
			text += "{\"id\":" + msg.m_iId + ",\"player\":\"" + CLW_Situation.Escape(msg.m_sName) + "\",\"role\":\"" + msg.m_sRole + "\",\"squad\":\"" + CLW_Situation.Escape(msg.m_sSquad) + "\"";
			text += ",\"grid\":\"" + CLW_Places.Grid(msg.m_vPos) + "\",\"pos\":[" + Math.Round(msg.m_vPos[0]) + "," + Math.Round(msg.m_vPos[2]) + "]";
			text += ",\"lang\":\"" + CLW_Situation.Escape(CLW_PlayerLang.Resolve(msg.m_iPlayer)) + "\"";
			text += ",\"text\":\"" + CLW_Situation.Escape(msg.m_sText) + "\",\"ageSec\":" + Math.Round(Math.Max(0, now - msg.m_fTime));
			string history = CLW_HqHistory.GetJson(msg.m_iPlayer);	// the asker's own last exchanges, oldest first (sidecar: "Recent conversation")
			if (!history.IsEmpty())
				text += ",\"history\":" + history;
			text += "}";
			written++;
		}

		if (written == 0)
			return "";

		return "[" + text + "]";
	}

	// ---- sidecar reply ----

	protected static CLW_HqMessage Find(int id, string factionKey)
	{
		foreach (CLW_HqMessage msg : s_aPending)
		{
			if (msg.m_iId == id && msg.m_sFaction == factionKey)
				return msg;
		}

		return null;
	}

	protected static void Remove(CLW_HqMessage msg)
	{
		int at = s_aPending.Find(msg);
		if (at >= 0)
			s_aPending.Remove(at);
	}

	// hqReplies / hqActions of one reply for this faction. Per message: the reply text first, then the actions' results (each from the
	// registry as the requesting player). Replies with fallback:true give the menu hint. Unknown / late message ids are ignored.
	static void Apply(string factionKey, array<ref CLW_HqActionDto> actions, array<ref CLW_HqReplyDto> replies)
	{
		bool anyActions = actions && !actions.IsEmpty();
		bool anyReplies = replies && !replies.IsEmpty();
		if (!anyActions && !anyReplies)
			return;

		if (anyReplies)
		{
			foreach (CLW_HqReplyDto reply : replies)
			{
				if (!reply)
					continue;

				CLW_HqMessage msg = Find(reply.msgId, factionKey);
				if (!msg)
				{
					Print(string.Format("CLW: hq reply for message %1 (%2) ignored: unknown or already answered", reply.msgId, factionKey), LogLevel.NORMAL);
					continue;
				}

				Remove(msg);
				if (reply.fallback)
				{
					Print(string.Format("CLW: hq #%1 %2: sidecar had no answer, menu hint", msg.m_iId, msg.m_sName), LogLevel.NORMAL);
					Fallback(msg.m_iPlayer);
					continue;
				}

				// protocol v4: the asker asked (in any words) to be answered in some language; it concerns this player only
				if (!reply.setLanguage.IsEmpty())
					CLW_HqLang.Apply(msg.m_iPlayer, reply.setLanguage, "llm");

				string line = reply.text;
				line = line.Trim();
				if (line.Length() > 300)
					line = line.Substring(0, 300);
				if (line.IsEmpty())
					line = CLW_Loc.W("#CLW-Hq_Roger");	// a wire: the client renders it
				else
					line = CLW_HqText.Param(line);	// LLM text, already in the player's language, shown as it is

				Print(string.Format("CLW: hq #%1 %2 reply: '%3'", msg.m_iId, msg.m_sName, line), LogLevel.NORMAL);
				// one hint with reply + action result (a second hint would replace the first at once; the reply alone was a
				// short chat line the owner did not see, DS e2e 2026-10-12)
				msg.m_sReply = line;
				CLW_HqHistory.Record(msg.m_iPlayer, msg.m_sText, reply.text);	// LLM reply only; fallback / rules answers are not recorded
				if (!HasActionsFor(msg, actions))
					CLW_RequestService.Answer(msg.m_iPlayer, CLW_Loc.W("#CLW-Req_Granted", line), true);
				RunActions(msg, actions);
			}
		}

		// actions whose message got no reply entry: still run them once (the message is then answered)
		if (anyActions)
		{
			foreach (CLW_HqActionDto stray : actions)
			{
				if (!stray)
					continue;

				CLW_HqMessage strayMsg = Find(stray.msgId, factionKey);
				if (!strayMsg)
					continue;

				Remove(strayMsg);
				RunActions(strayMsg, actions);
			}
		}
	}

	protected static void RunActions(CLW_HqMessage msg, array<ref CLW_HqActionDto> actions)
	{
		if (!actions)
			return;

		int count;
		foreach (CLW_HqActionDto dto : actions)
		{
			if (!dto || dto.msgId != msg.m_iId)
				continue;

			count++;
			if (count > CLW_HqConfig.MAX_ACTIONS_PER_MSG)
			{
				Print(string.Format("CLW: hq #%1: action %2 ignored (max %3 per message)", msg.m_iId, dto.action, CLW_HqConfig.MAX_ACTIONS_PER_MSG), LogLevel.WARNING);
				continue;
			}

			RunAction(msg, dto);
		}
	}

	// The registry action name of one of the five request kinds -> its kind ("" = not a /request kind).
	protected static string KindOfAction(string action)
	{
		if (action == CLW_RequestKind.ActionOf(CLW_RequestKind.VEHICLE))
			return CLW_RequestKind.VEHICLE;
		if (action == CLW_RequestKind.ActionOf(CLW_RequestKind.SQUAD))
			return CLW_RequestKind.SQUAD;
		if (action == CLW_RequestKind.ActionOf(CLW_RequestKind.RESUPPLY))
			return CLW_RequestKind.RESUPPLY;
		if (action == CLW_RequestKind.ActionOf(CLW_RequestKind.REINFORCE))
			return CLW_RequestKind.REINFORCE;
		if (action == CLW_RequestKind.ActionOf(CLW_RequestKind.RECON))
			return CLW_RequestKind.RECON;

		return "";
	}

	protected static string ArgValue(CLW_HqActionDto dto, string name)
	{
		if (!dto.args)
			return "";

		foreach (CLW_HqArgDto arg : dto.args)
		{
			if (arg && arg.name == name)
				return arg.value;
		}

		return "";
	}

	protected static void RunAction(CLW_HqMessage msg, CLW_HqActionDto dto)
	{
		string kind = KindOfAction(dto.action);
		if (!kind.IsEmpty())
		{
			// same code path as /request: executes through the registry, records the request, answers the player (hint + chat)
			string arg = ArgValue(dto, "where");
			if (arg.IsEmpty())
				arg = ArgValue(dto, "type");

			Print(string.Format("CLW: hq #%1 %2 action %3 -> /request %4 '%5'", msg.m_iId, msg.m_sName, dto.id, kind, arg), LogLevel.NORMAL);
			CLW_RequestService.Handle(msg.m_iPlayer, kind, arg);
			return;
		}

		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		CLW_ActionEntry entry = registry.Find(dto.action);
		string answer;
		if (!entry || entry.m_sAuthority != CLW_ActionEntry.AUTH_PLAYER)
		{
			answer = CLW_Loc.W("#CLW-Hq_NotAllowed", dto.action);
			Print(string.Format("CLW: hq #%1 %2 action %3 %4 rejected: unknown or not a player action", msg.m_iId, msg.m_sName, dto.id, dto.action), LogLevel.WARNING);
		}
		else
		{
			CLW_ActionCall call = new CLW_ActionCall();
			call.m_sAction = dto.action;
			call.m_sFaction = msg.m_sFaction;
			call.m_sAuthority = CLW_ActionEntry.AUTH_PLAYER;
			call.m_sSource = "player";
			call.m_iPlayerId = msg.m_iPlayer;
			call.m_sReason = "hq chat by " + msg.m_sName;
			CLW_RemoteCommander commander = CLW_GameMode.GetCommander(msg.m_sFaction);
			if (commander)
				call.m_Rules = commander.GetRules();

			foreach (CLW_HqArgDto arg2 : dto.args)
			{
				if (arg2)
					SetArg(call, entry, arg2);
			}

			string result;
			bool granted = registry.Run(call, result);
			if (granted)
				answer = CLW_Loc.W("#CLW-Req_Granted", call.PlayerWire(result));
			else
				answer = CLW_Loc.W("#CLW-Req_Refused", dto.action, call.PlayerWire(result));

			Print(string.Format("CLW: hq #%1 %2 action %3 %4 -> %5: %6", msg.m_iId, msg.m_sName, dto.id, dto.action, granted, result), LogLevel.NORMAL);
		}

		if (!msg.m_sReply.IsEmpty())
			answer = CLW_Loc.Lines(CLW_Loc.W("#CLW-Req_Granted", msg.m_sReply), answer);

		CLW_RequestService.Answer(msg.m_iPlayer, answer, true);
	}

	protected static bool HasActionsFor(CLW_HqMessage msg, array<ref CLW_HqActionDto> actions)
	{
		if (!actions)
			return false;

		foreach (CLW_HqActionDto dto : actions)
		{
			if (dto && dto.msgId == msg.m_iId)
				return true;
		}

		return false;
	}

	protected static void SetArg(CLW_ActionCall call, CLW_ActionEntry entry, CLW_HqArgDto arg)
	{
		foreach (CLW_ActionParam param : entry.m_aParams)
		{
			if (!param || param.m_sName != arg.name)
				continue;

			if (param.m_sType == CLW_ActionParam.TYPE_POSITION)
			{
				array<string> parts = {};
				arg.value.Split(",", parts, true);
				if (parts.Count() >= 2)
					call.SetPos(param.m_sName, parts[0].ToFloat(), parts[1].ToFloat());
			}
			else
			{
				call.SetString(param.m_sName, arg.value);
			}

			return;
		}
	}

	// ---- timeouts ----

	protected static void Tick()
	{
		CLW_HqHistory.Prune();
		float now = CLW_RequestService.Now();
		for (int i = s_aPending.Count() - 1; i >= 0; i--)
		{
			CLW_HqMessage msg = s_aPending[i];
			if (now - msg.m_fTime < CLW_HqConfig.TIMEOUT_SEC)
				continue;

			s_aPending.Remove(i);
			Print(string.Format("CLW: hq #%1 %2: no answer after %3 s, menu hint", msg.m_iId, msg.m_sName, Math.Round(CLW_HqConfig.TIMEOUT_SEC)), LogLevel.WARNING);
			Fallback(msg.m_iPlayer);
		}
	}
}

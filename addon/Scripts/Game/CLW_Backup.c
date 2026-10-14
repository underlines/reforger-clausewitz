// Help call "backup" (HQ comms WP1, design notes/hq-comms-env-traits-design.md section /backup): a player of ANY role (soldier or leader)
// calls for help. The executor CLW_Action_RequestBackup (CLW_ActionsComms.c, registry action REQUEST_BACKUP) orders the nearest free AI
// group to the player's position (DEFEND, same selection as REQUEST_REINFORCE), records the call here, wakes the faction's HQ with a
// war event and answers the player. This class holds the record (kept HELP_KEEP_SEC), the per-player cooldown, the situation JSON
// (helpCalls) and the chat entry /backup [note]. Server only. Log prefix "CLW: backup".
class CLW_BackupConfig
{
	static const float COOLDOWN_SEC = 120;			// per player, counted from the last help call
	static const float KEEP_SEC = 600;				// a help call stays in the record / situation this long
	static const int JSON_MAX = 5;					// open calls sent in the situation (newest first)
	static const int NOTE_MAX = 120;				// characters of the note kept
	static const int RECORD_MAX = 24;				// calls kept at most
}

class CLW_HelpCall
{
	int m_iId;
	int m_iPlayer;
	string m_sName;
	string m_sFaction;
	string m_sSquad;		// the caller's own squad (display name), "" when none
	vector m_vPos;
	string m_sNote;
	string m_sAnsweredBy;	// id of the AI group sent, "" = none was free
	float m_fTime;			// world time (s)
}

class CLW_Backup
{
	protected static ref array<ref CLW_HelpCall> s_aCalls = {};
	protected static ref map<int, float> s_mLastCall = new map<int, float>();	// player -> world time of the last help call
	protected static int s_iSeq;

	// War start / rematch: forget calls and cooldowns (statics survive a Play restart).
	static void Reset()
	{
		s_aCalls.Clear();
		s_mLastCall.Clear();
		s_iSeq = 0;
	}

	// Seconds until the player may call for backup again (0 = now).
	static float CooldownLeft(int playerId)
	{
		float last;
		if (!s_mLastCall.Find(playerId, last))
			return 0;

		float left = last + CLW_BackupConfig.COOLDOWN_SEC - CLW_RequestService.Now();
		if (left < 0)
			return 0;

		return left;
	}

	// Free text of the player: no line breaks / quotes noise, limited length.
	static string CleanNote(string note)
	{
		string text = note;
		text.Replace("\t", " ");
		text.Replace("\n", " ");
		text.Replace("\r", " ");
		text = text.Trim();
		if (text.Length() > CLW_BackupConfig.NOTE_MAX)
			text = text.Substring(0, CLW_BackupConfig.NOTE_MAX);

		return text;
	}

	// Records an accepted help call, starts the cooldown and pushes the war event so the HQ commander wakes early.
	// squadId = the AI group that was ordered ("" = none free). Returns the call id.
	static int Record(string faction, int playerId, string squadId, vector pos, string note)
	{
		CLW_HelpCall call = new CLW_HelpCall();
		s_iSeq++;
		call.m_iId = s_iSeq;
		call.m_iPlayer = playerId;
		call.m_sName = CLW_RequestService.PlayerName(playerId);
		call.m_sFaction = faction;
		call.m_vPos = pos;
		call.m_sNote = CleanNote(note);
		call.m_sAnsweredBy = squadId;
		call.m_fTime = CLW_RequestService.Now();
		SCR_AIGroup own = CLW_RequestService.GroupOf(playerId);
		if (own)
			call.m_sSquad = CLW_SquadGroups.DisplayName(own);

		s_aCalls.Insert(call);
		if (s_aCalls.Count() > CLW_BackupConfig.RECORD_MAX)
			s_aCalls.RemoveOrdered(0);

		s_mLastCall.Set(playerId, call.m_fTime);

		string text = "HELP CALL: " + call.m_sName + " at " + CLW_Places.Grid(pos) + " needs backup";
		if (!call.m_sNote.IsEmpty())
			text += ": " + call.m_sNote;
		if (!squadId.IsEmpty())
			text += " (" + squadId + " sent)";
		else
			text += " (no free group was available)";

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.PushEvent(faction, text);

		Print(string.Format("CLW: backup call #%1 player=%2(%3) faction=%4 grid=%5 answeredBy='%6' note='%7'", call.m_iId, playerId, call.m_sName, faction, CLW_Places.Grid(pos), squadId, call.m_sNote), LogLevel.NORMAL);
		return call.m_iId;
	}

	// Situation key helpCalls: "" when none, else a JSON array of the faction's open calls, newest first, max JSON_MAX:
	// [{"id":1,"player":"..","squad":"..","grid":"025-285","pos":[x,z],"note":"..","ageSec":40,"answeredBy":".."}]
	static string GetJson(string faction)
	{
		string text = "";
		int written;
		float now = CLW_RequestService.Now();
		for (int i = s_aCalls.Count() - 1; i >= 0 && written < CLW_BackupConfig.JSON_MAX; i--)
		{
			CLW_HelpCall call = s_aCalls[i];
			if (call.m_sFaction != faction || now - call.m_fTime > CLW_BackupConfig.KEEP_SEC)
				continue;

			if (written > 0)
				text += ",";
			text += "{\"id\":" + call.m_iId.ToString() + ",\"player\":\"" + CLW_Situation.Escape(call.m_sName) + "\",\"squad\":\"" + CLW_Situation.Escape(call.m_sSquad) + "\"";
			text += ",\"grid\":\"" + CLW_Places.Grid(call.m_vPos) + "\",\"pos\":[" + Math.Round(call.m_vPos[0]) + "," + Math.Round(call.m_vPos[2]) + "]";
			text += ",\"note\":\"" + CLW_Situation.Escape(call.m_sNote) + "\",\"ageSec\":" + Math.Round(Math.Max(0, now - call.m_fTime));
			text += ",\"answeredBy\":\"" + CLW_Situation.Escape(call.m_sAnsweredBy) + "\"}";
			written++;
		}

		if (written == 0)
			return "";

		return "[" + text + "]";
	}

	// ---- chat /backup [note] and HQ menu / radial "backup" ----

	static string Chat(int playerId, string note)
	{
		string answerWire;
		return ChatWire(playerId, note, answerWire);
	}

	// Runs REQUEST_BACKUP through the action registry (same path as an /hq action), answers the player (chat + hint).
	// answerWire = the answer the player got (a CLW_Loc wire), e.g. for the HQ menu status line. Returns the English answer.
	static string ChatWire(int playerId, string note, out string answerWire)
	{
		answerWire = "";
		if (!Replication.IsServer() || playerId <= 0)
			return "";

		string faction = CLW_RequestService.FactionKeyOf(playerId);
		string reply;
		string replyWire;
		bool granted;
		if (faction.IsEmpty())
		{
			reply = "you are not on a side";
			replyWire = CLW_Loc.W("#CLW-Req_NoSide");
		}
		else
		{
			CLW_ActionCall call = new CLW_ActionCall();
			call.m_sAction = "REQUEST_BACKUP";
			call.m_sFaction = faction;
			call.m_sAuthority = CLW_ActionEntry.AUTH_PLAYER;
			call.m_sSource = "player";
			call.m_iPlayerId = playerId;
			call.m_sReason = "help call by " + CLW_RequestService.PlayerName(playerId);
			call.SetString("note", note);
			CLW_RemoteCommander commander = CLW_GameMode.GetCommander(faction);
			if (commander)
				call.m_Rules = commander.GetRules();

			string result;
			granted = CLW_ActionRegistry.Get().Run(call, result);
			reply = result;
			replyWire = call.PlayerWire(result);
		}

		string verdict = "REFUSED";
		if (granted)
			verdict = "ACCEPTED";

		Print(string.Format("CLW: backup chat player=%1(%2) note='%3' -> %4: %5", playerId, CLW_RequestService.PlayerName(playerId), note, verdict, reply), LogLevel.NORMAL);
		if (granted)
			answerWire = CLW_Loc.W("#CLW-Req_Granted", replyWire);
		else
			answerWire = CLW_Loc.W("#CLW-Req_Refused", "#CLW-Bkp_Kind", replyWire);

		CLW_RequestService.Answer(playerId, answerWire, true);
		return reply;
	}
}

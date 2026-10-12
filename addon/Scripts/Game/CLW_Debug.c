// Debug text. Say() runs on the server: logs the line (Print), keeps it in a small ring buffer that the sidecar POST carries to the
// web dashboard (CLW_RemoteCommander), and shows it in the players' chat ONLY when the server owner switched it on
// ($profile:Clausewitz/overrides.json  {"debugChat":true}, default off, see dev/notes/overrides.md).
// Announce() / Show() are real player-facing UX (setup menu messages, /war replies, war start/over, popup fallback): never gated.
// Chat delivery is an owner RPC on SCR_PlayerController, see CLW_DebugPlayerController.c.

// Shape of the optional flag in overrides.json (other keys of the file are ignored; absent = false).
class CLW_DebugFlags
{
	bool debugChat;
}

class CLW_Debug
{
	static const int RING_MAX = 50;		// lines kept for the sidecar
	static const int LINE_MAX = 160;	// chars per line sent to the sidecar

	protected static bool s_bChatRead;
	protected static bool s_bChat;
	protected static int s_iSeq;
	protected static ref array<int> s_aIds = {};
	protected static ref array<float> s_aTimes = {};
	protected static ref array<string> s_aMsgs = {};

	// Server owner flag debugChat (read once, at the first debug line).
	static bool IsChatEnabled()
	{
		if (s_bChatRead)
			return s_bChat;

		s_bChatRead = true;
		if (!FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
			return false;

		string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
		if (text.IsEmpty())
			return false;

		SCR_JsonLoadContext ctx = new SCR_JsonLoadContext();
		CLW_DebugFlags flags = new CLW_DebugFlags();
		if (ctx.ImportFromString(text) && ctx.ReadValue("", flags))	// [verify] unknown keys ("entries") are skipped
			s_bChat = flags.debugChat;

		Print("CLW: debug chat " + s_bChat.ToString() + " (overrides.json debugChat)", LogLevel.NORMAL);
		return s_bChat;
	}

	// Debug / diagnostic line: log + sidecar ring buffer; chat only with debugChat.
	static void Say(string msg)
	{
		Print("CLW: " + msg, LogLevel.NORMAL);
		if (!Replication.IsServer())
			return;

		Record(msg);
		if (IsChatEnabled())
			Broadcast(msg);
	}

	// Player-facing server announcement: log + chat line for every player, always.
	static void Announce(string msg)
	{
		Print("CLW: " + msg, LogLevel.NORMAL);
		if (!Replication.IsServer())
			return;

		Broadcast(msg);
	}

	protected static void Broadcast(string msg)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int id : ids)
		{
			if (id == SCR_PlayerController.GetLocalPlayerId())
			{
				CLW_Debug.Show(msg);	// listen server host: no RPC needed
				continue;
			}

			SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(id));
			if (controller)
				controller.CLW_RpcShow(msg);
		}
	}

	protected static void Record(string msg)
	{
		string line = msg;
		if (line.Length() > LINE_MAX)
			line = line.Substring(0, LINE_MAX - 3) + "...";

		float now = 0;
		if (GetGame() && GetGame().GetWorld())
			now = GetGame().GetWorld().GetWorldTime() * 0.001;

		s_iSeq++;
		s_aIds.Insert(s_iSeq);
		s_aTimes.Insert(now);
		s_aMsgs.Insert(line);
		if (s_aIds.Count() > RING_MAX)
		{
			s_aIds.RemoveOrdered(0);
			s_aTimes.RemoveOrdered(0);
			s_aMsgs.RemoveOrdered(0);
		}
	}

	// JSON array [{"id":n,"t":sec,"msg":"..."}] of the lines newer than afterSeq (returned through lastSeq); "" when none.
	// Each faction's RemoteCommander keeps its own cursor, so every line goes out with both factions' posts (the sidecar dedupes).
	static string GetJsonSince(int afterSeq, out int lastSeq)
	{
		lastSeq = afterSeq;
		string json = "";
		for (int i = 0; i < s_aIds.Count(); i++)
		{
			if (s_aIds[i] <= afterSeq)
				continue;

			if (!json.IsEmpty())
				json += ",";
			json += "{\"id\":" + s_aIds[i].ToString() + ",\"t\":" + Math.Round(s_aTimes[i]).ToString() + ",\"msg\":\"" + CLW_Situation.Escape(s_aMsgs[i]) + "\"}";
			lastSeq = s_aIds[i];
		}

		if (json.IsEmpty())
			return "";

		return "[" + json + "]";
	}

	// Local display (always): player-facing text; also called by the client RPC.
	static void Show(string msg)
	{
		SCR_ChatPanelManager chat = SCR_ChatPanelManager.GetInstance();
		if (chat)
			chat.ShowHelpMessage("CLW: " + msg);
	}
}

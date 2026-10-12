// Client display of the faction SITREP (design: dev/notes/sitrep-design.md). Uses vanilla UI only:
//  1. The vanilla map JOURNAL (the "!" tool on the map and on the deploy screen map, SCR_MapJournalUI). The game mode carries a
//     SCR_RespawnBriefingComponent whose journal config (Configs/CLW/Journal/SitrepJournal.conf) has one entry per SITREP section;
//     this class rewrites the entry texts locally on the client (RewriteEntryMain), so the deploy screen and the in-game map show the
//     current SITREP. The text is local to the client and only ever holds the player's own faction (the server sends only that).
//  2. The vanilla HINT (SCR_HintManagerComponent.ShowCustomHint): a short summary on spawn / respawn and on big updates. The vanilla
//     "H" key (action HintToggle) shows the latest hint again; the chat command /sitrep shows the SITREP hint at any time.
// Every step logs "CLW: sitrep ...".
class CLW_SitrepClient
{
	static const float HINT_SECONDS = 28;
	static const float HINT_MIN_GAP_SEC = 240;		// automatic update hints at most this often
	static const int HINT_MAX_CHARS = 800;	// owner 2026-10-12: 330 showed only ~7 short lines

	protected static ref CLW_Sitrep s_Last;
	protected static string s_sFaction;
	protected static int s_iVersion = -1;
	protected static bool s_bHintPending = true;	// show the hint as soon as the player has a character
	protected static float s_fLastHintAt = -10000;
	protected static string s_sHintedMission;
	protected static bool s_bJournalFailedLogged;

	// Wire text from the server (CLW_Sitrep.ToWire).
	static void Receive(string wire)
	{
		string factionKey;
		int version;
		CLW_Sitrep sitrep = CLW_Sitrep.FromWire(wire, factionKey, version);
		if (!sitrep)
		{
			Print("CLW: sitrep client got an unreadable payload: " + wire.Substring(0, Math.Min(wire.Length(), 120)), LogLevel.WARNING);
			return;
		}

		s_Last = sitrep;
		s_sFaction = factionKey;
		s_iVersion = version;
		Print(string.Format("CLW: sitrep client received %1 v%2 source=%3 T+%4", factionKey, version, sitrep.m_sSource, CLW_Sitrep.FormatClock(sitrep.m_fWarTime)), LogLevel.NORMAL);
		UpdateJournal(factionKey, sitrep);

		float now = Now();
		if (!HasCharacter())
			return;	// deploy screen: the journal is updated, the hint follows with the spawn

		string mission = sitrep.GetBody(2);
		if (s_bHintPending)
			ShowHint(false);
		else if (mission != s_sHintedMission && now - s_fLastHintAt >= HINT_MIN_GAP_SEC)
			ShowHint(false);
	}

	// The local player got a character (spawn / respawn): show the current SITREP hint (immediately or when it arrives).
	static void OnLocalSpawn()
	{
		s_bHintPending = true;
		if (s_Last)
			ShowHint(false);
		else
			Print("CLW: sitrep client: player spawned, SITREP not received yet (hint follows)", LogLevel.NORMAL);
	}

	// Hint with the short form. force = the player asked (also shown again when nothing changed).
	static void ShowHint(bool force)
	{
		if (!s_Last)
		{
			Print("CLW: sitrep client: no SITREP yet", LogLevel.NORMAL);
			if (force)
				CLW_Debug.Show("no SITREP received yet, HQ staff is still working on it");
			return;
		}

		string title = "SITREP T+" + CLW_Sitrep.FormatClock(s_Last.m_fWarTime);
		if (s_Last.m_sSource == CLW_Sitrep.SOURCE_LLM)
			title += " (HQ)";
		string text = "";
		text += Short(s_Last.GetBody(2), 320);
		string enemy = Short(s_Last.GetBody(1), 240);
		if (!enemy.IsEmpty())
			text += "\nEnemy: " + enemy;
		string situation = Short(s_Last.GetBody(0), 220);
		if (!situation.IsEmpty())
			text += "\n" + situation;
		text += "\nFull report: map > journal. H = show again.";
		text = CLW_Sitrep.DisplaySafe(text);
		if (text.Length() > HINT_MAX_CHARS)
			text = text.Substring(0, HINT_MAX_CHARS - 3) + "...";

		bool shown = SCR_HintManagerComponent.ShowCustomHint(text, title, HINT_SECONDS, false);
		Print(string.Format("CLW: sitrep client hint shown=%1 force=%2 (%3 chars)", shown, force, text.Length()), LogLevel.NORMAL);
		if (!shown)
		{
			// hints disabled in the settings or no hint manager: chat fallback so the player still sees it
			CLW_Debug.Show(title + ": " + text);
		}

		s_bHintPending = false;
		s_fLastHintAt = Now();
		s_sHintedMission = s_Last.GetBody(2);
	}

	// First line of a body, line breaks flattened, limited.
	protected static string Short(string body, int maxChars)
	{
		string text = body;
		text.Replace("<br/>", " / ");
		text.Replace("\n", " / ");
		if (text.Length() > maxChars)
			text = text.Substring(0, maxChars - 3) + "...";

		return text;
	}

	// Rewrites the journal entries of the briefing component (local only).
	protected static void UpdateJournal(string factionKey, CLW_Sitrep sitrep)
	{
		SCR_RespawnBriefingComponent briefing = SCR_RespawnBriefingComponent.GetInstance();
		if (!briefing)
		{
			if (!s_bJournalFailedLogged)
			{
				s_bJournalFailedLogged = true;
				Print("CLW: sitrep journal unavailable: the game mode has no SCR_RespawnBriefingComponent (SITREP shows as hint only)", LogLevel.WARNING);
			}

			return;
		}

		array<string> noParams = {};
		for (int i = 0; i < CLW_SitrepTemplate.SECTION_COUNT; i++)
		{
			string body = sitrep.GetBody(i);
			if (body.IsEmpty())
				body = "-";

			body.Replace("\n", "<br/>");
			if (i == 0 && !sitrep.m_sHeadline.IsEmpty())
				body = sitrep.m_sHeadline + "<br/>" + body;

			briefing.RewriteEntryMain(factionKey, i, CLW_Sitrep.DisplaySafe(body), noParams);
		}
	}

	protected static bool HasCharacter()
	{
		PlayerController controller = GetGame().GetPlayerController();
		if (!controller)
			return false;

		return controller.GetControlledEntity() != null;
	}

	protected static float Now()
	{
		return GetGame().GetWorld().GetWorldTime() * 0.001;
	}

	// ---- chat command /sitrep ----

	static void RegisterChatCommand()
	{
		SCR_ChatPanelManager chat = SCR_ChatPanelManager.GetInstance();
		if (!chat)
			return;

		ChatCommandInvoker invoker = chat.GetCommandInvoker("sitrep");
		if (!invoker)
			return;

		invoker.Insert(OnChatCommand);
		SCR_PlayerController.CLW_MarkSitrepChatRegistered();
		Print("CLW: sitrep chat command /sitrep registered", LogLevel.NORMAL);
	}

	static void OnChatCommand(SCR_ChatPanel panel, string data)
	{
		Print("CLW: sitrep chat /sitrep", LogLevel.NORMAL);
		ShowHint(true);
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (controller)
			controller.CLW_SitrepRequest();	// also refresh from the server
	}
}

// Ticket counter HUD, server half: builds a compact status line and broadcasts it to every client
// (CLW_GameMode.CLW_BroadcastWarStatus -> CLW_WarHud on the clients). Sent every STATUS_PERIOD_MS and, debounced,
// whenever tickets / point owners / the phase change. Server only. Nothing here names a faction: keys and identities come from the scenario.
//
// Wire format: phase|timeLeftSec|winnerKey|side0|side1   (phase 0 SETUP, 1 RUNNING, 2 ENDED; timeLeftSec -1 = no limit)
//   side = key,identity,tickets,maxTickets,heldCount,heldValue,supplies   (tickets -1 = the mode does not use tickets;
//          supplies = the side's supply stock from CLW_WarState.GetSupplies, -1 during SETUP; the HUD shows only the local player's side)
class CLW_WarStatus
{
	static const int STATUS_PERIOD_MS = 2000;
	static const int STATUS_DEBOUNCE_MS = 300;

	protected CLW_WarState m_War;		// weak: owner
	protected ref map<string, int> m_mMaxTickets = new map<string, int>();
	protected bool m_bPending;

	void Init(CLW_WarState war)
	{
		m_War = war;
		GetGame().GetCallqueue().CallLater(Push, STATUS_PERIOD_MS, true);
	}

	// Something visible changed: push soon (several changes within the debounce window share one message).
	void Request()
	{
		if (m_bPending)
			return;

		m_bPending = true;
		GetGame().GetCallqueue().CallLater(PushPending, STATUS_DEBOUNCE_MS);
	}

	protected void PushPending()
	{
		m_bPending = false;
		Push();
	}

	void Push()
	{
		if (!m_War)
			return;

		SCR_BaseGameMode gameMode = SCR_BaseGameMode.Cast(GetGame().GetGameMode());	// CLW_GameMode or a taken-over one (CLW_GameModeHooks.c)
		if (gameMode)
			gameMode.CLW_BroadcastWarStatus(Build());
	}

	string Build()
	{
		int phase = 0;
		if (m_War.IsRunning())
			phase = 1;
		else if (m_War.IsEnded())
			phase = 2;

		if (phase == 0)
			m_mMaxTickets.Clear();

		bool usesTickets = true;
		CLW_WarMode mode = m_War.GetMode();
		if (mode)
			usesTickets = mode.UsesTickets();

		array<string> keys = {};
		m_War.GetFactionKeys(keys);
		string text = phase.ToString() + "|" + m_War.GetTimeLeftSec().ToString() + "|" + m_War.GetWinner();
		for (int i = 0; i < 2; i++)
		{
			string side = "|";
			if (i < keys.Count())
				side += BuildSide(keys[i], phase, usesTickets);
			text += side;
		}

		return text;
	}

	protected string BuildSide(string key, int phase, bool usesTickets)
	{
		int tickets = -1;
		int maxTickets = -1;
		if (usesTickets && phase != 0)
		{
			tickets = m_War.GetTickets(key);
			int known;
			m_mMaxTickets.Find(key, known);
			maxTickets = Math.Max(known, tickets);
			m_mMaxTickets.Set(key, maxTickets);
		}

		string identity = CLW_ScenarioConfig.GetIdentityText(key);
		string line = key + "," + identity + "," + tickets.ToString() + "," + maxTickets.ToString();
		line += "," + m_War.GetHeldCount(key).ToString() + "," + m_War.GetHeldValue(key).ToString();
		int supplies = -1;
		if (phase != 0)
			supplies = m_War.GetSupplies(key);
		line += "," + supplies.ToString();
		return line;
	}
}

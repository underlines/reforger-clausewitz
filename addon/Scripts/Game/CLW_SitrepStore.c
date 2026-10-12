// Server-side holder of the current SITREP per faction and the delivery to the players of THAT faction only (no enemy leak:
// every faction has its own entry, and a player only receives the entry of the faction he belongs to).
//
// Two sources per faction: the rules SITREP (CLW_SitrepBuilder, regenerated every RULES_INTERVAL_SEC by the faction's commander
// think, works without a sidecar) and the LLM SITREP (carried by the commander reply, see CLW_RemoteCommander). The LLM one replaces
// the rules one while it is at most LLM_FRESH_SEC old; afterwards the rules version is current again.
// Players get a copy when the content changes or when they appear (join, faction change, respawn handled by the client).
// Log lines are prefixed "CLW: sitrep".
class CLW_SitrepStore
{
	static const float RULES_INTERVAL_SEC = 20;
	static const float LLM_FRESH_SEC = 360;
	static const float SYNC_INTERVAL_SEC = 4;

	protected static ref map<string, ref CLW_Sitrep> s_mRules;
	protected static ref map<string, ref CLW_Sitrep> s_mLlm;
	protected static ref map<string, int> s_mVersion;		// faction -> content version of the current SITREP
	protected static ref map<string, string> s_mSignature;	// faction -> signature of the current SITREP
	protected static ref map<string, float> s_mRulesAt;		// faction -> real time of the last rules build
	protected static ref map<int, string> s_mSent;			// player id -> "faction:version" last delivered
	protected static float s_fLastSync = -1000;

	protected static void EnsureInit()
	{
		if (s_mRules)
			return;

		s_mRules = new map<string, ref CLW_Sitrep>();
		s_mLlm = new map<string, ref CLW_Sitrep>();
		s_mVersion = new map<string, int>();
		s_mSignature = new map<string, string>();
		s_mRulesAt = new map<string, float>();
		s_mSent = new map<int, string>();
	}

	// ---- producers ----

	// Rebuild the rules SITREP of a faction (throttled; force skips the throttle). Called from the faction's commander think.
	static void UpdateRules(string factionKey, CLW_ContactBook book, CLW_SquadRegistry registry, CLW_Orders orders, bool force = false)
	{
		if (!Replication.IsServer())
			return;

		EnsureInit();
		float now = CLW_Orders.Now();
		float last = -1000;
		s_mRulesAt.Find(factionKey, last);
		if (!force && now - last < RULES_INTERVAL_SEC)
			return;

		s_mRulesAt.Set(factionKey, now);
		CLW_Sitrep sitrep = CLW_SitrepBuilder.Build(factionKey, book, registry, orders);
		s_mRules.Set(factionKey, sitrep);
		Refresh(factionKey);
	}

	// The LLM SITREP of a faction (from the commander reply). A copy of the object is kept.
	static void SetLlm(string factionKey, CLW_Sitrep sitrep)
	{
		if (!Replication.IsServer() || !sitrep)
			return;

		EnsureInit();
		s_mLlm.Set(factionKey, sitrep);
		Print(string.Format("CLW: sitrep %1 LLM version received (%2 chars headline, mission '%3')", factionKey, sitrep.m_sHeadline.Length(), sitrep.GetBody(2)), LogLevel.NORMAL);
		Refresh(factionKey);
	}

	// ---- queries ----

	// Current SITREP of a faction: the LLM one while fresh, else the rules one. Null when none exists yet.
	static CLW_Sitrep Current(string factionKey)
	{
		EnsureInit();
		CLW_Sitrep llm;
		if (s_mLlm.Find(factionKey, llm) && llm)
		{
			CLW_WarState war = CLW_GameMode.GetWarState();
			float elapsed = 0;
			if (war)
				elapsed = war.GetElapsedSec();

			float age = elapsed - llm.m_fWarTime;
			if (age >= -5 && age <= LLM_FRESH_SEC)
				return llm;
		}

		CLW_Sitrep rules;
		if (s_mRules.Find(factionKey, rules))
			return rules;

		return null;
	}

	// JSON object of the rules SITREP for the sidecar POST ("" = none yet).
	static string GetRulesJson(string factionKey)
	{
		EnsureInit();
		CLW_Sitrep rules;
		if (s_mRules.Find(factionKey, rules) && rules)
			return rules.ToJson();

		return "";
	}

	// ---- delivery ----

	protected static void Refresh(string factionKey)
	{
		CLW_Sitrep current = Current(factionKey);
		if (!current)
			return;

		string signature = current.Signature();
		string known;
		if (!s_mSignature.Find(factionKey, known) || known != signature)
		{
			int version;
			s_mVersion.Find(factionKey, version);
			version++;
			s_mVersion.Set(factionKey, version);
			s_mSignature.Set(factionKey, signature);
			Print(string.Format("CLW: sitrep %1 new version %2 (source %3, T+%4)", factionKey, version, current.m_sSource, CLW_Sitrep.FormatClock(current.m_fWarTime)), LogLevel.NORMAL);
		}

		SyncPlayers(true);
	}

	// Sends every connected player the current SITREP of his own faction when he does not have this version yet.
	// Throttled to SYNC_INTERVAL_SEC unless forced; cheap when nothing changed.
	static void SyncPlayers(bool force = false)
	{
		if (!Replication.IsServer())
			return;

		EnsureInit();
		float now = CLW_Orders.Now();
		if (!force && now - s_fLastSync < SYNC_INTERVAL_SEC)
			return;

		s_fLastSync = now;
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int id : ids)
		{
			if (id <= 0)
				continue;

			Faction faction = SCR_FactionManager.SGetPlayerFaction(id);
			if (!faction)
			{
				s_mSent.Remove(id);	// no faction yet: deliver when he gets one
				continue;
			}

			string key = faction.GetFactionKey();
			CLW_Sitrep current = Current(key);
			if (!current)
				continue;

			int version;
			s_mVersion.Find(key, version);
			string stamp = key + ":" + version.ToString();
			string sent;
			if (s_mSent.Find(id, sent) && sent == stamp)
				continue;

			s_mSent.Set(id, stamp);
			Deliver(id, current.ToWire(key, version), key, version);
		}

		// forget players that left
		array<int> gone = {};
		foreach (int known, string stampOld : s_mSent)
		{
			if (!ids.Contains(known))
				gone.Insert(known);
		}

		foreach (int goneId : gone)
		{
			s_mSent.Remove(goneId);
		}
	}

	protected static void Deliver(int playerId, string wire, string factionKey, int version)
	{
		Print(string.Format("CLW: sitrep deliver %1 v%2 to player %3 (%4 chars)", factionKey, version, playerId, wire.Length()), LogLevel.NORMAL);
		if (playerId == SCR_PlayerController.GetLocalPlayerId())
		{
			CLW_SitrepClient.Receive(wire);	// listen-server host: no RPC
			return;
		}

		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
		if (controller)
			controller.CLW_SitrepSend(wire);
		else
			s_mSent.Remove(playerId);	// controller not ready: retry on the next sync
	}

	// A client asked for its SITREP again (e.g. /sitrep after joining): forget what was sent so the next sync delivers.
	static void Resend(int playerId)
	{
		EnsureInit();
		s_mSent.Remove(playerId);
		SyncPlayers(true);
	}

	// Rematch (CLW_Rematch.c): forget every SITREP and delivery record (EnsureInit rebuilds the maps on the next use).
	static void RematchReset()
	{
		s_mRules = null;
		s_mLlm = null;
		s_mVersion = null;
		s_mSignature = null;
		s_mRulesAt = null;
		s_mSent = null;
		s_fLastSync = -1000;
	}
}

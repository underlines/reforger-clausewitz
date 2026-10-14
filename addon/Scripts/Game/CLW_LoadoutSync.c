// Game <-> sidecar loadout sync, validation report and dashboard restart (loadouts plan R3 / R5 / R6; contract dev/notes/loadouts-impl.md).
// Server only. Log prefix: "CLW: loadouts".
//
// War start (CLW_WarRuntime.OnWarStarted -> OnWarStart):
//   1. the library in memory (else $profile:Clausewitz/loadouts.json, the cache of the last sync) is validated and the 7 + 1 spawn slots per
//      side are built and sent to the clients at once, so the spawn screen is never empty;
//   2. POST /loadouts/sync {v, round, gameVersion, addons, factions, players}; the reply (library, compositions, pins) replaces the library,
//      is written to the cache file, and the validation + slots are redone (clients get the new slots only when something changed);
//   3. the validation report: "CLW: loadouts validate <faction> ok=N degraded=N invalid=N inactive=N", POST /loadouts/status, and one
//      in-game system message per faction when active server loadouts are degraded / invalid (shown to each player at the first spawn).
// Without the sidecar (error / timeout) step 1 stands and the cache file is used; without a cache the library is empty (R2 role defaults).
// After a rematch / restart (OnRestart) the slots are cleared and one sync refreshes the cache for the next war start.
//
// Restart control (R5): GET /control every 15 s while a war runtime exists; the first restartId seen is the baseline, a higher one later
// restarts the war in place through CLW_Rematch.RestartNow (the rematch reset sequence, which re-runs the sync at the next war start).
//
// REST setup (base URL, header X-CLW-Token from overrides.json "sidecarToken") is CLW_ArsenalSaveHook.Context (G2). Kit strings are built with + only (no Format / Substring).

class CLW_ControlReply
{
	int restartId;
}

class CLW_ValidateCount
{
	int ok;
	int degraded;
	int invalid;
	int inactive;
}

// REST callback of the three requests; the kind tells OnReply which one answered, the round drops answers of an earlier war round.
class CLW_LoadoutRestCallback : RestCallback
{
	protected int m_iKind;
	protected int m_iRound;

	void CLW_LoadoutRestCallback(int kind, int round)
	{
		m_iKind = kind;
		m_iRound = round;
	}

	override void OnSuccess(string data, int dataSize)
	{
		CLW_LoadoutSync.OnReply(m_iKind, m_iRound, true, data, "");
	}

	override void OnError(int errorCode)
	{
		CLW_LoadoutSync.OnReply(m_iKind, m_iRound, false, "", string.Format("error code=%1 http=%2 result=%3", errorCode, GetHttpCode(), GetRestResult()));
	}

	override void OnTimeout()
	{
		CLW_LoadoutSync.OnReply(m_iKind, m_iRound, false, "", "timeout");
	}
}

// Player events of the hosting game mode: a joiner gets the slot state, a spawning player the validation message of the faction.
class CLW_LoadoutPlayerHooks
{
	void OnRegistered(int playerId)
	{
		GetGame().GetCallqueue().CallLater(CLW_LoadoutSlots.SendTo, 1500, false, playerId);
		GetGame().GetCallqueue().CallLater(CLW_LoadoutSlots.SendTo, 5000, false, playerId);
	}

	void OnSpawned(int playerId, IEntity entity)
	{
		CLW_LoadoutSync.WarnPlayer(playerId);
	}
}

class CLW_LoadoutSync
{
	static const int KIND_SYNC = 1;
	static const int KIND_STATUS = 2;
	static const int KIND_CONTROL = 3;
	static const int POLL_MS = 15000;
	static const int SYNC_TIMEOUT_S = 20;
	static const int CONTROL_TIMEOUT_S = 8;
	static const float CONTROL_STUCK_S = 40;

	protected static int s_iRound;
	protected static bool s_bPolling;
	protected static bool s_bBaselineSet;
	protected static int s_iBaseline;
	protected static bool s_bControlInFlight;
	protected static float s_fControlSentAt;
	protected static ref CLW_LoadoutRestCallback s_SyncCb;
	protected static ref CLW_LoadoutRestCallback s_StatusCb;
	protected static ref CLW_LoadoutRestCallback s_ControlCb;
	protected static ref CLW_LoadoutPlayerHooks s_Hooks;
	protected static SCR_BaseGameMode s_HookedMode;
	protected static ref map<string, string> s_mWarn = new map<string, string>();	// faction key -> message wire
	protected static ref map<int, bool> s_mWarned = new map<int, bool>();			// players told this round

	// ---- war flow ----

	// CLW_WarRuntime.OnWarStarted, first thing (before the start forces: G1 asks PickKitForRole while spawning them).
	static void OnWarStart()
	{
		if (!Replication.IsServer())
			return;

		s_iRound++;
		s_mWarn.Clear();
		s_mWarned.Clear();
		HookPlayers();
		array<string> keys = {};
		WarKeys(keys);
		if (CLW_LoadoutLibrary.Count() == 0)
			CLW_LoadoutLibrary.LoadCache();

		Print("CLW: loadouts war start round " + s_iRound.ToString() + ": library " + CLW_LoadoutLibrary.Count().ToString() + " entries from " + CLW_LoadoutLibrary.Source() + ", sides " + keys.Count().ToString(), LogLevel.NORMAL);
		Activate(keys, "war start", true);
		SendSync(keys);
		StartPolling();
	}

	// CLW_WarRuntime.RematchRestart (war back in SETUP): no slots until the next war start; one sync refreshes the cache.
	static void OnRestart()
	{
		if (!Replication.IsServer())
			return;

		s_iRound++;
		s_mWarn.Clear();
		s_mWarned.Clear();
		CLW_LoadoutSlots.Clear();
		CLW_LoadoutSlots.Publish("war restart", true);
		array<string> keys = {};
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (scenario)
			scenario.GetFactionKeys(keys);

		SendSync(keys);
		StartPolling();
	}

	protected static void WarKeys(notnull array<string> keys)
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.GetFactionKeys(keys);
	}

	protected static bool WarRunning()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		return war && war.IsRunning();
	}

	// Pins, validation, slots (and for the final pass the report).
	protected static void Activate(notnull array<string> keys, string reason, bool force)
	{
		if (!CLW_ArsenalPool.IsActive())
			Print("CLW: loadouts WARNING validating while the arsenal pool is not active: kin items would count as missing", LogLevel.WARNING);

		CLW_LoadoutLibrary.ParsePins(keys);
		CLW_LoadoutLibrary.Validate(keys);
		CLW_LoadoutSlots.Rebuild(keys);
		CLW_LoadoutSlots.Publish(reason, force);
	}

	protected static void HookPlayers()
	{
		SCR_BaseGameMode mode = CLW_WarRuntime.GetOwner();
		if (!mode || mode == s_HookedMode)
			return;

		if (!s_Hooks)
			s_Hooks = new CLW_LoadoutPlayerHooks();

		mode.GetOnPlayerRegistered().Insert(s_Hooks.OnRegistered);
		mode.GetOnPlayerSpawned().Insert(s_Hooks.OnSpawned);
		s_HookedMode = mode;
	}

	// ---- REST ----

	// G2's common REST setup (base URL, X-CLW-Token), with this request's own timeout.
	protected static RestContext Context(int timeoutSec)
	{
		RestContext ctx = CLW_ArsenalSaveHook.Context();
		if (ctx)
			ctx.SetTimeout(timeoutSec);

		return ctx;
	}

	protected static void SendSync(notnull array<string> keys)
	{
		RestContext ctx = Context(SYNC_TIMEOUT_S);
		if (!ctx)
		{
			Print("CLW: loadouts sync: no REST context, using the cache", LogLevel.WARNING);
			return;
		}

		string body = BuildSyncBody(keys);
		s_SyncCb = new CLW_LoadoutRestCallback(KIND_SYNC, s_iRound);
		int id = ctx.POST(s_SyncCb, "loadouts/sync", body);
		Print("CLW: loadouts sync POST round " + s_iRound.ToString() + " (" + body.Length().ToString() + " chars, request " + id.ToString() + ")", LogLevel.NORMAL);
		if (id < 0)
			Print("CLW: loadouts sync: POST not sent, using the cache", LogLevel.WARNING);
	}

	// Called by CLW_LoadoutRestCallback (main thread).
	static void OnReply(int kind, int round, bool ok, string data, string failure)
	{
		if (kind == KIND_SYNC)
			OnSyncReply(round, ok, data, failure);
		else if (kind == KIND_STATUS)
			OnStatusReply(ok, failure);
		else if (kind == KIND_CONTROL)
			OnControlReply(ok, data, failure);
	}

	protected static void OnSyncReply(int round, bool ok, string data, string failure)
	{
		if (round != s_iRound)
		{
			Print("CLW: loadouts sync reply of round " + round.ToString() + " ignored (now round " + s_iRound.ToString() + ")", LogLevel.NORMAL);
			return;
		}

		bool running = WarRunning();
		array<string> keys = {};
		WarKeys(keys);
		if (ok && CLW_LoadoutLibrary.Accept(data, "sidecar", true))
		{
			if (!running)
				return;	// restart pull: the cache file is fresh for the next war start

			Activate(keys, "sidecar library", false);
			Report(keys, true);
			return;
		}

		if (ok)
			failure = "reply not usable";

		Print("CLW: loadouts sync failed (" + failure + "): library from " + CLW_LoadoutLibrary.Source() + " (" + CLW_LoadoutLibrary.Count().ToString() + " entries)", LogLevel.WARNING);
		if (running)
			Report(keys, false);
	}

	protected static void OnStatusReply(bool ok, string failure)
	{
		if (!ok)
			Print("CLW: loadouts status POST failed (" + failure + ")", LogLevel.WARNING);
	}

	// ---- restart control ----

	protected static void StartPolling()
	{
		if (s_bPolling)
			return;

		s_bPolling = true;
		GetGame().GetCallqueue().CallLater(PollControl, POLL_MS, true);
	}

	protected static void PollControl()
	{
		if (!Replication.IsServer() || !CLW_WarRuntime.Get())
		{
			GetGame().GetCallqueue().Remove(PollControl);
			s_bPolling = false;
			s_bBaselineSet = false;	// a new world starts a new baseline
			return;
		}

		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		if (s_bControlInFlight && now - s_fControlSentAt < CONTROL_STUCK_S)
			return;

		RestContext ctx = Context(CONTROL_TIMEOUT_S);
		if (!ctx)
			return;

		s_ControlCb = new CLW_LoadoutRestCallback(KIND_CONTROL, s_iRound);
		s_bControlInFlight = true;
		s_fControlSentAt = now;
		if (ctx.GET(s_ControlCb, "control") < 0)
			s_bControlInFlight = false;
	}

	protected static void OnControlReply(bool ok, string data, string failure)
	{
		s_bControlInFlight = false;
		if (!ok)
			return;	// the sidecar is not running: nothing to log every 15 s

		SCR_JsonLoadContext ctx = new SCR_JsonLoadContext();
		CLW_ControlReply reply = new CLW_ControlReply();
		if (!ctx.ImportFromString(data) || !ctx.ReadValue("", reply))
			return;

		if (!s_bBaselineSet)
		{
			s_bBaselineSet = true;
			s_iBaseline = reply.restartId;
			Print("CLW: loadouts control baseline restartId=" + s_iBaseline.ToString(), LogLevel.NORMAL);
			return;
		}

		if (reply.restartId <= s_iBaseline)
			return;

		s_iBaseline = reply.restartId;
		Print("CLW: loadouts restart requested id=" + reply.restartId.ToString(), LogLevel.NORMAL);
		if (!CLW_Rematch.RestartNow(CLW_Loc.W("#CLW-Loadouts_RestartReason")))
			Print("CLW: loadouts restart id=" + reply.restartId.ToString() + " not done: the war is in setup (the next war start syncs anyway) or no war runtime", LogLevel.NORMAL);
	}

	// ---- sync body ----

	protected static string StringsJson(array<string> items)
	{
		string json = "[";
		if (items)
		{
			foreach (int i, string item : items)
			{
				if (i > 0)
					json += ",";

				json += "\"" + CLW_Situation.Escape(item) + "\"";
			}
		}

		return json + "]";
	}

	protected static string AddonsJson()
	{
		array<string> guids = {};
		GameProject.GetLoadedAddons(guids);
		string json = "[";
		foreach (int i, string guid : guids)
		{
			if (i > 0)
				json += ",";

			json += "{\"guid\":\"" + CLW_Situation.Escape(guid) + "\",\"name\":\"" + CLW_Situation.Escape(GameProject.GetAddonTitle(guid)) + "\"}";
		}

		return json + "]";
	}

	protected static string FactionsJson(notnull array<string> keys)
	{
		string json = "[";
		foreach (int i, string key : keys)
		{
			if (i > 0)
				json += ",";

			json += "{\"key\":\"" + CLW_Situation.Escape(key) + "\",\"name\":\"" + CLW_Situation.Escape(CLW_Loc.Resolve(CLW_Loc.FactionName(key))) + "\",\"side\":" + i.ToString() + "}";
		}

		return json + "]";
	}

	protected static string PlayersJson()
	{
		array<ref CLW_CustomLoadoutRec> records = {};
		CLW_CustomLoadouts.All(records);
		string json = "[";
		bool first = true;
		foreach (CLW_CustomLoadoutRec rec : records)
		{
			if (!rec)
				continue;

			if (!first)
				json += ",";

			first = false;
			json += CLW_CustomLoadouts.RecordJson(rec);	// G2: the record shape of the contract, kit escaped in chunks
		}

		return json + "]";
	}

	protected static string BuildSyncBody(notnull array<string> keys)
	{
		string body = "{\"v\":1,\"round\":" + CLW_Rematch.GetRound().ToString();
		body += ",\"gameVersion\":\"" + CLW_Situation.Escape(GetGame().GetBuildVersion()) + "\"";
		body += ",\"addons\":" + AddonsJson();
		body += ",\"factions\":" + FactionsJson(keys);
		body += ",\"players\":" + PlayersJson();
		body += "}";
		return body;
	}

	// ---- validation report (R6) ----

	protected static CLW_ValidateCount CountFor(notnull map<string, ref CLW_ValidateCount> counts, notnull array<string> order, string factionKey)
	{
		CLW_ValidateCount count;
		if (counts.Find(factionKey, count))
			return count;

		count = new CLW_ValidateCount();
		counts.Set(factionKey, count);
		order.Insert(factionKey);
		return count;
	}

	protected static void Tally(CLW_ValidateCount count, string status)
	{
		if (status == CLW_LoadoutLibrary.STATUS_OK)
			count.ok++;
		else if (status == CLW_LoadoutLibrary.STATUS_DEGRADED)
			count.degraded++;
		else if (status == CLW_LoadoutLibrary.STATUS_INACTIVE)
			count.inactive++;
		else
			count.invalid++;
	}

	// Library entries were validated by CLW_LoadoutLibrary.Validate; the players' custom loadouts are validated here. Logs one line per faction,
	// posts /loadouts/status (postStatus) and prepares the in-game message of every faction with active degraded / invalid server loadouts.
	protected static void Report(notnull array<string> keys, bool postStatus)
	{
		map<string, ref CLW_ValidateCount> counts = new map<string, ref CLW_ValidateCount>();
		array<string> order = {};
		foreach (string key : keys)
		{
			CountFor(counts, order, key);
		}

		array<CLW_LibraryEntry> entries = {};
		CLW_LoadoutLibrary.GetAll(entries);
		map<string, int> activeInvalid = new map<string, int>();
		map<string, int> activeDegraded = new map<string, int>();
		foreach (CLW_LibraryEntry entry : entries)
		{
			Tally(CountFor(counts, order, entry.faction), entry.status);
			if (!entry.active)
				continue;

			int bad;
			if (entry.status == CLW_LoadoutLibrary.STATUS_INVALID)
			{
				activeInvalid.Find(entry.faction, bad);
				activeInvalid.Set(entry.faction, bad + 1);
			}
			else if (entry.status == CLW_LoadoutLibrary.STATUS_DEGRADED)
			{
				activeDegraded.Find(entry.faction, bad);
				activeDegraded.Set(entry.faction, bad + 1);
			}
		}

		array<ref CLW_CustomLoadoutRec> records = {};
		CLW_CustomLoadouts.All(records);
		foreach (CLW_CustomLoadoutRec rec : records)
		{
			if (!rec)
				continue;

			if (!rec.missing)
				rec.missing = new array<string>();

			rec.missing.Clear();
			if (!keys.Contains(rec.faction))
			{
				rec.status = CLW_LoadoutLibrary.STATUS_INACTIVE;
			}
			else
			{
				array<string> missing = {};
				rec.status = CLW_Kits.Validate(rec.loadout, rec.faction, missing);
				foreach (string item : missing)
				{
					rec.missing.Insert(item);
				}
			}

			Tally(CountFor(counts, order, rec.faction), rec.status);
		}

		foreach (string factionKey : order)
		{
			CLW_ValidateCount count = counts.Get(factionKey);
			Print("CLW: loadouts validate " + factionKey + " ok=" + count.ok.ToString() + " degraded=" + count.degraded.ToString() + " invalid=" + count.invalid.ToString() + " inactive=" + count.inactive.ToString(), LogLevel.NORMAL);
		}

		if (postStatus)
			PostStatus(entries, records);

		// one message per war faction, shown to its players at their first spawn (and now to those who already play)
		s_mWarn.Clear();
		foreach (string warKey : keys)
		{
			int invalid;
			int degraded;
			activeInvalid.Find(warKey, invalid);
			activeDegraded.Find(warKey, degraded);
			if (invalid <= 0 && degraded <= 0)
				continue;

			s_mWarn.Set(warKey, CLW_Loc.W("#CLW-Loadouts_Warn", CLW_Loc.FactionName(warKey), CLW_Loc.N(invalid), CLW_Loc.N(degraded)));
			Print("CLW: loadouts validate " + warKey + " warning for its players: " + invalid.ToString() + " active server loadouts invalid, " + degraded.ToString() + " degraded", LogLevel.WARNING);
		}

		if (s_mWarn.IsEmpty())
			return;

		PlayerManager players = GetGame().GetPlayerManager();
		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			if (players.GetPlayerControlledEntity(playerId))
				WarnPlayer(playerId);
		}
	}

	protected static void PostStatus(notnull array<CLW_LibraryEntry> entries, notnull array<ref CLW_CustomLoadoutRec> records)
	{
		string body = "{\"v\":1,\"library\":[";
		bool first = true;
		foreach (CLW_LibraryEntry entry : entries)
		{
			if (!first)
				body += ",";

			first = false;
			body += "{\"id\":\"" + CLW_Situation.Escape(entry.id) + "\",\"status\":\"" + CLW_Situation.Escape(entry.status) + "\",\"missing\":" + StringsJson(entry.missing) + "}";
		}

		body += "],\"players\":[";
		first = true;
		foreach (CLW_CustomLoadoutRec rec : records)
		{
			if (!rec)
				continue;

			if (!first)
				body += ",";

			first = false;
			body += "{\"identityId\":\"" + CLW_Situation.Escape(rec.identityId) + "\",\"faction\":\"" + CLW_Situation.Escape(rec.faction) + "\"";
			body += ",\"status\":\"" + CLW_Situation.Escape(rec.status) + "\",\"missing\":" + StringsJson(rec.missing) + "}";
		}

		body += "]}";
		RestContext ctx = Context(SYNC_TIMEOUT_S);
		if (!ctx)
			return;

		s_StatusCb = new CLW_LoadoutRestCallback(KIND_STATUS, s_iRound);
		if (ctx.POST(s_StatusCb, "loadouts/status", body) < 0)
			Print("CLW: loadouts status POST not sent", LogLevel.WARNING);
	}

	// ---- player messages ----

	// Server: system message to one player (own language through the wire).
	static void Tell(int playerId, string wire)
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
		if (!controller)
			return;

		if (playerId == SCR_PlayerController.GetLocalPlayerId())
			CLW_Debug.Show(wire);	// listen server host: no RPC needed
		else
			controller.CLW_RpcShow(wire);
	}

	// The faction's validation message, once per player and round.
	static void WarnPlayer(int playerId)
	{
		if (s_mWarned.Contains(playerId) || s_mWarn.IsEmpty())
			return;

		Faction faction = SCR_FactionManager.SGetPlayerFaction(playerId);
		if (!faction)
			return;

		string wire;
		if (!s_mWarn.Find(faction.GetFactionKey(), wire))
			return;

		s_mWarned.Set(playerId, true);
		Tell(playerId, wire);
	}
}

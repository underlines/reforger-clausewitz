// LLM bridge (protocol v3 = v2 + action manifest): posts the faction situation to the local sidecar and executes the orders /
// purchases / mission offers it returns, each one through the action registry (CLW_ActionRegistry: validate + execute). Wraps the
// CLW_RuleCommander, which stays the fallback (timeout, error, empty or invalid reply). The registry manifest (what HQ may command in
// this war mode) goes to the sidecar as POST /manifest whenever its hash changes; every situation carries the hash, and a reply with
// needManifest makes us resend (sidecar restarted). v1 replies (orders with pos, MOVE/ATTACK/DEFEND) are accepted too. Server only.

// Shapes of the sidecar reply, filled by SCR_JsonLoadContext (field names = JSON keys). Absent fields keep their defaults. [verify]
class CLW_OrderDto
{
	string squad;
	string command;
	string target;
	ref array<float> pos = {};
	string reason;
}

class CLW_BuyDto
{
	string type;
	int count = 1;
	string reason;
	string command;		// spawn on assignment: the first order of every unit of this entry (MOVE | ATTACK | DEFEND | RECON) ...
	string target;		// ... with an objective id ...
	ref array<float> pos = {};	// ... or a position [x, z]
}

class CLW_OfferDto
{
	string kind;
	string target;
	ref array<float> pos = {};
	string brief;
}

class CLW_MissionDto
{
	string squad;
	ref array<ref CLW_OfferDto> offers = {};
}

// Reply channel "fob" (M3 phase F): ESTABLISH_FOB {site} or UPGRADE_FOB {fob}. command is optional (site -> establish, fob -> upgrade).
// M4.6 two-step tool: ESTABLISH_FOB {area, intent} (step 1, area "none" = decline; the game then asks POST /fobzoom for the exact spot) or
// ESTABLISH_FOB {pos} (a direct spot).
class CLW_FobDto
{
	string command;
	string site;
	string area;
	string intent;
	ref array<float> pos = {};
	string fob;
	string origin;	// CONVOY (M3 phase L): source and destination base ids ("HQ" or a FOB id); RESUPPLY uses fob
	string dest;
	string reason;
}

// Reply of POST /fobzoom: the exact FOB spot the model picked in the zoom (cell or pos), or a decline.
class CLW_FobZoomReply
{
	bool ok;
	bool decline;
	string error;
	string reason;
	string cell;	// zoom sub-cell id ("f7")
	ref array<float> pos = {};	// [x, z]
}

class CLW_FobZoomCallback : RestCallback
{
	protected CLW_RemoteCommander m_Owner;	// weak: the owner holds this callback

	void CLW_FobZoomCallback(CLW_RemoteCommander owner)
	{
		m_Owner = owner;
	}

	override void OnSuccess(string data, int dataSize)
	{
		if (m_Owner)
			m_Owner.OnFobZoomReply(true, data, "");
	}

	override void OnError(int errorCode)
	{
		if (m_Owner)
			m_Owner.OnFobZoomReply(false, "", string.Format("error code=%1 http=%2 result=%3", errorCode, GetHttpCode(), GetRestResult()));
	}

	override void OnTimeout()
	{
		if (m_Owner)
			m_Owner.OnFobZoomReply(false, "", "timeout");
	}
}

class CLW_OrdersReply
{
	ref array<ref CLW_FobDto> fobs = {};
	ref array<ref CLW_OrderDto> orders = {};
	ref array<ref CLW_BuyDto> buy = {};
	ref array<ref CLW_MissionDto> missions = {};
	bool needManifest;	// the sidecar does not know the manifest hash of our post: resend the manifest
	bool keep;	// true with nothing else = "nothing changed, keep the current orders"
	ref array<ref CLW_HqActionDto> hqActions = {};	// M3 phase A: finished HQ chat turns (see CLW_HqChat.c)
	ref array<ref CLW_HqReplyDto> hqReplies = {};
	ref CLW_SitrepDto sitrep = new CLW_SitrepDto();	// optional faction SITREP written by the model (empty = absent), see CLW_Sitrep.c
}

class CLW_RemoteCallback : RestCallback
{
	protected CLW_RemoteCommander m_Owner;	// weak: the owner holds this callback

	void CLW_RemoteCallback(CLW_RemoteCommander owner)
	{
		m_Owner = owner;
	}

	override void OnSuccess(string data, int dataSize)
	{
		if (m_Owner)
			m_Owner.OnReply(true, data, "");
	}

	override void OnError(int errorCode)
	{
		if (m_Owner)
			m_Owner.OnReply(false, "", string.Format("error code=%1 http=%2 result=%3", errorCode, GetHttpCode(), GetRestResult()));
	}

	override void OnTimeout()
	{
		if (m_Owner)
			m_Owner.OnReply(false, "", "timeout");
	}
}

// Reply of POST /manifest (fire and forget: only success / failure matters).
class CLW_ManifestCallback : RestCallback
{
	protected CLW_RemoteCommander m_Owner;	// weak: the owner holds this callback

	void CLW_ManifestCallback(CLW_RemoteCommander owner)
	{
		m_Owner = owner;
	}

	override void OnSuccess(string data, int dataSize)
	{
		if (m_Owner)
			m_Owner.OnManifestReply(true, "");
	}

	override void OnError(int errorCode)
	{
		if (m_Owner)
			m_Owner.OnManifestReply(false, string.Format("error code=%1 http=%2 result=%3", errorCode, GetHttpCode(), GetRestResult()));
	}

	override void OnTimeout()
	{
		if (m_Owner)
			m_Owner.OnManifestReply(false, "timeout");
	}
}

class CLW_RemoteCommander
{
	static bool ENABLED = true;
	static const int TIMEOUT_SECONDS = 60;		// above the sidecar's longest turn: HQ agent turn (agent.timeoutSec 40 s) + SITREP translation (i18n.translateTimeoutSec 10 s, parallel per language) + provider latency
	static const float STUCK_SECONDS = 75;		// a request in flight longer than this is considered lost (above TIMEOUT_SECONDS)
	static const float LOG_INTERVAL = 60;		// s: repeat the same fallback reason at most this often
	static const float RULES_INTERVAL = 25;		// s: the rule-commander fallback runs at most this often (we are polled every 10 s)
	static const float ONLINE_SECONDS = 90;		// IsOnline: a valid reply within this long ago (above one slow poll, TIMEOUT_SECONDS, plus the poll interval)
	static const int MAX_BUY_ENTRIES = 3;
	static const int MAX_OFFERS = 3;
	static const int MAX_BRIEF = 240;

	protected ref CLW_RuleCommander m_Rules = new CLW_RuleCommander();
	protected ref CLW_RemoteCallback m_Callback;	// must stay referenced until the reply arrives
	protected string m_sFaction;
	protected bool m_bInFlight;
	protected float m_fSentAt;
	protected string m_sLastReason;
	protected float m_fLastReasonAt = -1000;
	protected float m_fLastKeepAt = -1000;
	protected float m_fLastRulesAt = -1000;
	protected int m_iDebugSeq;		// last CLW_Debug line already sent with this faction's POST
	protected bool m_bSitrepRunning;	// war phase seen by the last SITREP refresh (a phase change forces a rebuild)
	protected ref CLW_ManifestCallback m_ManifestCallback;	// must stay referenced until the reply arrives
	protected string m_sManifestSent;		// hash the sidecar acknowledged ("" = none / lost)
	protected string m_sManifestPending;	// hash of the manifest POST in flight
	protected bool m_bManifestInFlight;
	protected float m_fManifestSentAt;
	protected float m_fLastManifestLogAt = -1000;
	protected float m_fLastOkAt = -1000;	// world time of the last valid sidecar reply (IsOnline)
	// FOB zoom (M4.6): step 2 of ESTABLISH_FOB, one request at a time per faction
	protected ref CLW_FobZoomCallback m_ZoomCallback;	// must stay referenced until the reply arrives
	protected bool m_bZoomInFlight;
	protected float m_fZoomSentAt;
	protected string m_sZoomArea;
	protected string m_sZoomIntent;
	protected string m_sZoomReason;
	protected string m_sZoomJson;		// the zoom digest (reused for the retry)
	protected int m_iZoomAttempt;
	protected string m_sZoomRetryReason;
	protected vector m_vZoomPrev;

	void Init(string factionKey, CLW_ContactBook book, CLW_SquadRegistry registry)
	{
		m_sFaction = factionKey;
		m_Rules.Init(factionKey, book, registry);
		CLW_BaseWatch.Reset(factionKey);
	}

	string GetFaction()
	{
		return m_sFaction;
	}

	CLW_RuleCommander GetRules()
	{
		return m_Rules;
	}

	// The LLM sidecar answered a situation recently (HQ chat only goes to it then, else the menu hint answers at once).
	bool IsOnline()
	{
		if (!ENABLED)
			return false;

		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		return now - m_fLastOkAt < ONLINE_SECONDS;
	}

	void Think()
	{
		if (!Replication.IsServer())
			return;

		UpdateSitrep();
		CLW_BaseWatch.Think(m_Rules, m_sFaction);	// HQ garrison + idle stand-down, independent of who commands (LLM or rules)

		if (!ENABLED)
		{
			ThinkRules();
			return;
		}

		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		if (m_bInFlight)
		{
			if (now - m_fSentAt < STUCK_SECONDS)
				return;
			m_bInFlight = false;
			Fallback("request lost (no callback)");
			return;
		}

		string body;
		int squads = m_Rules.BuildSituation(body);
		if (body.IsEmpty())
			return;

		// the rules SITREP rides along (the sidecar shows it on the dashboard when no LLM SITREP is current)
		string sitrepJson = CLW_SitrepStore.GetRulesJson(m_sFaction);
		if (!sitrepJson.IsEmpty() && body.Length() > 2 && body.Length() + sitrepJson.Length() < 58000)
			body = CLW_Str.DropLast(body) + ",\"sitrep\":" + sitrepJson + "}";

		// recent debug lines (CLW_Debug ring) for the dashboard debug log; every line goes with both factions' posts, the sidecar dedupes
		int debugLast;
		string debugJson = CLW_Debug.GetJsonSince(m_iDebugSeq, debugLast);
		if (!debugJson.IsEmpty() && body.Length() > 2 && body.Length() + debugJson.Length() < 58000)
		{
			body = CLW_Str.DropLast(body) + ",\"debug\":" + debugJson + "}";
			m_iDebugSeq = debugLast;
		}

		// protocol v3: the hash of the action manifest this situation was built for (the manifest itself goes to POST /manifest)
		string manifestHash = SyncManifest(now);
		if (!manifestHash.IsEmpty() && body.Length() > 2)
			body = CLW_Str.DropLast(body) + ",\"manifest\":\"" + manifestHash + "\"}";

		// M3 phase A: pending HQ chat messages of this faction (/hq <text>) for the sidecar's agent turn
		string hqJson = CLW_HqChat.GetJson(m_sFaction);
		if (!hqJson.IsEmpty() && body.Length() > 2)
			body = CLW_Str.DropLast(body) + ",\"hq\":" + hqJson + "}";

		// protocol v4: languages of this faction's players, for the translations of a broadcast SITREP (sitrep.text_by_lang in the reply)
		string langsJson = CLW_SitrepStore.GetLangsJson(m_sFaction);
		if (!langsJson.IsEmpty() && body.Length() > 2)
			body = CLW_Str.DropLast(body) + ",\"langs\":" + langsJson + "}";

		if (squads <= 0)
			ThinkRules();	// still POST the situation below: the sidecar/dashboard must learn the faction has no squads left (it may still buy)

		RestContext ctx = CLW_SidecarLink.Context(TIMEOUT_SECONDS, "cmd-" + m_sFaction);	// one context per faction: the two commanders run in parallel
		if (!ctx)
		{
			Fallback("no REST context");
			return;
		}

		m_Callback = new CLW_RemoteCallback(this);
		m_bInFlight = true;
		m_fSentAt = now;
		int id = ctx.POST(m_Callback, "orders", body);
		if (id < 0)
		{
			m_bInFlight = false;
			Fallback("POST not sent");
		}
	}

	// ---- action manifest (protocol v3) ----

	// Current manifest hash of this faction; (re)sends the manifest when the sidecar has not acknowledged this hash yet
	// (war start, mode / capability / faction change, lost manifest). Never blocks: the situation goes out either way.
	protected string SyncManifest(float now)
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		string hash;
		string json = registry.GetManifest(m_sFaction, hash);
		if (m_bManifestInFlight && now - m_fManifestSentAt > STUCK_SECONDS)
			m_bManifestInFlight = false;

		if (hash == m_sManifestSent || m_bManifestInFlight)
			return hash;

		RestContext ctx = CLW_SidecarLink.Context(TIMEOUT_SECONDS, "cmd-" + m_sFaction);	// one context per faction: the two commanders run in parallel
		if (!ctx)
			return hash;

		m_ManifestCallback = new CLW_ManifestCallback(this);
		m_sManifestPending = hash;
		m_bManifestInFlight = true;
		m_fManifestSentAt = now;
		if (ctx.POST(m_ManifestCallback, "manifest", json) < 0)
			m_bManifestInFlight = false;

		return hash;
	}

	// Called by the manifest REST callback (main thread).
	void OnManifestReply(bool ok, string failure)
	{
		m_bManifestInFlight = false;
		if (ok)
		{
			m_sManifestSent = m_sManifestPending;
			return;
		}

		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		if (now - m_fLastManifestLogAt > LOG_INTERVAL)
		{
			m_fLastManifestLogAt = now;
			Print(string.Format("CLW: RemoteCommander %1: manifest not delivered (retry next poll): %2", m_sFaction, failure), LogLevel.WARNING);
		}
	}

	// Called by the REST callback (main thread).
	void OnReply(bool ok, string data, string failure)
	{
		m_bInFlight = false;
		if (ok && data.IndexOf("needManifest") >= 0)
			m_sManifestSent = "";	// the sidecar lost / never got our manifest: resend with the next poll (also in SETUP)

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;	// SETUP / ENDED: nothing to execute

		if (!ok)
		{
			Fallback(failure);
			return;
		}

		SCR_JsonLoadContext loader = new SCR_JsonLoadContext();
		CLW_OrdersReply reply = new CLW_OrdersReply();
		if (!loader.ImportFromString(data) || !loader.ReadValue("", reply))
		{
			Fallback("reply is not valid JSON: " + data.Substring(0, Math.Min(data.Length(), 200)));
			return;
		}

		if (reply.needManifest)
			m_sManifestSent = "";	// the sidecar lost / never got our manifest: resend with the next poll

		m_fLastOkAt = GetGame().GetWorld().GetWorldTime() * 0.001;

		// HQ chat answers (independent of the orders: a "keep" reply carries them too)
		CLW_HqChat.Apply(m_sFaction, reply.hqActions, reply.hqReplies);

		// optional SITREP written by the model: independent of the orders (a "keep" reply may carry one)
		if (reply.sitrep)
		{
			CLW_Sitrep llmSitrep = reply.sitrep.ToSitrep(m_sFaction, war.GetElapsedSec());
			if (llmSitrep)
			{
				// protocol v4: translations of the same SITREP per recipient language (absent = v3 behaviour)
				map<string, ref CLW_Sitrep> byLang;
				if (data.IndexOf("text_by_lang") >= 0)
					byLang = CLW_SitrepLangs.Parse(data, m_sFaction, war.GetElapsedSec());

				CLW_SitrepStore.SetLlm(m_sFaction, llmSitrep, byLang);
			}
		}

		bool hasOrders = reply.orders && !reply.orders.IsEmpty();
		bool hasBuy = reply.buy && !reply.buy.IsEmpty();
		bool hasMissions = reply.missions && !reply.missions.IsEmpty();
		bool hasFobs = reply.fobs && !reply.fobs.IsEmpty();
		if (!hasOrders && !hasBuy && !hasMissions && !hasFobs)
		{
			if (reply.keep)
			{
				float keepNow = GetGame().GetWorld().GetWorldTime() * 0.001;
				if (keepNow - m_fLastKeepAt > LOG_INTERVAL)
				{
					m_fLastKeepAt = keepNow;
					Print(string.Format("CLW: RemoteCommander %1: keeping current orders", m_sFaction), LogLevel.NORMAL);
				}
				m_Rules.ThinkHumanLed();
				return;
			}

			Fallback("empty reply");
			return;
		}

		string summary = "";
		int applied = 0;
		// FOB/logistics first: they reserve their supplies and units before buys spend the rest and orders take the units
		// (DS run 2026-10-12: buys in the same reply left 100 of 300 and the ESTABLISH_FOB was refused)
		if (hasFobs)
			applied += ApplyFobs(reply.fobs, summary);
		if (hasOrders)
			applied += ApplyOrders(reply.orders, summary);
		if (hasBuy)
			applied += ApplyBuys(reply.buy, summary);
		if (hasMissions)
			applied += ApplyMissions(reply.missions, summary);

		if (applied == 0)
		{
			Fallback("no valid items in reply");
			return;
		}

		m_sLastReason = "";
		m_Rules.ThinkHumanLed();
		CLW_Debug.Say(string.Format("%1 HQ (LLM): %2 item(s): %3", m_sFaction, applied, summary));
	}

	// ---- commands: every item of the reply is a registry call (CLW_ActionRegistry.Validate + Execute) ----

	protected CLW_ActionCall NewCall(string action)
	{
		CLW_ActionCall call = new CLW_ActionCall();
		call.m_sAction = action;
		call.m_sFaction = m_sFaction;
		call.m_sAuthority = CLW_ActionEntry.AUTH_HQ;
		call.m_sSource = "llm";
		call.m_Rules = m_Rules;
		return call;
	}

	// ---- orders ----

	protected int ApplyOrders(notnull array<ref CLW_OrderDto> orders, out string summary)
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		array<string> done = {};
		int applied;
		foreach (CLW_OrderDto order : orders)
		{
			if (!order)
				continue;

			// "fall back" / "FALLBACK" / lower case -> registry name
			string commandName = order.command;
			CLW_ECommand parsed;
			if (CLW_Command.FromName(order.command, parsed))
				commandName = CLW_Command.ToName(parsed);

			CLW_ActionCall call = NewCall(commandName);
			call.m_sReason = order.reason;
			call.SetString("squad", order.squad);
			call.SetString("target", order.target);
			call.SetString("reason", order.reason);
			if (order.pos && order.pos.Count() >= 2)
				call.SetPos("pos", order.pos[0], order.pos[1]);

			string why = "";
			CLW_ActionEntry entry = registry.Find(commandName);
			if (order.squad.IsEmpty())
				why = "no squad";
			else if (done.Contains(order.squad))
				why = "duplicate squad";
			else if (!entry || entry.m_sChannel != "orders")
				why = "unknown command";
			else
			{
				string outcome = "";
				if (!registry.Run(call, outcome))
					why = outcome;
			}

			if (!why.IsEmpty())
			{
				Print(string.Format("CLW: RemoteCommander %1: order rejected (%2): squad=%3 command=%4 target=%5", m_sFaction, why, order.squad, order.command, order.target), LogLevel.WARNING);
				continue;
			}

			done.Insert(order.squad);
			applied++;
			if (!summary.IsEmpty())
				summary += ", ";
			summary += order.squad + " " + order.command;
			Print(string.Format("CLW: RemoteCommander %1: ORDER %2 %3 target='%4' pos=%5 (%6)", m_sFaction, order.command, order.squad, call.GetString("target"), call.GetPos("pos"), order.reason), LogLevel.NORMAL);
		}

		return applied;
	}

	// ---- purchases ----

	protected int ApplyBuys(notnull array<ref CLW_BuyDto> buys, out string summary)
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		int applied;
		foreach (int index, CLW_BuyDto buy : buys)
		{
			if (!buy)
				continue;

			if (index >= MAX_BUY_ENTRIES)
			{
				Print(string.Format("CLW: RemoteCommander %1: buy entry %2 ignored (max %3 per reply)", m_sFaction, buy.type, MAX_BUY_ENTRIES), LogLevel.WARNING);
				continue;
			}

			CLW_ActionCall call = NewCall("BUY");
			call.m_sReason = buy.reason;
			call.SetString("type", buy.type);
			call.SetInt("count", Math.Clamp(buy.count, 1, 3));
			call.SetString("reason", buy.reason);
			call.SetString("command", buy.command);	// spawn on assignment: the first order of the bought units
			call.SetString("target", buy.target);
			if (buy.pos && buy.pos.Count() >= 2)
				call.SetPos("pos", buy.pos[0], buy.pos[1]);
			string why = "";
			if (!registry.Validate(call, why))
			{
				Print(string.Format("CLW: RemoteCommander %1: buy rejected (%2)", m_sFaction, why), LogLevel.WARNING);
				continue;
			}

			// the executor logs every unit bought or refused
			string result = "";
			if (!registry.Execute(call, result))
				continue;

			applied += call.m_iApplied;
			if (!summary.IsEmpty())
				summary += ", ";
			summary += result;
		}

		return applied;
	}

	// ---- missions ----

	protected int ApplyMissions(notnull array<ref CLW_MissionDto> missions, out string summary)
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		int applied;
		array<string> seen = {};
		foreach (CLW_MissionDto mission : missions)
		{
			if (!mission)
				continue;

			string why = "";
			string result = "";
			if (seen.Contains(mission.squad))
			{
				why = "duplicate missions entry";
			}
			else
			{
				CLW_ActionCall call = NewCall("OFFER_MISSION");
				call.SetString("squad", mission.squad);
				call.m_Payload = mission;
				if (!registry.Run(call, result))
					why = result;
			}

			if (!why.IsEmpty())
			{
				Print(string.Format("CLW: RemoteCommander %1: missions rejected for %2: %3", m_sFaction, mission.squad, why), LogLevel.WARNING);
				continue;
			}

			seen.Insert(mission.squad);
			applied++;
			if (!summary.IsEmpty())
				summary += ", ";
			summary += result;
		}

		return applied;
	}

	// ---- FOBs (M3 phase F) ----

	protected int ApplyFobs(notnull array<ref CLW_FobDto> fobs, out string summary)
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		int applied;
		foreach (int index, CLW_FobDto item : fobs)
		{
			if (!item)
				continue;

			if (index >= 3)
			{
				Print(string.Format("CLW: RemoteCommander %1: fob entry ignored (max 3 per reply)", m_sFaction), LogLevel.WARNING);
				break;
			}

			string action = item.command;
			action.ToUpper();
			if (action.IsEmpty() && (!item.site.IsEmpty() || !item.area.IsEmpty()))
				action = "ESTABLISH_FOB";
			else if (action.IsEmpty() && !item.fob.IsEmpty())
				action = "UPGRADE_FOB";

			CLW_ActionEntry entry = registry.Find(action);
			if (!entry || entry.m_sChannel != "fob")
			{
				Print(string.Format("CLW: RemoteCommander %1: fob entry rejected (unknown command '%2')", m_sFaction, item.command), LogLevel.WARNING);
				continue;
			}

			CLW_ActionCall call = NewCall(action);
			call.m_sReason = item.reason;
			call.SetString("site", item.site);
			call.SetString("area", item.area);
			call.SetString("intent", item.intent);
			if (item.pos && item.pos.Count() >= 2)
				call.SetPos("pos", item.pos[0], item.pos[1]);
			call.SetString("fob", item.fob);
			call.SetString("origin", item.origin);
			call.SetString("dest", item.dest);
			call.SetString("reason", item.reason);
			string result = "";
			if (!registry.Run(call, result))
			{
				Print(string.Format("CLW: RemoteCommander %1: %2 rejected: %3", m_sFaction, action, result), LogLevel.WARNING);
				continue;
			}

			applied += call.m_iApplied;
			if (!summary.IsEmpty())
				summary += ", ";
			summary += result;
			Print(string.Format("CLW: RemoteCommander %1: %2 (%3)", m_sFaction, result, item.reason), LogLevel.NORMAL);
		}

		return applied;
	}

	// ---- FOB zoom: step 2 of the ESTABLISH_FOB tool (M4.6, dev/notes/fob-tool-design.md) ----

	// A zoom request is in flight (a lost one is forgotten after STUCK_SECONDS).
	bool IsFobZoomBusy()
	{
		if (!m_bZoomInFlight)
			return false;

		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		if (now - m_fZoomSentAt > STUCK_SECONDS)
		{
			m_bZoomInFlight = false;
			Print(string.Format("CLW: fob zoom %1: request for %2 lost (no callback), forgotten", m_sFaction, m_sZoomArea), LogLevel.WARNING);
			return false;
		}

		return true;
	}

	// Builds the zoom digest of 'area' and asks the sidecar (POST /fobzoom) for the exact spot. The answer comes to OnFobZoomReply.
	bool StartFobZoom(string area, string intent, string reason, out string why)
	{
		if (IsFobZoomBusy())
		{
			why = "a FOB spot request is already in progress";
			return false;
		}

		string digestWhy;
		string zoom = CLW_MapDigest.ZoomJson(m_sFaction, area, digestWhy);
		if (zoom.IsEmpty())
		{
			why = digestWhy;
			return false;
		}

		m_sZoomArea = area;
		m_sZoomIntent = intent;
		m_sZoomReason = reason;
		m_sZoomJson = zoom;
		m_iZoomAttempt = 0;
		m_sZoomRetryReason = "";
		Print(string.Format("CLW: fob zoom %1: area %2 intent '%3' requested (digest %4 chars, ~%5 tokens)", m_sFaction, area, intent, zoom.Length(), zoom.Length() / 4), LogLevel.NORMAL);
		return SendFobZoom(why);
	}

	protected bool SendFobZoom(out string why)
	{
		RestContext ctx = CLW_SidecarLink.Context(TIMEOUT_SECONDS, "fobzoom-" + m_sFaction);	// its own context: the commander call of this faction may be in flight
		if (!ctx)
		{
			why = "no REST context";
			return false;
		}

		m_iZoomAttempt++;
		string body = "{\"v\":1,\"faction\":\"" + CLW_Situation.Escape(m_sFaction) + "\",\"area\":\"" + CLW_Situation.Escape(m_sZoomArea) + "\",\"intent\":\"" + CLW_Situation.Escape(m_sZoomIntent) + "\",\"attempt\":" + m_iZoomAttempt;
		if (!m_sZoomRetryReason.IsEmpty())
			body += ",\"retry\":{\"reason\":\"" + CLW_Situation.Escape(m_sZoomRetryReason) + "\",\"pos\":[" + Math.Round(m_vZoomPrev[0]) + "," + Math.Round(m_vZoomPrev[2]) + "]}";
		body += ",\"zoom\":" + m_sZoomJson + "}";
		m_ZoomCallback = new CLW_FobZoomCallback(this);
		m_bZoomInFlight = true;
		m_fZoomSentAt = GetGame().GetWorld().GetWorldTime() * 0.001;
		if (ctx.POST(m_ZoomCallback, "fobzoom", body) < 0)
		{
			m_bZoomInFlight = false;
			why = "POST not sent";
			return false;
		}

		return true;
	}

	// Called by the zoom REST callback (main thread).
	void OnFobZoomReply(bool ok, string data, string failure)
	{
		m_bZoomInFlight = false;
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war || !war.IsRunning())
			return;

		if (!ok)
		{
			FobZoomFailed("zoom call failed: " + failure);
			return;
		}

		SCR_JsonLoadContext loader = new SCR_JsonLoadContext();
		CLW_FobZoomReply reply = new CLW_FobZoomReply();
		if (!loader.ImportFromString(data) || !loader.ReadValue("", reply))
		{
			FobZoomFailed("zoom reply is not valid JSON: " + data.Substring(0, Math.Min(data.Length(), 200)));
			return;
		}

		m_fLastOkAt = GetGame().GetWorld().GetWorldTime() * 0.001;
		if (!reply.ok)
		{
			FobZoomFailed("zoom reply error: " + reply.error);
			return;
		}

		if (reply.decline)
		{
			Print(string.Format("CLW: fob zoom %1: LLM declined area %2 after the zoom (%3)", m_sFaction, m_sZoomArea, reply.reason), LogLevel.NORMAL);
			if (CLW_FobPlanner.NoteFailure(m_sFaction, "declined (zoom)", reply.reason))
				CLW_FobDoctrine.Fallback(m_Rules, m_sFaction);

			return;
		}

		vector wish;
		string pickText;
		if (reply.pos && reply.pos.Count() >= 2)
		{
			wish = Vector(reply.pos[0], 0, reply.pos[1]);
			pickText = "pos " + wish.ToString();
			if (!CLW_MapDigest.InArea(m_sZoomArea, wish))
			{
				FobZoomRetryOrFail("the position " + wish.ToString() + " is outside area " + m_sZoomArea, wish);
				return;
			}
		}
		else if (!reply.cell.IsEmpty() && CLW_MapDigest.SubCellCenter(m_sZoomArea, reply.cell, wish))
		{
			pickText = "cell " + reply.cell;
		}
		else
		{
			FobZoomRetryOrFail("the reply names no valid sub-cell of area " + m_sZoomArea + " ('" + reply.cell + "') and no position", wish);
			return;
		}

		Print(string.Format("CLW: fob zoom %1: area %2 pick %3 -> wish %4 grid %5 (attempt %6, %7)", m_sFaction, m_sZoomArea, pickText, wish, CLW_Places.Grid(wish), m_iZoomAttempt, reply.reason), LogLevel.NORMAL);
		string why;
		CLW_FobSite site;
		if (!CLW_FobPlacer.Resolve(m_sFaction, wish, site, why))
		{
			FobZoomRetryOrFail(why, wish);
			return;
		}

		CLW_ActionCall call = NewCall("ESTABLISH_FOB");
		call.m_sReason = reply.reason;
		if (call.m_sReason.IsEmpty())
			call.m_sReason = m_sZoomIntent;
		call.SetString("site", site.m_sId);
		call.SetString("reason", call.m_sReason);
		string result;
		if (!CLW_ActionRegistry.Get().Run(call, result))
		{
			CLW_FobSites.RemoveSite(site.m_sId);
			Print(string.Format("CLW: RemoteCommander %1: ESTABLISH_FOB at %2 rejected: %3", m_sFaction, site.m_sId, result), LogLevel.WARNING);
			return;
		}

		CLW_Debug.Say(string.Format("%1 HQ (LLM): %2 (%3)", m_sFaction, result, call.m_sReason));
		Print(string.Format("CLW: RemoteCommander %1: %2 (%3)", m_sFaction, result, call.m_sReason), LogLevel.NORMAL);
	}

	// A rejected spot: one retry with the reason, then it counts as a failure.
	protected void FobZoomRetryOrFail(string why, vector wish)
	{
		if (m_iZoomAttempt < 2)
		{
			m_sZoomRetryReason = why;
			m_vZoomPrev = wish;
			string sendWhy;
			Print(string.Format("CLW: fob zoom %1: retry 2 of 2 for area %2 after: %3", m_sFaction, m_sZoomArea, why), LogLevel.NORMAL);
			if (SendFobZoom(sendWhy))
				return;

			FobZoomFailed("retry not sent: " + sendWhy);
			return;
		}

		FobZoomFailed("spot rejected twice: " + why);
	}

	protected void FobZoomFailed(string reason)
	{
		Print(string.Format("CLW: fob zoom %1: area %2 failed: %3", m_sFaction, m_sZoomArea, reason), LogLevel.WARNING);
		if (CLW_FobPlanner.NoteFailure(m_sFaction, "failed", reason))
			CLW_FobDoctrine.Fallback(m_Rules, m_sFaction);
	}

	// ---- SITREP ----

	// Rules SITREP of this faction (throttled by the store, forced when the war phase changed) and delivery to its players.
	protected void UpdateSitrep()
	{
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (!war)
			return;

		bool running = war.IsRunning();
		bool force = running != m_bSitrepRunning;
		m_bSitrepRunning = running;
		CLW_SitrepStore.UpdateRules(m_sFaction, m_Rules.GetBook(), CLW_GameMode.GetRegistry(), m_Rules.GetOrders(), force);
		CLW_SitrepStore.SyncPlayers();
	}

	// ---- fallback ----

	// Rule-commander fallback, throttled to RULES_INTERVAL (the first run is immediate).
	protected void ThinkRules()
	{
		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		if (now - m_fLastRulesAt < RULES_INTERVAL)
			return;
		m_fLastRulesAt = now;
		m_Rules.Think();
	}

	protected void Fallback(string reason)
	{
		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		if (reason != m_sLastReason || now - m_fLastReasonAt > LOG_INTERVAL)
		{
			m_sLastReason = reason;
			m_fLastReasonAt = now;
			Print(string.Format("CLW: RemoteCommander %1: fallback to rule commander: %2", m_sFaction, reason), LogLevel.WARNING);
		}

		ThinkRules();
	}
}

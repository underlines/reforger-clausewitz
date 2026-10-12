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

class CLW_OrdersReply
{
	ref array<ref CLW_OrderDto> orders = {};
	ref array<ref CLW_BuyDto> buy = {};
	ref array<ref CLW_MissionDto> missions = {};
	bool needManifest;	// the sidecar does not know the manifest hash of our post: resend the manifest
	bool keep;	// true with nothing else = "nothing changed, keep the current orders"
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
	static const string BASE_URL = "http://127.0.0.1:8788/";
	static const int TIMEOUT_SECONDS = 30;		// above the sidecar provider timeout (20 s), or slow LLM replies fall back to rules
	static const float STUCK_SECONDS = 45;		// a request in flight longer than this is considered lost
	static const float LOG_INTERVAL = 60;		// s: repeat the same fallback reason at most this often
	static const float RULES_INTERVAL = 25;		// s: the rule-commander fallback runs at most this often (we are polled every 10 s)
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

	void Init(string factionKey, CLW_ContactBook book, CLW_SquadRegistry registry)
	{
		m_sFaction = factionKey;
		m_Rules.Init(factionKey, book, registry);
	}

	string GetFaction()
	{
		return m_sFaction;
	}

	CLW_RuleCommander GetRules()
	{
		return m_Rules;
	}

	void Think()
	{
		if (!Replication.IsServer())
			return;

		UpdateSitrep();

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
			body = body.Substring(0, body.Length() - 1) + ",\"sitrep\":" + sitrepJson + "}";

		// recent debug lines (CLW_Debug ring) for the dashboard debug log; every line goes with both factions' posts, the sidecar dedupes
		int debugLast;
		string debugJson = CLW_Debug.GetJsonSince(m_iDebugSeq, debugLast);
		if (!debugJson.IsEmpty() && body.Length() > 2 && body.Length() + debugJson.Length() < 58000)
		{
			body = body.Substring(0, body.Length() - 1) + ",\"debug\":" + debugJson + "}";
			m_iDebugSeq = debugLast;
		}

		// protocol v3: the hash of the action manifest this situation was built for (the manifest itself goes to POST /manifest)
		string manifestHash = SyncManifest(now);
		if (!manifestHash.IsEmpty() && body.Length() > 2)
			body = body.Substring(0, body.Length() - 1) + ",\"manifest\":\"" + manifestHash + "\"}";

		if (squads <= 0)
			ThinkRules();	// still POST the situation below: the sidecar/dashboard must learn the faction has no squads left (it may still buy)

		RestContext ctx = GetGame().GetRestApi().GetContext(BASE_URL);
		if (!ctx)
		{
			Fallback("no REST context");
			return;
		}

		ctx.SetTimeout(TIMEOUT_SECONDS);
		ctx.SetHeaders("Content-Type,application/json");
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

		RestContext ctx = GetGame().GetRestApi().GetContext(BASE_URL);
		if (!ctx)
			return hash;

		ctx.SetTimeout(TIMEOUT_SECONDS);
		ctx.SetHeaders("Content-Type,application/json");
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

		// optional SITREP written by the model: independent of the orders (a "keep" reply may carry one)
		if (reply.sitrep)
		{
			CLW_Sitrep llmSitrep = reply.sitrep.ToSitrep(m_sFaction, war.GetElapsedSec());
			if (llmSitrep)
				CLW_SitrepStore.SetLlm(m_sFaction, llmSitrep);
		}

		bool hasOrders = reply.orders && !reply.orders.IsEmpty();
		bool hasBuy = reply.buy && !reply.buy.IsEmpty();
		bool hasMissions = reply.missions && !reply.missions.IsEmpty();
		if (!hasOrders && !hasBuy && !hasMissions)
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

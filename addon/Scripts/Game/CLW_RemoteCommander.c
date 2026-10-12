// LLM bridge (protocol v2): posts the faction situation to the local sidecar and executes the orders / purchases / mission
// offers it returns. Wraps the CLW_RuleCommander, which stays the fallback (timeout, error, empty or invalid reply).
// v1 replies (orders with pos, MOVE/ATTACK/DEFEND) are accepted too. Server only.

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
	bool keep;	// true with nothing else = "nothing changed, keep the current orders"
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

	// Called by the REST callback (main thread).
	void OnReply(bool ok, string data, string failure)
	{
		m_bInFlight = false;
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

	// ---- orders ----

	protected int ApplyOrders(notnull array<ref CLW_OrderDto> orders, out string summary)
	{
		array<string> done = {};
		int applied;
		foreach (CLW_OrderDto order : orders)
		{
			if (!order)
				continue;

			vector pos = vector.Zero;
			string why = ValidateOrder(order, done, pos);
			if (why.IsEmpty())
			{
				if (!m_Rules.ApplyOrder(order.squad, order.command, order.target, pos, order.reason, why))
				{
					if (why.IsEmpty())
						why = "not applied";
				}
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
			Print(string.Format("CLW: RemoteCommander %1: ORDER %2 %3 target='%4' pos=%5 (%6)", m_sFaction, order.command, order.squad, order.target, pos, order.reason), LogLevel.NORMAL);
		}

		return applied;
	}

	// Empty string = valid. pos is set when the order has a position (an objective id alone leaves it zero). Squad membership
	// and human-led squads are checked by ApplyOrder.
	protected string ValidateOrder(CLW_OrderDto order, notnull array<string> done, out vector pos)
	{
		if (order.squad.IsEmpty())
			return "no squad";
		if (done.Contains(order.squad))
			return "duplicate squad";
		CLW_ECommand command;
		if (!CLW_Command.FromName(order.command, command))
			return "unknown command";

		bool hasPos = order.pos && order.pos.Count() >= 2;
		if (hasPos)
		{
			for (int k = 0; k < 2; k++)
			{
				float v = order.pos[k];
				if (v != v || v > 100000 || v < -100000)
					return "pos not finite";
			}
			pos = Vector(order.pos[0], 0, order.pos[1]);
		}

		if (!order.target.IsEmpty())
		{
			CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
			if (points && points.Find(order.target))
				return "";	// objective id wins over a pos given with it

			if (!hasPos)
				return "unknown objective " + order.target;

			order.target = "";	// unknown id but a valid pos: use the pos
			return "";
		}

		if (!hasPos && command != CLW_ECommand.FALL_BACK)
			return "needs a target or pos";

		return "";
	}

	// ---- purchases ----

	protected int ApplyBuys(notnull array<ref CLW_BuyDto> buys, out string summary)
	{
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!spawner || !catalog)
		{
			Print(string.Format("CLW: RemoteCommander %1: buy ignored (no spawner / catalog)", m_sFaction), LogLevel.WARNING);
			return 0;
		}

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

			if (!catalog.Find(m_sFaction, buy.type))
			{
				Print(string.Format("CLW: RemoteCommander %1: buy rejected (unknown catalog type %2)", m_sFaction, buy.type), LogLevel.WARNING);
				continue;
			}

			int count = Math.Clamp(buy.count, 1, 3);
			for (int n = 0; n < count; n++)
			{
				string error;
				if (!spawner.Buy(m_sFaction, buy.type, "LLM: " + buy.reason, error))
				{
					Print(string.Format("CLW: RemoteCommander %1: buy %2 (%3/%4) refused: %5", m_sFaction, buy.type, n + 1, count, error), LogLevel.NORMAL);
					break;
				}

				applied++;
				if (!summary.IsEmpty())
					summary += ", ";
				summary += "buy " + buy.type;
				Print(string.Format("CLW: RemoteCommander %1: BUY %2 (%3/%4): %5", m_sFaction, buy.type, n + 1, count, buy.reason), LogLevel.NORMAL);
			}
		}

		return applied;
	}

	// ---- missions ----

	protected int ApplyMissions(notnull array<ref CLW_MissionDto> missions, out string summary)
	{
		int applied;
		array<string> seen = {};
		foreach (CLW_MissionDto mission : missions)
		{
			if (!mission)
				continue;

			string why;
			if (seen.Contains(mission.squad))
				why = "duplicate missions entry";
			else if (!m_Rules.IsCommandable(mission.squad))
				why = "unknown squad";
			else if (!m_Rules.IsHumanLed(mission.squad))
				why = "not human-led (missions are for human-led squads only)";
			else if (!mission.offers || mission.offers.IsEmpty())
				why = "no offers";

			if (!why.IsEmpty())
			{
				Print(string.Format("CLW: RemoteCommander %1: missions rejected for %2: %3", m_sFaction, mission.squad, why), LogLevel.WARNING);
				continue;
			}

			array<ref CLW_MissionOffer> offers = {};
			foreach (CLW_OfferDto dto : mission.offers)
			{
				if (!dto || offers.Count() >= MAX_OFFERS)
					continue;

				CLW_MissionOffer offer = ValidateOffer(dto);
				if (offer)
					offers.Insert(offer);
			}

			if (offers.IsEmpty())
			{
				Print(string.Format("CLW: RemoteCommander %1: missions for %2 dropped (no valid offer)", m_sFaction, mission.squad), LogLevel.WARNING);
				continue;
			}

			seen.Insert(mission.squad);
			CLW_MissionHook.Get().OnOffers(m_sFaction, mission.squad, offers);
			applied++;
			if (!summary.IsEmpty())
				summary += ", ";
			summary += mission.squad + " " + offers.Count() + " offer(s)";
			Print(string.Format("CLW: RemoteCommander %1: MISSION offers for %2: %3", m_sFaction, mission.squad, offers.Count()), LogLevel.NORMAL);
		}

		return applied;
	}

	protected CLW_MissionOffer ValidateOffer(CLW_OfferDto dto)
	{
		if (dto.kind != "CAPTURE" && dto.kind != "DEFEND" && dto.kind != "RECON" && dto.kind != "DESTROY")
		{
			Print(string.Format("CLW: RemoteCommander %1: offer rejected (bad kind %2)", m_sFaction, dto.kind), LogLevel.WARNING);
			return null;
		}

		CLW_MissionOffer offer = new CLW_MissionOffer();
		offer.m_sKind = dto.kind;
		offer.m_sSource = "llm";
		offer.m_sBrief = dto.brief;
		if (offer.m_sBrief.Length() > MAX_BRIEF)
			offer.m_sBrief = offer.m_sBrief.Substring(0, MAX_BRIEF);

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (!dto.target.IsEmpty() && points && points.Find(dto.target))
		{
			offer.m_sTarget = dto.target;
			return offer;
		}

		if (dto.pos && dto.pos.Count() >= 2)
		{
			float x = dto.pos[0];
			float z = dto.pos[1];
			if (x == x && z == z && Math.AbsFloat(x) < 100000 && Math.AbsFloat(z) < 100000)
			{
				offer.m_bHasPos = true;
				offer.m_vPos = Vector(x, 0, z);
				return offer;
			}
		}

		Print(string.Format("CLW: RemoteCommander %1: offer rejected (no valid target / pos, target='%2')", m_sFaction, dto.target), LogLevel.WARNING);
		return null;
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

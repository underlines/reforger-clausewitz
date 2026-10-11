// LLM bridge (M1): posts the faction situation to the local sidecar and executes the orders it returns.
// Wraps the CLW_RuleCommander, which stays the fallback (timeout, error, empty or invalid reply). Server only.

// Shape of the sidecar reply, filled by SCR_JsonLoadContext (field names = JSON keys).
class CLW_OrderDto
{
	string squad;
	string command;
	ref array<float> pos = {};
	string reason;
}

class CLW_OrdersReply
{
	ref array<ref CLW_OrderDto> orders = {};
	bool keep;	// optional: true with empty orders = "nothing changed, keep the current orders"
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
	static const int TIMEOUT_SECONDS = 15;
	static const float STUCK_SECONDS = 45;		// a request in flight longer than this is considered lost
	static const float LOG_INTERVAL = 60;		// s: repeat the same fallback reason at most this often

	protected ref CLW_RuleCommander m_Rules = new CLW_RuleCommander();
	protected ref CLW_RemoteCallback m_Callback;	// must stay referenced until the reply arrives
	protected string m_sFaction;
	protected bool m_bInFlight;
	protected float m_fSentAt;
	protected string m_sLastReason;
	protected float m_fLastReasonAt = -1000;
	protected float m_fLastKeepAt = -1000;

	void Init(string factionKey, CLW_ContactBook book, notnull array<string> squadNames)
	{
		m_sFaction = factionKey;
		m_Rules.Init(factionKey, book, squadNames);
	}

	void SetPatrolPoints(notnull array<vector> points)
	{
		m_Rules.SetPatrolPoints(points);
	}

	void Think()
	{
		if (!Replication.IsServer())
			return;

		if (!ENABLED)
		{
			m_Rules.Think();
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
		if (squads <= 0)
		{
			m_Rules.Think();
			return;
		}

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

		if (reply.keep && (!reply.orders || reply.orders.IsEmpty()))
		{
			float keepNow = GetGame().GetWorld().GetWorldTime() * 0.001;
			if (keepNow - m_fLastKeepAt > LOG_INTERVAL)
			{
				m_fLastKeepAt = keepNow;
				Print(string.Format("CLW: RemoteCommander %1: keeping current orders", m_sFaction), LogLevel.NORMAL);
			}
			return;
		}

		if (!reply.orders || reply.orders.IsEmpty())
		{
			Fallback("empty orders");
			return;
		}

		array<string> done = {};
		string summary = "";
		int applied = 0;
		foreach (CLW_OrderDto order : reply.orders)
		{
			if (!order)
				continue;

			string why = Validate(order, done);
			if (why.IsEmpty())
			{
				vector target = Vector(order.pos[0], 0, order.pos[1]);
				if (!m_Rules.ApplyOrder(order.squad, order.command, target, order.reason))
					why = "squad dead or unknown";
			}

			if (!why.IsEmpty())
			{
				Print(string.Format("CLW: RemoteCommander %1: order rejected (%2): squad=%3 command=%4", m_sFaction, why, order.squad, order.command), LogLevel.WARNING);
				continue;
			}

			done.Insert(order.squad);
			applied++;
			if (!summary.IsEmpty())
				summary += ", ";
			summary += order.squad + " " + order.command;
			Print(string.Format("CLW: RemoteCommander %1: %2 %3 -> %4 / %5 (%6)", m_sFaction, order.command, order.squad, Math.Round(order.pos[0]), Math.Round(order.pos[1]), order.reason), LogLevel.NORMAL);
		}

		if (applied == 0)
		{
			Fallback("no valid orders in reply");
			return;
		}

		m_sLastReason = "";
		CLW_Debug.Say(string.Format("%1 HQ (LLM): %2 order(s): %3", m_sFaction, applied, summary));
	}

	// Empty string = valid. Squad membership itself is checked by ApplyOrder.
	protected string Validate(CLW_OrderDto order, notnull array<string> done)
	{
		if (order.squad.IsEmpty())
			return "no squad";
		if (done.Contains(order.squad))
			return "duplicate squad";
		if (order.command != "MOVE" && order.command != "ATTACK" && order.command != "DEFEND")
			return "unknown command";
		if (!order.pos || order.pos.Count() < 2)
			return "pos needs [x,z]";
		for (int k = 0; k < 2; k++)
		{
			float v = order.pos[k];
			if (v != v || v > 100000 || v < -100000)
				return "pos not finite";
		}
		return "";
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

		m_Rules.Think();
	}
}

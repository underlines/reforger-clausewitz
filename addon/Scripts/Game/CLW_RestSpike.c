// REST spike: GET/POST with JSON body and custom headers against a local echo server
// (dev/tools/echo_server.py). Pure feasibility test for the LLM bridge.

class CLW_RestSpikeCallback : RestCallback
{
	protected string m_Tag;

	void CLW_RestSpikeCallback(string tag)
	{
		m_Tag = tag;
	}

	override void OnSuccess(string data, int dataSize)
	{
		PrintFormat("CLW: REST %1 OnSuccess http=%2 result=%3 size=%4 body=%5", m_Tag, GetHttpCode(), GetRestResult(), dataSize, data, level: LogLevel.NORMAL);
	}

	override void OnError(int errorCode)
	{
		PrintFormat("CLW: REST %1 OnError code=%2 http=%3 result=%4 body=%5", m_Tag, errorCode, GetHttpCode(), GetRestResult(), GetData(), level: LogLevel.NORMAL);
	}

	override void OnTimeout()
	{
		PrintFormat("CLW: REST %1 OnTimeout", m_Tag, level: LogLevel.NORMAL);
	}
}

class CLW_RestSpike
{
	static const string BASE_URL = "http://127.0.0.1:8787/";

	// Callbacks must stay referenced until the async result arrives.
	protected static ref CLW_RestSpikeCallback s_CbGet;
	protected static ref CLW_RestSpikeCallback s_CbPost;

	static void Run()
	{
		Print("CLW: REST spike start", LogLevel.NORMAL);
		DoGet();
		DoPost("{\"prompt\":\"hello\",\"n\":1}");
	}

	static void DoGet()
	{
		s_CbGet = new CLW_RestSpikeCallback("GET");
		RestContext ctx = GetGame().GetRestApi().GetContext(BASE_URL);
		if (!ctx)
		{
			Print("CLW: REST GET no context", LogLevel.ERROR);
			return;
		}
		ctx.SetTimeout(10);
		bool ok = ctx.SetHeaders("Authorization,Bearer clw-test-token,X-CLW-Test,spike");
		PrintFormat("CLW: REST GET SetHeaders ok=%1", ok, level: LogLevel.NORMAL);
		int id = ctx.GET(s_CbGet, "get?x=10&y=5");
		PrintFormat("CLW: REST GET sent id=%1", id, level: LogLevel.NORMAL);
	}

	static void DoPost(string json)
	{
		s_CbPost = new CLW_RestSpikeCallback("POST");
		RestContext ctx = GetGame().GetRestApi().GetContext(BASE_URL);
		if (!ctx)
		{
			Print("CLW: REST POST no context", LogLevel.ERROR);
			return;
		}
		ctx.SetTimeout(10);
		bool ok = ctx.SetHeaders("Content-Type,application/json,Authorization,Bearer clw-test-token,X-CLW-Test,spike");
		PrintFormat("CLW: REST POST SetHeaders ok=%1", ok, level: LogLevel.NORMAL);
		int id = ctx.POST(s_CbPost, "post", json);
		PrintFormat("CLW: REST POST sent id=%1", id, level: LogLevel.NORMAL);
	}
}

// Where the game finds the LLM sidecar and how it authenticates (every REST call to the sidecar goes through Context()).
// Base URL, first match wins:
//   1. command line  -clwSidecar http://host:8788/
//   2. $profile:Clausewitz/overrides.json  {"sidecarUrl": "http://host:8788/"}
//   3. default http://127.0.0.1:8788/ (sidecar on the same machine)
// Token (header X-CLW-Token, must equal the sidecar's gameToken), first match wins: -clwSidecarToken <token>, overrides.json "sidecarToken",
// none. Both are read once per process; the chosen URL is logged as "CLW: sidecar url ...".
class CLW_SidecarLink
{
	static const string DEFAULT_URL = "http://127.0.0.1:8788/";
	static const string CLI_URL = "clwSidecar";
	static const string CLI_TOKEN = "clwSidecarToken";

	protected static string s_sUrl;
	protected static string s_sToken;
	protected static bool s_bRead;

	// The sidecar base URL, always ending in "/".
	static string BaseUrl()
	{
		Read();
		return s_sUrl;
	}

	// The shared token of the game <-> sidecar channel ("" = none, no header is sent).
	static string Token()
	{
		Read();
		return s_sToken;
	}

	// A REST context on the sidecar with timeout, JSON content type and the token header (null when there is no REST context).
	static RestContext Context(int timeoutSeconds)
	{
		RestContext ctx = GetGame().GetRestApi().GetContext(BaseUrl());
		if (!ctx)
			return null;

		ctx.SetTimeout(timeoutSeconds);
		string token = Token();
		if (token.IsEmpty())
			ctx.SetHeaders("Content-Type,application/json");
		else
			ctx.SetHeaders("Content-Type,application/json,X-CLW-Token," + token);

		return ctx;
	}

	protected static void Read()
	{
		if (s_bRead)
			return;

		s_bRead = true;
		string fileUrl;
		string fileToken;
		if (FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
		{
			string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
			if (!text.IsEmpty())
			{
				string error;
				CLW_JsonNode root = CLW_JsonReader.Parse(text, error);
				if (root)
				{
					fileUrl = root.FieldText("sidecarUrl");
					fileToken = root.FieldText("sidecarToken");
				}
			}
		}

		fileUrl = fileUrl.Trim();
		fileToken = fileToken.Trim();
		string source = "default";
		string cli;
		if (System.GetCLIParam(CLI_URL, cli) && !cli.Trim().IsEmpty())
		{
			s_sUrl = cli.Trim();
			source = "command line -" + CLI_URL;
		}
		else if (!fileUrl.IsEmpty())
		{
			s_sUrl = fileUrl;
			source = "overrides.json sidecarUrl";
		}
		else
		{
			s_sUrl = DEFAULT_URL;
		}

		if (!s_sUrl.EndsWith("/"))
			s_sUrl = s_sUrl + "/";

		string cliToken;
		if (System.GetCLIParam(CLI_TOKEN, cliToken) && !cliToken.Trim().IsEmpty())
			s_sToken = cliToken.Trim();
		else
			s_sToken = fileToken;

		string tokenInfo = "no token";
		if (!s_sToken.IsEmpty())
			tokenInfo = "token set";
		Print(string.Format("CLW: sidecar url %1 (%2, %3)", s_sUrl, source, tokenInfo), LogLevel.NORMAL);
	}
}

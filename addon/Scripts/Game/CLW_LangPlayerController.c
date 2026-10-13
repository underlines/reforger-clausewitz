// Client -> server report of the game language, once on join (design: dev/notes/i18n-design.md section 3).
// The client sends WidgetManager.GetLanguage; the server stores it under the sender's identity (CLW_PlayerLang.OnClientReport).
// The sender is the controller's own player id, never a value from the client. Same RPC pattern as CLW_SitrepPlayerController.c.
modded class SCR_PlayerController
{
	protected static bool s_bClwLangReported;

	// Client side: report once (on the first controlled entity of the local player).
	void CLW_ReportLanguage()
	{
		if (s_bClwLangReported)
			return;

		s_bClwLangReported = true;
		string lang;
		WidgetManager.GetLanguage(lang);
		Print("CLW: lang client reports game language '" + lang + "'", LogLevel.NORMAL);
		if (lang.IsEmpty())
			return;

		if (Replication.IsServer())
		{
			CLW_PlayerLang.OnClientReport(GetPlayerId(), lang);
			return;
		}

		Rpc(CLW_LangRpcAsk_Report, lang);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_LangRpcAsk_Report(string lang)
	{
		if (lang.Length() > 16)
			return;

		CLW_PlayerLang.OnClientReport(GetPlayerId(), lang);
	}

	override void OnControlledEntityChanged(IEntity from, IEntity to)
	{
		super.OnControlledEntityChanged(from, to);

		if (this != GetGame().GetPlayerController())
			return;

		if (to)
			CLW_ReportLanguage();
	}
}

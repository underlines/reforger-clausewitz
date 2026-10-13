// RPC glue of the player requests and reports (M3 phase Q), same pattern as CLW_SitrepPlayerController.c.
// Client -> server: /request and the terminal actions ("<kind> [arg]"), /report. Server -> owner: the answer (hint + chat line) and the
// request costs for the terminal action labels. The listen-server host never uses the RPCs (the service calls the local code directly).
modded class SCR_PlayerController
{
	protected static bool s_bClwRequestChatRegistered;

	// ---- client -> server ----

	// args = "<kind> [arg]" (chat text after /request, or the kind of a terminal action); "" = the menu.
	void CLW_SendRequest(string args, vector terminalPos = vector.Zero)
	{
		if (Replication.IsServer())
		{
			CLW_RequestService.Chat(GetPlayerId(), args, terminalPos);
			return;
		}

		Rpc(CLW_RpcAsk_Request, args, terminalPos);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_RpcAsk_Request(string args, vector terminalPos)
	{
		CLW_RequestService.Chat(GetPlayerId(), args, terminalPos);	// the sender: never a player id from the client
	}

	void CLW_SendReport(string args)
	{
		if (Replication.IsServer())
		{
			CLW_Reports.Chat(GetPlayerId(), args);
			return;
		}

		Rpc(CLW_RpcAsk_Report, args);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_RpcAsk_Report(string args)
	{
		CLW_Reports.Chat(GetPlayerId(), args);
	}

	// M3 phase A: /hq <free text> to the HQ agent turn (CLW_HqChat).
	void CLW_SendHq(string text)
	{
		if (Replication.IsServer())
		{
			CLW_HqChat.Chat(GetPlayerId(), text);
			return;
		}

		Rpc(CLW_RpcAsk_Hq, text);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void CLW_RpcAsk_Hq(string text)
	{
		CLW_HqChat.Chat(GetPlayerId(), text);	// the sender: never a player id from the client
	}

	// ---- server -> owning client ----

	void CLW_RequestAnswer(string text, bool hint)
	{
		Rpc(CLW_RpcAsk_Answer, text, hint);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_Answer(string text, bool hint)
	{
		CLW_RequestClient.Show(text, hint);
	}

	void CLW_RequestCosts(string wire)
	{
		Rpc(CLW_RpcAsk_Costs, wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_Costs(string wire)
	{
		CLW_RequestClient.SetCosts(wire);
	}

	static void CLW_MarkRequestChatRegistered()
	{
		s_bClwRequestChatRegistered = true;
	}

	override void OnControlledEntityChanged(IEntity from, IEntity to)
	{
		super.OnControlledEntityChanged(from, to);

		if (this != GetGame().GetPlayerController())
			return;

		if (!s_bClwRequestChatRegistered)
			CLW_RequestClient.RegisterChatCommands();
	}
}

// Client side: chat commands, answers, cost cache for the action labels.
class CLW_RequestClient
{
	protected static ref map<string, string> s_mCosts = new map<string, string>();

	static void RegisterChatCommands()
	{
		SCR_ChatPanelManager chat = SCR_ChatPanelManager.GetInstance();
		if (!chat)
			return;

		ChatCommandInvoker request = chat.GetCommandInvoker("request");
		ChatCommandInvoker report = chat.GetCommandInvoker("report");
		if (!request || !report)
			return;

		request.Insert(OnRequestChat);
		report.Insert(OnReportChat);
		ChatCommandInvoker hq = chat.GetCommandInvoker("hq");
		if (hq)
			hq.Insert(OnHqChat);

		SCR_PlayerController.CLW_MarkRequestChatRegistered();
		Print("CLW: chat commands /request, /report and /hq registered", LogLevel.NORMAL);
	}

	static void OnRequestChat(SCR_ChatPanel panel, string data)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print("CLW: chat /request " + data, LogLevel.NORMAL);
		controller.CLW_SendRequest(data);
	}

	static void OnHqChat(SCR_ChatPanel panel, string data)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print("CLW: chat /hq " + data, LogLevel.NORMAL);
		controller.CLW_SendHq(data);
	}

	static void OnReportChat(SCR_ChatPanel panel, string data)
	{
		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller)
			return;

		Print("CLW: chat /report " + data, LogLevel.NORMAL);
		controller.CLW_SendReport(data);
	}

	// Chat line, plus a hint for the answers.
	static void Show(string text, bool hint)
	{
		string shown = CLW_Loc.Resolve(text);	// the server sends a CLW_Loc wire
		CLW_Debug.Show(shown);
		if (hint)
			SCR_HintManagerComponent.ShowCustomHint(shown, "HQ", 15);
	}

	// "vehicle=60;squad=120;resupply=20;" from the server (the force catalog only exists there).
	static void SetCosts(string wire)
	{
		s_mCosts.Clear();
		array<string> parts = {};
		wire.Split(";", parts, true);
		foreach (string part : parts)
		{
			int eq = part.IndexOf("=");
			if (eq <= 0)
				continue;

			s_mCosts.Set(part.Substring(0, eq), part.Substring(eq + 1, part.Length() - eq - 1));
		}
	}

	// Action label with the cost: "Request squad (120 supplies)".
	static string ActionLabel(string kind)
	{
		string cost;
		if (s_mCosts.Find(kind, cost))
			return CLW_Loc.F("#CLW-Req_ActionCost", CLW_RequestKind.LabelKey(kind), cost);

		return CLW_Loc.F("#CLW-Req_Action", CLW_RequestKind.LabelKey(kind));
	}
}

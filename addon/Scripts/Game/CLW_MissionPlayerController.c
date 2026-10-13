// Mission popups: server -> owning client (phase P4), same pattern as CLW_DebugPlayerController.c.
// The listen-server host never uses the RPC: CLW_MissionBoard.SendPopup calls CLW_MsnShowLocal directly.
modded class SCR_PlayerController
{
	// Server side: show a popup (title, second line, sound event name or "") to this controller's owner. title / text: plain or a
	// CLW_Loc wire.
	void CLW_MsnSend(string title, string text, string sound)
	{
		Rpc(CLW_MsnRpcAsk_Show, title, text, sound);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_MsnRpcAsk_Show(string title, string text, string sound)
	{
		CLW_MsnShowLocal(title, text, sound);
	}

	// Local display (client, or the listen-server host).
	static void CLW_MsnShowLocal(string title, string text, string sound)
	{
		SCR_PopUpNotification popup = SCR_PopUpNotification.GetInstance();
		if (!popup)
		{
			CLW_Debug.Show(CLW_Loc.Resolve(title) + ": " + CLW_Loc.Resolve(text));	// no popup system: chat line instead
			return;
		}

		// title / text are plain text or CLW_Loc wires (key + params from the server), formatted here in the player language
		// PopupMsg(text, duration, text2, prio, param1..4, text2param1..4, sound)  [verify] sound is a SCR_SoundEvent name
		popup.PopupMsg(CLW_Loc.Resolve(title), 8, CLW_Loc.Resolve(text), -1, "", "", "", "", "", "", "", "", sound);
	}
}

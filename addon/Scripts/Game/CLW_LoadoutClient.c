// Client cache of "which factions has the local player a saved custom loadout for" (own flag, independent of vanilla's saved-loadout state).
// Why: Workshop loadout editors (WCS_LoadoutEditor, Bacon, JPLE ...) replace the vanilla single-slot store behind
// SCR_ArsenalManagerComponent.GetLocalPlayerLoadoutAvailable(), so that flag can stay false although the server holds the kit. The server is
// the truth (CLW_CustomLoadouts); it sends the set of faction keys of the player's records to the owning client, which keeps them here.
// Wire (server -> owner): "<factionKey>=<itemCount>;" per record, "" = none. Sent on record stored, faction set, audit/connect, custom spawn.
// Log prefix "CLW: loadouts state".

modded class SCR_PlayerController
{
	// ---- server -> owning client ----

	void CLW_SendLoadoutState(string wire)
	{
		if (this == GetGame().GetPlayerController())
		{
			CLW_LoadoutClient.SetState(wire);	// listen-server host: the controller is local, no RPC
			return;
		}

		Rpc(CLW_RpcAsk_LoadoutState, wire);
	}

	[RplRpc(RplChannel.Reliable, RplRcver.Owner)]
	protected void CLW_RpcAsk_LoadoutState(string wire)
	{
		CLW_LoadoutClient.SetState(wire);
	}
}

class CLW_LoadoutClient
{
	protected static ref map<string, int> s_mFactions = new map<string, int>();	// faction key -> item count of the saved kit

	// ---- server ----

	// Sends the player's record set to the player's own client (no-op for a bot / unknown controller).
	static void Push(int playerId)
	{
		if (!Replication.IsServer() || playerId <= 0)
			return;

		PlayerManager players = GetGame().GetPlayerManager();
		if (!players || !players.IsPlayerConnected(playerId))
			return;

		SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
		if (!controller)
			return;

		string wire = CLW_CustomLoadouts.FactionsWire(CLW_CustomLoadouts.IdentityOf(playerId));
		controller.CLW_SendLoadoutState(wire);
		Print(string.Format("CLW: loadouts state sent to player %1: '%2'", playerId, wire), LogLevel.NORMAL);
	}

	// Push after a delay (the controller / identity may not be ready right after connect or a faction change).
	static void PushDelayed(int playerId, int delayMs)
	{
		GetGame().GetCallqueue().CallLater(Push, delayMs, false, playerId);
	}

	// ---- client ----

	static void SetState(string wire)
	{
		s_mFactions.Clear();
		array<string> parts = {};
		wire.Split(";", parts, true);
		foreach (string part : parts)
		{
			int eq = part.IndexOf("=");
			if (eq <= 0)
				continue;

			s_mFactions.Set(part.Substring(0, eq), part.Substring(eq + 1, part.Length() - eq - 1).ToInt());
		}

		Print("CLW: loadouts state received on client: '" + wire + "'", LogLevel.NORMAL);
	}

	// The local player has a saved custom loadout for this faction (as far as the server told us).
	static bool Has(string factionKey)
	{
		return !factionKey.IsEmpty() && s_mFactions.Contains(factionKey);
	}

	// Item count of the saved kit for the faction, 0 if none.
	static int ItemCount(string factionKey)
	{
		int count;
		s_mFactions.Find(factionKey, count);
		return count;
	}
}

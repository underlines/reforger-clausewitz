// Save hook of the player custom loadout (loadouts work package G2, R1). The vanilla "Save loadout" action of every arsenal box
// (SCR_SaveArsenalLoadout on ArsenalBox_Base.et and all its children, vanilla / RHS / WCS / ours) ends in
// SCR_ArsenalManagerComponent.SetPlayerArsenalLoadout on the server. The modded override below calls super first (vanilla decides whether the
// save is allowed and stores its string), then copies what vanilla stored into our record (CLW_CustomLoadouts: one per player per faction, file
// player-loadouts.json) and POSTs it to the sidecar (POST /loadouts/player, header X-CLW-Token from overrides.json "sidecarToken").
// Only while a Clausewitz war runs (CLW_ArsenalPool.IsActive). The kit stored is the VANILLA string, byte for byte.
//
// Restore: the vanilla store holds one loadout per player in memory only. When the server sets a player's faction (SCR_BaseGameMode
// .OnPlayerFactionSet_S) the record of that faction is put back into the vanilla slot, so the spawn screen offers it after a restart or a
// faction switch; with no record for that faction, a vanilla slot of another faction is cleared (vanilla would drop it at spawn anyway).
// Log prefix "CLW: loadouts" (saved / restore / sidecar).

// Reply of POST /loadouts/player (the answer only matters for the log).
class CLW_LoadoutPostCallback : RestCallback
{
	bool m_bDone;
	string m_sLabel;

	override void OnSuccess(string data, int dataSize)
	{
		m_bDone = true;
		Print("CLW: loadouts sidecar " + m_sLabel + ": ok", LogLevel.NORMAL);
	}

	override void OnError(int errorCode)
	{
		m_bDone = true;
		Print(string.Format("CLW: loadouts sidecar %1: failed (code=%2 http=%3 result=%4), the kit stays in the game file", m_sLabel, errorCode, GetHttpCode(), GetRestResult()), LogLevel.WARNING);
	}

	override void OnTimeout()
	{
		m_bDone = true;
		Print("CLW: loadouts sidecar " + m_sLabel + ": timeout, the kit stays in the game file", LogLevel.WARNING);
	}
}

class CLW_ArsenalSaveHook
{
	static const int TIMEOUT_SECONDS = 5;
	static const int RESTORE_DELAY_MS = 500;

	protected static ref array<ref CLW_LoadoutPostCallback> s_aCallbacks = {};	// POSTs in flight (the engine keeps no reference to a callback)
	protected static string s_sToken;
	protected static bool s_bTokenRead;

	// The shared token of the game <-> sidecar channel: overrides.json key "sidecarToken", "" when absent (nothing is sent then).
	static string Token()
	{
		if (s_bTokenRead)
			return s_sToken;

		s_bTokenRead = true;
		if (!FileIO.FileExists(CLW_CatalogOverrides.FILE_PATH))
			return s_sToken;

		string text = SCR_FileIOHelper.GetFileStringContent(CLW_CatalogOverrides.FILE_PATH, false);
		if (text.IsEmpty())
			return s_sToken;

		string error;
		CLW_JsonNode root = CLW_JsonReader.Parse(text, error);
		if (root)
			s_sToken = root.FieldText("sidecarToken");

		return s_sToken;
	}

	// The common REST setup for the loadout endpoints (null when there is no REST context).
	static RestContext Context()
	{
		RestContext ctx = GetGame().GetRestApi().GetContext(CLW_RemoteCommander.BASE_URL);
		if (!ctx)
			return null;

		ctx.SetTimeout(TIMEOUT_SECONDS);
		string token = Token();
		if (token.IsEmpty())
			ctx.SetHeaders("Content-Type,application/json");
		else
			ctx.SetHeaders("Content-Type,application/json,X-CLW-Token," + token);

		return ctx;
	}

	// POST one record to the sidecar (fire and forget, the file is the source of truth).
	static void PostRecord(notnull CLW_CustomLoadoutRec rec)
	{
		for (int i = s_aCallbacks.Count() - 1; i >= 0; i--)
		{
			if (s_aCallbacks[i].m_bDone)
				s_aCallbacks.Remove(i);
		}

		RestContext ctx = Context();
		if (!ctx)
		{
			Print("CLW: loadouts sidecar: no REST context, " + rec.identityId + " " + rec.faction + " not posted", LogLevel.WARNING);
			return;
		}

		CLW_LoadoutPostCallback callback = new CLW_LoadoutPostCallback();
		callback.m_sLabel = "POST /loadouts/player " + rec.identityId + " " + rec.faction;
		s_aCallbacks.Insert(callback);
		if (ctx.POST(callback, "loadouts/player", CLW_CustomLoadouts.RecordJson(rec)) < 0)
		{
			callback.m_bDone = true;
			Print("CLW: loadouts sidecar: " + callback.m_sLabel + " not sent", LogLevel.WARNING);
		}
	}

	// Called by the modded arsenal manager after vanilla handled a save (server).
	static void OnSaved(int playerId)
	{
		if (!CLW_ArsenalPool.IsActive())
			return;

		string vanillaKit = CLW_ArsenalSaveHook.VanillaKit(playerId);
		if (vanillaKit.IsEmpty())
			return;	// cleared, or vanilla refused and holds nothing

		string factionKey = CLW_Kits.FactionOf(vanillaKit);
		if (factionKey.IsEmpty() || factionKey == SCR_PlayerArsenalLoadout.ARSENALLOADOUT_FACTIONKEY_NONE)
		{
			Print("CLW: loadouts save of player " + playerId + " ignored, the kit has no faction", LogLevel.WARNING);
			return;
		}

		CLW_CustomLoadoutRec existing = CLW_CustomLoadouts.Get(CLW_CustomLoadouts.IdentityOf(playerId), factionKey);
		if (existing && existing.loadout == vanillaKit)
			return;	// vanilla refused the new save (the old kit is still the stored one) or nothing changed

		StoreKit(playerId, factionKey, vanillaKit, "vanilla");
	}

	// The one store path (vanilla save hook and CLW_SaveLoadoutAction): record in CLW_CustomLoadouts + POST to the sidecar. Null if not stored.
	protected static CLW_CustomLoadoutRec StoreKit(int playerId, string factionKey, string kit, string via)
	{
		CLW_CustomLoadoutRec rec = CLW_CustomLoadouts.Store(playerId, factionKey, kit);
		if (!rec)
			return null;

		Print(string.Format("CLW: loadouts saved %1 %2 items=%3 status=%4 missing=%5 via=%6", rec.identityId, rec.faction, rec.items.Count(), rec.status, rec.missing.Count(), via), LogLevel.NORMAL);
		PostRecord(rec);
		return rec;
	}

	// Server: save the kit the character wears (CLW_SaveLoadoutAction). Captures, refuses a kit invalid for the player faction (hint),
	// stores it like the vanilla save and puts it into the vanilla saved slot so the spawn screen offers it.
	static void StoreFromCharacter(int playerId, IEntity character, SCR_ArsenalComponent arsenal, string via)
	{
		if (!CLW_ArsenalPool.IsActive() || !character || !arsenal)
			return;

		SCR_ChimeraCharacter chimera = SCR_ChimeraCharacter.Cast(character);
		Faction userFaction;
		if (chimera)
			userFaction = chimera.GetFaction();

		if (!userFaction || !CLW_LoadLoadout.ArsenalAllows(arsenal, userFaction))
		{
			Print("CLW: loadouts save of player " + playerId + " refused, no faction or the arsenal serves another one", LogLevel.NORMAL);
			return;
		}

		string factionKey = userFaction.GetFactionKey();
		string kit = CLW_Kits.Capture(character);
		if (kit.IsEmpty())
		{
			Print("CLW: loadouts save of player " + playerId + " failed, the kit could not be captured", LogLevel.WARNING);
			return;
		}

		array<string> missing = {};
		if (CLW_Kits.Validate(kit, factionKey, missing) == CLW_Kits.STATUS_INVALID)
		{
			CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Loadout_SaveRefused", CLW_Loc.N(missing.Count())), true);
			Print(string.Format("CLW: loadouts save of player %1 refused, %2 items unavailable for %3", playerId, missing.Count(), factionKey), LogLevel.NORMAL);
			return;
		}

		CLW_CustomLoadoutRec rec = StoreKit(playerId, factionKey, kit, via);
		if (!rec)
			return;

		SCR_ArsenalManagerComponent manager;
		if (SCR_ArsenalManagerComponent.GetArsenalManager(manager))
			manager.CLW_RestoreLoadout(playerId, factionKey, rec.loadout);

		CLW_RequestService.Answer(playerId, CLW_Loc.W("#CLW-Loadout_Saved", CLW_Loc.N(rec.items.Count())), true);
	}

	// The kit vanilla currently holds for the player ("" if none).
	static string VanillaKit(int playerId)
	{
		SCR_ArsenalManagerComponent manager;
		if (!SCR_ArsenalManagerComponent.GetArsenalManager(manager))
			return "";

		SCR_ArsenalPlayerLoadout stored;
		if (!manager.GetPlayerArsenalLoadout(SCR_PlayerIdentityUtils.GetPlayerIdentityId(playerId), stored) || !stored)
			return "";

		return stored.loadout;
	}

	// The server set the faction of a player: bring the vanilla slot in line with our record of that faction.
	static void OnFactionSet(SCR_PlayerFactionAffiliationComponent factionComponent, Faction faction)
	{
		if (!faction || !factionComponent || !Replication.IsServer() || !CLW_ArsenalPool.IsActive())
			return;

		PlayerController controller = factionComponent.GetPlayerController();
		if (!controller)
			return;

		GetGame().GetCallqueue().CallLater(RestoreDelayed, RESTORE_DELAY_MS, false, controller.GetPlayerId(), faction.GetFactionKey());
	}

	protected static void RestoreDelayed(int playerId, string factionKey)
	{
		if (playerId <= 0 || !GetGame().GetPlayerManager().IsPlayerConnected(playerId))
			return;

		SCR_ArsenalManagerComponent manager;
		if (!SCR_ArsenalManagerComponent.GetArsenalManager(manager))
			return;

		CLW_CustomLoadoutRec rec = CLW_CustomLoadouts.Get(CLW_CustomLoadouts.IdentityOf(playerId), factionKey);
		string vanillaKit = VanillaKit(playerId);
		if (rec)
		{
			if (vanillaKit == rec.loadout)
				return;

			bool done = manager.CLW_RestoreLoadout(playerId, factionKey, rec.loadout);
			Print(string.Format("CLW: loadouts restore player %1 faction %2: kit of %3 items put into the vanilla slot (ok=%4, vanilla held %5 chars)", playerId, factionKey, rec.items.Count(), done, vanillaKit.Length()), LogLevel.NORMAL);
			return;
		}

		if (!vanillaKit.IsEmpty() && CLW_Kits.FactionOf(vanillaKit) != factionKey)
		{
			manager.SetPlayerArsenalLoadout(playerId, null, null, SCR_EArsenalSupplyCostType.RESPAWN_COST);
			Print(string.Format("CLW: loadouts restore player %1 faction %2: no record, the vanilla slot of another faction was cleared", playerId, factionKey), LogLevel.NORMAL);
		}
	}
}

modded class SCR_ArsenalManagerComponent
{
	override void SetPlayerArsenalLoadout(int playerId, GameEntity characterEntity, SCR_ArsenalComponent arsenalComponent, SCR_EArsenalSupplyCostType arsenalSupplyType)
	{
		super.SetPlayerArsenalLoadout(playerId, characterEntity, arsenalComponent, arsenalSupplyType);
		if (characterEntity)
			CLW_ArsenalSaveHook.OnSaved(playerId);
	}

	// Puts a kit into the vanilla saved-loadout slot of a player (what the persistence deserializer does with a stored loadout). The preview
	// data (clothing / weapon list of the spawn screen) is left empty; cost, faction and the kit string are set. Server only.
	bool CLW_RestoreLoadout(int playerId, string factionKey, string loadoutString)
	{
		if (!GetGameMode().IsMaster() || loadoutString.IsEmpty())
			return false;

		FactionManager factionManager = GetGame().GetFactionManager();
		if (!factionManager)
			return false;

		SCR_Faction faction = SCR_Faction.Cast(factionManager.GetFactionByKey(factionKey));
		if (!faction)
			return false;

		SCR_ArsenalPlayerLoadout restored = new SCR_ArsenalPlayerLoadout();
		restored.loadout = loadoutString;
		ComputeSuppliesCost(faction, restored, SCR_EArsenalSupplyCostType.RESPAWN_COST);
		restored.loadoutData = new SCR_PlayerLoadoutData();
		restored.loadoutData.LoadoutCost = restored.suppliesCost;
		restored.loadoutData.FactionIndex = factionManager.GetFactionIndex(faction);

		DoSetPlayerLoadout(SCR_PlayerIdentityUtils.GetPlayerIdentityId(playerId), restored);
		SyncPlayerloadout(playerId, restored);
		return true;
	}
}

modded class SCR_BaseGameMode
{
	override void OnPlayerFactionSet_S(SCR_PlayerFactionAffiliationComponent factionComponent, Faction faction)
	{
		super.OnPlayerFactionSet_S(factionComponent, faction);
		CLW_ArsenalSaveHook.OnFactionSet(factionComponent, faction);
	}
}

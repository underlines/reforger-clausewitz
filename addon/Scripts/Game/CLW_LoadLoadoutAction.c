// "Load my loadout" on every arsenal box (loadouts work package G2, R1). The action sits on the vanilla ArsenalBox_Base.et (our override in
// addon/Prefabs/Props/Military/Arsenal/ArsenalBoxes/), next to the vanilla "Save loadout" action, so it shows on vanilla, RHS, WCS and our
// HQ / FOB boxes that inherit the action list. Like the vanilla save action it is NOT local-effect-only: PerformAction runs on the server
// (every other machine returns at once); CanBeShown / CanBePerformed run on the client, which only knows what vanilla replicates to it
// (SCR_ArsenalManagerComponent: "has a saved loadout" and its faction index), so the server re-checks everything.
// Server flow (CLW_LoadLoadout.Run): record of identity + current faction (CLW_CustomLoadouts, put into the vanilla slot on faction pick)
// -> arsenal must be of the player's faction or a friendly one -> CLW_Kits.Validate (invalid = refused, degraded = unavailable items dropped)
// -> supplies: kit cost with the arsenal's cost type against the arsenal's own supply consumer, refused when too low -> CLW_Kits.Apply
// (whole gear replaced, old gear deleted) -> charge -> hint to the player. Only while a Clausewitz war runs.
// Log prefix "CLW: loadouts load".

class CLW_LoadLoadoutAction : ScriptedUserAction
{
	protected SCR_ArsenalComponent m_Arsenal;

	// No war-state gate here: CLW_ArsenalPool.IsActive is not reliable on clients; the server re-checks everything in CLW_LoadLoadout.Run.
	override bool CanBeShownScript(IEntity user)
	{
		SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(user);
		if (!character)
			return false;

		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller || controller.IsPossessing())
			return false;

		return FindArsenal() != null;
	}

	override bool CanBePerformedScript(IEntity user)
	{
		SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(user);
		if (!character)
			return false;

		SCR_ArsenalComponent arsenal = FindArsenal();
		if (!arsenal || !arsenal.IsArsenalEnabled())
			return false;

		Faction userFaction = character.GetFaction();
		if (!CLW_LoadLoadout.ArsenalAllows(arsenal, userFaction))
		{
			SetCannotPerformReason(CLW_Loc.Fmt("#CLW-Loadout_WrongArsenal"));
			return false;
		}

		if (!HasSavedLoadoutFor(userFaction))
		{
			SetCannotPerformReason(CLW_Loc.Fmt("#CLW-Loadout_NoneSaved"));
			return false;
		}

		return true;
	}

	override bool GetActionNameScript(out string outName)
	{
		outName = CLW_Loc.Fmt("#CLW-Loadout_Load");
		return true;
	}

	override void PerformAction(IEntity pOwnerEntity, IEntity pUserEntity)
	{
		if (!Replication.IsServer())
			return;

		int playerId = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(pUserEntity);
		if (playerId <= 0)
			return;

		CLW_LoadLoadout.Run(playerId, pOwnerEntity, pUserEntity);
	}

	// The arsenal of this box (the component sits on the box itself or in its hierarchy).
	protected SCR_ArsenalComponent FindArsenal()
	{
		if (m_Arsenal)
			return m_Arsenal;

		// self, parent, else the nearest arsenal within 4 m (the action may sit on the CLW_ArsenalActions helper next to a pack box)
		m_Arsenal = CLW_ArsenalFind.Of(GetOwner());
		return m_Arsenal;
	}

	// Client view: vanilla replicates "the local player has a saved loadout" and the faction index it was saved for. Our restore puts the
	// record of the current faction into that slot when the faction is picked, so the slot answers "is there a kit for this faction".
	protected bool HasSavedLoadoutFor(Faction userFaction)
	{
		SCR_ArsenalManagerComponent manager;
		if (!SCR_ArsenalManagerComponent.GetArsenalManager(manager) || !manager.GetLocalPlayerLoadoutAvailable())
			return false;

		if (!userFaction || !manager.m_LocalPlayerLoadoutData)
			return true;	// no data to compare: let the server decide

		FactionManager factionManager = GetGame().GetFactionManager();
		if (!factionManager)
			return true;

		return manager.m_LocalPlayerLoadoutData.FactionIndex == factionManager.GetFactionIndex(userFaction);
	}
}

class CLW_LoadLoadout
{
	// An arsenal serves the player's own faction and factions friendly to it; an arsenal without a faction serves everyone.
	static bool ArsenalAllows(SCR_ArsenalComponent arsenal, Faction userFaction)
	{
		if (!userFaction)
			return false;

		SCR_Faction arsenalFaction = arsenal.GetAssignedFaction();
		if (!arsenalFaction || arsenalFaction == userFaction)
			return true;

		SCR_Faction own = SCR_Faction.Cast(userFaction);
		if (!own)
			return false;

		return arsenalFaction.DoCheckIfFactionFriendly(own) || own.DoCheckIfFactionFriendly(arsenalFaction);
	}

	// Server: the whole load. Every refusal tells the player why and logs it.
	static void Run(int playerId, IEntity arsenalEntity, IEntity character)
	{
		string tag = "load player " + playerId;
		if (!CLW_ArsenalPool.IsActive())
			return;

		SCR_ChimeraCharacter chimera = SCR_ChimeraCharacter.Cast(character);
		Faction userFaction;
		if (chimera)
			userFaction = chimera.GetFaction();

		if (!userFaction)
		{
			Print("CLW: loadouts " + tag + ": refused, the character has no faction", LogLevel.NORMAL);
			return;
		}

		string factionKey = userFaction.GetFactionKey();
		SCR_ArsenalComponent arsenal = CLW_ArsenalFind.Of(arsenalEntity);

		if (!arsenal || !arsenal.IsArsenalEnabled())
		{
			Print("CLW: loadouts " + tag + ": refused, no enabled arsenal on " + arsenalEntity.ToString(), LogLevel.NORMAL);
			return;
		}

		if (!ArsenalAllows(arsenal, userFaction))
		{
			Tell(playerId, CLW_Loc.W("#CLW-Loadout_WrongArsenal"));
			Print("CLW: loadouts " + tag + ": refused, the arsenal belongs to another faction than " + factionKey, LogLevel.NORMAL);
			return;
		}

		CLW_CustomLoadoutRec rec = CLW_CustomLoadouts.Get(CLW_CustomLoadouts.IdentityOf(playerId), factionKey);
		if (!rec)
		{
			Tell(playerId, CLW_Loc.W("#CLW-Loadout_NoneSaved"));
			Print("CLW: loadouts " + tag + ": refused, no saved loadout for faction " + factionKey, LogLevel.NORMAL);
			return;
		}

		array<string> missing = {};
		string status = CLW_Kits.Validate(rec.loadout, factionKey, missing);
		if (status == CLW_Kits.STATUS_INVALID)
		{
			Tell(playerId, CLW_Loc.W("#CLW-Loadout_Invalid"));
			Print(string.Format("CLW: loadouts %1: refused, the saved kit is invalid for %2 (%3 items unavailable)", tag, factionKey, missing.Count()), LogLevel.NORMAL);
			return;
		}

		// supplies: the arsenal's own consumer, like taking the items one by one
		int cost = CLW_Kits.CostAt(rec.loadout, factionKey, arsenal.GetSupplyCostType());
		SCR_ResourceConsumer consumer;
		if (cost > 0 && arsenal.IsArsenalUsingSupplies())
		{
			SCR_ResourceComponent resources = SCR_ResourceComponent.FindResourceComponent(arsenal.GetOwner());
			if (resources)
				consumer = resources.GetConsumer(EResourceGeneratorID.DEFAULT, EResourceType.SUPPLIES);
		}

		if (consumer)
		{
			float available = consumer.GetAggregatedResourceValue();
			if (available < cost)
			{
				Tell(playerId, CLW_Loc.W("#CLW-Loadout_NoSupplies", CLW_Loc.N(cost), CLW_Loc.N(Math.Round(available))));
				Print(string.Format("CLW: loadouts %1: refused, the kit costs %2 supplies and the arsenal has %3", tag, cost, available), LogLevel.NORMAL);
				return;
			}
		}

		array<string> unavailable = {};
		if (!CLW_Kits.Apply(character, rec.loadout, tag, unavailable))
		{
			Tell(playerId, CLW_Loc.W("#CLW-Loadout_Failed"));
			Print("CLW: loadouts " + tag + ": the kit could not be applied", LogLevel.WARNING);
			return;
		}

		float charged;
		if (consumer && cost > 0)
		{
			consumer.RequestConsumtion(cost);
			charged = cost;
		}

		if (unavailable.IsEmpty())
			Tell(playerId, CLW_Loc.W("#CLW-Loadout_Loaded", CLW_Loc.N(Math.Round(charged))));
		else
			Tell(playerId, CLW_Loc.W("#CLW-Loadout_LoadedPartial", CLW_Loc.N(unavailable.Count()), CLW_Loc.N(Math.Round(charged))));

		Print(string.Format("CLW: loadouts %1 done: faction=%2 status=%3 unavailable=%4 cost=%5 charged=%6 supplies=%7", tag, factionKey, status, unavailable.Count(), cost, charged, consumer != null), LogLevel.NORMAL);
	}

	// Hint + chat line to one player (the same channel as the /request answers; 'text' is a CLW_Loc wire).
	protected static void Tell(int playerId, string text)
	{
		CLW_RequestService.Answer(playerId, text, true);
	}
}

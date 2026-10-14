// "Save my loadout" on every arsenal box (loadouts work package G2, R1). Independent of SCR_SaveArsenalLoadout so it also works where mods
// (WCS_LoadoutEditor) hide every vanilla save action. Sits on ArsenalBox_Base.et (our override) and on the helper CLW_ArsenalActions.et.
// Shown only while no other SCR_SaveArsenalLoadout action of the same box (vanilla / WCS slot save) is shown, so there is never a duplicate
// next to a working save; that sibling check is throttled (1 s, cached). PerformAction runs on the server and stores the worn kit through
// CLW_ArsenalSaveHook.StoreFromCharacter (the same store path as the vanilla save hook). Log prefix "CLW: loadouts".

class CLW_SaveLoadoutAction : ScriptedUserAction
{
	protected const int SIBLING_CHECK_MS = 1000;

	protected SCR_ArsenalComponent m_Arsenal;
	protected float m_fNextSiblingCheck;
	protected bool m_bSiblingShown;

	override bool CanBeShownScript(IEntity user)
	{
		SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(user);
		if (!character)
			return false;

		SCR_PlayerController controller = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!controller || controller.IsPossessing())
			return false;

		if (!FindArsenal())
			return false;

		return !SiblingSaveShown(user);
	}

	// Is another save action of this box currently shown to the user? (cached for SIBLING_CHECK_MS)
	protected bool SiblingSaveShown(IEntity user)
	{
		float now = GetGame().GetWorld().GetWorldTime();
		if (now < m_fNextSiblingCheck)
			return m_bSiblingShown;

		m_fNextSiblingCheck = now + SIBLING_CHECK_MS;
		m_bSiblingShown = false;
		ActionsManagerComponent actions = ActionsManagerComponent.Cast(GetOwner().FindComponent(ActionsManagerComponent));
		if (!actions)
			return false;

		array<BaseUserAction> list = {};
		actions.GetActionsList(list);
		foreach (BaseUserAction other : list)
		{
			if (other == this || !SCR_SaveArsenalLoadout.Cast(other))
				continue;

			if (other.CanBeShown(user))
			{
				m_bSiblingShown = true;
				break;
			}
		}

		return m_bSiblingShown;
	}

	override bool CanBePerformedScript(IEntity user)
	{
		SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(user);
		if (!character)
			return false;

		SCR_ArsenalComponent arsenal = FindArsenal();
		if (!arsenal || !arsenal.IsArsenalEnabled())
			return false;

		if (!CLW_LoadLoadout.ArsenalAllows(arsenal, character.GetFaction()))
		{
			SetCannotPerformReason(CLW_Loc.Fmt("#CLW-Loadout_WrongArsenal"));
			return false;
		}

		return true;
	}

	override bool GetActionNameScript(out string outName)
	{
		outName = CLW_Loc.Fmt("#CLW-Loadout_Save");
		return true;
	}

	override void PerformAction(IEntity pOwnerEntity, IEntity pUserEntity)
	{
		if (!Replication.IsServer())
			return;

		int playerId = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(pUserEntity);
		if (playerId <= 0)
			return;

		CLW_ArsenalSaveHook.StoreFromCharacter(playerId, pUserEntity, CLW_ArsenalFind.Of(pOwnerEntity), "action");
	}

	protected SCR_ArsenalComponent FindArsenal()
	{
		if (!m_Arsenal)
			m_Arsenal = CLW_ArsenalFind.Of(GetOwner());

		return m_Arsenal;
	}
}

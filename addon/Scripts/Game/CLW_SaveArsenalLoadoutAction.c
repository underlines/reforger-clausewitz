// "Save loadout" for arsenal boxes whose own action list lacks the vanilla one (pack boxes like RHS ArsenalBox_RHS / ArsenalBox_USMC).
// Carried by the helper entity Prefabs/Systems/CLW_ArsenalActions.et that CLW_Services.SetupArsenals spawns at such a box. Identical to the
// vanilla SCR_SaveArsenalLoadout except that the arsenal is found through CLW_ArsenalFind (self, parent, nearest within 4 m); the vanilla
// PerformAction then ends in SetPlayerArsenalLoadout, which CLW_ArsenalSaveHook already captures.

class CLW_SaveArsenalLoadoutAction : SCR_SaveArsenalLoadout
{
	override protected bool UserActionInit()
	{
		SCR_ArsenalManagerComponent.GetArsenalManager(m_ArsenalManager);
		if (!m_ArsenalManager)
			return false;

		m_ArsenalComponent = CLW_ArsenalFind.Of(GetOwner());
		if (!m_ArsenalComponent)
			return false;

		m_ArsenalComponent.SetHasSaveArsenalAction(true);
		return true;
	}
}

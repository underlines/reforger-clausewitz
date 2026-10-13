// Finds the arsenal an action belongs to when the action does not sit on the arsenal box itself. Used by CLW_SaveArsenalLoadoutAction and
// CLW_LoadLoadoutAction, which also live on the helper entity CLW_ArsenalActions.et that CLW_Services spawns next to pack boxes whose own
// action list lacks the save / load loadout actions. Order: the entity itself, its parent, else the nearest entity with a
// SCR_ArsenalComponent within SEARCH_M (the helper is placed at the box's transform, so the box is at distance ~0).
class CLW_ArsenalFind
{
	static const float SEARCH_M = 4;

	protected SCR_ArsenalComponent m_Best;
	protected float m_fBestSq = 1e12;
	protected vector m_vFrom;

	static SCR_ArsenalComponent Of(IEntity owner)
	{
		if (!owner)
			return null;

		SCR_ArsenalComponent arsenal = SCR_ArsenalComponent.FindArsenalComponent(owner);
		if (!arsenal && owner.GetParent())
			arsenal = SCR_ArsenalComponent.FindArsenalComponent(owner.GetParent());

		if (arsenal)
			return arsenal;

		CLW_ArsenalFind finder = new CLW_ArsenalFind();
		finder.m_vFrom = owner.GetOrigin();
		owner.GetWorld().QueryEntitiesBySphere(finder.m_vFrom, SEARCH_M, finder.OnEntity, null, EQueryEntitiesFlags.ALL);
		return finder.m_Best;
	}

	// QueryEntitiesBySphere callback. Return true to continue.
	protected bool OnEntity(IEntity entity)
	{
		SCR_ArsenalComponent arsenal = SCR_ArsenalComponent.Cast(entity.FindComponent(SCR_ArsenalComponent));
		if (!arsenal)
			return true;

		float distSq = vector.DistanceSq(entity.GetOrigin(), m_vFrom);
		if (distSq < m_fBestSq)
		{
			m_fBestSq = distSq;
			m_Best = arsenal;
		}

		return true;
	}
}

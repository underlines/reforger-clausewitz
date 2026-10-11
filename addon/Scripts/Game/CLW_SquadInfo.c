// Small read-only helpers describing a squad for the situation report (type, member health).
class CLW_SquadInfo
{
	static const float HEALTHY_THRESHOLD = 0.99;

	// Human-readable squad type from the group prefab name, e.g. "Group_USSR_RifleSquad.et" -> "USSR RifleSquad". Falls back to "Squad".
	static string TypeName(notnull SCR_AIGroup group)
	{
		EntityPrefabData data = group.GetPrefabData();
		if (!data)
			return "Squad";

		string path = data.GetPrefabName();
		if (path == "")
			return "Squad";

		int slash = -1;
		for (int i = 0; i < path.Length(); i++)
		{
			if (path.Get(i) == "/")
				slash = i;
		}
		string name = path.Substring(slash + 1, path.Length() - slash - 1);
		int dot = name.IndexOf(".");
		if (dot > 0)
			name = name.Substring(0, dot);
		name.Replace("Group_", "");
		name.Replace("_", " ");
		if (name == "")
			return "Squad";

		return name;
	}

	// Controlled entities of the group agents (GetAIMembers() returns nothing for scene-placed squads; verified 2026-10-12).
	static array<IEntity> Members(notnull SCR_AIGroup group)
	{
		array<IEntity> result = {};
		array<AIAgent> agents = {};
		group.GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			IEntity entity = agent.GetControlledEntity();
			if (entity)
				result.Insert(entity);
		}

		return result;
	}

	// Living AI members. group.GetAgentsCount() still counts agents whose body is dead (a corpse leader keeps the squad "alive" at 1/6).
	static int AliveCount(notnull SCR_AIGroup group)
	{
		array<IEntity> members = Members(group);
		if (!members)
			return 0;

		int alive = 0;
		foreach (IEntity member : members)
		{
			if (!member)
				continue;

			CharacterControllerComponent controller = CharacterControllerComponent.Cast(member.FindComponent(CharacterControllerComponent));
			if (controller && controller.GetLifeState() == ECharacterLifeState.DEAD)
				continue;

			alive++;
		}

		return alive;
	}

	// Count wounded members (not alive-and-healthy) and average health [0..1] of the living ones.
	static void Health(notnull SCR_AIGroup group, out int wounded, out float avgHealth)
	{
		wounded = 0;
		avgHealth = 1;

		array<IEntity> members = Members(group);
		if (!members)
			return;

		float sum = 0;
		int counted = 0;
		foreach (IEntity member : members)
		{
			if (!member)
				continue;

			CharacterControllerComponent controller = CharacterControllerComponent.Cast(member.FindComponent(CharacterControllerComponent));
			ECharacterLifeState life = ECharacterLifeState.ALIVE;
			if (controller)
				life = controller.GetLifeState();
			if (life == ECharacterLifeState.DEAD)
				continue;

			float health = 1;
			DamageManagerComponent damage = DamageManagerComponent.Cast(member.FindComponent(DamageManagerComponent));
			if (damage)
				health = damage.GetHealthScaled();

			sum += health;
			counted++;
			if (life != ECharacterLifeState.ALIVE || health < HEALTHY_THRESHOLD)
				wounded++;
		}

		if (counted > 0)
			avgHealth = sum / counted;
	}
}

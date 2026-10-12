// A believed enemy position as seen by one of our groups. Input for the RuleCommander.
class CLW_Contact
{
	static const string KIND_INFANTRY = "infantry";
	static const string KIND_VEHICLE = "vehicle";
	static const string KIND_UNKNOWN = "unknown";

	int m_iId;				// unique within a CLW_ContactBook, 0 until reported
	string m_sObserverFaction;	// faction whose book holds this contact
	string m_sObserver;		// name of the group that sees the enemy
	string m_sEnemyFaction;
	vector m_vPos;			// average position of the cluster
	int m_iCount;			// number of detected targets in the cluster
	float m_fLastSeen;		// world time (s) of the newest target report
	string m_sKind = "unknown";	// "vehicle" if any target of the cluster is a vehicle, "infantry" if only characters, else "unknown"

	bool IsVehicle()
	{
		return m_sKind == KIND_VEHICLE;
	}

	// Generic vehicle test (no prefab names): a Vehicle, or anything with a compartment manager, or a character sitting in one.
	static bool IsVehicleEntity(IEntity entity)
	{
		if (!entity)
			return false;

		if (Vehicle.Cast(entity) || entity.FindComponent(BaseCompartmentManagerComponent))
			return true;

		IEntity root = entity.GetRootParent();
		if (root && root != entity)
			return Vehicle.Cast(root) != null || root.FindComponent(BaseCompartmentManagerComponent) != null;

		return false;
	}

	string Describe()
	{
		// m_fLastSeen is world time in seconds (target timestamps), GetWorldTime() is ms
		return string.Format("observer=%1 enemy=%2 kind=%3 count=%4 pos=%5 age_s=%6", m_sObserver, m_sEnemyFaction, m_sKind, m_iCount, m_vPos.ToString(), Math.Round(GetGame().GetWorld().GetWorldTime() * 0.001 - m_fLastSeen));
	}

	// Build one contact per target cluster in the group's perception. Server only.
	static void FromGroup(SCR_AIGroup group, string observerName, notnull array<ref CLW_Contact> outContacts)
	{
		SCR_AIGroupUtilityComponent utility = group.GetGroupUtilityComponent();
		if (!utility || !utility.m_Perception)
			return;

		foreach (SCR_AIGroupTargetCluster cluster : utility.m_Perception.m_aTargetClusters)
		{
			if (!cluster || cluster.m_aTargets.IsEmpty())
				continue;

			CLW_Contact contact = new CLW_Contact();
			contact.m_sObserver = observerName;
			contact.m_iCount = cluster.m_aTargets.Count();

			vector sum;
			bool anyVehicle;
			bool anyEntity;
			foreach (SCR_AITargetInfo target : cluster.m_aTargets)
			{
				sum += target.m_vWorldPos;
				if (target.m_Entity)
				{
					anyEntity = true;
					if (IsVehicleEntity(target.m_Entity))
						anyVehicle = true;
				}

				if (target.m_fTimestamp > contact.m_fLastSeen)
					contact.m_fLastSeen = target.m_fTimestamp;
				if (target.m_Faction && contact.m_sEnemyFaction.IsEmpty())
					contact.m_sEnemyFaction = target.m_Faction.GetFactionKey();
			}
			contact.m_vPos = sum / contact.m_iCount;
			if (anyVehicle)
				contact.m_sKind = KIND_VEHICLE;
			else if (anyEntity)
				contact.m_sKind = KIND_INFANTRY;
			outContacts.Insert(contact);
		}
	}
}

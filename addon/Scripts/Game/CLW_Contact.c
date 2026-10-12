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
	vector m_vPos;			// average position of the cluster's live targets
	int m_iCount;			// number of LIVE targets in the cluster (destroyed / disarmed ones are skipped; crew counts as a target)
	int m_iVehicles;		// distinct live vehicles (by root entity; their crew is not counted again)
	int m_iInfantry;		// live characters that are not inside a vehicle
	float m_fLastSeen;		// world time (s) of the newest target report
	string m_sKind = "unknown";	// "vehicle" if any live target of the cluster is a vehicle, "infantry" if only characters, else "unknown"
	string m_sSource = "ai";	// "ai" (group perception) or "human" (player report / enemy map marker, CLW_Reports)
	float m_fConfidence = 1;	// 0..1: how much the report is trusted (AI sightings 1)

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
		string text = string.Format("observer=%1 enemy=%2 kind=%3 count=%4 (veh %5, inf %6) pos=%7 age_s=%8", m_sObserver, m_sEnemyFaction, m_sKind, m_iCount, m_iVehicles, m_iInfantry, m_vPos.ToString(), Math.Round(GetGame().GetWorld().GetWorldTime() * 0.001 - m_fLastSeen));
		if (m_sSource != "ai")
			text += " source=" + m_sSource + " conf=" + m_fConfidence.ToString(-1, 2);
		return text;
	}

	// Build one contact per target cluster in the group's perception. Server only.
	// Only LIVE targets count (category DESTROYED / DISARMED and destroyed damage state are skipped); a cluster without one is dropped.
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

			vector sum;
			bool anyEntity;
			array<IEntity> vehicles = {};
			foreach (SCR_AITargetInfo target : cluster.m_aTargets)
			{
				if (!target)
					continue;
				if (target.m_eCategory == EAITargetInfoCategory.DESTROYED || target.m_eCategory == EAITargetInfoCategory.DISARMED)
					continue;
				if (target.m_DamageManager && target.m_DamageManager.GetState() == EDamageState.DESTROYED)
					continue;

				contact.m_iCount++;
				sum += target.m_vWorldPos;
				if (target.m_Entity)
				{
					anyEntity = true;
					if (IsVehicleEntity(target.m_Entity))
					{
						// one vehicle, whether the target is the hull or a crew member inside it
						IEntity root = target.m_Entity.GetRootParent();
						if (!root)
							root = target.m_Entity;
						if (!vehicles.Contains(root))
							vehicles.Insert(root);
					}
					else if (ChimeraCharacter.Cast(target.m_Entity))
						contact.m_iInfantry++;
				}

				if (target.m_fTimestamp > contact.m_fLastSeen)
					contact.m_fLastSeen = target.m_fTimestamp;
				if (target.m_Faction && contact.m_sEnemyFaction.IsEmpty())
					contact.m_sEnemyFaction = target.m_Faction.GetFactionKey();
			}

			if (contact.m_iCount <= 0)
				continue;

			contact.m_iVehicles = vehicles.Count();
			contact.m_vPos = sum / contact.m_iCount;
			if (contact.m_iVehicles > 0)
				contact.m_sKind = KIND_VEHICLE;
			else if (anyEntity)
				contact.m_sKind = KIND_INFANTRY;
			outContacts.Insert(contact);
		}
	}
}

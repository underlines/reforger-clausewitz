// A believed enemy position as seen by one of our groups. Input for the RuleCommander.
class CLW_Contact
{
	string m_sObserver;		// name of the group that sees the enemy
	string m_sEnemyFaction;
	vector m_vPos;			// average position of the cluster
	int m_iCount;			// number of detected targets in the cluster
	float m_fLastSeen;		// world time (s) of the newest target report

	string Describe()
	{
		// m_fLastSeen is world time in seconds (target timestamps), GetWorldTime() is ms
		return string.Format("observer=%1 enemy=%2 count=%3 pos=%4 age_s=%5", m_sObserver, m_sEnemyFaction, m_iCount, m_vPos.ToString(), Math.Round(GetGame().GetWorld().GetWorldTime() * 0.001 - m_fLastSeen));
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
			foreach (SCR_AITargetInfo target : cluster.m_aTargets)
			{
				sum += target.m_vWorldPos;
				if (target.m_fTimestamp > contact.m_fLastSeen)
					contact.m_fLastSeen = target.m_fTimestamp;
				if (target.m_Faction && contact.m_sEnemyFaction.IsEmpty())
					contact.m_sEnemyFaction = target.m_Faction.GetFactionKey();
			}
			contact.m_vPos = sum / contact.m_iCount;
			outContacts.Insert(contact);
		}
	}
}

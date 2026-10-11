// Per-faction picture of known enemy contacts: merges repeated sightings, drops stale ones.
// Server only. Fed by CLW_Contact.FromGroup, read by CLW_ContactMarkers and CLW_RuleCommander.
class CLW_ContactBook
{
	static const float MERGE_DISTANCE = 60;		// m: sightings closer than this are the same contact
	static const float EXPIRE_SECONDS = 120;	// contact is dropped this long after its newest sighting

	protected ref array<ref CLW_Contact> m_aContacts = {};
	protected int m_iNextId = 1;

	// Add or merge sightings reported by one of our groups. observerFaction = the faction owning the book.
	void Report(string observerFaction, notnull array<ref CLW_Contact> sightings)
	{
		foreach (CLW_Contact sighting : sightings)
		{
			sighting.m_sObserverFaction = observerFaction;
			CLW_Contact known = FindNear(sighting);
			if (known)
			{
				known.m_vPos = sighting.m_vPos;
				known.m_iCount = sighting.m_iCount;
				known.m_fLastSeen = Math.Max(known.m_fLastSeen, sighting.m_fLastSeen);
				known.m_sObserver = sighting.m_sObserver;
				continue;
			}

			sighting.m_iId = m_iNextId++;
			m_aContacts.Insert(sighting);
			CLW_Debug.Say(observerFaction + ": new contact " + sighting.Describe());
		}
	}

	// Remove stale contacts. Returns the ids removed so markers can follow.
	void Expire(notnull array<int> outRemovedIds)
	{
		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		for (int i = m_aContacts.Count() - 1; i >= 0; i--)
		{
			if (now - m_aContacts[i].m_fLastSeen > EXPIRE_SECONDS)
			{
				outRemovedIds.Insert(m_aContacts[i].m_iId);
				m_aContacts.Remove(i);
			}
		}
	}

	// Contacts known to the given faction (it only holds its own picture, so this is all of them).
	void GetContacts(notnull array<CLW_Contact> outContacts)
	{
		foreach (CLW_Contact contact : m_aContacts)
		{
			outContacts.Insert(contact);
		}
	}

	int Count()
	{
		return m_aContacts.Count();
	}

	protected CLW_Contact FindNear(CLW_Contact sighting)
	{
		foreach (CLW_Contact contact : m_aContacts)
		{
			if (contact.m_sEnemyFaction == sighting.m_sEnemyFaction && vector.DistanceXZ(contact.m_vPos, sighting.m_vPos) < MERGE_DISTANCE)
				return contact;
		}
		return null;
	}
}

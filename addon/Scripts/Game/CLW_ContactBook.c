// Per-faction picture of known enemy contacts: merges repeated sightings, drops stale ones.
// Server only. Fed by CLW_Contact.FromGroup, read by CLW_ContactMarkers and CLW_RuleCommander.
class CLW_ContactBook
{
	static const float MERGE_DISTANCE = 60;		// m: sightings closer than this are the same contact
	static const float EXPIRE_SECONDS = 120;	// contact is dropped this long after its newest sighting

	protected ref array<ref CLW_Contact> m_aContacts = {};
	protected ref array<int> m_aPendingRemoved = {};	// ids dropped by RemoveNow, handed out by the next Expire so the map markers follow
	protected int m_iNextId = 1;

	// Add or merge sightings reported by one of our groups. observerFaction = the faction owning the book.
	void Report(string observerFaction, notnull array<ref CLW_Contact> sightings)
	{
		foreach (CLW_Contact sighting : sightings)
		{
			ReportOne(observerFaction, sighting);
		}
	}

	// Same as Report for one sighting; returns the id of the contact that now holds it (new, or the known one it merged into).
	int ReportOne(string observerFaction, notnull CLW_Contact sighting)
	{
		sighting.m_sObserverFaction = observerFaction;
		CLW_Contact known = FindNear(sighting);
		if (known)
		{
			// a human report never overwrites what an AI group sees; it only keeps that contact alive
			if (sighting.m_sSource != "ai" && known.m_sSource == "ai")
			{
				known.m_fLastSeen = Math.Max(known.m_fLastSeen, sighting.m_fLastSeen);
				return known.m_iId;
			}

			known.m_sSource = sighting.m_sSource;
			known.m_fConfidence = sighting.m_fConfidence;
			known.m_vPos = sighting.m_vPos;
			known.m_iCount = sighting.m_iCount;
			known.m_iVehicles = sighting.m_iVehicles;
			known.m_iInfantry = sighting.m_iInfantry;
			known.m_fLastSeen = Math.Max(known.m_fLastSeen, sighting.m_fLastSeen);
			known.m_sObserver = sighting.m_sObserver;
			// a vehicle seen in the cluster keeps the contact a vehicle contact; otherwise the newest kind wins
			if (sighting.m_sKind == CLW_Contact.KIND_VEHICLE || known.m_sKind != CLW_Contact.KIND_VEHICLE)
				known.m_sKind = sighting.m_sKind;
			return known.m_iId;
		}

		sighting.m_iId = m_iNextId++;
		m_aContacts.Insert(sighting);
		CLW_Debug.Say(observerFaction + ": new contact " + sighting.Describe());
		return sighting.m_iId;
	}

	// The contact with this id, null when it is gone (expired or removed).
	CLW_Contact Find(int contactId)
	{
		foreach (CLW_Contact contact : m_aContacts)
		{
			if (contact.m_iId == contactId)
				return contact;
		}

		return null;
	}

	// Drops a contact at once (a player cancelled his report). The next Expire returns its id so the map marker goes too.
	bool RemoveNow(int contactId)
	{
		for (int i = m_aContacts.Count() - 1; i >= 0; i--)
		{
			if (m_aContacts[i].m_iId == contactId)
			{
				m_aContacts.Remove(i);
				m_aPendingRemoved.Insert(contactId);
				return true;
			}
		}

		return false;
	}

	// Remove stale contacts. Returns the ids removed so markers can follow.
	void Expire(notnull array<int> outRemovedIds)
	{
		foreach (int pendingId : m_aPendingRemoved)
		{
			outRemovedIds.Insert(pendingId);
		}

		m_aPendingRemoved.Clear();
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

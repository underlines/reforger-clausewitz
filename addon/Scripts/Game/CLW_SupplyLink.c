// Supply link (M3 phase B): the physical supply containers of a base (the supply containers of its service compositions: supply cache,
// HQ storage) mirror the side's supplies ledger in CLW_WarState, so vanilla consumers (the arsenal draws from containers in range) spend
// the same supplies as the commanders' purchases.
//   ledger -> containers   the ledger value is written into the containers (up to their capacity) whenever it differs
//   containers -> ledger   a drop (arsenal kit) or rise (delivery) since the last sync is applied to the ledger as a delta
// Polled every ~1.5 s by CLW_Services (server only). Phase L: the stock of a link is CLW_SupplyStore.Get(side, base): the side ledger (= HQ stock) for the HQ, and for every base when logistics is off; the own stock of a FOB when logistics is on. Push/Poll are the only places that know.
// Log prefix: "CLW: services supply".
class CLW_SupplyLink
{
	protected static ref array<ref CLW_SupplyLink> s_aLinks = {};

	string m_sSide;
	string m_sBase;
	protected ref array<SCR_ResourceContainer> m_aContainers = {};	// owned by the components of the services
	protected float m_fLast;			// container total after our last write / accepted read
	protected float m_fCapacity;
	protected bool m_bPushed;

	static void Reset()
	{
		s_aLinks.Clear();
	}

	static CLW_SupplyLink Find(string sideKey, string baseId)
	{
		foreach (CLW_SupplyLink link : s_aLinks)
		{
			if (link.m_sSide == sideKey && link.m_sBase == baseId)
				return link;
		}

		return null;
	}

	// Forgets the link of a base (a lost FOB) without touching the ledger.
	static void Remove(string sideKey, string baseId)
	{
		for (int i = s_aLinks.Count() - 1; i >= 0; i--)
		{
			if (s_aLinks[i].m_sSide == sideKey && s_aLinks[i].m_sBase == baseId)
				s_aLinks.Remove(i);
		}
	}

	static CLW_SupplyLink GetOrCreate(string sideKey, string baseId)
	{
		CLW_SupplyLink link = Find(sideKey, baseId);
		if (link)
			return link;

		link = new CLW_SupplyLink();
		link.m_sSide = sideKey;
		link.m_sBase = baseId;
		s_aLinks.Insert(link);
		return link;
	}

	static void PollAll(notnull CLW_WarState war)
	{
		foreach (CLW_SupplyLink link : s_aLinks)
		{
			link.Poll(war);
		}
	}

	// Gathers the supply containers of the given entities (not yet known ones only). Returns the number of containers linked in total.
	int Collect(notnull array<IEntity> entities)
	{
		foreach (IEntity entity : entities)
		{
			SCR_ResourceComponent resources = SCR_ResourceComponent.Cast(entity.FindComponent(SCR_ResourceComponent));
			if (!resources)
				continue;

			SCR_ResourceContainer container = resources.GetContainer(EResourceType.SUPPLIES);
			if (!container || container.GetMaxResourceValue() <= 0 || m_aContainers.Contains(container))
				continue;

			m_aContainers.Insert(container);
			m_fCapacity += container.GetMaxResourceValue();
			string prefabName = "?";
			EntityPrefabData prefabData = entity.GetPrefabData();
			if (prefabData)
				prefabName = prefabData.GetPrefabName();
			Print(string.Format("CLW: services supply link %1 %2: container %3 %4/%5 (%6)", m_sSide, m_sBase, m_aContainers.Count(), container.GetResourceValue(), container.GetMaxResourceValue(), prefabName), LogLevel.NORMAL);
		}

		return m_aContainers.Count();
	}

	int GetContainerCount()
	{
		return m_aContainers.Count();
	}

	float GetCapacity()
	{
		return m_fCapacity;
	}

	// Current stock in the containers (what a vanilla consumer sees).
	float GetStock()
	{
		float total;
		foreach (SCR_ResourceContainer container : m_aContainers)
		{
			if (container)
				total += container.GetResourceValue();
		}

		return total;
	}

	// First write: the ledger value goes into the containers.
	void Push(notnull CLW_WarState war)
	{
		int ledger = CLW_SupplyStore.Get(m_sSide, m_sBase);
		float written = Write(ledger);
		m_bPushed = true;
		Print(string.Format("CLW: services supply link %1 %2 active: %3 containers, capacity %4, ledger %5 -> containers %6", m_sSide, m_sBase, m_aContainers.Count(), m_fCapacity, ledger, written), LogLevel.NORMAL);
	}

	// One sync step: containers -> ledger (delta since the last step), then ledger -> containers.
	void Poll(notnull CLW_WarState war)
	{
		if (!m_bPushed)
			return;

		float observed = GetStock();
		float delta = observed - m_fLast;
		int whole = Math.Round(delta);
		int ledger = CLW_SupplyStore.Get(m_sSide, m_sBase);
		if (whole != 0)
		{
			CLW_SupplyStore.Add(m_sSide, m_sBase, whole);
			int after = CLW_SupplyStore.Get(m_sSide, m_sBase);
			Print(string.Format("CLW: services supply sync %1 %2: containers %3 -> %4 (delta %5): ledger %6 -> %7", m_sSide, m_sBase, m_fLast, observed, whole, ledger, after), LogLevel.NORMAL);
			m_fLast += whole;	// the fractional rest stays for the next step
			ledger = after;
		}

		float wanted = Math.Min(ledger, m_fCapacity);
		if (Math.AbsFloat(wanted - observed) >= 1)
		{
			float written = Write(wanted);
			Print(string.Format("CLW: services supply sync %1 %2: ledger %3 -> containers %4 (was %5, capacity %6)", m_sSide, m_sBase, ledger, written, observed, m_fCapacity), LogLevel.NORMAL);
		}
	}

	// Fills the containers in order up to their capacity with 'total'; returns the stock actually in them afterwards.
	protected float Write(float total)
	{
		float remaining = Math.Max(0, total);
		foreach (SCR_ResourceContainer container : m_aContainers)
		{
			if (!container)
				continue;

			float part = Math.Min(container.GetMaxResourceValue(), remaining);
			container.SetResourceValue(part);
			remaining -= part;
		}

		m_fLast = GetStock();
		return m_fLast;
	}
}

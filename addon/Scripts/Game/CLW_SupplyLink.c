// Supply link (M3 phase B): the physical supply containers of a base (the supply containers of its service compositions: supply cache,
// HQ storage) mirror the side's supplies ledger in CLW_WarState, so vanilla consumers (the arsenal draws from containers in range) spend
// the same supplies as the commanders' purchases.
//   ledger -> containers   the ledger value is written into the containers (up to their capacity) whenever it differs
//   containers -> ledger   a drop (arsenal kit) or rise (delivery) since the last sync is applied to the ledger as a delta
// Polled every ~1.5 s by CLW_Services (server only). Phase L: the stock of a link is CLW_SupplyStore.Get(side, base): the side ledger (= HQ stock) for the HQ, and for every base when logistics is off; the own stock of a FOB when logistics is on. Push/Poll are the only places that know.
// Log prefix: "CLW: services supply".
class CLW_SupplyLink
{
	static const float KIT_RANGE_M = 40;	// a kit / depot request is attributed to the nearest human of the side within this range of the service
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

			// a virtual container is a view over other containers (HQ storage over its crates, DS run 2026-10-12):
			// writing it fills the real ones and the sum counts them twice
			if (SCR_ResourceContainerVirtual.Cast(container))
			{
				Print(string.Format("CLW: services supply link %1 %2: skipped virtual container %3/%4", m_sSide, m_sBase, container.GetResourceValue(), container.GetMaxResourceValue()), LogLevel.NORMAL);
				continue;
			}

			m_aContainers.Insert(container);
			m_fCapacity += container.GetMaxResourceValue();
			string prefabName = "?";
			EntityPrefabData prefabData = entity.GetPrefabData();
			if (prefabData)
				prefabName = prefabData.GetPrefabName();
			Print(string.Format("CLW: services supply link %1 %2: container %3 %4/%5 (%6 %7)", m_sSide, m_sBase, m_aContainers.Count(), container.GetResourceValue(), container.GetMaxResourceValue(), container.ClassName(), prefabName), LogLevel.NORMAL);
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
			if (whole < 0)
				LogTaken(-whole, ledger, after);

			ledger = after;
		}

		float wanted = Math.Min(ledger, m_fCapacity);
		if (Math.AbsFloat(wanted - observed) >= 1)
		{
			float written = Write(wanted);
			Print(string.Format("CLW: services supply sync %1 %2: ledger %3 -> containers %4 (was %5, capacity %6)", m_sSide, m_sBase, ledger, written, observed, m_fCapacity), LogLevel.NORMAL);
		}
	}

	// P11 (owner round 3: kit purchases were not attributable): a drop of the container stock that the ledger just took over is a vanilla
	// consumer (arsenal kit, depot request). The consumer is the human of this side standing nearest to a service of this base within
	// KIT_RANGE_M; if that service is an arsenal it is a kit: "CLW: kit <player> <side> took kit cost <n> at <base> (stock a -> b)".
	// Other drops are logged as "CLW: services supply taken ..." with the nearest service kind. Log only, no state.
	protected void LogTaken(int amount, int stockBefore, int stockAfter)
	{
		array<CLW_ServiceInstance> insts = {};
		CLW_Services.GetInstances(m_sSide, m_sBase, insts);
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players || insts.IsEmpty())
		{
			Print(string.Format("CLW: services supply taken %1 %2: %3 (stock %4 -> %5), consumer unknown (no services / players)", m_sSide, m_sBase, amount, stockBefore, stockAfter), LogLevel.NORMAL);
			return;
		}

		array<int> ids = {};
		players.GetPlayers(ids);
		string bestName;
		string bestKind;
		float bestDist = KIT_RANGE_M;
		foreach (int id : ids)
		{
			IEntity body = players.GetPlayerControlledEntity(id);
			if (!body)
				continue;

			FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(body.FindComponent(FactionAffiliationComponent));
			if (!affiliation || !affiliation.GetAffiliatedFaction() || affiliation.GetAffiliatedFaction().GetFactionKey() != m_sSide)
				continue;

			vector at = body.GetOrigin();
			foreach (CLW_ServiceInstance inst : insts)
			{
				if (inst.m_sKind != CLW_ServiceKind.ARSENAL && inst.m_sKind != CLW_ServiceKind.VEHICLE_DEPOT)
					continue;

				float dist = vector.DistanceXZ(at, inst.m_vPos);
				if (dist < bestDist)
				{
					bestDist = dist;
					bestName = players.GetPlayerName(id);
					bestKind = inst.m_sKind;
				}
			}
		}

		if (bestName.IsEmpty())
		{
			Print(string.Format("CLW: services supply taken %1 %2: %3 (stock %4 -> %5), no player of the side within %6 m of an arsenal / depot (script or third-party consumer)", m_sSide, m_sBase, amount, stockBefore, stockAfter, KIT_RANGE_M), LogLevel.NORMAL);
			return;
		}

		if (bestKind == CLW_ServiceKind.ARSENAL)
			Print(string.Format("CLW: kit %1 %2 took kit cost %3 at %4 (stock %5 -> %6)", bestName, m_sSide, amount, m_sBase, stockBefore, stockAfter), LogLevel.NORMAL);
		else
			Print(string.Format("CLW: services supply taken %1 %2: %3 by %4 at the %5 (stock %6 -> %7)", m_sSide, m_sBase, amount, bestName, bestKind, stockBefore, stockAfter), LogLevel.NORMAL);
	}

	// Fills the containers in order up to their capacity with 'total'; returns the stock actually in them afterwards.
	// A container that ignores SetResourceValue (DS run 2026-10-12: all 15 HQ containers stayed 0, e.g. ones managed by
	// a vanilla encapsulator) is dropped from the link, so the stock lands in containers that take it.
	protected float Write(float total)
	{
		float remaining = Math.Max(0, total);
		for (int i = m_aContainers.Count() - 1; i >= 0; i--)
		{
			if (!m_aContainers[i])
				m_aContainers.Remove(i);
		}

		int index = 0;
		while (index < m_aContainers.Count())
		{
			SCR_ResourceContainer container = m_aContainers[index];
			float part = Math.Min(container.GetMaxResourceValue(), remaining);
			container.SetResourceValue(part);
			if (Math.AbsFloat(container.GetResourceValue() - part) >= 1)
			{
				Print(string.Format("CLW: services supply link %1 %2: container %3 ignores writes (wanted %4, has %5): dropped", m_sSide, m_sBase, index + 1, part, container.GetResourceValue()), LogLevel.WARNING);
				m_fCapacity -= container.GetMaxResourceValue();
				m_aContainers.Remove(index);
				continue;
			}

			remaining -= part;
			index++;
		}

		m_fLast = GetStock();
		return m_fLast;
	}
}

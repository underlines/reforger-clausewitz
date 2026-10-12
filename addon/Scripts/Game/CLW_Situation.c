// Protocol v3 (= v2 + manifest hash, added by CLW_RemoteCommander) situation builder (spec section 8): serialises one faction's view of the war as the sidecar's POST /orders body.
// Server only. Pure read access to the war systems; the only side effect is draining the faction's event queue.
// Keys, kinds, catalog types and objective ids all come from the data, nothing is hard-wired.
class CLW_Situation
{
	static const int MAX_BODY = 55000;			// the sidecar accepts 60 KB: trim before reaching it
	static const int MAX_CONTACTS = 40;
	static const int MAX_CONTACTS_TRIMMED = 12;
	static const float ENGAGED_DISTANCE = 150;	// m: a contact this close makes a unit "engaged"

	protected string m_sFaction;
	protected CLW_Orders m_Orders;		// weak: owned by the rule commander

	void Init(string factionKey, CLW_Orders orders)
	{
		m_sFaction = factionKey;
		m_Orders = orders;
	}

	// Escape a string for use inside a JSON string literal.
	static string Escape(string text)
	{
		string r = text;
		r.Replace("\\", "\\\\");
		r.Replace("\"", "\\\"");
		r.Replace("\n", " ");
		r.Replace("\r", " ");
		r.Replace("\t", " ");
		return r;
	}

	static string BoolText(bool value)
	{
		if (value)
			return "true";

		return "false";
	}

	// Builds the body. Returns the number of squads in it. withEvents drains the faction's event queue into the body.
	int Build(CLW_ContactBook book, out string json)
	{
		json = "";
		float now = GetGame().GetWorld().GetWorldTime() * 0.001;
		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		if (!war || !registry || !m_Orders)
			return 0;

		array<CLW_Contact> contacts = {};
		if (book)
			book.GetContacts(contacts);

		int squadCount;
		string squadsJson = BuildSquads(registry, contacts, now, squadCount);
		string warJson = BuildWar(war);
		string objectivesJson = BuildObjectives(points);
		string basesJson = BuildBases(war);
		string catalogJson = BuildCatalog(true);
		string contactsJson = BuildContacts(contacts, now, MAX_CONTACTS);

		array<string> events = {};
		war.DrainEvents(m_sFaction, events);
		string eventsJson = "";
		foreach (int e, string text : events)
		{
			if (e >= CLW_WarConfig.EVENT_QUEUE_MAX)
				break;

			if (e > 0)
				eventsJson += ",";
			eventsJson += "\"" + Escape(text) + "\"";
		}

		// concatenate: string.Format truncates its result at ~8 KB
		string head = "{\"v\":3,\"faction\":\"" + Escape(m_sFaction) + "\",\"side\":" + SideIndex(m_sFaction);
		head += ",\"identity\":\"" + Escape(CLW_ScenarioConfig.GetIdentityText(m_sFaction)) + "\",\"time\":" + Math.Round(now) + ",\"timeLeftSec\":" + war.GetTimeLeftSec();
		json = head + ",\"war\":" + warJson + ",\"objectives\":[" + objectivesJson + "],\"bases\":[" + basesJson + "],\"catalog\":[" + catalogJson + "],\"squads\":[" + squadsJson + "],\"contacts\":[" + contactsJson + "],\"events\":[" + eventsJson + "]}";
		if (json.Length() > MAX_BODY)
		{
			// drop the catalog descriptions and old contacts first
			catalogJson = BuildCatalog(false);
			contactsJson = BuildContacts(contacts, now, MAX_CONTACTS_TRIMMED);
			json = head + ",\"war\":" + warJson + ",\"objectives\":[" + objectivesJson + "],\"bases\":[" + basesJson + "],\"catalog\":[" + catalogJson + "],\"squads\":[" + squadsJson + "],\"contacts\":[" + contactsJson + "],\"events\":[" + eventsJson + "]}";
			Print(string.Format("CLW: situation %1 body trimmed to %2 bytes (catalog descriptions and contacts dropped)", m_sFaction, json.Length()), LogLevel.WARNING);
		}

		AppendFobBlocks(json);
		AppendBaseRules(json);
		AppendLogistics(json);
		AppendRequests(json);
		AppendStructures(json);
		return squadCount;
	}

	// Own base/FOB structures (structures[]: base, kind, label, pos [x,z], grid) for the dashboard map. Appended after the trim.
	protected void AppendStructures(inout string json)
	{
		if (json.Length() < 2)
			return;

		string structures = CLW_StructureMarkers.StructuresJson(m_sFaction);
		if (structures.IsEmpty())
			return;

		json = CLW_Str.DropLast(json) + ",\"structures\":[" + structures + "]}";
	}

	// M3 phase Q: recent player requests of this faction (requests[], needs the `requests` capability). Appended after the trim.
	protected void AppendRequests(inout string json)
	{
		if (json.Length() < 2 || !CLW_Capabilities.Has("requests"))
			return;

		json = CLW_Str.DropLast(json) + ",\"requests\":" + CLW_RequestService.GetJson(m_sFaction) + "}";
	}

	// M3 phase L: convoys[] and logisticsRules (only with the `logistics` capability and a supply truck in the catalog). Appended after the trim.
	protected void AppendLogistics(inout string json)
	{
		if (json.Length() < 2 || !CLW_Capabilities.Has("logistics") || !CLW_Convoys.HasTruckData(m_sFaction))
			return;

		string extra = ",\"convoys\":[" + CLW_Convoys.ConvoysJson(m_sFaction) + "],\"logisticsRules\":" + CLW_Convoys.RulesJson(m_sFaction);
		json = CLW_Str.DropLast(json) + extra + "}";
	}

	// Spawn on assignment (owner playtest 2026-10-12): garrison size, idle stand-down time and the recent stand-downs of this side
	// (baseRules). Appended after the trim.
	protected void AppendBaseRules(inout string json)
	{
		if (json.Length() < 2)
			return;

		json = CLW_Str.DropLast(json) + ",\"baseRules\":" + CLW_BaseWatch.RulesJson(m_sFaction) + "}";
	}

	// M3 phase F: own FOBs, candidate sites and the FOB rules (only with the `fob` capability and at least one FOB allowed).
	// Appended after the trim so the blocks (a few hundred bytes) are always sent.
	protected void AppendFobBlocks(inout string json)
	{
		if (json.Length() < 2 || !CLW_Capabilities.Has("fob") || CLW_FobManager.GetMax() <= 0)
			return;

		string fobsJson = CLW_FobManager.FobsJson(m_sFaction);
		string sitesJson = CLW_FobManager.SitesJson(m_sFaction);
		string extra = ",\"fobs\":[" + fobsJson + "],\"fobSites\":[" + sitesJson + "],\"fobRules\":" + CLW_FobManager.RulesJson(m_sFaction);
		json = CLW_Str.DropLast(json) + extra + "}";
	}

	// ---- parts ----

	protected string BuildWar(CLW_WarState war)
	{
		string enemy = war.GetEnemyKey(m_sFaction);
		int income = war.GetIncome(m_sFaction);
		int interval = Math.Max(1, war.GetIncomeIntervalSec());
		int incomePerMinute = Math.Round(income * 60.0 / interval);
		int liveAI;
		int vehicles;
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (spawner)
		{
			liveAI = spawner.GetLiveAI(m_sFaction);
			vehicles = spawner.GetLiveVehicles(m_sFaction);
		}

		string modeJson = "{\"id\":\"\",\"name\":\"\",\"params\":{}}";
		CLW_WarMode mode = war.GetMode();
		if (mode)
			modeJson = "{\"id\":\"" + Escape(mode.GetId()) + "\",\"name\":\"" + Escape(mode.GetName()) + "\",\"params\":" + mode.ValuesText() + "}";

		string phase = war.GetPhaseName();
		phase.ToLower();
		string text = "{\"tickets\":" + war.GetTickets(m_sFaction) + ",\"enemyTickets\":" + war.GetTickets(enemy) + ",\"supplies\":" + war.GetSupplies(m_sFaction);
		if (CLW_Capabilities.Has("logistics"))
			text += ",\"supplyTotal\":" + CLW_SupplyStore.GetTotal(m_sFaction);	// supplies = HQ stock; the total adds the FOB stocks
		text += ",\"income\":" + incomePerMinute + ",\"liveAI\":" + liveAI + ",\"aiCap\":" + war.GetAiCap(m_sFaction);
		text += ",\"vehicles\":" + vehicles + ",\"vehicleCap\":" + war.GetVehicleCap(m_sFaction);
		int engineActive;
		int engineLimit;
		if (CLW_ForceSpawner.GetEngineAI(engineActive, engineLimit))
		{
			int headroom = engineLimit - engineActive - CLW_WarConfig.ENGINE_AI_MARGIN;
			if (spawner)
				headroom = spawner.GetEngineHeadroom();
			text += ",\"engineAI\":{\"active\":" + engineActive + ",\"limit\":" + engineLimit + ",\"headroom\":" + Math.Max(0, headroom) + "}";
		}

		text += ",\"mode\":" + modeJson + ",\"phase\":\"" + phase + "\",\"sides\":[" + BuildSides(war) + "]}";
		return text;
	}

	protected static int SideIndex(string key)
	{
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!scenario)
			return -1;

		return scenario.GetSideIndex(key);
	}

	// Both sides, side A first: {"faction":..,"identity":..}
	protected string BuildSides(CLW_WarState war)
	{
		string text = "";
		array<string> keys = {};
		war.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			if (!text.IsEmpty())
				text += ",";
			text += "{\"faction\":\"" + Escape(key) + "\",\"identity\":\"" + Escape(CLW_ScenarioConfig.GetIdentityText(key)) + "\"}";
		}

		return text;
	}

	// progress is from this faction's point of view: +100 = fully ours, -100 = fully the enemy's.
	protected string BuildObjectives(CLW_ControlPoints points)
	{
		string text = "";
		if (!points)
			return text;

		for (int i = 0; i < points.Count(); i++)
		{
			CLW_ControlPoint point = points.Get(i);
			if (!point)
				continue;

			string owner = "none";
			if (!point.m_sOwner.IsEmpty())
				owner = point.m_sOwner;
			if (!text.IsEmpty())
				text += ",";
			text += "{\"id\":\"" + Escape(point.m_sId) + "\",\"name\":\"" + Escape(point.m_sName) + "\",\"pos\":[" + Math.Round(point.m_vPos[0]) + "," + Math.Round(point.m_vPos[2]) + "],\"grid\":\"" + CLW_Places.Grid(point.m_vPos) + "\",\"radius\":" + Math.Round(point.m_fRadius);
			text += ",\"value\":" + point.m_iValue + ",\"owner\":\"" + Escape(owner) + "\",\"contested\":" + BoolText(point.m_bContested) + ",\"progress\":" + Math.Round(points.ProgressFor(point, m_sFaction)) + "}";
		}

		return text;
	}

	protected string BuildBases(CLW_WarState war)
	{
		string text = "";
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (!bases)
			return text;

		array<string> keys = {};
		war.GetFactionKeys(keys);
		foreach (string key : keys)
		{
			vector pos = bases.GetBasePos(key);
			if (pos == vector.Zero)
				continue;

			if (!text.IsEmpty())
				text += ",";
			text += "{\"id\":\"" + CLW_Services.BASE_HQ + "\",\"faction\":\"" + Escape(key) + "\",\"pos\":[" + Math.Round(pos[0]) + "," + Math.Round(pos[2]) + "]";
			if (key == m_sFaction && CLW_Capabilities.Has("logistics"))
				text += ",\"stock\":" + CLW_SupplyStore.Get(key, CLW_Services.BASE_HQ);
			text += "}";
		}

		return text;
	}

	protected string BuildCatalog(bool withDesc)
	{
		string text = "";
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (!catalog)
			return text;

		array<CLW_ForceEntry> entries = {};
		catalog.GetEntries(m_sFaction, entries);
		foreach (CLW_ForceEntry entry : entries)
		{
			if (!text.IsEmpty())
				text += ",";
			text += "{\"type\":\"" + Escape(entry.m_sType) + "\",\"kind\":\"" + Escape(entry.m_sKind) + "\",\"cost\":" + entry.m_iCost;
			if (withDesc)
				text += ",\"desc\":\"" + Escape(entry.m_sName + " (" + entry.Describe() + ")") + "\"";
			text += "}";
		}

		return text;
	}

	protected string BuildContacts(notnull array<CLW_Contact> contacts, float now, int maxCount)
	{
		// newest first when trimming: collect indices by age
		array<CLW_Contact> sorted = {};
		foreach (CLW_Contact contact : contacts)
		{
			int at = sorted.Count();
			for (int k = 0; k < sorted.Count(); k++)
			{
				if (contact.m_fLastSeen > sorted[k].m_fLastSeen)
				{
					at = k;
					break;
				}
			}
			sorted.InsertAt(contact, at);
		}

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		string text = "";
		int written;
		foreach (CLW_Contact seen : sorted)
		{
			if (written >= maxCount)
				break;

			if (written > 0)
				text += ",";
			// size in words ("a vehicle platoon with dismounts"), counts split into vehicles / infantry, nearest objective or named place, 1 km map grid
			string size = CLW_Places.SizeText(seen.m_iVehicles, seen.m_iInfantry, seen.m_iCount);
			text += "{\"pos\":[" + Math.Round(seen.m_vPos[0]) + "," + Math.Round(seen.m_vPos[1]) + "," + Math.Round(seen.m_vPos[2]) + "],\"size\":\"" + Escape(size) + "\",\"kind\":\"" + Escape(seen.m_sKind) + "\"";
			text += ",\"vehicles\":" + seen.m_iVehicles + ",\"infantry\":" + seen.m_iInfantry + ",\"place\":\"" + Escape(CLW_Places.PlaceLabel(seen.m_vPos, points)) + "\",\"grid\":\"" + CLW_Places.Grid(seen.m_vPos) + "\"";
			text += ",\"ageSec\":" + Math.Round(Math.Max(0, now - seen.m_fLastSeen)) + "}";
			written++;
		}

		return text;
	}

	protected string BuildSquads(CLW_SquadRegistry registry, notnull array<CLW_Contact> contacts, float now, out int squadCount)
	{
		string text = "";
		squadCount = 0;
		array<string> ids = {};
		registry.GetCommandableIds(m_sFaction, ids);
		foreach (string id : ids)
		{
			SCR_AIGroup group = registry.Find(id);
			if (!group)
				continue;

			int alive = m_Orders.UnitAlive(id);
			vector pos = vector.Zero;
			if (alive <= 0 || !m_Orders.UnitPos(id, pos))
				continue;

			int start = m_Orders.StartStrength(id, alive);
			CLW_VehicleUnit unit = m_Orders.FindVehicleUnit(id);
			CLW_UnitOrder order = m_Orders.GetOrder(id);
			bool human = CLW_RuleCommander.HasHumanLeader(group);

			float yaw = 0;
			IEntity leader = CLW_SquadInfo.LeaderEntity(group);
			if (unit && unit.GetVehicle())
				yaw = unit.GetVehicle().GetYawPitchRoll()[0];
			else if (leader)
				yaw = leader.GetYawPitchRoll()[0];
			m_Orders.Sample(id, pos, yaw);
			CLW_UnitTrack track = m_Orders.Track(id);
			float speed = 0;
			float heading = yaw;
			if (track)
			{
				speed = track.m_fSpeed;
				if (track.m_fLastTime >= 0)
					heading = track.m_fHeading;
			}
			int headingDeg = Math.Round(heading);
			headingDeg = headingDeg % 360;
			if (headingDeg < 0)
				headingDeg += 360;

			int wounded = 0;
			float avgHealth = 1;
			CLW_SquadInfo.Health(group, wounded, avgHealth);
			if (unit && unit.GetCargoGroup())
			{
				int cargoWounded = 0;
				float cargoHealth = 1;
				CLW_SquadInfo.Health(unit.GetCargoGroup(), cargoWounded, cargoHealth);
				wounded += cargoWounded;
				avgHealth = (avgHealth + cargoHealth) * 0.5;
			}

			bool engaged = false;
			foreach (CLW_Contact seen : contacts)
			{
				if (vector.DistanceXZ(pos, seen.m_vPos) < ENGAGED_DISTANCE)
				{
					engaged = true;
					break;
				}
			}

			string stateText = "idle";
			if (engaged)
				stateText = "engaged";
			else if (order && order.m_sStatus == CLW_Orders.STATUS_ACTIVE)
			{
				stateText = "moving";
				if (order.m_eCommand == CLW_ECommand.ATTACK)
					stateText = "attacking";
				else if (order.m_eCommand == CLW_ECommand.DEFEND)
					stateText = "defending";
			}

			string kind = registry.GetKind(id);
			float vehicleHealth = 0;
			string vehicleJson = "null";
			if (unit)
			{
				vehicleHealth = unit.GetVehicleHealth();
				vehicleJson = "{\"type\":\"" + Escape(unit.GetVehicleTypeName()) + "\",\"health\":" + vehicleHealth.ToString(-1, 2) + ",\"mounted\":" + BoolText(unit.IsMounted()) + ",\"state\":\"" + unit.GetStateName() + "\"}";
				kind = unit.GetKind();
			}

			string objectiveJson = "null";
			if (order)
			{
				objectiveJson = "{\"command\":\"" + CLW_Command.ToName(order.m_eCommand) + "\"";
				if (!order.m_sTarget.IsEmpty())
					objectiveJson += ",\"target\":\"" + Escape(order.m_sTarget) + "\"";
				objectiveJson += ",\"pos\":[" + Math.Round(order.m_vPos[0]) + "," + Math.Round(order.m_vPos[2]) + "],\"reason\":\"" + Escape(order.m_sReason) + "\",\"ageSec\":" + Math.Round(Math.Max(0, now - order.m_fIssuedAt)) + ",\"status\":\"" + order.m_sStatus + "\"}";
			}

			// dormant = every agent of the unit (crew + linked cargo) sits in the engine's max AI LOD (not kept active by CLW_Activity)
			int lodTotal;
			int lodMax;
			CLW_SquadInfo.LodCounts(group, lodTotal, lodMax);
			if (unit && unit.GetCargoGroup())
			{
				int cargoTotal;
				int cargoMax;
				CLW_SquadInfo.LodCounts(unit.GetCargoGroup(), cargoTotal, cargoMax);
				lodTotal += cargoTotal;
				lodMax += cargoMax;
			}
			bool dormant = lodTotal > 0 && lodMax == lodTotal;

			string missionJson = "null";
			if (human)
			{
				string hookJson = CLW_MissionHook.Get().GetMissionJson(m_sFaction, id);
				if (!hookJson.IsEmpty())
					missionJson = hookJson;
			}

			if (squadCount > 0)
				text += ",";
			text += "{\"id\":\"" + Escape(id) + "\",\"kind\":\"" + Escape(kind) + "\",\"type\":\"" + Escape(CLW_SquadInfo.TypeName(group)) + "\",\"pos\":[" + Math.Round(pos[0]) + "," + Math.Round(pos[1]) + "," + Math.Round(pos[2]) + "]";
			text += ",\"agents\":" + alive + ",\"startAgents\":" + start + ",\"vehicle\":" + vehicleJson + ",\"humanLed\":" + BoolText(human);
			text += ",\"wounded\":" + wounded + ",\"health\":" + avgHealth.ToString(-1, 2) + ",\"speed\":" + speed.ToString(-1, 1) + ",\"heading\":" + headingDeg + ",\"state\":\"" + stateText + "\",\"dormant\":" + BoolText(dormant);
			string reservedFor = CLW_FobManager.ReservedFor(id);
			if (!reservedFor.IsEmpty())
				text += ",\"reserved\":\"" + Escape(reservedFor) + "\"";	// M3 phase F: tied up building a FOB, orders are refused
			if (CLW_BaseWatch.IsGarrison(id))
				text += ",\"garrison\":true";	// defends the HQ with a standing order: do not order it (refused)
			int idleSec = CLW_BaseWatch.IdleSec(id);
			if (idleSec > 0)
				text += ",\"idleSec\":" + idleSec;	// seconds without an active order; at the base it is stood down after baseRules.idleDespawnMin
			text += ",\"objective\":" + objectiveJson + ",\"mission\":" + missionJson + "}";
			squadCount++;
		}

		return text;
	}
}

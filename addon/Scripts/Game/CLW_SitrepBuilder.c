// Rule-based faction SITREP (works without any LLM), written in natural NATO/US military prose: no raw counts, no coordinates,
// no unit ids, no faction keys ("we / our forces / the enemy"). Composes the template sections (CLW_SitrepTemplate) from the SAME
// fog-limited picture the commander uses: this faction's contact book, its own units/orders, the public war state
// (objectives, tickets). Nothing about the enemy beyond the contact book and public values is read. Server only.
// Everything (names, places, sizes) comes from the scenario, the control points, the map and the registry.
//
// Sections: headline = BLUF, situation, enemy (shown as "Intel"), mission (one SMEAC statement), execution, logistics.
class CLW_SitrepArea
{
	string m_sKey;
	string m_sShort;			// "Timber Hub (C)" / "Newfield" / "grid 26-28"
	CLW_ControlPoint m_Point;	// objective of the area, null for a place / grid area
	int m_iVehicles;
	int m_iInfantry;
	int m_iCount;
	int m_iContacts;
	float m_fSeen;				// newest sighting (world s)
	vector m_vSum;
}

class CLW_SitrepEffort
{
	CLW_ECommand m_eCommand;
	string m_sTarget;			// noun phrase: "Timber Hub (C)", "our HQ", "the area around Newfield"
	int m_iCount;				// units
}

class CLW_SitrepBuilder
{
	static const int MAX_INTEL_AREAS = 3;
	static const float AREA_OBJECTIVE_M = 1000;		// a contact this close to an objective is reported for that objective
	static const float APPROACH_M = 900;			// "on the northern approach to" up to this distance
	static const float HQ_NEAR_M = 250;				// "close to our HQ"
	static const float PLACE_NEAR_M = 3000;
	static const float CONTACT_STALE_SEC = 90;		// older contacts are marked "dated"

	// Builds the SITREP for one faction. Never returns null.
	static CLW_Sitrep Build(string factionKey, CLW_ContactBook book, CLW_SquadRegistry registry, CLW_Orders orders)
	{
		CLW_Sitrep sitrep = new CLW_Sitrep();
		sitrep.m_sUnit = factionKey;
		sitrep.m_sSource = CLW_Sitrep.SOURCE_RULES;
		float now = CLW_Orders.Now();
		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (!war)
		{
			sitrep.SetHeadline("BLUF: no war picture available yet.");
			return sitrep;
		}

		sitrep.m_fWarTime = war.GetElapsedSec();
		if (!war.IsRunning())
		{
			BuildNotRunning(sitrep, war);
			return sitrep;
		}

		string enemyKey = war.GetEnemyKey(factionKey);
		array<CLW_Contact> contacts = {};
		if (book)
			book.GetContacts(contacts);

		// ---- unit picture ----
		array<string> ids = {};
		if (registry)
			registry.GetCommandableIds(factionKey, ids);
		int unitCount;
		int wornDown;
		int ineffective;
		int humanSquads;
		foreach (string id : ids)
		{
			int alive = 0;
			if (orders)
				alive = orders.UnitAlive(id);
			if (alive <= 0)
				continue;

			unitCount++;
			if (orders && orders.IsWeak(id))
			{
				if (orders.Strength(id) < 0.34)
					ineffective++;
				else
					wornDown++;
			}

			SCR_AIGroup group = registry.Find(id);
			if (group && CLW_RuleCommander.HasHumanLeader(group))
				humanSquads++;
		}

		// ---- objectives ----
		array<CLW_ControlPoint> held = {};
		array<CLW_ControlPoint> lost = {};
		array<CLW_ControlPoint> neutral = {};
		array<CLW_ControlPoint> contested = {};
		int total;
		if (points)
		{
			total = points.Count();
			for (int i = 0; i < total; i++)
			{
				CLW_ControlPoint point = points.Get(i);
				if (!point)
					continue;

				if (point.m_bContested)
					contested.Insert(point);
				if (point.m_sOwner == factionKey)
					held.Insert(point);
				else if (point.m_sOwner.IsEmpty())
					neutral.Insert(point);
				else
					lost.Insert(point);
			}
		}

		int tickets = war.GetTickets(factionKey);
		int enemyTickets = war.GetTickets(enemyKey);
		int ticketState = TicketState(tickets, enemyTickets);
		int pointState = 0;
		if (total > 0)
		{
			float share = held.Count() * 1.0 / total;
			if (share >= 0.6)
				pointState = 1;
			else if (share < 0.4)
				pointState = -1;
		}
		int momentum = ticketState + pointState;
		if (ticketState >= 0 && momentum < 0)
			momentum = 0;	// few objectives early on is not "under pressure" while tickets hold

		// ---- efforts (from the active orders) ----
		array<ref CLW_SitrepEffort> efforts = {};
		int idle;
		CollectEfforts(ids, orders, points, factionKey, efforts, idle);

		// ---- headline: BLUF ----
		string head = "BLUF: " + MomentumWords(momentum) + ".";
		if (total > 0)
			head += " We hold " + ObjectivesWords(held.Count(), total) + ";";
		if (!efforts.IsEmpty())
			head += " main effort: " + MissionVerb(efforts[0].m_eCommand) + " " + efforts[0].m_sTarget + ".";
		else
			head += " forces are consolidating.";
		sitrep.SetHeadline(head);

		// ---- S: situation ----
		string situation = "";
		if (total > 0)
		{
			if (held.Count() == total)
				situation = "We hold every objective.";
			else
			{
				if (held.IsEmpty())
					situation = "We hold no objectives.";
				else
					situation = "We hold " + ListWords(held) + ".";
				if (!lost.IsEmpty())
					situation += " The enemy holds " + ListWords(lost) + ".";
				if (!neutral.IsEmpty())
					situation += " " + Cap(ListWords(neutral)) + " remain" + PluralS(neutral.Count()) + " open.";
			}
			if (!contested.IsEmpty())
				situation += " " + Cap(ListWords(contested)) + " " + IsAre(contested.Count()) + " contested.";
		}
		if (tickets >= 0)
			situation += "\n" + TicketWords(ticketState);
		int timeLeft = war.GetTimeLeftSec();
		if (timeLeft >= 0)
			situation += " " + TimeLeftWords(timeLeft);
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (spawner)
			situation += " " + ForceStateWords(spawner.GetLiveAI(factionKey), war.GetAiCap(factionKey));
		sitrep.SetBody(0, situation);

		// ---- E: intel ----
		sitrep.SetBody(1, BuildIntel(contacts, now, points, factionKey));

		// ---- M + X ----
		sitrep.SetBody(2, BuildMission(efforts, ticketState));
		sitrep.SetBody(3, BuildExecution(efforts, idle, humanSquads));

		// ---- L: logistics ----
		sitrep.SetBody(4, BuildLogistics(factionKey, war, spawner, wornDown, ineffective));
		return sitrep;
	}

	protected static void BuildNotRunning(CLW_Sitrep sitrep, CLW_WarState war)
	{
		if (war.IsEnded())
		{
			string winner = war.GetWinner();
			if (winner.IsEmpty())
			{
				sitrep.SetHeadline("BLUF: hostilities have ended in a draw.");
				sitrep.SetBody(0, "The war is over: draw.");
			}
			else
			{
				sitrep.SetHeadline("BLUF: hostilities have ended.");
				sitrep.SetBody(0, "The war is over. Victory goes to " + winner + ".");
			}
			return;
		}

		sitrep.SetHeadline("BLUF: forces are assembling, no contact yet.");
		sitrep.SetBody(0, "The war has not started yet. HQ staff is assembling; the first full SITREP follows when forces deploy.");
		sitrep.SetBody(2, "Our forces seize and hold the objectives to preserve our tickets.");
		sitrep.SetBody(3, "Pick a squad in the deploy menu; squad leaders receive HQ tasking once the war runs.");
	}

	// ---------------------------------------------------------------- intel

	// Contacts grouped by area (nearest objective / named place), strongest first, max 3 lines + one assessment line.
	protected static string BuildIntel(notnull array<CLW_Contact> contacts, float now, CLW_ControlPoints points, string factionKey)
	{
		if (contacts.IsEmpty())
			return "No enemy contact reported.";

		array<ref CLW_SitrepArea> areas = {};
		foreach (CLW_Contact contact : contacts)
		{
			string key;
			string shortName;
			CLW_ControlPoint areaPoint;
			AreaOf(contact.m_vPos, points, key, shortName, areaPoint);
			CLW_SitrepArea area = FindArea(areas, key);
			if (!area)
			{
				area = new CLW_SitrepArea();
				area.m_sKey = key;
				area.m_sShort = shortName;
				area.m_Point = areaPoint;
				areas.Insert(area);
			}

			area.m_iVehicles += contact.m_iVehicles;
			area.m_iInfantry += contact.m_iInfantry;
			area.m_iCount += contact.m_iCount;
			area.m_iContacts++;
			area.m_vSum += contact.m_vPos;
			if (contact.m_fLastSeen > area.m_fSeen)
				area.m_fSeen = contact.m_fLastSeen;
		}

		// strongest first
		array<CLW_SitrepArea> sorted = {};
		foreach (CLW_SitrepArea candidate : areas)
		{
			int at = sorted.Count();
			int score = CLW_Places.Score(candidate.m_iVehicles, candidate.m_iInfantry, candidate.m_iCount);
			for (int k = 0; k < sorted.Count(); k++)
			{
				if (score > CLW_Places.Score(sorted[k].m_iVehicles, sorted[k].m_iInfantry, sorted[k].m_iCount))
				{
					at = k;
					break;
				}
			}

			sorted.InsertAt(candidate, at);
		}

		string text = "";
		int shown;
		foreach (CLW_SitrepArea area2 : sorted)
		{
			if (shown >= MAX_INTEL_AREAS)
				break;

			vector pos = area2.m_vSum / area2.m_iContacts;
			int strength = CLW_Places.Score(area2.m_iVehicles, area2.m_iInfantry, area2.m_iCount);
			string noun = "infantry activity";
			if (area2.m_iVehicles > 0)
				noun = "movement";
			string line = CLW_Places.Magnitude(strength) + " enemy " + noun + " " + Where(pos, points, factionKey);
			line += ", grid " + CLW_Places.Grid(pos) + ": " + CLW_Places.SizeText(area2.m_iVehicles, area2.m_iInfantry, area2.m_iCount);
			line += ", " + AgeWords(Math.Round(Math.Max(0, now - area2.m_fSeen))) + ".";
			if (shown > 0)
				text += "\n";
			text += line;
			shown++;
		}

		if (sorted.Count() >= 2)
			text += "\n" + Assessment(sorted[0], sorted[1], factionKey);

		return text;
	}

	// One line on the likely enemy main effort (strongest area) and the secondary one.
	protected static string Assessment(CLW_SitrepArea top, CLW_SitrepArea second, string factionKey)
	{
		string aim = "centred on " + top.m_sShort;
		if (top.m_Point)
		{
			if (top.m_Point.m_sOwner == factionKey)
				aim = "aimed at " + top.m_sShort + ", which we hold";
			else if (top.m_Point.m_sOwner.IsEmpty())
				aim = "to take " + top.m_sShort;
			else
				aim = "to reinforce " + top.m_sShort;
		}

		string preposition = "near";
		if (second.m_Point)
			preposition = "at";

		return "Assessment: enemy main effort likely " + aim + "; secondary activity " + preposition + " " + second.m_sShort + ".";
	}

	protected static CLW_SitrepArea FindArea(notnull array<ref CLW_SitrepArea> areas, string key)
	{
		foreach (CLW_SitrepArea area : areas)
		{
			if (area.m_sKey == key)
				return area;
		}

		return null;
	}

	// Which area a position belongs to: the nearest objective within 1 km, else the nearest named place, else its map grid square.
	protected static void AreaOf(vector pos, CLW_ControlPoints points, out string key, out string shortName, out CLW_ControlPoint areaPoint)
	{
		areaPoint = null;
		float objDist;
		CLW_ControlPoint point = CLW_Places.NearestObjective(pos, points, objDist);
		if (point && objDist <= AREA_OBJECTIVE_M)
		{
			areaPoint = point;
			key = "o" + point.m_sId;
			shortName = Label(point);
			return;
		}

		float placeDist;
		vector placePos;
		string place = CLW_Places.Nearest(pos, PLACE_NEAR_M, placeDist, placePos);
		if (!place.IsEmpty())
		{
			key = "p" + place;
			shortName = place;
			return;
		}

		string grid = CLW_Places.Grid(pos);
		key = "g" + grid;
		shortName = "grid " + grid;
	}

	// ---------------------------------------------------------------- mission / execution

	// Per (command, target) the number of units with an ACTIVE order, most units first. idle = units without one.
	protected static void CollectEfforts(notnull array<string> ids, CLW_Orders orders, CLW_ControlPoints points, string factionKey, notnull array<ref CLW_SitrepEffort> efforts, out int idle)
	{
		idle = 0;
		foreach (string id : ids)
		{
			if (!orders || orders.UnitAlive(id) <= 0)
				continue;

			CLW_UnitOrder order = orders.GetOrder(id);
			if (!order || order.m_sStatus != CLW_Orders.STATUS_ACTIVE)
			{
				idle++;
				continue;
			}

			string target = TargetNoun(order, points, factionKey);
			CLW_SitrepEffort found;
			foreach (CLW_SitrepEffort effort : efforts)
			{
				if (effort.m_eCommand == order.m_eCommand && effort.m_sTarget == target)
				{
					found = effort;
					break;
				}
			}

			if (!found)
			{
				found = new CLW_SitrepEffort();
				found.m_eCommand = order.m_eCommand;
				found.m_sTarget = target;
				efforts.Insert(found);
			}

			found.m_iCount++;
		}

		// most units first (insertion sort; fall-back efforts never lead)
		for (int a = 1; a < efforts.Count(); a++)
		{
			CLW_SitrepEffort current = efforts[a];
			int b = a - 1;
			while (b >= 0 && EffortWeight(efforts[b]) < EffortWeight(current))
			{
				efforts.Set(b + 1, efforts[b]);
				b--;
			}

			efforts.Set(b + 1, current);
		}
	}

	protected static int EffortWeight(CLW_SitrepEffort effort)
	{
		if (effort.m_eCommand == CLW_ECommand.FALL_BACK)
			return effort.m_iCount - 1000;

		return effort.m_iCount;
	}

	// One SMEAC-style mission statement: who, what, where, why.
	protected static string BuildMission(notnull array<ref CLW_SitrepEffort> efforts, int ticketState)
	{
		if (efforts.IsEmpty())
			return "Our forces consolidate and screen, then take and hold the objectives to preserve our tickets.";

		CLW_SitrepEffort lead = efforts[0];
		string text = "Our forces " + MissionVerb(lead.m_eCommand) + " " + lead.m_sTarget;
		if (efforts.Count() > 1 && efforts[1].m_eCommand != CLW_ECommand.FALL_BACK)
			text += " and " + MissionVerb(efforts[1].m_eCommand) + " " + efforts[1].m_sTarget;

		return text + " " + MissionPurpose(lead.m_eCommand, ticketState) + ".";
	}

	protected static string MissionPurpose(CLW_ECommand command, int ticketState)
	{
		switch (command)
		{
			case CLW_ECommand.DEFEND:
				if (ticketState > 0)
					return "to protect our lead";
				return "to deny the enemy the ground";
			case CLW_ECommand.RECON:
				return "to locate the enemy main body";
			case CLW_ECommand.FALL_BACK:
				return "to regroup and preserve combat power";
		}

		if (ticketState < 0)
			return "to turn the ticket count";
		if (ticketState > 0)
			return "to keep the pressure on and hold our lead";

		return "to bleed enemy tickets";
	}

	// Main effort / supporting effort / withdrawing / reserve, counted in words; never unit ids.
	protected static string BuildExecution(notnull array<ref CLW_SitrepEffort> efforts, int idle, int humanSquads)
	{
		string text = "";
		string supporting = "";
		string withdrawing = "";
		int supportShown;
		for (int i = 0; i < efforts.Count(); i++)
		{
			CLW_SitrepEffort effort = efforts[i];
			string clause = Elements(effort.m_iCount) + " " + EffortVerb(effort.m_eCommand, effort.m_iCount) + " " + effort.m_sTarget;
			if (effort.m_eCommand == CLW_ECommand.FALL_BACK)
			{
				withdrawing = clause;
				continue;
			}

			if (i == 0)
			{
				text = "Main effort: " + clause + ".";
				continue;
			}

			if (supportShown >= 2)
				continue;

			if (supporting.IsEmpty())
				supporting = clause;
			else
				supporting += "; " + clause;
			supportShown++;
		}

		if (!supporting.IsEmpty())
			text = AppendLine(text, "Supporting: " + supporting + ".");
		if (!withdrawing.IsEmpty())
			text = AppendLine(text, "Withdrawing: " + withdrawing + ".");
		if (idle > 0)
			text = AppendLine(text, "Reserve: " + Elements(idle) + " held back.");
		if (humanSquads > 0)
			text = AppendLine(text, "Squad leaders: check your HQ tasking in the task list (J).");
		if (text.IsEmpty())
			text = "No elements tasked yet.";

		return text;
	}

	protected static string EffortVerb(CLW_ECommand command, int count)
	{
		bool one = count == 1;
		switch (command)
		{
			case CLW_ECommand.ATTACK:
				if (one)
					return "assaults";
				return "assault";
			case CLW_ECommand.DEFEND:
				if (one)
					return "defends";
				return "defend";
			case CLW_ECommand.RECON:
				if (one)
					return "reconnoitres";
				return "reconnoitre";
			case CLW_ECommand.FALL_BACK:
				if (one)
					return "falls back to";
				return "fall back to";
		}

		if (one)
			return "moves to";

		return "move to";
	}

	protected static string MissionVerb(CLW_ECommand command)
	{
		switch (command)
		{
			case CLW_ECommand.ATTACK: return "seize";
			case CLW_ECommand.DEFEND: return "hold";
			case CLW_ECommand.RECON: return "reconnoitre";
			case CLW_ECommand.FALL_BACK: return "withdraw to";
		}

		return "occupy";
	}

	// ---------------------------------------------------------------- logistics

	protected static string BuildLogistics(string factionKey, CLW_WarState war, CLW_ForceSpawner spawner, int wornDown, int ineffective)
	{
		int supplies = war.GetSupplies(factionKey);
		int incomeSec = Math.Max(1, war.GetIncomeIntervalSec());
		int perMinute = Math.Round(war.GetIncome(factionKey) * 60.0 / incomeSec);

		string supplyState = "adequate";
		if (supplies <= 0)
			supplyState = "exhausted";
		else if (perMinute > 0 && supplies < perMinute)
			supplyState = "tight";
		else if (perMinute <= 0 && supplies < 100)
			supplyState = "tight";
		else if (perMinute > 0 && supplies > perMinute * 4)
			supplyState = "plentiful";

		string text = "Supplies are " + supplyState;
		if (perMinute <= 0)
			text += " with no resupply income.";
		else if (perMinute < 20)
			text += " with a modest income.";
		else if (perMinute < 60)
			text += " with a steady income.";
		else
			text += " with a strong income.";

		if (spawner)
		{
			int liveVehicles = spawner.GetLiveVehicles(factionKey);
			int vehicleCap = war.GetVehicleCap(factionKey);
			text += " " + VehicleWords(liveVehicles, vehicleCap);
		}

		string worn = "";
		if (wornDown > 0)
			worn = Elements(wornDown) + " worn down";
		if (ineffective > 0)
		{
			if (!worn.IsEmpty())
				worn += ", ";
			worn += Elements(ineffective) + " combat-ineffective";
		}

		if (worn.IsEmpty())
			text += " All elements remain combat-effective.";
		else
			text += " " + Cap(worn) + ".";

		text += " " + ReinforceWords(factionKey, supplies, war, spawner);
		return text;
	}

	protected static string VehicleWords(int live, int cap)
	{
		if (live <= 0)
			return "No vehicles are in service.";

		if (cap <= 0)
			return "Vehicles are in service.";

		float ratio = live * 1.0 / cap;
		if (ratio < 0.35)
			return "Few vehicles are in service.";
		if (ratio < 0.75)
			return "About half our vehicles are in service.";

		return "Most of our vehicles are in service.";
	}

	protected static string ReinforceWords(string factionKey, int supplies, CLW_WarState war, CLW_ForceSpawner spawner)
	{
		if (spawner && war.GetAiCap(factionKey) > 0 && spawner.GetLiveAI(factionKey) >= war.GetAiCap(factionKey))
			return "We are at force cap, no reinforcements needed.";

		int cheapest = -1;
		CLW_ForceCatalog catalog = CLW_GameMode.GetForceCatalog();
		if (catalog)
		{
			array<CLW_ForceEntry> entries = {};
			catalog.GetEntries(factionKey, entries);
			foreach (CLW_ForceEntry entry : entries)
			{
				if (!entry.m_bEnabled)
					continue;

				if (cheapest < 0 || entry.m_iCost < cheapest)
					cheapest = entry.m_iCost;
			}
		}

		if (cheapest >= 0 && supplies >= cheapest)
			return "Reinforcements can be bought.";

		return "We cannot afford reinforcements for now.";
	}

	// ---------------------------------------------------------------- words

	// -2 badly behind .. +2 well ahead; 0 when tickets are not in play.
	protected static int TicketState(int tickets, int enemyTickets)
	{
		if (tickets < 0 || enemyTickets < 0 || tickets + enemyTickets <= 0)
			return 0;

		float ratio = tickets * 1.0 / (tickets + enemyTickets);
		if (ratio >= 0.58)
			return 2;
		if (ratio >= 0.53)
			return 1;
		if (ratio >= 0.47)
			return 0;
		if (ratio >= 0.40)
			return -1;

		return -2;
	}

	protected static string TicketWords(int state)
	{
		switch (state)
		{
			case 2: return "We are well ahead on tickets.";
			case 1: return "We hold a narrow lead on tickets.";
			case 0: return "Tickets are roughly even.";
			case -1: return "We are behind on tickets.";
		}

		return "We are badly behind on tickets.";
	}

	protected static string MomentumWords(int momentum)
	{
		if (momentum >= 2)
			return "Initiative is ours";
		if (momentum == 1)
			return "We hold a slight advantage";
		if (momentum == 0)
			return "The fight is evenly balanced";
		if (momentum == -1)
			return "We are under pressure";

		return "We are losing ground";
	}

	protected static string ObjectivesWords(int held, int total)
	{
		if (held >= total)
			return "every objective";
		if (held <= 0)
			return "no objectives";

		float share = held * 1.0 / total;
		if (share >= 0.6)
			return "most objectives";
		if (share >= 0.4)
			return "about half the objectives";

		return "a few objectives";
	}

	protected static string ForceStateWords(int live, int cap)
	{
		if (cap <= 0)
			return "";

		float ratio = live * 1.0 / cap;
		if (ratio >= 0.85)
			return "Our forces are at full strength.";
		if (ratio >= 0.6)
			return "Our forces are in good shape.";
		if (ratio >= 0.35)
			return "Our forces are at reduced strength.";

		return "Our forces are badly depleted.";
	}

	// Rounded time left in words.
	protected static string TimeLeftWords(int seconds)
	{
		int minutes = Math.Round(seconds / 60.0);
		if (seconds < 60)
			return "Less than a minute remains.";
		if (minutes <= 1)
			return "About a minute remains.";
		if (minutes >= 70)
			return "Over an hour remains.";
		if (minutes >= 58)
			return "About an hour remains.";
		if (minutes > 12)
			minutes = ((minutes + 2) / 5) * 5;

		return "About " + NumberWord(minutes) + " minutes remain.";
	}

	protected static string AgeWords(int ageSec)
	{
		if (ageSec < 20)
			return "just now";
		if (ageSec < 75)
			return "within the last minute";
		if (ageSec < CONTACT_STALE_SEC + 30)
			return "in the last few minutes";

		return "earlier (dated)";
	}

	static string NumberWord(int n)
	{
		array<string> small = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
		if (n < 0)
			return "no";
		if (n < 20)
			return small[n];
		if (n >= 100)
			return "many";

		array<string> tens = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
		int ten = n / 10;
		int unit = n - ten * 10;
		if (unit == 0)
			return tens[ten];

		return tens[ten] + "-" + small[unit];
	}

	protected static string Elements(int n)
	{
		if (n == 1)
			return "one element";

		return NumberWord(n) + " elements";
	}

	protected static string PluralS(int n)
	{
		if (n == 1)
			return "s";

		return "";
	}

	protected static string IsAre(int n)
	{
		if (n == 1)
			return "is";

		return "are";
	}

	protected static string Cap(string text)
	{
		if (text.IsEmpty())
			return text;

		string first = text.Substring(0, 1);
		first.ToUpper();
		return first + text.Substring(1, text.Length() - 1);
	}

	protected static string AppendLine(string text, string line)
	{
		if (text.IsEmpty())
			return line;

		return text + "\n" + line;
	}

	// "Timber Hub (C) and Ridge Junction (B)"; more than three: the first three and "N more".
	protected static string ListWords(notnull array<CLW_ControlPoint> list)
	{
		string text = "";
		int n = list.Count();
		int shown = Math.Min(n, 3);
		for (int i = 0; i < shown; i++)
		{
			string item = Label(list[i]);
			if (i == 0)
				text = item;
			else if (i == shown - 1 && n == shown)
				text += " and " + item;
			else
				text += ", " + item;
		}

		if (n > shown)
			text += " and " + NumberWord(n - shown) + " more";

		return text;
	}

	protected static string Label(CLW_ControlPoint point)
	{
		return point.m_sName + " (" + point.m_sId + ")";
	}

	// ---------------------------------------------------------------- places

	// Target of an order as a noun phrase.
	protected static string TargetNoun(CLW_UnitOrder order, CLW_ControlPoints points, string factionKey)
	{
		if (!order.m_sTarget.IsEmpty() && points)
		{
			CLW_ControlPoint point = points.Find(order.m_sTarget);
			if (point)
				return Label(point);
		}

		return PlaceNoun(order.m_vPos, points, factionKey);
	}

	// "our HQ", "Timber Hub (C)", "the northern approach to Ridge Junction (B)", "the area around Newfield".
	protected static string PlaceNoun(vector pos, CLW_ControlPoints points, string factionKey)
	{
		if (NearOwnHq(pos, factionKey))
			return "our HQ";

		float objDist;
		CLW_ControlPoint point = CLW_Places.NearestObjective(pos, points, objDist);
		if (point)
		{
			if (objDist <= Math.Max(point.m_fRadius, 120) * 1.2)
				return Label(point);
			if (objDist <= APPROACH_M)
				return "the " + CLW_Places.CompassAdjective(CLW_Places.CompassIndex(point.m_vPos, pos)) + " approach to " + Label(point);
		}

		float placeDist;
		vector placePos;
		string place = CLW_Places.Nearest(pos, PLACE_NEAR_M, placeDist, placePos);
		if (!place.IsEmpty())
			return "the area around " + place;

		return "grid " + CLW_Places.Grid(pos);
	}

	// Location with its preposition: "at Timber Hub (C)", "on the northern approach to Ridge Junction (B)", "near Newfield",
	// "close to our HQ", "in open country". Never coordinates.
	static string Where(vector pos, CLW_ControlPoints points, string factionKey)
	{
		if (NearOwnHq(pos, factionKey))
			return "close to our HQ";

		float objDist;
		CLW_ControlPoint point = CLW_Places.NearestObjective(pos, points, objDist);
		if (point)
		{
			if (objDist <= Math.Max(point.m_fRadius, 120) * 1.2)
				return "at " + Label(point);
			if (objDist <= APPROACH_M)
				return "on the " + CLW_Places.CompassAdjective(CLW_Places.CompassIndex(point.m_vPos, pos)) + " approach to " + Label(point);
		}

		float placeDist;
		vector placePos;
		string place = CLW_Places.Nearest(pos, PLACE_NEAR_M, placeDist, placePos);
		if (!place.IsEmpty())
		{
			if (placeDist < 200)
				return "at " + place;

			return "near " + place;
		}

		return "in open country";
	}

	protected static bool NearOwnHq(vector pos, string factionKey)
	{
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (!bases || factionKey.IsEmpty())
			return false;

		vector hq = bases.GetBasePos(factionKey);
		if (hq == vector.Zero)
			return false;

		return vector.DistanceXZ(pos, hq) < HQ_NEAR_M;
	}
}

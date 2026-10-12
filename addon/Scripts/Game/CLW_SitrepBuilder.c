// Rule-based faction SITREP (works without any LLM): composes the template sections (CLW_SitrepTemplate) from the SAME fog-limited
// picture the commander uses: this faction's contact book, its own units/orders, the public war state (objectives, tickets).
// Nothing about the enemy beyond the contact book and public values is read. Server only. No faction keys / names are
// hard-wired: everything comes from the scenario, the control points and the registry.
class CLW_SitrepBuilder
{
	static const int MAX_CONTACT_LINES = 4;
	static const int MAX_UNIT_LINES = 5;
	static const float CONTACT_STALE_SEC = 90;		// older contacts are listed as "dated"

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
			sitrep.SetHeadline(factionKey + " SITREP: no war state");
			return sitrep;
		}

		sitrep.m_fWarTime = war.GetElapsedSec();
		if (!war.IsRunning())
		{
			BuildNotRunning(sitrep, factionKey, war);
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
		int people;
		int wounded;
		int humanSquads;
		array<string> weakUnits = {};
		foreach (string id : ids)
		{
			int alive = 0;
			if (orders)
				alive = orders.UnitAlive(id);
			if (alive <= 0)
				continue;

			unitCount++;
			people += alive;
			if (orders && orders.IsWeak(id))
			{
				int start = orders.StartStrength(id, alive);
				weakUnits.Insert(id + " " + alive + "/" + start);
			}

			SCR_AIGroup group = registry.Find(id);
			if (group && CLW_RuleCommander.HasHumanLeader(group))
				humanSquads++;
		}

		// ---- objectives ----
		string held = "";
		string lost = "";
		string neutral = "";
		string contested = "";
		int total;
		int heldCount;
		if (points)
		{
			total = points.Count();
			for (int i = 0; i < total; i++)
			{
				CLW_ControlPoint point = points.Get(i);
				if (!point)
					continue;

				string label = point.m_sId + " " + point.m_sName;
				if (point.m_bContested)
					contested = Append(contested, label);
				if (point.m_sOwner == factionKey)
				{
					heldCount++;
					held = Append(held, label);
				}
				else if (point.m_sOwner.IsEmpty())
					neutral = Append(neutral, label);
				else
					lost = Append(lost, label);
			}
		}

		int tickets = war.GetTickets(factionKey);
		int enemyTickets = war.GetTickets(enemyKey);
		int supplies = war.GetSupplies(factionKey);
		int timeLeft = war.GetTimeLeftSec();

		// ---- headline ----
		string head = factionKey + " SITREP T+" + CLW_Sitrep.FormatClock(sitrep.m_fWarTime);
		if (tickets >= 0)
			head += " | tickets " + tickets + " vs " + enemyTickets;
		head += " | points " + heldCount + "/" + total + " | " + unitCount + " units, " + contacts.Count() + " contacts";
		sitrep.SetHeadline(head);

		// ---- S: situation ----
		string situation = "Holding " + ListOrNone(held) + ".";
		if (!lost.IsEmpty())
			situation += " Enemy holds " + lost + ".";
		if (!neutral.IsEmpty())
			situation += " Open: " + neutral + ".";
		if (!contested.IsEmpty())
			situation += " Contested: " + contested + ".";
		if (tickets >= 0)
			situation += "\nTickets " + tickets + " (enemy " + enemyTickets + ").";
		if (timeLeft >= 0)
			situation += " " + CLW_Sitrep.FormatClock(timeLeft) + " left.";
		sitrep.SetBody(0, situation);

		// ---- E: enemy (SALUTE, condensed to what this faction has seen) ----
		sitrep.SetBody(1, BuildEnemy(contacts, now, points));

		// ---- M + X: from the active orders ----
		string mission;
		string execution;
		BuildMissionExecution(factionKey, ids, orders, registry, points, humanSquads, mission, execution);
		sitrep.SetBody(2, mission);
		sitrep.SetBody(3, execution);

		// ---- L: logistics ----
		string logistics = "Supplies " + supplies;
		int incomeSec = Math.Max(1, war.GetIncomeIntervalSec());
		int perMinute = Math.Round(war.GetIncome(factionKey) * 60.0 / incomeSec);
		if (perMinute > 0)
			logistics += " (+" + perMinute + "/min)";
		CLW_ForceSpawner spawner = CLW_GameMode.GetForceSpawner();
		if (spawner)
			logistics += ". AI " + spawner.GetLiveAI(factionKey) + "/" + war.GetAiCap(factionKey) + ", vehicles " + spawner.GetLiveVehicles(factionKey) + "/" + war.GetVehicleCap(factionKey);
		logistics += ". " + people + " personnel";
		if (!weakUnits.IsEmpty())
		{
			logistics += ". Weak:";
			for (int w = 0; w < weakUnits.Count() && w < MAX_UNIT_LINES; w++)
			{
				logistics += " " + weakUnits[w];
			}
		}

		sitrep.SetBody(4, logistics + ".");
		return sitrep;
	}

	protected static void BuildNotRunning(CLW_Sitrep sitrep, string factionKey, CLW_WarState war)
	{
		string phase = war.GetPhaseName();
		sitrep.SetHeadline(factionKey + " SITREP | war " + phase);
		if (war.IsEnded())
		{
			string winner = war.GetWinner();
			if (winner.IsEmpty())
				sitrep.SetBody(0, "The war is over: draw.");
			else
				sitrep.SetBody(0, "The war is over. Winner: " + winner + ".");
			return;
		}

		sitrep.SetBody(0, "The war has not started yet. HQ staff is assembling; the first full SITREP follows when forces deploy.");
		sitrep.SetBody(2, "Capture and hold the objectives, preserve tickets.");
		sitrep.SetBody(3, "Pick a squad in the deploy menu; your squad leader receives HQ missions once the war runs.");
	}

	// SALUTE-condensed: size, kind, location, age; freshest first. Positions are the contact's average position.
	protected static string BuildEnemy(notnull array<CLW_Contact> contacts, float now, CLW_ControlPoints points)
	{
		if (contacts.IsEmpty())
			return "No enemy contact reported.";

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

		string text = "";
		int shown;
		foreach (CLW_Contact seen : sorted)
		{
			if (shown >= MAX_CONTACT_LINES)
				break;

			if (shown > 0)
				text += "\n";
			int age = Math.Round(Math.Max(0, now - seen.m_fLastSeen));
			text += seen.m_iCount.ToString() + "x " + seen.m_sEnemyFaction + " " + seen.m_sKind + ", " + Where(seen.m_vPos, points) + ", " + AgeText(age);
			shown++;
		}

		if (sorted.Count() > shown)
			text += "\n+" + (sorted.Count() - shown) + " older reports.";

		return text;
	}

	// Mission = the objective that most units are working on; execution = a few unit orders + what players can do.
	protected static void BuildMissionExecution(string factionKey, notnull array<string> ids, CLW_Orders orders, CLW_SquadRegistry registry, CLW_ControlPoints points, int humanSquads, out string mission, out string execution)
	{
		map<string, int> perTarget = new map<string, int>();
		array<string> lines = {};
		int idle;
		int active;
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

			active++;
			string target = TargetLabel(order, points);
			int count = 0;
			perTarget.Find(target, count);
			perTarget.Set(target, count + 1);
			if (lines.Count() < MAX_UNIT_LINES)
				lines.Insert(id + " " + CLW_Command.ToName(order.m_eCommand) + " " + target);
		}

		string mainTarget;
		int mainCount;
		foreach (string key, int value : perTarget)
		{
			if (value > mainCount)
			{
				mainCount = value;
				mainTarget = key;
			}
		}

		if (mainTarget.IsEmpty())
			mission = "Consolidate, scout and take or hold the objectives; preserve tickets.";
		else
			mission = "Main effort: " + mainTarget + " (" + mainCount + " units). Take and hold the objectives, preserve tickets.";

		execution = "";
		foreach (string line : lines)
		{
			if (!execution.IsEmpty())
				execution += "\n";
			execution += line;
		}

		if (active > lines.Count())
			execution += "\n+" + (active - lines.Count()) + " more units tasked.";
		if (idle > 0)
		{
			if (!execution.IsEmpty())
				execution += "\n";
			execution += idle.ToString() + " units idle / in reserve.";
		}

		if (humanSquads > 0)
		{
			if (!execution.IsEmpty())
				execution += "\n";
			execution += "Squad leaders: accept an HQ mission in the task list (J).";
		}

		if (execution.IsEmpty())
			execution = "No units tasked yet.";
	}

	protected static string TargetLabel(CLW_UnitOrder order, CLW_ControlPoints points)
	{
		if (!order.m_sTarget.IsEmpty() && points)
		{
			CLW_ControlPoint point = points.Find(order.m_sTarget);
			if (point)
				return point.m_sId + " " + point.m_sName;
		}

		return Where(order.m_vPos, points);
	}

	// "near C Timber Hub (300 m NE)" or a grid reference when no objective is within 1500 m.
	static string Where(vector pos, CLW_ControlPoints points)
	{
		CLW_ControlPoint best;
		float bestDist = 1000000;
		if (points)
		{
			for (int i = 0; i < points.Count(); i++)
			{
				CLW_ControlPoint point = points.Get(i);
				if (!point)
					continue;

				float d = vector.DistanceXZ(pos, point.m_vPos);
				if (d < bestDist)
				{
					bestDist = d;
					best = point;
				}
			}
		}

		string grid = "grid " + GridPart(pos[0]) + " " + GridPart(pos[2]);
		if (!best || bestDist > 1500)
			return grid;

		int dist = Math.Round(bestDist / 10) * 10;
		if (dist < 60)
			return "at " + best.m_sId + " " + best.m_sName;

		return dist.ToString() + " m " + Compass(best.m_vPos, pos) + " of " + best.m_sId + " " + best.m_sName + " (" + grid + ")";
	}

	protected static string GridPart(float value)
	{
		int cell = Math.Floor(Math.Max(0, value) / 100);
		string text = cell.ToString();
		while (text.Length() < 3)
		{
			text = "0" + text;
		}

		return text;
	}

	// Compass direction from "from" to "to" (8 points; x = east, z = north).
	protected static string Compass(vector from, vector to)
	{
		float dx = to[0] - from[0];
		float dz = to[2] - from[2];
		float angle = Math.Atan2(dx, dz) * Math.RAD2DEG;	// 0 = north, 90 = east
		if (angle < 0)
			angle += 360;

		int index = Math.Round(angle / 45) % 8;
		switch (index)
		{
			case 0: return "N";
			case 1: return "NE";
			case 2: return "E";
			case 3: return "SE";
			case 4: return "S";
			case 5: return "SW";
			case 6: return "W";
			case 7: return "NW";
		}

		return "N";
	}

	protected static string AgeText(int ageSec)
	{
		if (ageSec < 15)
			return "now";
		if (ageSec < 120)
			return ageSec.ToString() + " s ago";

		int minutes = ageSec / 60;
		return minutes.ToString() + " min ago";
	}

	protected static string Append(string list, string item)
	{
		if (list.IsEmpty())
			return item;

		return list + ", " + item;
	}

	protected static string ListOrNone(string list)
	{
		if (list.IsEmpty())
			return "no objective";

		return list;
	}
}

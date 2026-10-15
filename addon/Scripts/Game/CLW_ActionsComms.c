// Player comms entries on the action registry (HQ comms WP1): REPORT_CONTACT, REPORT_CANCEL (capability "reports", logic in
// CLW_Reports) and REQUEST_BACKUP (capability "requests", logic in CLW_Backup). Authority "player", channel "requests", tag "request":
// the /hq agent turn sees them as core tools, so "/hq 2 enemy infantry about 100 m north" becomes a real report. Registered from
// CLW_ActionsRequests.Register(). The executors fill call.m_sResultWire; the caller (CLW_HqChat.RunAction, chat, menu) answers the player.
// Executors are shared per entry: no per-call state in members.

class CLW_Action_ReportContact : CLW_ActionExecutor
{
	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (call.m_iPlayerId <= 0)
		{
			error = "no reporting player";
			call.m_sResultWire = CLW_Loc.W("#CLW-Req_NoPlayer");
			return false;
		}

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		string wire;
		bool ok = CLW_Reports.Submit(call.m_iPlayerId, call.GetString("kind"), call.GetString("count"), call.GetString("distance"), call.GetString("direction"), result, wire);
		call.m_sResultWire = wire;
		if (ok)
			call.m_iApplied = 1;

		string verdict = "REFUSED";
		if (ok)
			verdict = "ACCEPTED";

		Print(string.Format("CLW: report action player=%1(%2) kind='%3' count='%4' dist='%5' dir='%6' -> %7: %8", call.m_iPlayerId, CLW_RequestService.PlayerName(call.m_iPlayerId), call.GetString("kind"), call.GetString("count"), call.GetString("distance"), call.GetString("direction"), verdict, result), LogLevel.NORMAL);
		return ok;
	}
}

class CLW_Action_ReportCancel : CLW_ActionExecutor
{
	override bool Validate(CLW_ActionCall call, out string error)
	{
		if (call.m_iPlayerId <= 0)
		{
			error = "no reporting player";
			call.m_sResultWire = CLW_Loc.W("#CLW-Req_NoPlayer");
			return false;
		}

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		string wire;
		bool ok = CLW_Reports.Cancel(call.m_iPlayerId, result, wire);
		call.m_sResultWire = wire;
		if (ok)
			call.m_iApplied = 1;

		Print(string.Format("CLW: report cancel action player=%1(%2) -> %3: %4", call.m_iPlayerId, CLW_RequestService.PlayerName(call.m_iPlayerId), ok, result), LogLevel.NORMAL);
		return ok;
	}
}

// REQUEST_BACKUP: help call. Any role (the leader check of the base class does not apply to this kind), own cooldown (CLW_BackupConfig).
// The group selection (FindGroup) and the order are those of REQUEST_REINFORCE; the call is recorded even when no group is free.
class CLW_Action_RequestBackup : CLW_Action_RequestOrder
{
	protected override string KindName()
	{
		return "backup";
	}

	override bool Validate(CLW_ActionCall call, out string error)
	{
		call.SetString("where", "here");
		if (!super.Validate(call, error))
			return false;

		float left = CLW_Backup.CooldownLeft(call.m_iPlayerId);
		if (left > 0)
			return Refuse(call, error, "backup was called a moment ago, try again in " + Math.Round(left) + " s", CLW_Loc.W("#CLW-Bkp_Cooldown", CLW_Loc.N(Math.Round(left))));

		return true;
	}

	override bool Execute(CLW_ActionCall call, out string result)
	{
		vector pos;
		if (!CLW_RequestService.PlayerPos(call.m_iPlayerId, pos))
			return Refuse(call, result, "you are not deployed", CLW_Loc.W("#CLW-Req_NotDeployed"));

		float distance;
		string squad = FindGroup(call, pos, distance);
		if (!squad.IsEmpty())
		{
			string why;
			string reason = "help call by " + CLW_RequestService.PlayerName(call.m_iPlayerId);
			if (!call.m_Rules.ApplyOrder(squad, "DEFEND", "", pos, reason, why))
			{
				Print(string.Format("CLW: backup could not order %1: %2", squad, why), LogLevel.WARNING);
				squad = "";
			}
		}

		CLW_Backup.Record(call.m_sFaction, call.m_iPlayerId, squad, pos, call.GetString("note"));
		call.m_iApplied = 1;
		if (squad.IsEmpty())
		{
			result = "help call received, but no free group is available. HQ has been informed.";
			call.m_sResultWire = CLW_Loc.W("#CLW-Bkp_NoGroup");
			return true;
		}

		result = string.Format("help call received. %1 is moving to your position, %2 m away.", squad, Math.Round(distance));
		call.m_sResultWire = CLW_Loc.W("#CLW-Bkp_Sent", squad, CLW_Loc.N(Math.Round(distance)));
		return true;
	}
}

class CLW_ActionsComms
{
	// Called by CLW_ActionsRequests.Register() (right after the registry reset).
	static void Register()
	{
		CLW_ActionRegistry registry = CLW_ActionRegistry.Get();
		Add(registry, "REPORT_CONTACT", "A player reports an enemy contact to HQ (same as the chat command /report): HQ marks it on the map and the commander uses it. Use it when the player says what he sees and where, e.g. '2 enemy infantry 100 m north'. Needs the kind and the distance; the direction defaults to 'ahead'.", new CLW_Action_ReportContact(), "reports")
			.AddParam("kind", "string", true, "inf (infantry), veh (vehicle or armor), air (aircraft, helicopter) or unknown")
			.AddParam("count", "string", false, "number of enemies or vehicles, default 1")
			.AddParam("distance", "string", true, "distance from the player in metres, 10 to 3000 (a number, e.g. 150)")
			.AddParam("direction", "string", false, "N, NE, E, SE, S, SW, W, NW, a compass bearing in degrees 0-359 (0 = north, 90 = east), or 'ahead' = where the player is looking right now (default)");
		Add(registry, "REPORT_CANCEL", "A player takes back his last contact report (the contact disappears from the map and the HQ picture), e.g. 'cancel my report', 'that was a mistake'. Only his most recent report, and only while it is still there.", new CLW_Action_ReportCancel(), "reports");
		Add(registry, "REQUEST_BACKUP", "A player calls for backup / help because he is in trouble (urgent, any role, not only squad leaders). HQ sends the nearest free AI group to defend the player's position and tells the commander. Cooldown 2 minutes.", new CLW_Action_RequestBackup(), "requests")
			.AddParam("note", "string", false, "what is happening (short), e.g. 'under heavy fire from the north'");
		Print("CLW: comms entries registered (REPORT_CONTACT, REPORT_CANCEL, REQUEST_BACKUP)", LogLevel.NORMAL);
	}

	protected static CLW_ActionEntry Add(CLW_ActionRegistry registry, string name, string description, CLW_ActionExecutor executor, string capability)
	{
		CLW_ActionEntry entry = CLW_ActionEntry.Create(name, CLW_ActionEntry.LAYER_PRIMITIVE, description, executor);
		entry.m_sAuthority = CLW_ActionEntry.AUTH_PLAYER;
		entry.m_sChannel = "requests";
		entry.m_iCost = 0;
		entry.m_aRequires.Insert(capability);
		entry.m_aTags.Insert("request");
		registry.Register(entry);
		return entry;
	}
}

// Mission board for human squad leaders (spec section 7). Replaces the rule commander's legacy "one forced task" path.
//
// Per human-led squad (leader = a real player) the board keeps a CLW_MissionRec:
//   offers  : 2-3 vanilla ExtendedTask tasks, ownership + visibility EXECUTOR, executor = the leader, NOT assigned.
//   accept  : the player assigns himself in the J menu / map (SCR_Task.GetOnTaskAssigneeAdded) -> sibling offers are deleted.
//   abandon : SCR_Task.GetOnTaskAssigneeRemoved while the task is not finished -> HQ replans, new offers after a pause.
//   finish  : per kind rule (CAPTURE / DEFEND / RECON / DESTROY) -> COMPLETED + reward, or timeout -> FAILED.
// Offers come from the LLM (OnOffers, when fresh) else from the rule commander's plan (BuildOffers).
// Server only. Every state change logs a "CLW: mission ..." line and pushes a "mission ..." event (the sidecar trigger matches it).

// Offers sent by the LLM, waiting for the next publish window of that squad.
class CLW_PendingOffers
{
	ref array<ref CLW_MissionOffer> m_aOffers = {};
	float m_fAt;
}

// Everything the board knows about one human-led squad.
class CLW_MissionRec
{
	string m_sKey;
	string m_sFaction;
	string m_sSquad;
	int m_iPlayer;

	// published offers (parallel arrays; the tasks are entities and read null once deleted)
	ref array<ref CLW_MissionOffer> m_aOffers = {};
	ref array<SCR_Task> m_aTasks = {};
	float m_fOfferedAt = -100000;
	float m_fNextOfferAt;

	// the accepted mission
	SCR_Task m_Task;
	ref CLW_MissionOffer m_Offer;
	float m_fAcceptedAt;
	float m_fHoldSec;
	float m_fClearSince = -1;
	float m_fLastEval = -1;
	int m_iLastProgress = -1;
	bool m_bFinishing;
	bool m_bHoldLost;

	ref array<int> m_aExecutors = {};		// player ids added as task executors (the leader + every connected human of the squad)
	ref array<SCR_Task> m_aFinished = {};	// COMPLETED / FAILED tasks kept for the player's finished tab (bounded)
	ref array<SCR_Task> m_aDoomed = {};		// tasks to delete on the next flush
}

class CLW_MissionBoard : CLW_MissionHook
{
	static const int TICK_MS = 5000;
	static const float OFFER_REFRESH_SEC = 180;		// published offers are replaced at most this often
	static const float PAUSE_AFTER_END_SEC = 60;	// after decline / abandon / completion / failure
	static const float EMPTY_RETRY_SEC = 30;		// nothing to offer: try again later
	static const float LLM_FRESH_SEC = 300;			// LLM offers older than this are ignored
	static const float TIMEOUT_SEC = 900;			// accepted mission fails after 15 min
	static const float DEFEND_HOLD_SEC = 240;
	static const float DEFEND_NEAR_M = 250;			// the leader must be this close to the point (radius added) to count as holding
	static const float RECON_RADIUS_M = 75;
	static const float DESTROY_RADIUS_M = 150;
	static const float DESTROY_CLEAR_SEC = 60;		// no contact left at the target for this long
	static const float CONTACT_NEAR_M = 120;		// a contact this close to the target pos is "still there"
	static const float POS_HOLD_RADIUS_M = 100;		// CAPTURE / DEFEND offered as a bare position
	static const int REWARD_SUPPLIES = 60;
	static const int REWARD_MAJOR_SUPPLIES = 100;
	static const int MAJOR_VALUE = 2;
	static const int MAX_FINISHED_KEPT = 3;
	static const int MAX_OFFERS = 3;
	static const ResourceName TASK_PREFAB = "{CF6A2A13DF7B1018}Prefabs/Tasks/ExtendedTask.et";	// vanilla ExtendedTask (progress bar)

	protected static bool s_bHandlersAdded;
	protected static int s_iCounter;
	protected static ref CLW_MissionBoard s_Board;

	protected ref map<string, ref CLW_MissionRec> m_mRecs = new map<string, ref CLW_MissionRec>();
	protected ref map<string, ref CLW_PendingOffers> m_mPending = new map<string, ref CLW_PendingOffers>();
	protected ref map<string, string> m_mTaskToRec = new map<string, string>();	// task id -> rec key
	protected float m_fLastNoOfferLog = -1000;

	// ---- install ----

	// Create the board, make it the active hook, start the tick, listen to the vanilla assign / unassign events.
	static void Install()
	{
		if (!Replication.IsServer())
			return;

		if (s_Board)
		{
			s_Board.CleanupAll("reinstall");
			GetGame().GetCallqueue().Remove(s_Board.Tick);
		}

		s_Board = new CLW_MissionBoard();
		CLW_MissionHook.Set(s_Board);
		GetGame().GetCallqueue().CallLater(s_Board.Tick, TICK_MS, true);
		if (!s_bHandlersAdded)
		{
			s_bHandlersAdded = true;
			SCR_Task.GetOnTaskAssigneeAdded().Insert(OnAssigneeAdded);		// [verify] static invoker (task, executor, requesterID)
			SCR_Task.GetOnTaskAssigneeRemoved().Insert(OnAssigneeRemoved);
		}

		Print("CLW: mission board installed", LogLevel.NORMAL);
	}

	protected static bool s_bTickQueued;

	// A squad changed hands (leader changed, last human left): re-evaluate soon instead of waiting for the 5 s tick.
	// Debounced; Tick() itself cleans up records of squads that are no longer human-led or whose leader changed.
	static void RequestTick()
	{
		if (!s_Board || !Replication.IsServer() || s_bTickQueued)
			return;

		s_bTickQueued = true;
		GetGame().GetCallqueue().CallLater(TickNow, 300);
	}

	protected static void TickNow()
	{
		s_bTickQueued = false;
		if (s_Board)
			s_Board.Tick();
	}

	// Static trampolines: the invokers are static, the board instance is replaceable.
	protected static void OnAssigneeAdded(SCR_Task task, SCR_TaskExecutor executor, int requesterID)
	{
		if (s_Board && Replication.IsServer())
			s_Board.HandleAssigneeAdded(task, executor);
	}

	protected static void OnAssigneeRemoved(SCR_Task task, SCR_TaskExecutor executor, int requesterID)
	{
		if (s_Board && Replication.IsServer())
			s_Board.HandleAssigneeRemoved(task, executor);
	}

	// ---- hook interface ----

	override bool OwnsHumanSquads()
	{
		return true;
	}

	override void OnOffers(string factionKey, string squadId, notnull array<ref CLW_MissionOffer> offers)
	{
		if (offers.IsEmpty())
			return;

		CLW_PendingOffers pending = new CLW_PendingOffers();
		pending.m_fAt = CLW_Orders.Now();
		foreach (CLW_MissionOffer offer : offers)
		{
			pending.m_aOffers.Insert(offer);
		}

		m_mPending.Set(Key(factionKey, squadId), pending);
		Print(string.Format("CLW: mission LLM offers queued for %1/%2: %3", factionKey, squadId, offers.Count()), LogLevel.NORMAL);
	}

	override string GetMissionJson(string factionKey, string squadId)
	{
		CLW_MissionRec rec;
		if (!m_mRecs.Find(Key(factionKey, squadId), rec) || !rec)
			return "{\"state\":\"none\"}";

		if (rec.m_Task && rec.m_Offer)
			return OfferJson("accepted", rec.m_Offer);

		foreach (CLW_MissionOffer offer : rec.m_aOffers)
		{
			return OfferJson("offered", offer);
		}

		return "{\"state\":\"none\"}";
	}

	// Factions are final now: drop every task and record (keys may have changed).
	override void OnWarStarted()
	{
		super.OnWarStarted();
		CleanupAll("war started");
		m_mPending.Clear();
	}

	// ---- player request (REQUEST_MISSION) ----

	// State of a squad's mission line for a player who asks for a task: none (no record yet) | other (the squad leader is someone
	// else) | accepted | offered (offers published) | queued (offers waiting for the next publish) | free.
	string RequestState(string factionKey, string squadId, int playerId)
	{
		string key = Key(factionKey, squadId);
		CLW_MissionRec rec;
		if (!m_mRecs.Find(key, rec) || !rec)
			return "none";

		if (rec.m_iPlayer != playerId)
			return "other";

		if (rec.m_Task)
			return "accepted";

		if (!rec.m_aOffers.IsEmpty())
			return "offered";

		if (m_mPending.Contains(key))
			return "queued";

		return "free";
	}

	// Number of offers the player still has open (published or waiting); the request cap counts it.
	int PendingOffers(string factionKey, string squadId)
	{
		string key = Key(factionKey, squadId);
		CLW_MissionRec rec;
		int count;
		if (m_mRecs.Find(key, rec) && rec)
			count = rec.m_aOffers.Count();

		CLW_PendingOffers pending;
		if (m_mPending.Find(key, pending) && pending)
			count += pending.m_aOffers.Count();

		return count;
	}

	// Publish these offers (made by the rule commander for the asking player) at the next tick, skipping the pause timers.
	bool QueueNow(string factionKey, string squadId, notnull array<ref CLW_MissionOffer> offers)
	{
		string key = Key(factionKey, squadId);
		CLW_MissionRec rec;
		if (offers.IsEmpty() || !m_mRecs.Find(key, rec) || !rec)
			return false;

		CLW_PendingOffers pending = new CLW_PendingOffers();
		pending.m_fAt = CLW_Orders.Now();
		foreach (CLW_MissionOffer offer : offers)
		{
			pending.m_aOffers.Insert(offer);
		}

		m_mPending.Set(key, pending);
		rec.m_fNextOfferAt = 0;
		Print(string.Format("CLW: mission offers queued on player request for %1/%2: %3", factionKey, squadId, offers.Count()), LogLevel.NORMAL);
		RequestTick();
		return true;
	}

	// ---- helpers ----

	protected static string Key(string factionKey, string squadId)
	{
		return factionKey + "/" + squadId;
	}

	protected string TargetText(CLW_MissionOffer offer)
	{
		if (offer.m_bHasPos)
			return string.Format("%1/%2", Math.Round(offer.m_vPos[0]), Math.Round(offer.m_vPos[2]));

		return offer.m_sTarget;
	}

	protected string OfferJson(string state, CLW_MissionOffer offer)
	{
		string target;
		if (offer.m_bHasPos)
			target = "[" + Math.Round(offer.m_vPos[0]) + "," + Math.Round(offer.m_vPos[2]) + "]";
		else
			target = "\"" + CLW_Situation.Escape(offer.m_sTarget) + "\"";

		return "{\"state\":\"" + state + "\",\"kind\":\"" + offer.m_sKind + "\",\"target\":" + target + "}";
	}

	// Mission kind as a param: the key of its label ("#CLW-Msn_Kind_CAPTURE"); an unknown kind stays as it is.
	protected string KindLabel(string kind)
	{
		if (kind == "CAPTURE" || kind == "DEFEND" || kind == "RECON" || kind == "DESTROY")
			return "#CLW-Msn_Kind_" + kind;

		return kind;
	}

	protected string PointName(string id)
	{
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (!points)
			return "";

		CLW_ControlPoint point = points.Find(id);
		if (!point)
			return "";

		return point.m_sName;
	}

	// Task title as a CLW_Loc wire: "[HQ] Capture C - Timber Hub" (position offers: "[HQ] Recon 2600/2750"). One key per kind and
	// flat params only (the task text is formatted by the vanilla task UI, which cannot render nested wires).
	protected string TaskName(CLW_MissionOffer offer)
	{
		if (!offer.m_bHasPos)
		{
			string name = PointName(offer.m_sTarget);
			if (!name.IsEmpty())
				return CLW_Loc.W("#CLW-Msn_TaskNamed_" + offer.m_sKind, TargetText(offer), name);
		}

		return CLW_Loc.W("#CLW-Msn_Task_" + offer.m_sKind, TargetText(offer));
	}

	// Task description: the brief of the offer (a wire from the rules, or prose from the LLM), else a default line per kind.
	protected string Brief(CLW_MissionOffer offer)
	{
		if (!offer.m_sBrief.IsEmpty())
			return offer.m_sBrief;

		return CLW_Loc.W("#CLW-Msn_BriefDefault_" + offer.m_sKind, TargetText(offer));
	}

	// Target position of an offer (surface Y). False for an unknown objective id.
	protected bool OfferPos(CLW_MissionOffer offer, out vector pos)
	{
		if (offer.m_bHasPos)
		{
			pos = offer.m_vPos;
			pos[1] = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
			return true;
		}

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (!points)
			return false;

		CLW_ControlPoint point = points.Find(offer.m_sTarget);
		if (!point)
			return false;

		pos = point.m_vPos;
		return true;
	}

	protected CLW_MissionRec RecOfTask(SCR_Task task)
	{
		if (!task)
			return null;

		string key;
		if (!m_mTaskToRec.Find(task.GetTaskID(), key))
			return null;

		CLW_MissionRec rec;
		if (m_mRecs.Find(key, rec))
			return rec;

		return null;
	}

	protected void DeleteTaskSafe(SCR_Task task)
	{
		if (!task)
			return;

		string id = task.GetTaskID();
		m_mTaskToRec.Remove(id);
		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (taskSystem)
			taskSystem.DeleteTask(task);
	}

	// ---- popup + log ----

	protected void Notify(CLW_MissionRec rec, string title, string text, string sound, string logLine)
	{
		CLW_Debug.Say("mission: " + logLine);
		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.PushEvent(rec.m_sFaction, "mission " + logLine);

		CLW_MissionBoard.SendPopup(rec.m_iPlayer, title, text, sound);
	}

	// Popup + sound to one player: local path on the listen-server host, owner RPC otherwise (CLW_MissionPlayerController.c).
	static void SendPopup(int playerId, string title, string text, string sound)
	{
		if (playerId <= 0)
			return;

		if (playerId == SCR_PlayerController.GetLocalPlayerId())
		{
			SCR_PlayerController.CLW_MsnShowLocal(title, text, sound);
			return;
		}

		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return;

		SCR_PlayerController controller = SCR_PlayerController.Cast(players.GetPlayerController(playerId));
		if (controller)
			controller.CLW_MsnSend(title, text, sound);
	}

	// ---- tick ----

	protected void Tick()
	{
		if (!Replication.IsServer())
			return;

		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_SquadRegistry registry = CLW_GameMode.GetRegistry();
		CLW_ScenarioConfig scenario = CLW_ScenarioConfig.Get();
		if (!war || !registry || !scenario)
			return;

		if (!war.IsRunning())
		{
			// SETUP (nothing yet) or ENDED: no tasks stay around
			if (!m_mRecs.IsEmpty())
				CleanupAll("war not running");
			return;
		}

		float now = CLW_Orders.Now();
		array<string> seen = {};
		array<string> keys = {};
		scenario.GetFactionKeys(keys);	// read at use time: the keys can change at war start
		foreach (string factionKey : keys)
		{
			CLW_RemoteCommander commander = CLW_GameMode.GetCommander(factionKey);
			if (!commander)
				continue;

			CLW_RuleCommander rules = commander.GetRules();
			if (!rules)
				continue;

			array<string> ids = {};
			registry.GetCommandableIds(factionKey, ids);
			foreach (string squadId : ids)
			{
				SCR_AIGroup group = registry.Find(squadId);
				if (!group)
					continue;

				int playerId = LeaderPlayerId(group);
				string key = Key(factionKey, squadId);
				CLW_MissionRec rec;
				m_mRecs.Find(key, rec);
				if (playerId <= 0)
				{
					// leader entity gone (dead / respawning): keep the record while that player is still connected and in the squad
					if (rec && rec.m_iPlayer > 0 && GetGame().GetPlayerManager().IsPlayerConnected(rec.m_iPlayer) && group.IsPlayerInGroup(rec.m_iPlayer))
					{
						seen.Insert(key);
						Evaluate(rec, rules, group, now, false);
					}
					continue;
				}

				seen.Insert(key);
				if (rec && rec.m_iPlayer != playerId)
					TransferLeader(rec, playerId, group);	// the mission (or its offers) goes on with the new human leader

				if (!rec)
				{
					rec = new CLW_MissionRec();
					rec.m_sKey = key;
					rec.m_sFaction = factionKey;
					rec.m_sSquad = squadId;
					rec.m_iPlayer = playerId;
					rec.m_aExecutors.Insert(playerId);
					m_mRecs.Set(key, rec);
					Print(string.Format("CLW: mission squad %1 (%2) is human-led by player %3", squadId, factionKey, playerId), LogLevel.NORMAL);
				}

				Evaluate(rec, rules, group, now, true);
			}
		}

		// records whose squad is gone / not human-led any more / player left
		array<string> stale = {};
		foreach (string recKey, CLW_MissionRec existing : m_mRecs)
		{
			if (!seen.Contains(recKey))
				stale.Insert(recKey);
		}

		foreach (string staleKey : stale)
		{
			CLW_MissionRec gone;
			if (m_mRecs.Find(staleKey, gone) && gone)
				Cleanup(gone, "squad no longer human-led / player left");
		}
	}

	protected int LeaderPlayerId(notnull SCR_AIGroup group)
	{
		IEntity leader = CLW_SquadInfo.HumanLeaderEntity(group);
		if (!leader)
			return 0;

		return GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(leader);
	}

	// ---- per-squad state machine ----

	protected void Evaluate(CLW_MissionRec rec, CLW_RuleCommander rules, SCR_AIGroup group, float now, bool leaderAlive)
	{
		FlushDoomed(rec);
		SyncExecutors(rec, group);
		if (rec.m_Task)
		{
			EvaluateAccepted(rec, rules, group, now, leaderAlive);
			return;
		}

		// the accepted task vanished from under us (deleted elsewhere)
		if (rec.m_Offer)
		{
			rec.m_Offer = null;
			rec.m_fNextOfferAt = now + PAUSE_AFTER_END_SEC;
			Notify(rec, CLW_Loc.W("#CLW-Msn_LostTitle"), CLW_Loc.W("#CLW-Msn_LostText"), "", string.Format("%1 task disappeared, replanning", rec.m_sSquad));
		}

		PruneOffers(rec, now);
		if (!leaderAlive)
			return;

		CLW_PendingOffers pending;
		bool havePending = m_mPending.Find(rec.m_sKey, pending) && pending;
		if (havePending && now - pending.m_fAt > LLM_FRESH_SEC)
		{
			m_mPending.Remove(rec.m_sKey);
			havePending = false;
		}

		bool haveOffers = !rec.m_aOffers.IsEmpty();
		if (now < rec.m_fNextOfferAt)
			return;

		if (haveOffers)
		{
			// replace on schedule only, and only when something new is waiting or the old ones are stale
			if (now - rec.m_fOfferedAt < OFFER_REFRESH_SEC)
				return;
			ClearOffers(rec, "expired");
		}

		array<ref CLW_MissionOffer> fresh = {};
		if (havePending)
		{
			foreach (CLW_MissionOffer one : pending.m_aOffers)
				fresh.Insert(one);
			m_mPending.Remove(rec.m_sKey);
		}
		else
		{
			rules.BuildOffers(rec.m_sSquad, fresh);
		}

		Publish(rec, fresh, now);
	}

	// Drop offers whose premise is gone (a CAPTURE target already ours) or whose task was deleted.
	protected void PruneOffers(CLW_MissionRec rec, float now)
	{
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		for (int i = rec.m_aOffers.Count() - 1; i >= 0; i--)
		{
			CLW_MissionOffer offer = rec.m_aOffers[i];
			SCR_Task task = rec.m_aTasks[i];
			string why;
			if (!task)
				why = "task removed";
			else if (offer.m_sKind == "CAPTURE" && !offer.m_bHasPos && points)
			{
				CLW_ControlPoint point = points.Find(offer.m_sTarget);
				if (point && point.m_sOwner == rec.m_sFaction)
				{
					// our side took it while it was only offered: show it as completed (no reward) instead of deleting it silently
					CompleteTakenOffer(rec, i);
					continue;
				}
			}

			if (why.IsEmpty())
				continue;

			Print(string.Format("CLW: mission offer expired for %1: %2 %3 (%4)", rec.m_sSquad, offer.m_sKind, TargetText(offer), why), LogLevel.NORMAL);
			DeleteTaskSafe(task);
			rec.m_aOffers.Remove(i);
			rec.m_aTasks.Remove(i);
		}
	}

	// A CAPTURE offer the leader never accepted, whose objective our side took meanwhile: COMPLETED without reward, kept in the
	// finished tab, the leader gets a note. (Before M4.6 it was deleted silently and looked like a mission that never completed.)
	protected void CompleteTakenOffer(CLW_MissionRec rec, int index)
	{
		CLW_MissionOffer offer = rec.m_aOffers[index];
		SCR_Task task = rec.m_aTasks[index];
		rec.m_aOffers.Remove(index);
		rec.m_aTasks.Remove(index);

		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (taskSystem)
			taskSystem.SetTaskState(task, SCR_ETaskState.COMPLETED);

		rec.m_aFinished.Insert(task);
		while (rec.m_aFinished.Count() > MAX_FINISHED_KEPT)
		{
			DeleteTaskSafe(rec.m_aFinished[0]);
			rec.m_aFinished.RemoveOrdered(0);
		}

		Print(string.Format("CLW: mission offer %1 %2 captured while offered -> completed, no reward (%3)", offer.m_sKind, TargetText(offer), rec.m_sSquad), LogLevel.NORMAL);
		Notify(rec, CLW_Loc.W("#CLW-Msn_TakenTitle"), CLW_Loc.W("#CLW-Msn_TakenText", TaskName(offer)), SoundTaskSucceed(), string.Format("%1 offer %2 %3 completed without reward (objective taken while offered)", rec.m_sSquad, offer.m_sKind, TargetText(offer)));
	}

	protected void ClearOffers(CLW_MissionRec rec, string why)
	{
		foreach (int i, SCR_Task task : rec.m_aTasks)
		{
			CLW_MissionOffer offer = rec.m_aOffers[i];
			Print(string.Format("CLW: mission offer %1 for %2: %3 %4", why, rec.m_sSquad, offer.m_sKind, TargetText(offer)), LogLevel.NORMAL);
			DeleteTaskSafe(task);
		}

		rec.m_aOffers.Clear();
		rec.m_aTasks.Clear();
	}

	// Create the offer tasks (max 3, no duplicates, valid targets only) and tell the leader.
	protected void Publish(CLW_MissionRec rec, notnull array<ref CLW_MissionOffer> offers, float now)
	{
		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (!taskSystem)
		{
			Print("CLW: mission no SCR_TaskSystem, offers skipped", LogLevel.WARNING);
			rec.m_fNextOfferAt = now + EMPTY_RETRY_SEC;
			return;
		}

		array<string> used = {};
		foreach (CLW_MissionOffer offer : offers)
		{
			if (!offer || rec.m_aOffers.Count() >= MAX_OFFERS)
				continue;

			string signature = offer.m_sKind + "|" + TargetText(offer);
			vector pos;
			if (used.Contains(signature) || !OfferPos(offer, pos))
				continue;

			SCR_Task task = CreateOfferTask(taskSystem, rec, offer, pos);
			if (!task)
				continue;

			used.Insert(signature);
			rec.m_aOffers.Insert(offer);
			rec.m_aTasks.Insert(task);
			m_mTaskToRec.Set(task.GetTaskID(), rec.m_sKey);
			Print(string.Format("CLW: mission offered to player %1 (%2/%3): %4 %5 [%6] '%7'", rec.m_iPlayer, rec.m_sFaction, rec.m_sSquad, offer.m_sKind, TargetText(offer), offer.m_sSource, CLW_Loc.Resolve(offer.m_sBrief)), LogLevel.NORMAL);
		}

		int count = rec.m_aOffers.Count();
		if (count == 0)
		{
			rec.m_fNextOfferAt = now + EMPTY_RETRY_SEC;
			if (now - m_fLastNoOfferLog >= 120)
			{
				m_fLastNoOfferLog = now;
				Print(string.Format("CLW: mission nothing to offer %1 (%2)", rec.m_sSquad, rec.m_sFaction), LogLevel.NORMAL);
			}
			return;
		}

		rec.m_fOfferedAt = now;
		string kinds;	// English, for the log / event only
		array<string> pairs = {};	// kind key + target of each offer, for the popup (at most MAX_OFFERS = 3)
		foreach (CLW_MissionOffer published : rec.m_aOffers)
		{
			if (!kinds.IsEmpty())
				kinds += ", ";
			kinds += published.m_sKind + " " + TargetText(published);
			pairs.Insert(KindLabel(published.m_sKind));
			pairs.Insert(TargetText(published));
		}

		string newText;
		if (count == 1)
			newText = CLW_Loc.W("#CLW-Msn_NewText1", pairs[0], pairs[1]);
		else if (count == 2)
			newText = CLW_Loc.W("#CLW-Msn_NewText2", pairs[0], pairs[1], pairs[2], pairs[3]);
		else
			newText = CLW_Loc.W("#CLW-Msn_NewText3", pairs[0], pairs[1], pairs[2], pairs[3], pairs[4], pairs[5]);

		Notify(rec, CLW_Loc.W("#CLW-Msn_NewTitle"), newText, SoundTaskCreated(), string.Format("%1 offered %2: %3", rec.m_sSquad, count, kinds));
	}

	protected SCR_Task CreateOfferTask(SCR_TaskSystem taskSystem, CLW_MissionRec rec, CLW_MissionOffer offer, vector pos)
	{
		s_iCounter++;
		string taskId = "CLW_Msn_" + rec.m_sFaction + "_" + s_iCounter;
		SCR_Task task = taskSystem.CreateTask(TASK_PREFAB, taskId, "", "", pos);
		if (!task)
		{
			Print("CLW: mission CreateTask failed for " + taskId, LogLevel.WARNING);
			return null;
		}

		CLW_Loc.SetTaskName(task, TaskName(offer));	// key + params: every client formats the title in its own language
		CLW_Loc.SetTaskDescription(task, Brief(offer));

		// visible to the leading PLAYER and every human of his squad (one executor per player, never the group: a real GroupID would
		// silence the commander voice and move marker). Not assigned: only the leader may accept (HandleAssigneeAdded rejects the rest).
		taskSystem.SetTaskOwnership(task, SCR_ETaskOwnership.EXECUTOR);
		taskSystem.SetTaskVisibility(task, SCR_ETaskVisibility.EXECUTOR);
		SCR_TaskExecutor executor = SCR_TaskExecutor.FromPlayerID(rec.m_iPlayer);
		if (executor)
			taskSystem.AddTaskExecutor(task, executor);

		foreach (int memberId : rec.m_aExecutors)
		{
			if (memberId == rec.m_iPlayer)
				continue;

			SCR_TaskExecutor memberExecutor = SCR_TaskExecutor.FromPlayerID(memberId);
			if (memberExecutor)
				taskSystem.AddTaskExecutor(task, memberExecutor);
		}

		return task;
	}

	// ---- squad visibility (M4.6) ----

	// Every task of the record that is alive: offers, the accepted one, the finished ones kept for the finished tab.
	protected void CollectTasks(CLW_MissionRec rec, notnull array<SCR_Task> result)
	{
		foreach (SCR_Task offerTask : rec.m_aTasks)
		{
			if (offerTask)
				result.Insert(offerTask);
		}

		if (rec.m_Task && !result.Contains(rec.m_Task))
			result.Insert(rec.m_Task);

		foreach (SCR_Task finishedTask : rec.m_aFinished)
		{
			if (finishedTask && !result.Contains(finishedTask))
				result.Insert(finishedTask);
		}
	}

	// Keep the task executors equal to "the leader + every connected human in the squad": members see the offers and the accepted mission
	// (map marker, J menu, progress, completion) read-only. One executor PER PLAYER, never the group. Runs every tick and on squad events.
	protected void SyncExecutors(CLW_MissionRec rec, SCR_AIGroup group)
	{
		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		PlayerManager players = GetGame().GetPlayerManager();
		if (!taskSystem || !players || !group)
			return;

		array<int> wanted = {};
		wanted.Insert(rec.m_iPlayer);
		foreach (int memberId : group.GetPlayerIDs())
		{
			if (memberId > 0 && !wanted.Contains(memberId) && players.IsPlayerConnected(memberId))
				wanted.Insert(memberId);
		}

		array<SCR_Task> tasks = {};
		CollectTasks(rec, tasks);

		foreach (int addId : wanted)
		{
			if (rec.m_aExecutors.Contains(addId))
				continue;

			rec.m_aExecutors.Insert(addId);
			foreach (SCR_Task addTask : tasks)
			{
				taskSystem.AddTaskExecutor(addTask, SCR_TaskExecutor.FromPlayerID(addId));
			}

			Print(string.Format("CLW: mission %1 visible to player %2 (%3 tasks, %4 viewers)", rec.m_sSquad, addId, tasks.Count(), rec.m_aExecutors.Count()), LogLevel.NORMAL);
		}

		for (int i = rec.m_aExecutors.Count() - 1; i >= 0; i--)
		{
			int oldId = rec.m_aExecutors[i];
			if (wanted.Contains(oldId))
				continue;

			rec.m_aExecutors.Remove(i);
			foreach (SCR_Task dropTask : tasks)
			{
				taskSystem.RemoveTaskExecutor(dropTask, SCR_TaskExecutor.FromPlayerID(oldId));
			}

			Print(string.Format("CLW: mission %1 hidden from player %2 (left the squad / disconnected, %3 viewers left)", rec.m_sSquad, oldId, rec.m_aExecutors.Count()), LogLevel.NORMAL);
		}
	}

	// The squad has a new human leader (the old one died and another human took over, left the squad, or came back as leader):
	// the offers and the accepted mission go on with him; he can accept / abandon from now on.
	protected void TransferLeader(CLW_MissionRec rec, int newPlayerId, SCR_AIGroup group)
	{
		int oldPlayerId = rec.m_iPlayer;
		rec.m_bFinishing = true;	// the unassign below must not count as an abandon
		rec.m_iPlayer = newPlayerId;
		SyncExecutors(rec, group);	// the new leader must be an executor before he is assigned
		if (rec.m_Task)
		{
			SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
			if (taskSystem)
			{
				SCR_TaskExecutor oldExecutor = SCR_TaskExecutor.FromPlayerID(oldPlayerId);
				SCR_TaskExecutor newExecutor = SCR_TaskExecutor.FromPlayerID(newPlayerId);
				if (rec.m_Task.IsTaskAssignedTo(oldExecutor))
					taskSystem.UnassignTask(rec.m_Task, oldExecutor);
				taskSystem.AssignTask(rec.m_Task, newExecutor);
				if (!rec.m_Task.IsTaskAssignedTo(newExecutor))
					Print(string.Format("CLW: mission %1 could not assign the task to the new leader %2", rec.m_sSquad, newPlayerId), LogLevel.WARNING);
			}
		}

		rec.m_bFinishing = false;
		Print(string.Format("CLW: mission %1 leader %2 -> %3 (accepted mission %4, offers %5)", rec.m_sSquad, oldPlayerId, newPlayerId, rec.m_Task != null, rec.m_aOffers.Count()), LogLevel.NORMAL);
		if (rec.m_Task && rec.m_Offer)
			Notify(rec, CLW_Loc.W("#CLW-Msn_HandoverTitle"), CLW_Loc.W("#CLW-Msn_HandoverText", TaskName(rec.m_Offer)), "", string.Format("%1 leader is now player %2, mission %3 %4 goes on", rec.m_sSquad, newPlayerId, rec.m_Offer.m_sKind, TargetText(rec.m_Offer)));
	}

	// A squad member assigned himself to a mission task (vanilla allows it for every executor): take it back and tell him why.
	protected void RejectAssignee(SCR_Task task, int playerId)
	{
		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (!task || !taskSystem)
			return;

		CLW_MissionRec rec = RecOfTask(task);
		if (rec && rec.m_iPlayer == playerId)
			return;	// became the leader meanwhile

		SCR_TaskExecutor member = SCR_TaskExecutor.FromPlayerID(playerId);
		if (!task.IsTaskAssignedTo(member))
			return;

		taskSystem.UnassignTask(task, member);
		Print(string.Format("CLW: mission assign by player %1 refused (not the squad leader), unassigned again", playerId), LogLevel.NORMAL);
		SendPopup(playerId, CLW_Loc.W("#CLW-Msn_LeaderOnlyTitle"), CLW_Loc.W("#CLW-Msn_LeaderOnlyText"), "");
	}

	// ---- accept / abandon ----

	protected void HandleAssigneeAdded(SCR_Task task, SCR_TaskExecutor executor)
	{
		CLW_MissionRec rec = RecOfTask(task);
		if (!rec)
			return;

		SCR_TaskExecutorPlayer player = SCR_TaskExecutorPlayer.Cast(executor);
		if (!player)
			return;

		// Vanilla lets EVERY owner executor assign itself (SCR_TaskSystem.CanTaskBeAssignedTo, ownership EXECUTOR): a squad member
		// pressing "assign" in the J menu would become an assignee. Only the leader accepts / works the mission: unassign the member.
		if (player.GetPlayerID() != rec.m_iPlayer)
		{
			GetGame().GetCallqueue().CallLater(RejectAssignee, 100, false, task, player.GetPlayerID());	// not inside the invoker
			return;
		}

		if (rec.m_Task)
			return;

		int index = rec.m_aTasks.Find(task);
		if (index < 0)
			return;

		float now = CLW_Orders.Now();
		CLW_MissionOffer offer = rec.m_aOffers[index];
		rec.m_Task = task;
		rec.m_Offer = offer;
		rec.m_fAcceptedAt = now;
		rec.m_fHoldSec = 0;
		rec.m_fClearSince = -1;
		rec.m_fLastEval = now;
		rec.m_iLastProgress = -1;
		rec.m_bFinishing = false;
		rec.m_bHoldLost = false;

		// delete the sibling offers (next tick at the latest; not inside the invoker)
		for (int i = rec.m_aTasks.Count() - 1; i >= 0; i--)
		{
			if (i != index && rec.m_aTasks[i])
				rec.m_aDoomed.Insert(rec.m_aTasks[i]);
		}
		rec.m_aOffers.Clear();
		rec.m_aTasks.Clear();
		GetGame().GetCallqueue().CallLater(FlushDoomed, 300, false, rec);

		if (offer.m_sKind == "DEFEND")
		{
			SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
			if (taskSystem)
				taskSystem.ShowProgressForTask(task, true);
		}

		Notify(rec, CLW_Loc.W("#CLW-Msn_AcceptedTitle"), TaskName(offer), SoundTaskAccept(), string.Format("%1 accepted %2 %3", rec.m_sSquad, offer.m_sKind, TargetText(offer)));
	}

	protected void HandleAssigneeRemoved(SCR_Task task, SCR_TaskExecutor executor)
	{
		CLW_MissionRec rec = RecOfTask(task);
		if (!rec || rec.m_Task != task || rec.m_bFinishing)
			return;

		// only the leader leaving the task is an abandon (a member unassigned by RejectAssignee / SyncExecutors is not)
		SCR_TaskExecutorPlayer removedPlayer = SCR_TaskExecutorPlayer.Cast(executor);
		if (removedPlayer && removedPlayer.GetPlayerID() != rec.m_iPlayer)
			return;

		SCR_ETaskState state = task.GetTaskState();
		if (state == SCR_ETaskState.COMPLETED || state == SCR_ETaskState.FAILED || state == SCR_ETaskState.CANCELLED)
			return;

		float now = CLW_Orders.Now();
		CLW_MissionOffer offer = rec.m_Offer;
		rec.m_Task = null;
		rec.m_Offer = null;
		rec.m_fNextOfferAt = now + PAUSE_AFTER_END_SEC;
		rec.m_aDoomed.Insert(task);
		GetGame().GetCallqueue().CallLater(FlushDoomed, 300, false, rec);
		string what;
		if (offer)
			what = offer.m_sKind + " " + TargetText(offer);
		Notify(rec, CLW_Loc.W("#CLW-Msn_AbandonedTitle"), CLW_Loc.W("#CLW-Msn_AbandonedText"), SoundTaskCancelled(), string.Format("%1 abandoned %2, new offers in %3 s", rec.m_sSquad, what, PAUSE_AFTER_END_SEC));
	}

	protected void FlushDoomed(CLW_MissionRec rec)
	{
		if (!rec)
			return;

		foreach (SCR_Task task : rec.m_aDoomed)
		{
			DeleteTaskSafe(task);
		}
		rec.m_aDoomed.Clear();
	}

	// ---- accepted mission: completion rules ----

	protected void EvaluateAccepted(CLW_MissionRec rec, CLW_RuleCommander rules, SCR_AIGroup group, float now, bool leaderAlive)
	{
		CLW_MissionOffer offer = rec.m_Offer;
		float dt = 0;
		if (rec.m_fLastEval >= 0)
			dt = Math.Clamp(now - rec.m_fLastEval, 0, 30);
		rec.m_fLastEval = now;

		if (now - rec.m_fAcceptedAt > TIMEOUT_SEC)
		{
			Finish(rec, false, "timed out after " + Math.Round(TIMEOUT_SEC / 60) + " min", CLW_Loc.W("#CLW-Msn_Why_Timeout", CLW_Loc.N(Math.Round(TIMEOUT_SEC / 60))), now);
			return;
		}

		vector target;
		if (!OfferPos(offer, target))
		{
			Finish(rec, false, "target no longer exists", CLW_Loc.W("#CLW-Msn_Why_Gone"), now);
			return;
		}

		vector leaderPos;
		bool hasLeader = false;
		if (leaderAlive)
		{
			IEntity leader = CLW_SquadInfo.HumanLeaderEntity(group);
			if (leader)
			{
				leaderPos = leader.GetOrigin();
				hasLeader = true;
			}
		}

		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		CLW_ControlPoint point;
		if (!offer.m_bHasPos && points)
			point = points.Find(offer.m_sTarget);

		if (offer.m_sKind == "CAPTURE")
		{
			bool done;
			if (point)
				done = point.m_sOwner == rec.m_sFaction;
			else
				done = hasLeader && vector.DistanceXZ(leaderPos, target) <= RECON_RADIUS_M;
			if (done)
				Finish(rec, true, "objective taken", CLW_Loc.W("#CLW-Msn_Why_Objective"), now);
		}
		else if (offer.m_sKind == "RECON")
		{
			if (hasLeader && vector.DistanceXZ(leaderPos, target) <= RECON_RADIUS_M)
				Finish(rec, true, "target reached", CLW_Loc.W("#CLW-Msn_Why_Reached"), now);
		}
		else if (offer.m_sKind == "DEFEND")
		{
			bool holding = hasLeader;
			if (holding)
			{
				float near = POS_HOLD_RADIUS_M;
				if (point)
				{
					near = DEFEND_NEAR_M + point.m_fRadius;
					holding = point.m_sOwner == rec.m_sFaction;
				}
				holding = holding && vector.DistanceXZ(leaderPos, target) <= near;
			}

			if (holding)
			{
				rec.m_fHoldSec += dt;
				rec.m_bHoldLost = false;
			}
			else
			{
				if (rec.m_fHoldSec > 0 && !rec.m_bHoldLost)
					Print(string.Format("CLW: mission %1 hold interrupted at %2 s (%3)", rec.m_sSquad, Math.Round(rec.m_fHoldSec), TargetText(offer)), LogLevel.NORMAL);
				rec.m_bHoldLost = true;
				rec.m_fHoldSec = 0;
			}

			UpdateProgress(rec, Math.Min(99, rec.m_fHoldSec / DEFEND_HOLD_SEC * 100));
			if (rec.m_fHoldSec >= DEFEND_HOLD_SEC)
				Finish(rec, true, "held " + Math.Round(DEFEND_HOLD_SEC / 60) + " min", CLW_Loc.W("#CLW-Msn_Why_Held", CLW_Loc.N(Math.Round(DEFEND_HOLD_SEC / 60))), now);
		}
		else if (offer.m_sKind == "DESTROY")
		{
			bool clear;
			if (hasLeader && vector.DistanceXZ(leaderPos, target) <= DESTROY_RADIUS_M)
				clear = !ContactNear(rules, target);

			if (clear)
			{
				if (rec.m_fClearSince < 0)
					rec.m_fClearSince = now;
				if (now - rec.m_fClearSince >= DESTROY_CLEAR_SEC)
					Finish(rec, true, "no contact left", CLW_Loc.W("#CLW-Msn_Why_Clear"), now);
			}
			else
			{
				rec.m_fClearSince = -1;
			}
		}
	}

	protected bool ContactNear(CLW_RuleCommander rules, vector pos)
	{
		CLW_ContactBook book = rules.GetBook();
		if (!book)
			return false;

		array<CLW_Contact> contacts = {};
		book.GetContacts(contacts);
		foreach (CLW_Contact contact : contacts)
		{
			if (vector.DistanceXZ(contact.m_vPos, pos) <= CONTACT_NEAR_M)
				return true;
		}

		return false;
	}

	protected void UpdateProgress(CLW_MissionRec rec, float percent)
	{
		int whole = Math.Round(percent);
		if (whole == rec.m_iLastProgress)
			return;

		rec.m_iLastProgress = whole;
		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (taskSystem && rec.m_Task)
			taskSystem.SetTaskProgress(rec.m_Task, whole);
	}

	// Completed (reward) or failed. why = short English reason for the log / event, whyWire = the same for the popup (CLW_Loc wire).
	protected void Finish(CLW_MissionRec rec, bool success, string why, string whyWire, float now)
	{
		CLW_MissionOffer offer = rec.m_Offer;
		SCR_Task task = rec.m_Task;
		rec.m_bFinishing = true;
		rec.m_Task = null;
		rec.m_Offer = null;
		rec.m_fNextOfferAt = now + PAUSE_AFTER_END_SEC;
		rec.m_fHoldSec = 0;

		SCR_TaskSystem taskSystem = SCR_TaskSystem.GetInstance();
		if (task && taskSystem)
		{
			if (success)
				taskSystem.SetTaskState(task, SCR_ETaskState.COMPLETED);
			else
				taskSystem.SetTaskState(task, SCR_ETaskState.FAILED);

			rec.m_aFinished.Insert(task);
			while (rec.m_aFinished.Count() > MAX_FINISHED_KEPT)
			{
				DeleteTaskSafe(rec.m_aFinished[0]);
				rec.m_aFinished.RemoveOrdered(0);
			}
		}
		rec.m_bFinishing = false;

		string what = offer.m_sKind + " " + TargetText(offer);
		if (!success)
		{
			Notify(rec, CLW_Loc.W("#CLW-Msn_FailedTitle"), CLW_Loc.W("#CLW-Msn_FailedText", TaskName(offer), whyWire), SoundTaskFailed(), string.Format("%1 failed %2 (%3)", rec.m_sSquad, what, why));
			return;
		}

		int reward = REWARD_SUPPLIES;
		if (!offer.m_bHasPos)
		{
			CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
			CLW_ControlPoint point;
			if (points)
				point = points.Find(offer.m_sTarget);
			if (point && point.m_iValue >= MAJOR_VALUE)
				reward = REWARD_MAJOR_SUPPLIES;
		}

		CLW_WarState war = CLW_GameMode.GetWarState();
		if (war)
			war.AddSupplies(rec.m_sFaction, reward);

		Notify(rec, CLW_Loc.W("#CLW-Msn_CompleteTitle"), CLW_Loc.W("#CLW-Msn_CompleteText", TaskName(offer), CLW_Loc.N(reward)), SoundTaskSucceed(), string.Format("%1 completed %2 (%3), reward +%4 supplies", rec.m_sSquad, what, why, reward));
	}

	// ---- cleanup ----

	protected void Cleanup(CLW_MissionRec rec, string why)
	{
		Print(string.Format("CLW: mission cleanup %1/%2: %3", rec.m_sFaction, rec.m_sSquad, why), LogLevel.NORMAL);
		ClearOffers(rec, "removed (" + why + ")");
		if (rec.m_Task)
		{
			if (rec.m_Offer)
				Print(string.Format("CLW: mission %1 dropped %2 %3 (%4)", rec.m_sSquad, rec.m_Offer.m_sKind, TargetText(rec.m_Offer), why), LogLevel.NORMAL);
			DeleteTaskSafe(rec.m_Task);
		}

		foreach (SCR_Task finished : rec.m_aFinished)
		{
			DeleteTaskSafe(finished);
		}

		foreach (SCR_Task doomed : rec.m_aDoomed)
		{
			DeleteTaskSafe(doomed);
		}

		rec.m_Task = null;
		rec.m_Offer = null;
		rec.m_aFinished.Clear();
		rec.m_aDoomed.Clear();
		m_mRecs.Remove(rec.m_sKey);
		m_mPending.Remove(rec.m_sKey);
	}

	protected void CleanupAll(string why)
	{
		array<ref CLW_MissionRec> all = {};
		foreach (string key, CLW_MissionRec rec : m_mRecs)
		{
			all.Insert(rec);
		}

		foreach (CLW_MissionRec each : all)
		{
			Cleanup(each, why);
		}

		m_mRecs.Clear();
		m_mTaskToRec.Clear();
	}

	// ---- sounds (vanilla task events; empty string = no sound) ----

	protected string SoundTaskCreated()
	{
		return SCR_SoundEvent.TASK_CREATED;
	}

	protected string SoundTaskAccept()
	{
		return SCR_SoundEvent.TASK_ACCEPT;
	}

	protected string SoundTaskSucceed()
	{
		return SCR_SoundEvent.TASK_SUCCEED;
	}

	protected string SoundTaskFailed()
	{
		return SCR_SoundEvent.TASK_FAILED;
	}

	protected string SoundTaskCancelled()
	{
		return SCR_SoundEvent.TASK_CANCELED;
	}
}

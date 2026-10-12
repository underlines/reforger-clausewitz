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

	protected string KindLabel(string kind)
	{
		if (kind == "CAPTURE")
			return "Capture";
		if (kind == "DEFEND")
			return "Defend";
		if (kind == "RECON")
			return "Recon";
		if (kind == "DESTROY")
			return "Destroy";

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

	// Task title: "[HQ] Capture C - Timber Hub" (position offers: "[HQ] Recon 2600/2750").
	protected string TaskName(CLW_MissionOffer offer)
	{
		string text = "[HQ] " + KindLabel(offer.m_sKind) + " " + TargetText(offer);
		if (!offer.m_bHasPos)
		{
			string name = PointName(offer.m_sTarget);
			if (!name.IsEmpty())
				text += " - " + name;
		}

		return text;
	}

	protected string Brief(CLW_MissionOffer offer)
	{
		if (!offer.m_sBrief.IsEmpty())
			return offer.m_sBrief;

		return "HQ: " + KindLabel(offer.m_sKind) + " " + TargetText(offer) + ".";
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
				{
					Cleanup(rec, "leader changed to player " + playerId);
					rec = null;
				}

				if (!rec)
				{
					rec = new CLW_MissionRec();
					rec.m_sKey = key;
					rec.m_sFaction = factionKey;
					rec.m_sSquad = squadId;
					rec.m_iPlayer = playerId;
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
			Notify(rec, "Mission lost", "The task was removed.", "", string.Format("%1 task disappeared, replanning", rec.m_sSquad));
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
					why = "point already ours";
			}

			if (why.IsEmpty())
				continue;

			Print(string.Format("CLW: mission offer expired for %1: %2 %3 (%4)", rec.m_sSquad, offer.m_sKind, TargetText(offer), why), LogLevel.NORMAL);
			DeleteTaskSafe(task);
			rec.m_aOffers.Remove(i);
			rec.m_aTasks.Remove(i);
		}
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
			Print(string.Format("CLW: mission offered to player %1 (%2/%3): %4 %5 [%6] '%7'", rec.m_iPlayer, rec.m_sFaction, rec.m_sSquad, offer.m_sKind, TargetText(offer), offer.m_sSource, offer.m_sBrief), LogLevel.NORMAL);
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
		string kinds;
		foreach (CLW_MissionOffer published : rec.m_aOffers)
		{
			if (!kinds.IsEmpty())
				kinds += ", ";
			kinds += KindLabel(published.m_sKind) + " " + TargetText(published);
		}

		Notify(rec, "HQ: new missions", string.Format("%1 new mission(s), open the task list (J): %2", count, kinds), SoundTaskCreated(), string.Format("%1 offered %2: %3", rec.m_sSquad, count, kinds));
	}

	protected SCR_Task CreateOfferTask(SCR_TaskSystem taskSystem, CLW_MissionRec rec, CLW_MissionOffer offer, vector pos)
	{
		s_iCounter++;
		string taskId = "CLW_Msn_" + rec.m_sFaction + "_" + s_iCounter;
		SCR_Task task = taskSystem.CreateTask(TASK_PREFAB, taskId, TaskName(offer), Brief(offer), pos);
		if (!task)
		{
			Print("CLW: mission CreateTask failed for " + taskId, LogLevel.WARNING);
			return null;
		}

		// visible and assignable to the leading PLAYER only; not assigned (the player accepts it himself)
		taskSystem.SetTaskOwnership(task, SCR_ETaskOwnership.EXECUTOR);
		taskSystem.SetTaskVisibility(task, SCR_ETaskVisibility.EXECUTOR);
		SCR_TaskExecutor executor = SCR_TaskExecutor.FromPlayerID(rec.m_iPlayer);
		if (executor)
			taskSystem.AddTaskExecutor(task, executor);

		return task;
	}

	// ---- accept / abandon ----

	protected void HandleAssigneeAdded(SCR_Task task, SCR_TaskExecutor executor)
	{
		CLW_MissionRec rec = RecOfTask(task);
		if (!rec || rec.m_Task)
			return;

		SCR_TaskExecutorPlayer player = SCR_TaskExecutorPlayer.Cast(executor);
		if (!player || player.GetPlayerID() != rec.m_iPlayer)
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

		Notify(rec, "Mission accepted", TaskName(offer), SoundTaskAccept(), string.Format("%1 accepted %2 %3", rec.m_sSquad, offer.m_sKind, TargetText(offer)));
	}

	protected void HandleAssigneeRemoved(SCR_Task task, SCR_TaskExecutor executor)
	{
		CLW_MissionRec rec = RecOfTask(task);
		if (!rec || rec.m_Task != task || rec.m_bFinishing)
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
		Notify(rec, "Mission abandoned", "HQ will replan.", SoundTaskCancelled(), string.Format("%1 abandoned %2, new offers in %3 s", rec.m_sSquad, what, PAUSE_AFTER_END_SEC));
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
			Finish(rec, false, "timed out after " + Math.Round(TIMEOUT_SEC / 60) + " min", now);
			return;
		}

		vector target;
		if (!OfferPos(offer, target))
		{
			Finish(rec, false, "target no longer exists", now);
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
				Finish(rec, true, "objective taken", now);
		}
		else if (offer.m_sKind == "RECON")
		{
			if (hasLeader && vector.DistanceXZ(leaderPos, target) <= RECON_RADIUS_M)
				Finish(rec, true, "target reached", now);
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
				Finish(rec, true, "held " + Math.Round(DEFEND_HOLD_SEC / 60) + " min", now);
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
					Finish(rec, true, "no contact left", now);
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

	// Completed (reward) or failed. why = short reason for the log / popup.
	protected void Finish(CLW_MissionRec rec, bool success, string why, float now)
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
			Notify(rec, "Mission failed", TaskName(offer) + ": " + why, SoundTaskFailed(), string.Format("%1 failed %2 (%3)", rec.m_sSquad, what, why));
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

		Notify(rec, "Mission complete", string.Format("%1 (+%2 supplies)", TaskName(offer), reward), SoundTaskSucceed(), string.Format("%1 completed %2 (%3), reward +%4 supplies", rec.m_sSquad, what, why, reward));
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

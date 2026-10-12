// Unit orders (spec section 6): the one place where a commander's decision (rules or LLM) becomes waypoints / a vehicle unit
// order, and where the order lifecycle is tracked (active -> done | failed). Server only, one CLW_Orders per faction.
//
// A "unit" is a commandable registry id: an infantry group, or a vehicle unit addressed by its crew group's id.
// Infantry gets waypoints (CLW_Waypoints); vehicle units get CLW_VehicleUnit.Order (never waypoints on their groups).

// One order and its outcome.
class CLW_UnitOrder
{
	string m_sSquad;
	CLW_ECommand m_eCommand;
	string m_sTarget;			// objective id, "" = plain position
	vector m_vPos;				// resolved target position (surface Y)
	string m_sReason;
	string m_sSource;			// who ordered: CLW_Orders.SOURCE_*
	float m_fIssuedAt;			// world time (s)
	float m_fEndedAt = -1;		// world time (s) of done / failed
	string m_sStatus = "active";
	string m_sStatusWhy;
	bool m_bVehicle;			// executed by a CLW_VehicleUnit
	bool m_bWaypoints;			// false for missions of human-led squads (no waypoints)
	vector m_vProgressPos;		// stuck watch (infantry)
	float m_fProgressAt = -1;
	bool m_bUnstuck;			// the one unstuck attempt of this order was used
}

// Per-unit bookkeeping next to the order: start strength and a speed / heading sample for the situation.
class CLW_UnitTrack
{
	int m_iSerial;				// registry adoption serial: a re-adopted group starts fresh
	int m_iStartCount;			// highest living count seen
	vector m_vLastPos;
	float m_fLastTime = -1;
	float m_fSpeed;
	float m_fHeading;
	ref CLW_UnitOrder m_Order;
}

class CLW_Orders
{
	static const string STATUS_ACTIVE = "active";
	static const string STATUS_DONE = "done";
	static const string STATUS_FAILED = "failed";

	static const string SOURCE_RULES = "rules";
	static const string SOURCE_LLM = "llm";
	static const string SOURCE_MISSION = "mission";

	static const float ARRIVE_M = 40;				// arrived when the unit is this close to the target
	static const float STABILITY_SEC = 45;			// no re-order within this unless done / failed / weak
	static const float WAYPOINT_GRACE_SEC = 15;		// ignore "no waypoint" this long after issuing
	static const float STUCK_SEC = 120;				// infantry without progress this long fails its order
	static const float STUCK_MIN_PROGRESS_M = 15;
	static const float WEAK_STRENGTH = 0.5;			// living / start strength below this = weak
	static const float WEAK_VEHICLE_HEALTH = 0.4;	// vehicle health below this = weak
	static const float UPDATE_MIN_SEC = 4;
	static const float WAYPOINT_DONE_RADIUS_M = 100;	// waypoints finished within this of the target = done, else failed
	static const float VEHICLE_STOPPED_DONE_M = 150;	// vehicle unit idle within this of the target = arrived
	static const float SAMPLE_MIN_SEC = 2;

	protected string m_sFaction;
	protected CLW_SquadRegistry m_Registry;		// weak: owned by the game mode
	protected ref map<string, ref CLW_UnitTrack> m_mTracks = new map<string, ref CLW_UnitTrack>();
	protected float m_fLastUpdate = -1000;

	void Init(string factionKey, CLW_SquadRegistry registry)
	{
		m_sFaction = factionKey;
		m_Registry = registry;
	}

	string GetFaction()
	{
		return m_sFaction;
	}

	static float Now()
	{
		return GetGame().GetWorld().GetWorldTime() * 0.001;
	}

	// ---- tracks ----

	// Track of an adopted unit (created on first use). Null for an unknown id.
	CLW_UnitTrack Track(string id)
	{
		if (!m_Registry)
			return null;

		int serial = m_Registry.GetSerial(id);
		if (serial == 0)
			return null;

		CLW_UnitTrack track;
		if (m_mTracks.Find(id, track) && track)
		{
			if (track.m_iSerial == serial)
				return track;

			m_mTracks.Remove(id);
		}

		track = new CLW_UnitTrack();
		track.m_iSerial = serial;
		track.m_iStartCount = m_Registry.GetStartStrength(id);
		m_mTracks.Set(id, track);
		return track;
	}

	CLW_UnitOrder GetOrder(string id)
	{
		CLW_UnitTrack track;
		if (m_mTracks.Find(id, track) && track)
			return track.m_Order;

		return null;
	}

	// Forget the current order of a unit (a human gave it back to HQ): the commander may issue a new one at once.
	void ForgetOrder(string id)
	{
		CLW_UnitTrack track;
		if (m_mTracks.Find(id, track) && track)
			track.m_Order = null;
	}

	// Drop the tracks of units that are not listed any more. Ids removed are appended to outRemoved.
	void Prune(notnull array<string> liveIds, notnull array<string> outRemoved)
	{
		array<string> stale = {};
		foreach (string id, CLW_UnitTrack track : m_mTracks)
		{
			if (!liveIds.Contains(id) || (track && track.m_iSerial != m_Registry.GetSerial(id)))
				stale.Insert(id);
		}

		foreach (string gone : stale)
		{
			m_mTracks.Remove(gone);
			outRemoved.Insert(gone);
		}
	}

	// ---- unit facts ----

	CLW_VehicleUnit FindVehicleUnit(string id)
	{
		CLW_VehicleManager vehicles = CLW_GameMode.GetVehicleManager();
		if (!vehicles)
			return null;

		return vehicles.Find(id);
	}

	// Living people of the unit (vehicle unit: crew + embarked infantry).
	int UnitAlive(string id)
	{
		CLW_VehicleUnit unit = FindVehicleUnit(id);
		if (unit)
			return unit.GetCrewAlive() + unit.GetCargoAlive();

		if (!m_Registry)
			return 0;

		SCR_AIGroup group = m_Registry.Find(id);
		if (!group)
			return 0;

		return CLW_SquadInfo.AliveCount(group);
	}

	// Position of the unit (vehicle position, else the leader). False when the unit has none.
	bool UnitPos(string id, out vector pos)
	{
		CLW_VehicleUnit unit = FindVehicleUnit(id);
		if (unit)
		{
			pos = unit.GetPosition();
			return pos != vector.Zero;
		}

		if (!m_Registry)
			return false;

		SCR_AIGroup group = m_Registry.Find(id);
		if (!group)
			return false;

		IEntity leader = CLW_SquadInfo.LeaderEntity(group);
		if (!leader)
			return false;

		pos = leader.GetOrigin();
		return true;
	}

	// Raises the start strength when more people are alive than ever seen. Returns the start strength.
	int StartStrength(string id, int alive)
	{
		CLW_UnitTrack track = Track(id);
		if (!track)
			return alive;

		if (track.m_iStartCount < alive)
			track.m_iStartCount = alive;

		return track.m_iStartCount;
	}

	// Living / start strength, 1 when unknown.
	float Strength(string id)
	{
		int alive = UnitAlive(id);
		int start = StartStrength(id, alive);
		if (start <= 0)
			return 1;

		return alive * 1.0 / start;
	}

	bool IsWeak(string id)
	{
		if (Strength(id) < WEAK_STRENGTH)
			return true;

		CLW_VehicleUnit unit = FindVehicleUnit(id);
		if (unit && unit.GetVehicleHealth() < WEAK_VEHICLE_HEALTH)
			return true;

		return false;
	}

	// Speed / heading from the position delta since the previous sample (for the situation).
	void Sample(string id, vector pos, float yaw)
	{
		CLW_UnitTrack track = Track(id);
		if (!track)
			return;

		float now = Now();
		if (track.m_fLastTime >= 0 && now - track.m_fLastTime < SAMPLE_MIN_SEC)
			return;

		if (track.m_fLastTime < 0)
			track.m_fHeading = yaw;
		else
		{
			vector delta = pos - track.m_vLastPos;
			float planar = Math.Sqrt(delta[0] * delta[0] + delta[2] * delta[2]);
			track.m_fSpeed = planar / (now - track.m_fLastTime);
			if (planar > 2)
				track.m_fHeading = Math.Atan2(delta[0], delta[2]) * Math.RAD2DEG;
		}

		track.m_vLastPos = pos;
		track.m_fLastTime = now;
	}

	// ---- stability ----

	// True when the unit may get a new order now: no order, order finished, unit weak, or the order is old enough.
	bool CanReorder(string id)
	{
		CLW_UnitOrder order = GetOrder(id);
		if (!order || order.m_sStatus != STATUS_ACTIVE)
			return true;

		if (IsWeak(id))
			return true;

		return Now() - order.m_fIssuedAt >= STABILITY_SEC;
	}

	// ---- issuing ----

	// Resolve objective id / position / default (FALL_BACK without target = own base) into a position. why on failure.
	bool ResolveTarget(CLW_ECommand command, string targetId, vector pos, out vector resolved, out string why)
	{
		if (!targetId.IsEmpty())
		{
			CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
			CLW_ControlPoint point;
			if (points)
				point = points.Find(targetId);
			if (point)
			{
				resolved = point.m_vPos;
				return true;
			}

			if (pos == vector.Zero)
			{
				why = "unknown objective " + targetId;
				return false;
			}
		}

		if (pos == vector.Zero)
		{
			if (command == CLW_ECommand.FALL_BACK)
			{
				CLW_Bases bases = CLW_GameMode.GetBases();
				if (bases)
					resolved = bases.GetBasePos(m_sFaction);
				if (resolved != vector.Zero)
					return true;
			}

			why = "no target";
			return false;
		}

		resolved = pos;
		resolved[1] = GetGame().GetWorld().GetSurfaceY(resolved[0], resolved[2]);
		return true;
	}

	// Execute a command for one unit. targetId = objective id or "" (then pos). Returns false (why set) when the order was not
	// applied: unknown / hidden / human-led unit, stable order, bad target, waypoint failure. Re-issuing the same order is a no-op (true).
	bool Issue(string squadId, CLW_ECommand command, string targetId, vector pos, string reason, string source, out string why)
	{
		if (!m_Registry)
		{
			why = "no registry";
			return false;
		}

		SCR_AIGroup group = m_Registry.Find(squadId);
		CLW_UnitTrack track = Track(squadId);
		if (!group || !track)
		{
			why = "unknown squad";
			return false;
		}

		if (m_Registry.IsHidden(squadId))
		{
			why = "linked cargo group (ordered through its vehicle unit)";
			return false;
		}

		if (CLW_RuleCommander.HasHumanLeader(group))
		{
			why = "human-led squad (missions only)";
			return false;
		}

		if (UnitAlive(squadId) <= 0)
		{
			why = "squad has no living members";
			return false;
		}

		vector target;
		if (!ResolveTarget(command, targetId, pos, target, why))
			return false;

		CLW_UnitOrder old = track.m_Order;
		if (old && old.m_sStatus == STATUS_ACTIVE)
		{
			if (old.m_eCommand == command && old.m_sTarget == targetId && vector.DistanceXZ(old.m_vPos, target) < 50)
			{
				why = "";
				return true;	// already executing exactly this
			}

			float age = Now() - old.m_fIssuedAt;
			if (age < STABILITY_SEC && old.m_sSource == source && !IsWeak(squadId))
			{
				why = string.Format("order stable (%1 s old, %2)", Math.Round(age), CLW_Command.ToName(old.m_eCommand));
				return false;
			}
		}

		CLW_UnitOrder order = new CLW_UnitOrder();
		order.m_sSquad = squadId;
		order.m_eCommand = command;
		order.m_sTarget = targetId;
		order.m_vPos = target;
		order.m_sReason = reason;
		order.m_sSource = source;
		order.m_fIssuedAt = Now();
		order.m_bWaypoints = true;

		CLW_VehicleUnit unit = FindVehicleUnit(squadId);
		if (unit)
		{
			order.m_bVehicle = true;
			if (!unit.Order(command, target, reason))
			{
				why = "vehicle unit refused the order";
				return false;
			}
		}
		else if (!ExecuteInfantry(group, command, target))
		{
			why = "waypoint spawn failed";
			return false;
		}

		track.m_Order = order;
		string targetText = "pos " + Math.Round(target[0]) + "/" + Math.Round(target[2]);
		if (!targetId.IsEmpty())
			targetText = "objective " + targetId + " (" + Math.Round(target[0]) + "/" + Math.Round(target[2]) + ")";
		Print(string.Format("CLW: orders %1 %2 %3 -> %4 [%5, %6] %7", m_sFaction, squadId, CLW_Command.ToName(command), targetText, source, order.m_bVehicle, reason), LogLevel.NORMAL);
		return true;
	}

	// Record a mission-style order for a human-led squad: bookkeeping only (done / failed detection), no waypoints.
	void Record(string squadId, CLW_ECommand command, string targetId, vector pos, string reason)
	{
		CLW_UnitTrack track = Track(squadId);
		if (!track)
			return;

		vector target;
		string why;
		if (!ResolveTarget(command, targetId, pos, target, why))
			return;

		CLW_UnitOrder order = new CLW_UnitOrder();
		order.m_sSquad = squadId;
		order.m_eCommand = command;
		order.m_sTarget = targetId;
		order.m_vPos = target;
		order.m_sReason = reason;
		order.m_sSource = SOURCE_MISSION;
		order.m_fIssuedAt = Now();
		order.m_bWaypoints = false;
		track.m_Order = order;
	}

	// Waypoints for an infantry group. MOVE / ATTACK / DEFEND are one waypoint; RECON = move then hold there;
	// FALL_BACK = forced move (ignores combat) then hold there.
	protected bool ExecuteInfantry(SCR_AIGroup group, CLW_ECommand command, vector target)
	{
		CLW_Waypoints.Clear(group);
		float moveRadius = CLW_WarConfig.ARRIVE_RADIUS_M / 2;
		AIWaypoint first;
		switch (command)
		{
			case CLW_ECommand.ATTACK:
			{
				first = CLW_Waypoints.AddAttack(group, target);
				break;
			}
			case CLW_ECommand.DEFEND:
			{
				first = CLW_Waypoints.AddDefend(group, target);
				break;
			}
			case CLW_ECommand.RECON:
			{
				first = CLW_Waypoints.AddMove(group, target, moveRadius);
				if (first)
					CLW_Waypoints.AddDefend(group, target);
				break;
			}
			case CLW_ECommand.FALL_BACK:
			{
				first = CLW_Waypoints.AddForcedMove(group, target, moveRadius);
				if (first)
					CLW_Waypoints.AddDefend(group, target);
				break;
			}
			default:
			{
				first = CLW_Waypoints.AddMove(group, target, moveRadius);
				break;
			}
		}

		return first != null;
	}

	// ---- lifecycle ----

	// Evaluate every active order of this faction: done (arrived / objective captured) or failed (stuck, unit lost,
	// vehicle unit dissolved, waypoints lost). Self-throttled; call freely.
	void Update()
	{
		float now = Now();
		if (now - m_fLastUpdate < UPDATE_MIN_SEC)
			return;

		m_fLastUpdate = now;
		foreach (string id, CLW_UnitTrack track : m_mTracks)
		{
			if (track && track.m_Order && track.m_Order.m_sStatus == STATUS_ACTIVE)
				Evaluate(id, track, now);
		}
	}

	protected void Finish(CLW_UnitOrder order, string status, string why, float now)
	{
		order.m_sStatus = status;
		order.m_sStatusWhy = why;
		order.m_fEndedAt = now;
		Print(string.Format("CLW: orders %1 %2 %3 %4 -> %5: %6 (after %7 s)", m_sFaction, order.m_sSquad, CLW_Command.ToName(order.m_eCommand), order.m_sTarget, status, why, Math.Round(now - order.m_fIssuedAt)), LogLevel.NORMAL);
	}

	// False for groups that are linked cargo and still mounted in their vehicle unit (not stuck, just riding).
	protected bool CanUnstuck(string id)
	{
		CLW_VehicleManager mgr = CLW_GameMode.GetVehicleManager();
		if (!mgr || !mgr.IsLinkedCargo(id))
			return true;

		CLW_VehicleUnit carrier = mgr.FindByCargo(id);
		return carrier && carrier.GetStateName() == "DISMOUNTED";
	}

	protected void Evaluate(string id, CLW_UnitTrack track, float now)
	{
		CLW_UnitOrder order = track.m_Order;
		float age = now - order.m_fIssuedAt;
		int alive = UnitAlive(id);
		if (alive <= 0)
		{
			Finish(order, STATUS_FAILED, "unit lost", now);
			return;
		}

		CLW_VehicleUnit unit = FindVehicleUnit(id);
		if (order.m_bVehicle && !unit)
		{
			Finish(order, STATUS_FAILED, "vehicle unit dissolved", now);
			return;
		}

		vector pos;
		if (!UnitPos(id, pos))
			return;

		float dist = vector.DistanceXZ(pos, order.m_vPos);
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		CLW_ControlPoint point;
		if (points && !order.m_sTarget.IsEmpty())
			point = points.Find(order.m_sTarget);

		bool arrived = dist <= ARRIVE_M;
		switch (order.m_eCommand)
		{
			case CLW_ECommand.ATTACK:
			{
				if (point)
				{
					if (point.m_sOwner == m_sFaction && age > 5)
					{
						Finish(order, STATUS_DONE, "objective " + point.m_sId + " captured", now);
						return;
					}
				}
				else if (arrived || (unit && unit.GetStateName() == "DISMOUNTED" && age > 20))
				{
					Finish(order, STATUS_DONE, "arrived", now);
					return;
				}
				break;
			}
			case CLW_ECommand.DEFEND:
			{
				// a defence is a standing order: it only ends when the point is lost
				if (point && !point.m_sOwner.IsEmpty() && point.m_sOwner != m_sFaction && age > 20)
				{
					Finish(order, STATUS_FAILED, "objective " + point.m_sId + " lost to " + point.m_sOwner, now);
					return;
				}
				break;
			}
			default:
			{
				if (arrived)
				{
					Finish(order, STATUS_DONE, "arrived", now);
					return;
				}
				break;
			}
		}

		if (!order.m_bWaypoints)
			return;

		if (unit)
		{
			// a vehicle unit that stopped (arrived, or gave up when stuck: it holds in place)
			if (age > 20 && unit.GetStateName() == "MOUNTED_IDLE")
			{
				if (dist <= VEHICLE_STOPPED_DONE_M)
					Finish(order, STATUS_DONE, "vehicle arrived (" + Math.Round(dist) + " m)", now);
				else
					Finish(order, STATUS_FAILED, "vehicle stopped " + Math.Round(dist) + " m short (stuck or blocked)", now);
			}
			return;
		}

		SCR_AIGroup group = m_Registry.Find(id);
		if (!group)
			return;

		if (age > WAYPOINT_GRACE_SEC && !group.GetCurrentWaypoint())
		{
			if (dist <= WAYPOINT_DONE_RADIUS_M)
				Finish(order, STATUS_DONE, "waypoints completed " + Math.Round(dist) + " m from the target", now);
			else
				Finish(order, STATUS_FAILED, "waypoints lost, " + Math.Round(dist) + " m from the target", now);
			return;
		}

		// infantry stuck watch (a standing DEFEND is allowed to stay put)
		if (order.m_eCommand == CLW_ECommand.DEFEND)
			return;

		if (order.m_fProgressAt < 0)
		{
			order.m_vProgressPos = pos;
			order.m_fProgressAt = now;
			return;
		}

		if (now - order.m_fProgressAt >= STUCK_SEC)
		{
			float moved = vector.DistanceXZ(pos, order.m_vProgressPos);
			if (moved < STUCK_MIN_PROGRESS_M && !arrived)
			{
				// one unstuck attempt per order (members stuck on / in a vehicle), then a fresh waypoint
				if (!order.m_bUnstuck && CanUnstuck(id))
				{
					order.m_bUnstuck = true;
					int freed = CLW_Unstuck.FreeMembers(group, "order stuck: moved " + Math.Round(moved) + " m in " + Math.Round(now - order.m_fProgressAt) + " s", false);
					CLW_Waypoints.Clear(group);
					CLW_Waypoints.AddForCommand(group, order.m_eCommand, order.m_vPos);
					Print(string.Format("CLW: orders %1 %2 stuck: unstuck freed %3, waypoint re-issued", m_sFaction, order.m_sSquad, freed), LogLevel.NORMAL);
					order.m_vProgressPos = pos;
					order.m_fProgressAt = now;
					return;
				}

				Finish(order, STATUS_FAILED, "stuck: moved " + Math.Round(moved) + " m in " + Math.Round(now - order.m_fProgressAt) + " s", now);
				return;
			}

			order.m_vProgressPos = pos;
			order.m_fProgressAt = now;
		}
	}
}

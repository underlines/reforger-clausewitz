// Code tunables of the M2 war systems (timers, thresholds). Content (factions, positions, economy numbers) is NOT here:
// it lives in the scenario config (Configs/CLW/Scenarios/<World>.conf, see CLW_ScenarioConfig).
class CLW_WarConfig
{
	// Used when the CLW_GameMode prefab has no scenario config set (or it fails to load). Logged when used.
	static const ResourceName DEFAULT_SCENARIO = "Configs/CLW/Scenarios/Arland.conf";

	// Control points (spec section 1)
	static const int CAPTURE_TICK_MS = 2000;
	static const float CAPTURE_RATE_PCT_PER_SEC = 4;	// per 2 counted men: rate = 4 * min(count, CAPTURE_MAX_COUNTED) / 2
	static const int CAPTURE_MAX_COUNTED = 4;
	static const float CAPTURE_DECAY_PCT_PER_SEC = 1;	// back toward the current owner when nobody is present

	// War state (spec section 2)
	static const int WAR_TICK_MS = 5000;
	static const int SUMMARY_INTERVAL_SEC = 60;
	static const int EVENT_QUEUE_MAX = 10;

	// Vehicle ticket cost class: "heavy" = at least one TURRET compartment and at least this many compartments.
	static const int HEAVY_VEHICLE_MIN_COMPARTMENTS = 4;

	// Bases
	static const float WATER_MARGIN_M = 0.5;	// terrain this close to (or below) the ocean height counts as water
	static const int SCENE_SPAWN_RECHECK_MS = 4000;	// second pass disabling scene-placed spawn points

	// Engine-level waypoint prefab defaults (not faction content): one place for later phases.
	static const ResourceName WAYPOINT_MOVE = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";
	static const ResourceName WAYPOINT_FORCED_MOVE = "{06E1B6EBD480C6E0}Prefabs/AI/Waypoints/AIWaypoint_ForcedMove.et";
	static const ResourceName WAYPOINT_ATTACK = "{B3E7B8DC2BAB8ACC}Prefabs/AI/Waypoints/AIWaypoint_SearchAndDestroy.et";
	static const ResourceName WAYPOINT_DEFEND = "{93291E72AC23930F}Prefabs/AI/Waypoints/AIWaypoint_Defend.et";
	static const ResourceName WAYPOINT_GET_IN = "{712F4795CF8B91C7}Prefabs/AI/Waypoints/AIWaypoint_GetIn.et";
	static const ResourceName WAYPOINT_GET_OUT = "{C40316EE26846CAB}Prefabs/AI/Waypoints/AIWaypoint_GetOut.et";

	// ---- Force spawning (P2) ----
	static const int SPAWNER_TICK_MS = 1000;
	static const float SPAWN_RADIUS_M = 60;			// free spot search radius around the base
	static const float SPAWN_VEHICLE_CYLINDER_R = 3;	// free-spot cylinder for vehicles
	static const float SPAWN_VEHICLE_CYLINDER_H = 3;
	static const float SPAWN_CARGO_OFFSET_M = 8;		// embarked infantry group is spawned this far beside the vehicle
	static const int SPAWN_STAGGER_SEC = 3;				// start forces are spawned this many seconds apart
	static const float SPAWN_CREW_TIMEOUT_SEC = 30;
	static const float SPAWN_MEMBERS_TIMEOUT_SEC = 20;	// wait for the members of a spawned group
	static const float SPAWN_JOB_TIMEOUT_SEC = 120;		// whole purchase

	// ---- Placement (CLW_Placement / CLW_BasePlacement) ----
	static const float PLACE_SLOT_CLEAR_M = 6;			// a slot is occupied when a vehicle or character is within this distance
	static const float PLACE_ROAD_SPACING_M = 15;		// automatic road spots are at least this far apart
	static const float PLACE_ROAD_SEARCH_M = 200;		// roads further than this from the base are not used
	static const float PLACE_ROAD_STEP_M = 5;			// road polyline resampling step
	static const float PLACE_ROAD_MIN_WIDTH_M = 3;		// narrower roads are trails, not for vehicles
	static const float PLACE_ROAD_OFF_PENALTY_M = 60;	// roads other than the nearest one count this much further away
	static const int PLACE_ROAD_MAX_SPOTS = 16;
	static const float PLACE_MAX_SLOPE_TAN = 0.466;		// tan(25 deg)
	static const float PLACE_NAVMESH_TOL_M = 2.5;		// a spot counts as on the navmesh when it snaps within this distance
	static const float PLACE_BASE_NAVMESH_TOL_M = 12;	// base validation tolerance
	static const float PLACE_BASE_ROAD_WARN_M = 120;	// base further than this from any road is reported as unusable for vehicles
	static const int PLACE_GROUND_ATTEMPTS = 40;
	static const float PLACE_CHAR_CLEAR_M = 1.5;		// infantry spot: no other character closer than this
	static const float PLACE_SLOT_CHAR_CLEAR_M = 2.5;	// a character this close to a vehicle slot blocks it
	static const float PLACE_CARGO_MIN_M = 5;			// cargo group ring around its vehicle
	static const float PLACE_CARGO_MAX_M = 14;

	// ---- Catalog auto-discovery (P2) ----
	static const float DISCOVER_COST_SCALE = 0.15;		// catalog supply cost -> our economy
	static const int DISCOVER_MIN_COST_VEHICLE = 60;
	static const int DISCOVER_MIN_COST_GROUP = 30;
	static const int DISCOVER_COST_PER_MEMBER = 12;		// group cost when the catalog gives none (or a placeholder below the group floor)
	static const int DISCOVER_COST_VEHICLE_FALLBACK = 100;
	// Normalized costs of discovered entries (pack supply costs are inconsistent): soldiers x per-soldier, vehicle base by kind.
	static const int DISCOVER_SOLDIER_COST = 11;
	static const int DISCOVER_ARMED_BONUS = 15;			// armed vehicle of a kind whose base assumes no weapon (motorized)
	static const int DISCOVER_WEIGHT = 5;				// buy-mix weight of discovered entries
	static const int DISCOVER_MECH_MIN_CARGO = 5;
	static const int DISCOVER_LIGHT_MAX_CARGO = 2;		// armed + armoured/APC label with at most this many cargo seats = armored_recon
	static const int DISCOVER_MOTORIZED_MIN_CARGO = 4;
	static const int DISCOVER_LARGE_CARGO_GUESS = 8;	// cargo seats assumed when the probe saw few but the label says "large passenger capacity"
	static const int DISCOVER_MIN_GROUP_MEMBERS = 2;
	static const int DISCOVER_AT_TEAM_MAX_MEMBERS = 4;	// fallback anti-armour detection (armour-piercing group trait) only for groups this small
	static const float DISCOVER_NONCOMBAT_SHARE = 0.5;	// groups with this share of medic/sapper/radio members are not buyable infantry
	static const float DISCOVER_RECON_SHARE = 0.5;		// share of scout/sharpshooter members that makes a recon group

	// Cost of a discovered infantry group.
	static int DiscoveredGroupCost(int members)
	{
		return Math.Max(DISCOVER_MIN_COST_GROUP, members * DISCOVER_SOLDIER_COST);
	}

	// Cost of a discovered vehicle by kind (crew and embarked infantry are added by the caller).
	static int DiscoveredVehicleCost(string kind, bool armed)
	{
		int baseCost = 100;
		if (kind == CLW_ForceKind.MOTORIZED)
			baseCost = 50;
		else if (kind == CLW_ForceKind.LIGHT)
			baseCost = 60;
		else if (kind == CLW_ForceKind.MECH)
			baseCost = 180;
		else if (kind == CLW_ForceKind.ARMOR)
			baseCost = 280;
		else if (kind == CLW_ForceKind.ARMORED_RECON)
			baseCost = 120;

		if (armed && kind == CLW_ForceKind.MOTORIZED)
			baseCost += DISCOVER_ARMED_BONUS;

		return baseCost;
	}

	// ---- Vehicle units (P2, spec section 5) ----
	static const int VEHICLE_TICK_MS = 2000;
	static const float DISMOUNT_DIST_M = 250;			// ATTACK: dismount this far short of the target
	static const float DISMOUNT_DIST_TRUCK_M = 400;		// ... for unarmed transports (kind motorized)
	static const float OVERWATCH_DIST_M = 150;			// armed vehicle overwatch point, short of the target on the approach line
	static const float DEFEND_BEHIND_M = 50;			// DEFEND: vehicle holds this far short of the target
	static const float ARRIVE_RADIUS_M = 35;
	static const float ORDER_GRACE_SEC = 6;				// ignore "waypoint finished" this long after issuing one
	static const float DISMOUNT_FORCE_SEC = 20;			// teleport-dismount stragglers after this
	static const float REMOUNT_MIN_DIST_M = 600;		// an order farther than this while dismounted = remount first
	static const float REMOUNT_TIMEOUT_SEC = 90;
	static const float STUCK_SAMPLE_SEC = 10;
	static const int STUCK_SAMPLES = 5;					// 4 intervals = 40 s window
	static const float STUCK_MIN_PROGRESS_M = 15;
	static const float STUCK_GIVE_UP_SEC = 90;			// total time without progress before the infantry dismounts and walks
	static const float STUCK_REISSUE_SEC = 40;
	static const float VEHICLE_IMMOBILE_HEALTH = 0.1;	// vehicle health at or below = unusable
	static const int TRUCK_REFUND = 30;					// supplies refunded when an empty truck returns to base
	static const float RTB_DELETE_RADIUS_M = 45;
	static const float RTB_TIMEOUT_SEC = 600;
}

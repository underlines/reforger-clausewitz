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
}

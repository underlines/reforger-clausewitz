// Force profile data (spec 0a): what a faction can buy. Vanilla ships Configs/CLW/Factions/US|USSR|FIA.conf;
// compat addons ship their own. Factions without a profile fall back to catalog auto-discovery (CLW_ForceCatalog).

// The closed generic set of unit kinds the commanders reason about.
class CLW_ForceKind
{
	static const string INFANTRY = "infantry";
	static const string RECON = "recon";
	static const string MOTORIZED = "motorized";		// unarmed transport + infantry
	static const string LIGHT = "light";				// armed light vehicle
	static const string MECH = "mech";					// armed vehicle with cargo + infantry
	static const string ARMOR = "armor";				// armed vehicle without cargo (tanks from mod packs)
	static const string ARMORED_RECON = "armored_recon";
	static const string LOGISTICS = "logistics";			// unarmed supply truck (M3 phase L): never fights, convoys only (CLW_Convoys); cargo = CLW_ForceEntry.m_iCargo

	static bool IsKnown(string kind)
	{
		return kind == INFANTRY || kind == RECON || kind == MOTORIZED || kind == LIGHT || kind == MECH || kind == ARMOR || kind == ARMORED_RECON || kind == LOGISTICS;
	}

	// True for kinds that are normally built from a vehicle (profile entries may still omit it).
	static bool IsVehicleKind(string kind)
	{
		return kind == MOTORIZED || kind == LIGHT || kind == MECH || kind == ARMOR || kind == ARMORED_RECON || kind == LOGISTICS;
	}

	// Short tag used in generated entity names (US_Mech1).
	static string Tag(string kind)
	{
		if (kind == RECON)
			return "Rcn";
		if (kind == MOTORIZED)
			return "Mot";
		if (kind == LIGHT)
			return "Car";
		if (kind == MECH)
			return "Mech";
		if (kind == ARMOR)
			return "Arm";
		if (kind == ARMORED_RECON)
			return "ARcn";
		if (kind == LOGISTICS)
			return "Log";

		return "Inf";
	}
}

[BaseContainerProps()]
class CLW_ForceEntry
{
	// No field initializers on [Attribute] members: they override the values loaded from the .conf (seen 2026-10-12).
	// Config instances get the Attribute default; code that uses `new` sets every field itself.
	[Attribute("", UIWidgets.EditBox, "Type id, free-form and unique per faction (e.g. rifle_squad, mechanized)")]
	string m_sType;

	[Attribute("", UIWidgets.EditBox, "Display name (e.g. 'Mechanized (LAV-25 + fire team)')")]
	string m_sName;

	[Attribute("", UIWidgets.EditBox, "Vehicle name only, for vehicle units (e.g. 'LAV-25'); empty = display name")]
	string m_sVehicleName;

	[Attribute("infantry", UIWidgets.EditBox, "Kind: infantry, recon, motorized, light, mech, armor, armored_recon")]
	string m_sKind;

	[Attribute("100", UIWidgets.EditBox, "Cost in supplies")]
	int m_iCost;

	[Attribute("", UIWidgets.ResourceNamePicker, "Infantry group prefab (infantry kinds)", "et")]
	ResourceName m_GroupPrefab;

	[Attribute("", UIWidgets.ResourceNamePicker, "Vehicle prefab (vehicle kinds)", "et")]
	ResourceName m_VehiclePrefab;

	[Attribute("", UIWidgets.ResourceNamePicker, "Infantry group prefab embarked in the vehicle (motorized, mech)", "et")]
	ResourceName m_CargoGroupPrefab;

	[Attribute("10", UIWidgets.EditBox, "Weight in the commander's buy mix")]
	int m_iWeight;

	[Attribute("1", UIWidgets.CheckBox, "Entry can be bought")]
	bool m_bEnabled;

	[Attribute("0", UIWidgets.EditBox, "Supply cargo in supplies a convoy truck carries (kind logistics). 0 = not a supply truck")]
	int m_iCargo;

	[Attribute("0", UIWidgets.CheckBox, "Anti-armour team: the rule commander buys it first when enemy vehicles are known")]
	bool m_bAntiArmor;

	// ---- runtime facts filled by CLW_ForceCatalog (not data) ----
	string m_sSource;			// "profile" or "discovered"
	int m_iCargoSeats = -1;		// real cargo seats of the vehicle (probe), -1 unknown
	int m_iCrewSeats = -1;		// pilot + turret seats with a default occupant, -1 unknown
	bool m_bArmed;
	int m_iGroupMembers;		// infantry group size (own group)
	int m_iCargoMembers;		// embarked group size
	bool m_bChecked;			// prefabs probed
	int m_iVehicleCost;			// vehicle share of m_iCost (discovered entries), 0 = derive from cost minus embarked soldiers

	bool HasVehicle()
	{
		return !m_VehiclePrefab.IsEmpty();
	}

	string GetVehicleName()
	{
		if (!m_sVehicleName.IsEmpty())
			return m_sVehicleName;

		return m_sName;
	}

	// Expected AI count of one purchase (own group, embarked group, crew).
	int ExpectedAI()
	{
		int total = m_iGroupMembers + m_iCargoMembers;
		if (HasVehicle())
		{
			if (m_iCrewSeats > 0)
				total += m_iCrewSeats;
			else
				total += 2;
		}

		return Math.Max(1, total);
	}

	// One-line description for the situation: "cost 280 | LAV-25, 2 crew + 7 seats armed, 4 embarked".
	string Describe()
	{
		string text = m_sKind + ", cost " + m_iCost;
		if (HasVehicle())
		{
			text += ", " + GetVehicleName();
			if (m_iCrewSeats >= 0)
				text += ", " + m_iCrewSeats + " crew";
			if (m_iCargoSeats >= 0)
				text += ", " + m_iCargoSeats + " seats";
			if (m_bArmed)
				text += ", armed";
		}
		if (m_iGroupMembers > 0)
			text += ", " + m_iGroupMembers + " infantry";
		if (m_iCargoMembers > 0)
			text += ", " + m_iCargoMembers + " embarked";

		return text;
	}

	CLW_ForceEntry CloneEntry()
	{
		CLW_ForceEntry copy = new CLW_ForceEntry();
		copy.m_sType = m_sType;
		copy.m_sName = m_sName;
		copy.m_sVehicleName = m_sVehicleName;
		copy.m_sKind = m_sKind;
		copy.m_iCost = m_iCost;
		copy.m_GroupPrefab = m_GroupPrefab;
		copy.m_VehiclePrefab = m_VehiclePrefab;
		copy.m_CargoGroupPrefab = m_CargoGroupPrefab;
		copy.m_iWeight = m_iWeight;
		copy.m_bEnabled = m_bEnabled;
		copy.m_bAntiArmor = m_bAntiArmor;
		copy.m_iCargo = m_iCargo;
		copy.m_sSource = m_sSource;
		copy.m_iCargoSeats = m_iCargoSeats;
		copy.m_iCrewSeats = m_iCrewSeats;
		copy.m_bArmed = m_bArmed;
		copy.m_iGroupMembers = m_iGroupMembers;
		copy.m_iCargoMembers = m_iCargoMembers;
		copy.m_bChecked = m_bChecked;
		copy.m_iVehicleCost = m_iVehicleCost;
		return copy;
	}
}

// The closed set of base service kinds (M3 design section 3). Data (CLW_ServiceEntry) maps a kind to a composition prefab.
class CLW_ServiceKind
{
	static const string COMMAND_POST = "command_post";	// HQ composition: base logic, spawn, radio
	static const string VEHICLE_DEPOT = "vehicle_depot";
	static const string ARSENAL = "arsenal";
	static const string MEDICAL = "medical";
	static const string REPAIR = "repair";
	static const string SUPPLY = "supply";				// supply cache (stock the arsenal draws from)
	static const string FORTIFICATION = "fortification";	// FOB kit only: sandbags etc., no stock, no marker, no request terminal

	static bool IsKnown(string kind)
	{
		return kind == COMMAND_POST || kind == VEHICLE_DEPOT || kind == ARSENAL || kind == MEDICAL || kind == REPAIR || kind == SUPPLY || kind == FORTIFICATION;
	}

	// The important structures of a base that get a map marker by default (command post, arsenal, vehicle depot / requests, supply cache).
	// Field hospital and repair bay stay off the map unless the service entry says m_iMapMarker = 1.
	static bool DefaultMapMarker(string kind)
	{
		return kind == COMMAND_POST || kind == ARSENAL || kind == VEHICLE_DEPOT || kind == SUPPLY;
	}

	// Default footprint radius (m) of the composition, used by the free-spot search when the data gives none.
	static float DefaultRadius(string kind)
	{
		if (kind == COMMAND_POST)
			return 10;
		if (kind == MEDICAL)
			return 13;
		if (kind == VEHICLE_DEPOT || kind == REPAIR)
			return 9;
		if (kind == SUPPLY)
			return 9;

		if (kind == FORTIFICATION)
			return 1.5;

		return 3;	// arsenal box
	}
}

// One base service of a faction: kind -> composition prefab (+ optional placement).
[BaseContainerProps()]
class CLW_ServiceEntry
{
	[Attribute("", UIWidgets.EditBox, "Kind: command_post, vehicle_depot, arsenal, medical, repair, supply, fortification (FOB kit)")]
	string m_sKind;

	[Attribute("", UIWidgets.ResourceNamePicker, "Composition prefab spawned for this service", "et")]
	ResourceName m_Prefab;

	[Attribute("0 0 0", UIWidgets.EditBox, "Optional offset from the base anchor (x y z, y ignored, surface snapped). 0 0 0 = automatic free spot around the base")]
	vector m_vOffset;

	[Attribute("-1", UIWidgets.EditBox, "Heading in degrees (Enforce yaw). Negative = automatic: facing the base centre")]
	float m_fYaw;

	[Attribute("0", UIWidgets.EditBox, "Footprint radius (m) for the free-spot search. 0 = default of the kind")]
	float m_fRadius;

	[Attribute("0 0 0", UIWidgets.EditBox, "Footprint for the collision-free placement: half width X, height Y, half depth Z (m). 0 0 0 = derived from the prefab's child objects (no spawn needed), else a square from the radius")]
	vector m_vFootprint;

	[Attribute("1", UIWidgets.CheckBox, "Service is spawned")]
	bool m_bEnabled;

	[Attribute("-1", UIWidgets.EditBox, "Own-side map marker: -1 = by kind (command_post, arsenal, vehicle_depot, supply yes; medical, repair no), 0 = none, 1 = marker")]
	int m_iMapMarker;

	[Attribute("", UIWidgets.EditBox, "Role id (CLW_PropResolver): fob.arsenal_box, fob.supply_box, fort.sandbag_long, ... Empty = 'service.' + kind. Data for the kit entries; the prefab is the current candidate for the role")]
	string m_sRole;

	string GetRole()
	{
		if (!m_sRole.IsEmpty())
			return m_sRole;

		return "service." + m_sKind;
	}

	bool ShowsMapMarker()
	{
		if (m_iMapMarker >= 0)
			return m_iMapMarker > 0;

		return CLW_ServiceKind.DefaultMapMarker(m_sKind);
	}
}

[BaseContainerProps(configRoot: true)]
class CLW_FactionProfile
{
	[Attribute("", UIWidgets.EditBox, "Faction key this profile is for (e.g. US). Profiles are looked up by this key")]
	string m_sFactionKey;

	// Base services (command post, depot, arsenal, ...) as composition prefabs. Empty = the services of the kin faction below,
	// else of a friendly faction that has some.
	[Attribute()]
	ref array<ref CLW_ServiceEntry> m_aServices;

	// FOB kit (compact cluster at a new FOB: arsenal box, supply box, sandbags). Offsets are relative to the cluster centre (= the FOB
	// spawn point), x = right, z = forward, y ignored; yaw relative to the cluster, negative = facing the centre. Empty = kin/fallback
	// faction's kit; no kit anywhere = the tier-1 services (m_aServices) as before.
	[Attribute()]
	ref array<ref CLW_ServiceEntry> m_aFobKit;

	[Attribute("", UIWidgets.EditBox, "Faction key whose services this faction uses when it has none of its own (e.g. a pack faction naming the vanilla faction it is built on). Empty = automatic friendly faction")]
	string m_sServiceFallbackFaction;

	[Attribute("1", UIWidgets.CheckBox, "Also add entries auto-discovered from the faction's entity catalogs that the list below does not cover (mod packs that extend the vanilla catalogs show up as kind armor, mech, ...)")]
	bool m_bIncludeDiscovered;

	[Attribute()]
	ref array<ref CLW_ForceEntry> m_aEntries;

	// Load a profile .conf. Null (and a log) when the resource is missing or wrong.
	static CLW_FactionProfile Load(ResourceName path)
	{
		if (path.IsEmpty())
			return null;

		CLW_FactionProfile profile = SCR_ConfigHelperT<CLW_FactionProfile>.GetConfigObject(path);
		if (!profile)
		{
			Print("CLW: faction profile failed to load: " + path, LogLevel.ERROR);
			return null;
		}

		if (!profile.m_aEntries)
			profile.m_aEntries = {};
		if (!profile.m_aServices)
			profile.m_aServices = {};
		if (!profile.m_aFobKit)
			profile.m_aFobKit = {};

		return profile;
	}
}

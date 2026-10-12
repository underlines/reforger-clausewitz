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

	static bool IsKnown(string kind)
	{
		return kind == INFANTRY || kind == RECON || kind == MOTORIZED || kind == LIGHT || kind == MECH || kind == ARMOR || kind == ARMORED_RECON;
	}

	// True for kinds that are normally built from a vehicle (profile entries may still omit it).
	static bool IsVehicleKind(string kind)
	{
		return kind == MOTORIZED || kind == LIGHT || kind == MECH || kind == ARMOR || kind == ARMORED_RECON;
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

		return "Inf";
	}
}

[BaseContainerProps()]
class CLW_ForceEntry
{
	[Attribute("", UIWidgets.EditBox, "Type id, free-form and unique per faction (e.g. rifle_squad, mechanized)")]
	string m_sType;

	[Attribute("", UIWidgets.EditBox, "Display name (e.g. 'Mechanized (LAV-25 + fire team)')")]
	string m_sName;

	[Attribute("", UIWidgets.EditBox, "Vehicle name only, for vehicle units (e.g. 'LAV-25'); empty = display name")]
	string m_sVehicleName;

	[Attribute("infantry", UIWidgets.EditBox, "Kind: infantry, recon, motorized, light, mech, armor, armored_recon")]
	string m_sKind = "infantry";

	[Attribute("100", UIWidgets.EditBox, "Cost in supplies")]
	int m_iCost = 100;

	[Attribute("", UIWidgets.ResourceNamePicker, "Infantry group prefab (infantry kinds)", "et")]
	ResourceName m_GroupPrefab;

	[Attribute("", UIWidgets.ResourceNamePicker, "Vehicle prefab (vehicle kinds)", "et")]
	ResourceName m_VehiclePrefab;

	[Attribute("", UIWidgets.ResourceNamePicker, "Infantry group prefab embarked in the vehicle (motorized, mech)", "et")]
	ResourceName m_CargoGroupPrefab;

	[Attribute("10", UIWidgets.EditBox, "Weight in the commander's buy mix")]
	int m_iWeight = 10;

	[Attribute("1", UIWidgets.CheckBox, "Entry can be bought")]
	bool m_bEnabled = true;

	// ---- runtime facts filled by CLW_ForceCatalog (not data) ----
	string m_sSource;			// "profile" or "discovered"
	int m_iCargoSeats = -1;		// real cargo seats of the vehicle (probe), -1 unknown
	int m_iCrewSeats = -1;		// pilot + turret seats with a default occupant, -1 unknown
	bool m_bArmed;
	int m_iGroupMembers;		// infantry group size (own group)
	int m_iCargoMembers;		// embarked group size
	bool m_bChecked;			// prefabs probed

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
		copy.m_sSource = m_sSource;
		copy.m_iCargoSeats = m_iCargoSeats;
		copy.m_iCrewSeats = m_iCrewSeats;
		copy.m_bArmed = m_bArmed;
		copy.m_iGroupMembers = m_iGroupMembers;
		copy.m_iCargoMembers = m_iCargoMembers;
		copy.m_bChecked = m_bChecked;
		return copy;
	}
}

[BaseContainerProps(configRoot: true)]
class CLW_FactionProfile
{
	[Attribute("", UIWidgets.EditBox, "Faction key this profile is for (e.g. US). Profiles are looked up by this key")]
	string m_sFactionKey;

	[Attribute("1", UIWidgets.CheckBox, "Also add entries auto-discovered from the faction's entity catalogs that the list below does not cover (mod packs that extend the vanilla catalogs show up as kind armor, mech, ...)")]
	bool m_bIncludeDiscovered = true;

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

		return profile;
	}
}

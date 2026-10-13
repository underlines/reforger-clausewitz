// Symbol table of the friendly map layer (design: dev/notes/map-friendly-design.md 2.4). Client side only.
// Enum members are resolved by name at runtime so a renamed member cannot break compilation; a missing one is logged once and skipped.
class CLW_FriendlySymbols
{
	protected static ref map<string, int> s_mIcons;
	protected static ref map<string, bool> s_mMissingLogged;
	protected static ref map<string, bool> s_mNoConfigLogged;
	protected static ref map<string, bool> s_mKindLogged;

	protected static int Icon(string name)
	{
		if (!s_mIcons)
			s_mIcons = new map<string, int>();

		int cached;
		if (s_mIcons.Find(name, cached))
			return cached;

		int value = typename.StringToEnum(EMilitarySymbolIcon, name);
		if (value <= 0)
		{
			value = 0;
			if (!s_mMissingLogged)
				s_mMissingLogged = new map<string, bool>();

			if (!s_mMissingLogged.Contains(name))
			{
				s_mMissingLogged.Set(name, true);
				Print("CLW: friendly client symbol flag EMilitarySymbolIcon." + name + " not found, skipped", LogLevel.WARNING);
			}
		}

		s_mIcons.Set(name, value);
		return value;
	}

	// Kind (CLW_ForceKind or air_rotary / air_fixed / at / mortar) -> dimension + icon flags. Unknown kinds draw as infantry.
	static bool Resolve(string kind, bool isSquad, out EMilitarySymbolDimension dimension, out int iconFlags)
	{
		dimension = EMilitarySymbolDimension.LAND;
		iconFlags = 0;

		if (kind == "recon")
			iconFlags = Icon("RECON");
		else if (kind == "motorized")
		{
			iconFlags = Icon("MOTORIZED");
			if (isSquad)
				iconFlags = iconFlags | Icon("INFANTRY");
		}
		else if (kind == "light")
			iconFlags = Icon("MOTORIZED") | Icon("RECON");
		else if (kind == "mech")
			iconFlags = Icon("INFANTRY") | Icon("ARMOR");
		else if (kind == "armor")
			iconFlags = Icon("ARMOR");
		else if (kind == "armored_recon")
			iconFlags = Icon("ARMOR") | Icon("RECON");
		else if (kind == "logistics")
			iconFlags = Icon("MOTORIZED") | Icon("SUPPLY");
		else if (kind == "air_rotary")
		{
			dimension = EMilitarySymbolDimension.AIR;
			iconFlags = Icon("ROTARY_WING");
		}
		else if (kind == "air_fixed")
		{
			dimension = EMilitarySymbolDimension.AIR;
			iconFlags = Icon("FIXED_WING");
		}
		else if (kind == "at")
			iconFlags = Icon("ANTITANK");
		else if (kind == "mortar")
			iconFlags = Icon("MORTAR");

		if (iconFlags == 0)
			iconFlags = Icon("INFANTRY");

		return true;
	}

	// NATO frame marker, not yet inserted. Null when the marker config lacks the identity / dimension (the caller skips the entry).
	static SCR_MapMarkerBase CreateMilitary(int identity, string kind, bool isSquad, string label, int worldX, int worldZ)
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return null;

		EMilitarySymbolDimension dimension;
		int iconFlags;
		Resolve(kind, isSquad, dimension, iconFlags);
		int dimensionId = dimension;
		int squadId = 0;
		if (isSquad)
			squadId = 1;

		SCR_MapMarkerBase marker = manager.PrepareMilitaryMarker(identity, dimension, iconFlags);
		if (!marker)
		{
			if (!s_mNoConfigLogged)
				s_mNoConfigLogged = new map<string, bool>();

			string failKey = identity.ToString() + "/" + kind;
			if (!s_mNoConfigLogged.Contains(failKey))
			{
				s_mNoConfigLogged.Set(failKey, true);
				Print("CLW: friendly client PrepareMilitaryMarker returned null (identity " + identity.ToString() + " kind " + kind + " dimension " + dimensionId.ToString() + " icons " + iconFlags.ToString() + "): marker config lacks it", LogLevel.WARNING);
			}

			return null;
		}

		if (!s_mKindLogged)
			s_mKindLogged = new map<string, bool>();

		string kindKey = kind + "/" + squadId.ToString();
		if (!s_mKindLogged.Contains(kindKey))
		{
			s_mKindLogged.Set(kindKey, true);
			Print("CLW: friendly client symbol kind=" + kind + " squad=" + squadId.ToString() + " identity=" + identity.ToString() + " dimension=" + dimensionId.ToString() + " icons=" + iconFlags.ToString() + " configId=" + marker.GetMarkerConfigID().ToString(), LogLevel.NORMAL);
		}

		marker.SetCustomText(label);
		marker.SetWorldPos(worldX, worldZ);
		return marker;
	}
}

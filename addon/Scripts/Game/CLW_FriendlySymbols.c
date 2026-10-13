// Symbol table of the friendly map layer (design: dev/notes/map-friendly-design.md 2.4). Client side only.
// Enum members are resolved by name at runtime so a renamed member cannot break compilation; a missing one is logged once and skipped.
class CLW_FriendlySymbols
{
	protected static ref map<string, int> s_mIcons;
	protected static ref map<string, bool> s_mMissingLogged;
	protected static bool s_bNoConfigLogged;

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

		SCR_MapMarkerBase marker = manager.PrepareMilitaryMarker(identity, dimension, iconFlags);
		if (!marker)
		{
			if (!s_bNoConfigLogged)
			{
				s_bNoConfigLogged = true;
				Print("CLW: friendly client PrepareMilitaryMarker returned null (identity " + identity.ToString() + " kind " + kind + "): marker config lacks it", LogLevel.WARNING);
			}

			return null;
		}

		marker.SetCustomText(label);
		marker.SetWorldPos(worldX, worldZ);
		return marker;
	}

	// The "me" arrow: PLACED_CUSTOM marker, icon and colour entry found by name in the marker config (never by index).
	static SCR_MapMarkerBase CreateMe()
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return null;

		SCR_MapMarkerConfig config = manager.GetMarkerConfig();
		if (!config)
			return null;

		SCR_MapMarkerEntryPlaced placed = SCR_MapMarkerEntryPlaced.Cast(config.GetMarkerEntryConfigByType(SCR_EMapMarkerType.PLACED_CUSTOM));
		if (!placed)
			return null;

		int iconIndex = FindIcon(placed, "arrow-small");
		if (iconIndex < 0)
			iconIndex = FindIcon(placed, "dot-2");
		if (iconIndex < 0)
			iconIndex = 0;

		int colorIndex = FindColor(placed, "white");
		if (colorIndex < 0)
			colorIndex = FindColor(placed, "orange");
		if (colorIndex < 0)
			colorIndex = 0;

		SCR_MapMarkerBase marker = new SCR_MapMarkerBase();
		marker.SetType(SCR_EMapMarkerType.PLACED_CUSTOM);
		marker.SetIconEntry(iconIndex);
		marker.SetColorEntry(colorIndex);
		return marker;
	}

	protected static int FindIcon(SCR_MapMarkerEntryPlaced placed, string quadName)
	{
		array<ref SCR_MarkerIconEntry> icons = placed.GetIconEntries();
		if (!icons)
			return -1;

		foreach (int i, SCR_MarkerIconEntry entry : icons)
		{
			ResourceName imageset, glow;
			string quad;
			entry.GetIconResource(imageset, glow, quad);
			if (quad == quadName)
				return i;
		}

		return -1;
	}

	protected static int FindColor(SCR_MapMarkerEntryPlaced placed, string colorName)
	{
		array<ref SCR_MarkerColorEntry> colors = placed.GetColorEntries();
		if (!colors)
			return -1;

		foreach (int i, SCR_MarkerColorEntry entry : colors)
		{
			if (entry.GetName() == colorName)
				return i;
		}

		return -1;
	}
}

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
		{
			Print("CLW: friendly client CreateMilitary: no marker manager (label " + label + ")", LogLevel.WARNING);
			return null;
		}

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

			string failKey = identity.ToString() + "/" + kind + "/" + label;
			SCR_MapMarkerConfig cfgDbg = manager.GetMarkerConfig();
			SCR_MapMarkerEntryMilitary milDbg;
			if (cfgDbg)
				milDbg = SCR_MapMarkerEntryMilitary.Cast(cfgDbg.GetMarkerEntryConfigByType(SCR_EMapMarkerType.PLACED_MILITARY));

			Print("CLW: friendly client PrepareMilitaryMarker null: cfg=" + (cfgDbg != null).ToString() + " military=" + (milDbg != null).ToString() + " factionId=" + manager.GetFactionIDFromMilitarySymbol(identity).ToString() + " label=" + label, LogLevel.WARNING);
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

// The "me" marker: a PLACED_CUSTOM marker (the map's own custom-marker set, not a military symbol) so it can point where the player looks.
// The icon is found by its imageset quad name and the colour by its name in the marker config, never by a hard-coded index.
class CLW_FriendlyMeMarker
{
	static const string ICON_QUAD = "objective-marker-2";	// "general" tab, 29th icon: points south at rotation 0
	static const string COLOR_NAME = "orange";
	static const int ROTATION_AT_NORTH = 180;				// the icon points south at 0 degrees

	// Drawn above every other marker (widgets of one parent stack by z-order, the military symbols are larger).
	static const int Z_ORDER = 10000;

	static void RaiseToTop(SCR_MapMarkerBase marker)
	{
		Widget root = marker.GetRootWidget();
		if (root && root.GetZOrder() != Z_ORDER)
			root.SetZOrder(Z_ORDER);
	}

	// Not yet inserted. Null when the marker config lacks the custom-marker entry or the icon.
	static SCR_MapMarkerBase Create(string label, int worldX, int worldZ, float yawDegrees)
	{
		SCR_MapMarkerManagerComponent manager = SCR_MapMarkerManagerComponent.GetInstance();
		if (!manager)
			return null;

		SCR_MapMarkerConfig config = manager.GetMarkerConfig();
		if (!config)
			return null;

		SCR_MapMarkerEntryPlaced placed = SCR_MapMarkerEntryPlaced.Cast(config.GetMarkerEntryConfigByType(SCR_EMapMarkerType.PLACED_CUSTOM));
		if (!placed)
		{
			Print("CLW: friendly client 'me' marker: the marker config has no custom-marker entry", LogLevel.WARNING);
			return null;
		}

		int iconId = -1;
		array<ref SCR_MarkerIconEntry> icons = placed.GetIconEntries();
		for (int i = 0; i < icons.Count(); i++)
		{
			ResourceName imageset, glow;
			string quad;
			placed.GetIconEntry(i, imageset, glow, quad);
			if (quad == ICON_QUAD)
			{
				iconId = i;
				break;
			}
		}

		if (iconId < 0)
		{
			Print("CLW: friendly client 'me' marker: icon '" + ICON_QUAD + "' not in the marker config", LogLevel.WARNING);
			return null;
		}

		int colorId = 0;
		array<ref SCR_MarkerColorEntry> colors = placed.GetColorEntries();
		for (int c = 0; c < colors.Count(); c++)
		{
			if (colors[c].GetName() == COLOR_NAME)
			{
				colorId = c;
				break;
			}
		}

		SCR_MapMarkerBase marker = new SCR_MapMarkerBase();
		marker.SetType(SCR_EMapMarkerType.PLACED_CUSTOM);
		marker.SetIconEntry(iconId);
		marker.SetColorEntry(colorId);
		marker.SetCustomText(label);
		marker.SetWorldPos(worldX, worldZ);
		marker.SetRotation(Rotation(yawDegrees));
		return marker;
	}

	// Entity yaw (0 = north, clockwise) -> widget rotation of an icon that points south at 0.
	static int Rotation(float yawDegrees)
	{
		int angle = Math.Round(yawDegrees) + ROTATION_AT_NORTH;
		while (angle < 0)
		{
			angle += 360;
		}

		while (angle >= 360)
		{
			angle -= 360;
		}

		return angle;
	}

	// Turn a live marker (data + widget).
	static void SetHeading(SCR_MapMarkerBase marker, float yawDegrees)
	{
		int angle = Rotation(yawDegrees);
		if (marker.GetRotation() == angle)
			return;

		marker.SetRotation(angle);
		SCR_MapMarkerWidgetComponent widget = marker.GetMarkerComponent();
		if (widget)
			widget.SetRotation(angle);
	}
}

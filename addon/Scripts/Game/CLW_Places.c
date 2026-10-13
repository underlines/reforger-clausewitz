// Named map places + coarse map grid + size words, for human-readable reports (SITREP, LLM situation). Server only.
//
// Places: read once, lazily, from the map entity (same source as CLW_WorldInfo.SendLocations: MapItems of every descriptor type
// whose name contains NAME / BASE / PORT, display name translated). When the world has no map entity (or no named item), a
// warning is logged once and the control point (objective) names are used as the only places.
//
// Grid scheme: the vanilla map draws a grid whose labels are SCR_MapEntity.GetGridPos(pos, gx, gz, resMin 2, resMax 4): per axis the
// decimal digits of the world coordinate for 10 km, 1 km and 100 m (X = east, Z = north), so a 100 m cell reads "262 288" on the map
// legend. The text grid here is the SAME function and precision (resMin 2, resMax 4), written "262-288" (east-north), so players
// read it straight off the map labels. (Verified 2026-10-12: resMin 3 gave "02-02" for x=2535, z=2853, i.e. 10 km + 1 km digits.)
class CLW_Place
{
	string m_sName;
	vector m_vPos;
	string m_sType;	// map descriptor type text, lower case without the MDT_ prefix (e.g. name_village); "" = unknown
}

class CLW_Places
{
	static const int MAX_PLACES = 800;
	static const float DUPLICATE_RADIUS = 150;	// m: same-named items closer than this are one place

	protected static ref array<ref CLW_Place> s_aPlaces;
	protected static bool s_bBuilt;
	protected static int s_iEmptyBuilds;	// builds that found no map item (the map entity may not have its items yet, e.g. at game start)
	static const int EMPTY_RETRIES = 3;

	// Build the place list once (idempotent). Safe to call from every report.
	static void Ensure()
	{
		if (s_bBuilt)
			return;

		s_bBuilt = true;
		s_aPlaces = {};
		bool typed;
		MapEntity mapEnt = SCR_MapEntity.GetMapInstance();
		if (mapEnt)
		{
			for (int t = 0; t < EMapDescriptorType.MDT_COUNT; t++)
			{
				string typeName = typename.EnumToString(EMapDescriptorType, t);
				if (!typeName.Contains("NAME") && !typeName.Contains("BASE") && !typeName.Contains("PORT"))
					continue;

				array<MapItem> found = {};
				mapEnt.GetByType(found, t);
				string typeKey = typeName;
				typeKey.Replace("MDT_", "");
				typeKey.ToLower();
				foreach (MapItem item : found)
				{
					if (!item || s_aPlaces.Count() >= MAX_PLACES)
						continue;

					string name = WidgetManager.Translate(item.GetDisplayName());
					if (name.IsEmpty())
						continue;

					AddPlace(name, ItemPos(item), typeKey);
					typed = true;
				}
			}
		}

		if (!typed && s_iEmptyBuilds < EMPTY_RETRIES)
		{
			// no typed place yet: do not cache this result for good, the next caller builds again (a few times)
			s_iEmptyBuilds++;
			s_bBuilt = false;
		}

		if (s_aPlaces.IsEmpty())
		{
			Print("CLW: places: no named map places available on this server, using objective names only", LogLevel.WARNING);
			CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
			if (points)
			{
				for (int i = 0; i < points.Count(); i++)
				{
					CLW_ControlPoint point = points.Get(i);
					if (point && !point.m_sName.IsEmpty())
						AddPlace(point.m_sName, point.m_vPos, "");
				}
			}
		}

		string sample;
		for (int s = 0; s < s_aPlaces.Count() && s < 3; s++)
		{
			sample += " '" + s_aPlaces[s].m_sName + "' " + s_aPlaces[s].m_sType + " " + s_aPlaces[s].m_vPos.ToString();
		}
		Print("CLW: places: " + s_aPlaces.Count() + " named places cached, e.g." + sample, LogLevel.NORMAL);
	}

	// World position of a map item. MapItem.GetPos() is (0,0) on a dedicated server (the map is never built headless, seen 2026-10-12:
	// every location at the origin), so the owner entity's origin is used then. GetPos() is 2D (x, z) packed into a vector [verify order].
	static vector ItemPos(MapItem item)
	{
		if (!item)
			return vector.Zero;

		vector pos = item.GetPos();
		if (pos[0] != 0 || pos[1] != 0 || pos[2] != 0)
		{
			if (pos[2] == 0 && pos[1] != 0)
				return Vector(pos[0], 0, pos[1]);	// (x, z, 0)

			return pos;
		}

		IEntity owner = item.Entity();
		if (owner)
			return owner.GetOrigin();

		return pos;
	}

	protected static void AddPlace(string name, vector pos, string type)
	{
		foreach (CLW_Place known : s_aPlaces)
		{
			if (known.m_sName == name && vector.DistanceXZ(known.m_vPos, pos) < DUPLICATE_RADIUS)
				return;
		}

		CLW_Place place = new CLW_Place();
		place.m_sName = name;
		place.m_vPos = pos;
		place.m_sType = type;
		s_aPlaces.Insert(place);
	}

	// Forget the cached places (rebuilt on the next use). Used after a random layout moved the objectives when the list was built from
	// the objective names (no map items on this server).
	static void Invalidate()
	{
		s_bBuilt = false;
		s_aPlaces = null;
	}

	// All cached places (builds the list on first use). Auto scenario / random layout read them for wishes and objectives.
	static void GetAll(notnull array<ref CLW_Place> outPlaces)
	{
		Ensure();
		foreach (CLW_Place place : s_aPlaces)
		{
			outPlaces.Insert(place);
		}
	}

	// True when at least one place came from a map item (has a descriptor type).
	static bool HasTypedPlaces()
	{
		Ensure();
		foreach (CLW_Place place : s_aPlaces)
		{
			if (!place.m_sType.IsEmpty())
				return true;
		}

		return false;
	}

	// Nearest named place within maxDist whose descriptor type contains one of the given texts. "" when none.
	static string NearestOfTypes(vector pos, float maxDist, notnull array<string> typeParts, out float dist, out vector placePos)
	{
		Ensure();
		string best = "";
		dist = maxDist;
		foreach (CLW_Place place : s_aPlaces)
		{
			if (place.m_sType.IsEmpty())
				continue;

			bool match;
			foreach (string part : typeParts)
			{
				if (place.m_sType.Contains(part))
				{
					match = true;
					break;
				}
			}

			if (!match)
				continue;

			float d = vector.DistanceXZ(pos, place.m_vPos);
			if (d < dist)
			{
				dist = d;
				best = place.m_sName;
				placePos = place.m_vPos;
			}
		}

		return best;
	}

	// Nearest named place within maxDist (m). "" when none. dist = metres to it.
	static string Nearest(vector pos, float maxDist, out float dist, out vector placePos)
	{
		Ensure();
		string best = "";
		dist = maxDist;
		foreach (CLW_Place place : s_aPlaces)
		{
			float d = vector.DistanceXZ(pos, place.m_vPos);
			if (d < dist)
			{
				dist = d;
				best = place.m_sName;
				placePos = place.m_vPos;
			}
		}

		return best;
	}

	// Nearest control point (objective) to a position, null when none. dist = metres.
	static CLW_ControlPoint NearestObjective(vector pos, CLW_ControlPoints points, out float dist)
	{
		CLW_ControlPoint best;
		dist = 1000000;
		if (!points)
			return null;

		for (int i = 0; i < points.Count(); i++)
		{
			CLW_ControlPoint point = points.Get(i);
			if (!point)
				continue;

			float d = vector.DistanceXZ(pos, point.m_vPos);
			if (d < dist)
			{
				dist = d;
				best = point;
			}
		}

		return best;
	}

	// Short place name for data (JSON): the objective name when within 700 m of one, else the nearest named place (3 km), else "".
	static string PlaceLabel(vector pos, CLW_ControlPoints points)
	{
		float objDist;
		CLW_ControlPoint point = NearestObjective(pos, points, objDist);
		if (point && objDist <= 700)
			return point.m_sName;

		float dist;
		vector placePos;
		return Nearest(pos, 3000, dist, placePos);
	}

	// ---- grid ----

	// 100 m grid reference "025-285" (east-north): exactly the 100 m cell labels of the vanilla map legend (see header). 1 km squares
	// (resMin 3) were too coarse on small maps (Arland: 16 squares, all "0x"), owner decision 2026-10-12.
	static string Grid(vector pos)
	{
		int gridX;
		int gridZ;
		SCR_MapEntity.GetGridPos(pos, gridX, gridZ, 2, 4);
		return Pad3(gridX) + "-" + Pad3(gridZ);
	}

	protected static string Pad3(int value)
	{
		string text = value.ToString();
		while (text.Length() < 3)
		{
			text = "0" + text;
		}

		return text;
	}

	// ---- compass ----

	// 0..7 = N, NE, E, SE, S, SW, W, NW; direction from "from" to "to" (x = east, z = north).
	static int CompassIndex(vector from, vector to)
	{
		float dx = to[0] - from[0];
		float dz = to[2] - from[2];
		float angle = Math.Atan2(dx, dz) * Math.RAD2DEG;	// 0 = north, 90 = east
		if (angle < 0)
			angle += 360;

		int index = Math.Round(angle / 45);	// no % on floats in Enforce: round to int first
		return index % 8;
	}

	// "north", "north-east", ...
	static string CompassWord(int index)
	{
		switch (index)
		{
			case 0: return "north";
			case 1: return "north-east";
			case 2: return "east";
			case 3: return "south-east";
			case 4: return "south";
			case 5: return "south-west";
			case 6: return "west";
			case 7: return "north-west";
		}

		return "north";
	}

	// "northern", "north-eastern", ...
	static string CompassAdjective(int index)
	{
		switch (index)
		{
			case 0: return "northern";
			case 1: return "north-eastern";
			case 2: return "eastern";
			case 3: return "south-eastern";
			case 4: return "southern";
			case 5: return "south-western";
			case 6: return "western";
			case 7: return "north-western";
		}

		return "northern";
	}

	// ---- size words ----

	// Rough strength of a contact for sorting: a vehicle weighs like a squad-ish handful of infantry.
	static int Score(int vehicles, int infantry, int count)
	{
		int inf = infantry;
		if (vehicles <= 0 && inf <= 0)
			inf = count;

		return inf + vehicles * 5;
	}

	// "Light" / "Moderate" / "Significant" / "Large" for the report headline of a contact.
	static string Magnitude(int score)
	{
		if (score <= 4)
			return "Light";
		if (score <= 12)
			return "Moderate";
		if (score <= 30)
			return "Significant";

		return "Large";
	}

	static string InfantryText(int infantry)
	{
		if (infantry <= 2)
			return "a few infantry";
		if (infantry <= 5)
			return "an infantry fire team";
		if (infantry <= 12)
			return "a squad-sized infantry element";
		if (infantry <= 40)
			return "a platoon-sized infantry element";

		return "a company-sized infantry force";
	}

	static string VehicleText(int vehicles)
	{
		if (vehicles <= 1)
			return "a single vehicle";
		if (vehicles <= 3)
			return "a section of vehicles";
		if (vehicles <= 8)
			return "a vehicle platoon";

		return "a large mechanized force";
	}

	// Full size words for a contact ("a vehicle platoon with dismounts"). count = all live targets (used when no breakdown is known).
	static string SizeText(int vehicles, int infantry, int count)
	{
		if (vehicles > 0 && infantry >= 6)
			return InfantryText(infantry) + " with vehicle support";
		if (vehicles > 0 && infantry > 0)
			return VehicleText(vehicles) + " with dismounts";
		if (vehicles > 0)
			return VehicleText(vehicles);
		if (infantry > 0)
			return InfantryText(infantry);
		if (count > 0)
			return InfantryText(count);

		return "an unidentified element";
	}

	// ---- the same words as keys (player-facing text: the client formats them, see CLW_Loc.c) ----

	static string MagnitudeKey(int score)
	{
		if (score <= 4)
			return "#CLW-Sit_Mag_1";
		if (score <= 12)
			return "#CLW-Sit_Mag_2";
		if (score <= 30)
			return "#CLW-Sit_Mag_3";

		return "#CLW-Sit_Mag_4";
	}

	// Compass direction as an adjective key: northern, north-eastern, ...
	static string CompassAdjectiveKey(int index)
	{
		return "#CLW-Sit_CompassAdj_" + index.ToString();
	}

	static string InfantryWire(int infantry)
	{
		if (infantry <= 2)
			return CLW_Loc.W("#CLW-Sit_Inf_1");
		if (infantry <= 5)
			return CLW_Loc.W("#CLW-Sit_Inf_2");
		if (infantry <= 12)
			return CLW_Loc.W("#CLW-Sit_Inf_3");
		if (infantry <= 40)
			return CLW_Loc.W("#CLW-Sit_Inf_4");

		return CLW_Loc.W("#CLW-Sit_Inf_5");
	}

	static string VehicleWire(int vehicles)
	{
		if (vehicles <= 1)
			return CLW_Loc.W("#CLW-Sit_Veh_1");
		if (vehicles <= 3)
			return CLW_Loc.W("#CLW-Sit_Veh_2");
		if (vehicles <= 8)
			return CLW_Loc.W("#CLW-Sit_Veh_3");

		return CLW_Loc.W("#CLW-Sit_Veh_4");
	}

	// SizeText as a wire.
	static string SizeWire(int vehicles, int infantry, int count)
	{
		if (vehicles > 0 && infantry >= 6)
			return CLW_Loc.W("#CLW-Sit_SizeWithVehicles", InfantryWire(infantry));
		if (vehicles > 0 && infantry > 0)
			return CLW_Loc.W("#CLW-Sit_SizeWithDismounts", VehicleWire(vehicles));
		if (vehicles > 0)
			return VehicleWire(vehicles);
		if (infantry > 0)
			return InfantryWire(infantry);
		if (count > 0)
			return InfantryWire(count);

		return CLW_Loc.W("#CLW-Sit_SizeUnknown");
	}

	// Map marker labels (short).
	static string InfantryShort(int infantry)
	{
		if (infantry <= 2)
			return "few inf";
		if (infantry <= 5)
			return "fire team";
		if (infantry <= 12)
			return "squad";
		if (infantry <= 40)
			return "platoon";

		return "company";
	}

	static string VehicleShort(int vehicles)
	{
		if (vehicles <= 1)
			return "vehicle";
		if (vehicles <= 3)
			return "veh section";
		if (vehicles <= 8)
			return "veh platoon";

		return "mech force";
	}
}

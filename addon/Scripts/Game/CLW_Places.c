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
}

class CLW_Places
{
	static const int MAX_PLACES = 800;
	static const float DUPLICATE_RADIUS = 150;	// m: same-named items closer than this are one place

	protected static ref array<ref CLW_Place> s_aPlaces;
	protected static bool s_bBuilt;

	// Build the place list once (idempotent). Safe to call from every report.
	static void Ensure()
	{
		if (s_bBuilt)
			return;

		s_bBuilt = true;
		s_aPlaces = {};
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
				foreach (MapItem item : found)
				{
					if (!item || s_aPlaces.Count() >= MAX_PLACES)
						continue;

					string name = WidgetManager.Translate(item.GetDisplayName());
					if (name.IsEmpty())
						continue;

					AddPlace(name, item.GetPos());
				}
			}
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
						AddPlace(point.m_sName, point.m_vPos);
				}
			}
		}

		Print("CLW: places: " + s_aPlaces.Count() + " named places cached", LogLevel.NORMAL);
	}

	protected static void AddPlace(string name, vector pos)
	{
		foreach (CLW_Place known : s_aPlaces)
		{
			if (known.m_sName == name && vector.DistanceXZ(known.m_vPos, pos) < DUPLICATE_RADIUS)
				return;
		}

		CLW_Place place = new CLW_Place();
		place.m_sName = name;
		place.m_vPos = pos;
		s_aPlaces.Insert(place);
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

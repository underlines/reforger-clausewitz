// Map digest for the LLM commander (M4.6 FOB tool, dev/notes/fob-tool-design.md; reusable for the 13a "LLM spatial picture"). Server only.
// ONE builder, two resolutions:
//   MacroJson(side)        the whole map on a coarse grid (cell 500-1600 m, at most 20 x 20): per cell 2 glyphs terrain + road, towns, POIs,
//                          objectives by cell, HQ / front / own FOB cells, known enemy contacts by cell. Sent as `fobMap` in the situation.
//   ZoomJson(side, area)   one macro cell at 100 m resolution: 3 glyphs per sub-cell (elevation digit, terrain, road), POIs, objectives, contacts,
//                          own bases, `toward` the front. Sent in POST /fobzoom.
// The terrain classes (water / coast / town / mountain / hills / forest / open) and the road classes are computed ONCE per world from
// height samples, tree probes, the engine road network and CLW_Places, then cached (Reset() on war start). Nothing here knows a map,
// a prefab or a faction: places come from the map descriptors, rules from CLW_FobPlacer. Log prefix: "CLW: fob map".
// APIs: BaseWorld.GetBoundBox / GetSurfaceY / GetOceanHeight / QueryEntitiesBySphere / QueryEntitiesByAABB, RoadNetworkManager.GetRoadsInAABB,
// BaseRoad.GetPoints / GetWidth, BaseTree / TreeEntity / Building (same calls as CLW_FreeSpot / CLW_RandomLayout).

// QueryEntitiesBySphere callback: counts trees.
class CLW_TreeProbe
{
	int m_iCount;

	bool OnEntity(IEntity entity)
	{
		if (entity && (BaseTree.Cast(entity) || TreeEntity.Cast(entity)))
			m_iCount++;

		return true;
	}
}

// QueryEntitiesByAABB callback: counts buildings per sub-cell of the zoom area. An entity belongs to the tile holding its origin.
class CLW_BuildingBins
{
	float m_fTileX0;
	float m_fTileZ0;
	float m_fTileX1;
	float m_fTileZ1;
	float m_fX0;
	float m_fZTop;
	float m_fSub = 100;
	int m_iN;
	ref array<int> m_aBins = {};

	bool OnEntity(IEntity entity)
	{
		if (!entity || !Building.Cast(entity))
			return true;

		vector origin = entity.GetOrigin();
		if (origin[0] < m_fTileX0 || origin[0] >= m_fTileX1 || origin[2] < m_fTileZ0 || origin[2] >= m_fTileZ1)
			return true;

		int bc = Math.Floor((origin[0] - m_fX0) / m_fSub);
		int br = Math.Floor((m_fZTop - origin[2]) / m_fSub);
		if (bc < 0 || bc >= m_iN || br < 0 || br >= m_iN)
			return true;

		m_aBins[br * m_iN + bc] = m_aBins[br * m_iN + bc] + 1;
		return true;
	}
}

class CLW_MapDigest
{
	static const string COL_LETTERS = "ABCDEFGHIJKLMNOPQRST";
	static const string SUB_LETTERS = "abcdefghijklmnop";
	static const int MACRO_TARGET = 10;				// macro cells along the longer axis
	static const float CELL_MIN_M = 500;
	static const float CELL_MAX_M = 1600;
	static const int MAX_AXIS = 20;
	static const float SUB_M = 100;					// zoom resolution
	static const int HEIGHT_SAMPLES = 5;			// per macro cell and axis
	static const int FOREST_PROBES = 3;				// per macro cell and axis
	static const float FOREST_RADIUS_M = 30;
	static const int FOREST_TREES = 8;				// trees within the probe radius = a forest probe
	static const float FOREST_CELL_FRACTION = 0.45;	// share of forest probes that makes a forest cell
	static const float COAST_FRACTION = 0.15;
	static const float SEA_FRACTION = 0.55;
	static const float MOUNTAIN_RELIEF_M = 90;
	static const float HILL_RELIEF_M = 40;
	static const float MAIN_ROAD_WIDTH_M = 6;
	static const float MIN_ROAD_WIDTH_M = 2;
	static const int ZOOM_FOREST_TREES = 10;		// trees in the four probes of a sub-cell = forest
	static const int ZOOM_TOWN_BUILDINGS = 5;
	static const float ZOOM_TILE_M = 400;
	static const int MAX_TOWNS = 14;
	static const int MAX_POIS = 8;
	static const int MAX_CONTACT_CELLS = 8;
	static const float LOG_GAP_SEC = 300;

	protected static bool s_bBuilt;
	protected static bool s_bFailed;
	protected static float s_fCell;
	protected static float s_fX0;
	protected static float s_fZTop;
	protected static float s_fX1;
	protected static float s_fZBottom;
	protected static int s_iCols;
	protected static int s_iRows;
	protected static ref array<string> s_aTerrain = {};		// per macro cell (row-major, row 0 = north)
	protected static ref array<string> s_aRoad = {};
	protected static ref array<float> s_aRelief = {};
	protected static string s_sStaticJson;
	protected static ref map<string, int> s_mLogChars = new map<string, int>();
	protected static ref map<string, float> s_mLogAt = new map<string, float>();

	static void Reset()
	{
		s_bBuilt = false;
		s_bFailed = false;
		s_aTerrain.Clear();
		s_aRoad.Clear();
		s_aRelief.Clear();
		s_sStaticJson = "";
		s_mLogChars.Clear();
		s_mLogAt.Clear();
	}

	static int Cols()
	{
		return s_iCols;
	}

	static int Rows()
	{
		return s_iRows;
	}

	static float CellSize()
	{
		return s_fCell;
	}

	// ---- cell ids ----

	static string MacroId(int col, int row)
	{
		return COL_LETTERS.Substring(col, 1) + (row + 1).ToString();
	}

	static string SubId(int col, int row)
	{
		return SUB_LETTERS.Substring(col, 1) + (row + 1).ToString();
	}

	// "D5" -> col 3, row 4 (row 0 = north). False when the id is not a cell of this map.
	static bool ParseArea(string area, out int col, out int row)
	{
		if (!Ensure() || area.Length() < 2 || area.Length() > 3)
			return false;

		string text = area;
		text.ToUpper();
		col = COL_LETTERS.IndexOf(text.Substring(0, 1));
		string rowText = text.Substring(1, text.Length() - 1);
		if (col < 0 || col >= s_iCols || !CLW_ModeParam.IsNumber(rowText))
			return false;

		row = rowText.ToInt() - 1;
		return row >= 0 && row < s_iRows;
	}

	static bool AreaValid(string area)
	{
		int col;
		int row;
		return ParseArea(area, col, row);
	}

	// Macro cell id of a position, "" when the position is outside the digest grid.
	static string CellOf(vector pos)
	{
		int col;
		int row;
		if (!CellIndex(pos, col, row))
			return "";

		return MacroId(col, row);
	}

	protected static bool CellIndex(vector pos, out int col, out int row)
	{
		if (!Ensure())
			return false;

		return CellIndexRaw(pos, col, row);
	}

	// Cell of a position without building the digest (used while it is being built).
	protected static bool CellIndexRaw(vector pos, out int col, out int row)
	{
		col = Math.Floor((pos[0] - s_fX0) / s_fCell);
		row = Math.Floor((s_fZTop - pos[2]) / s_fCell);
		return col >= 0 && col < s_iCols && row >= 0 && row < s_iRows;
	}

	// The macro area box: x0, zTop (north edge), size.
	static bool AreaBox(string area, out float x0, out float zTop, out float size)
	{
		int col;
		int row;
		if (!ParseArea(area, col, row))
			return false;

		x0 = s_fX0 + col * s_fCell;
		zTop = s_fZTop - row * s_fCell;
		size = s_fCell;
		return true;
	}

	static bool InArea(string area, vector pos)
	{
		float x0;
		float zTop;
		float size;
		if (!AreaBox(area, x0, zTop, size))
			return false;

		return pos[0] >= x0 && pos[0] < x0 + size && pos[2] <= zTop && pos[2] > zTop - size;
	}

	// Centre of a zoom sub-cell ("f7") of an area. False when either id is invalid.
	static bool SubCellCenter(string area, string sub, out vector pos)
	{
		float x0;
		float zTop;
		float size;
		if (!AreaBox(area, x0, zTop, size) || sub.Length() < 2 || sub.Length() > 3)
			return false;

		string text = sub;
		text.ToLower();
		int sc = SUB_LETTERS.IndexOf(text.Substring(0, 1));
		string rowText = text.Substring(1, text.Length() - 1);
		int n = ZoomN();
		if (sc < 0 || sc >= n || !CLW_ModeParam.IsNumber(rowText))
			return false;

		int sr = rowText.ToInt() - 1;
		if (sr < 0 || sr >= n)
			return false;

		float step = size / n;
		pos = Vector(x0 + (sc + 0.5) * step, 0, zTop - (sr + 0.5) * step);
		CLW_Placement.SurfaceAt(pos);
		return true;
	}

	protected static int ZoomN()
	{
		int n = Math.Round(s_fCell / SUB_M);
		return Math.Clamp(n, 1, SUB_LETTERS.Length());
	}

	// Short terrain words at a position for text pictures (13a): "village", "forest", "hills +60 m", "open", "water".
	static string TerrainWordAt(vector pos)
	{
		int col;
		int row;
		if (!CellIndex(pos, col, row))
			return "";

		int index = row * s_iCols + col;
		string glyph = s_aTerrain[index];
		if (glyph == "~")
			return "water";
		if (glyph == "c")
			return "coast";
		if (glyph == "T")
			return "town";
		if (glyph == "v")
			return "village";
		if (glyph == "f")
			return "forest";
		if (glyph == "M")
			return "mountains +" + Math.Round(s_aRelief[index]).ToString() + " m";
		if (glyph == "h")
			return "hills +" + Math.Round(s_aRelief[index]).ToString() + " m";

		return "open";
	}

	// ---- build (once per world) ----

	protected static bool IsWaterAt(float x, float z)
	{
		BaseWorld world = GetGame().GetWorld();
		return world.GetSurfaceY(x, z) <= world.GetOceanHeight(x, z) + CLW_WarConfig.WATER_MARGIN_M;
	}

	protected static int CountTreesAround(float x, float z, float radius)
	{
		CLW_TreeProbe probe = new CLW_TreeProbe();
		GetGame().GetWorld().QueryEntitiesBySphere(Vector(x, GetGame().GetWorld().GetSurfaceY(x, z), z), radius, probe.OnEntity, null, EQueryEntitiesFlags.STATIC | EQueryEntitiesFlags.WITH_OBJECT);
		return probe.m_iCount;
	}

	// Builds the macro classes once. False (logged once) when the world has no usable bounds.
	static bool Ensure()
	{
		if (s_bBuilt)
			return true;

		if (s_bFailed)
			return false;

		BaseWorld world = GetGame().GetWorld();
		if (!world)
			return false;

		int startMs = System.GetTickCount();
		vector mins;
		vector maxs;
		world.GetBoundBox(mins, maxs);
		float spanX = maxs[0] - mins[0];
		float spanZ = maxs[2] - mins[2];
		if (spanX < 200 || spanZ < 200)
		{
			s_bFailed = true;
			Print(string.Format("CLW: fob map: world bounds unusable (%1 x %2 m), no map digest", Math.Round(spanX), Math.Round(spanZ)), LogLevel.WARNING);
			return false;
		}

		float span = Math.Max(spanX, spanZ);
		s_fCell = Math.Clamp(Math.Round(span / MACRO_TARGET / SUB_M) * SUB_M, CELL_MIN_M, CELL_MAX_M);
		s_iCols = Math.Clamp(Math.Ceil(spanX / s_fCell), 1, MAX_AXIS);
		s_iRows = Math.Clamp(Math.Ceil(spanZ / s_fCell), 1, MAX_AXIS);
		s_fX0 = mins[0];
		s_fZTop = maxs[2];
		s_fX1 = s_fX0 + s_iCols * s_fCell;
		s_fZBottom = s_fZTop - s_iRows * s_fCell;
		s_aTerrain.Clear();
		s_aRoad.Clear();
		s_aRelief.Clear();
		int cells = s_iCols * s_iRows;
		for (int ci = 0; ci < cells; ci++)
		{
			s_aTerrain.Insert(".");
			s_aRoad.Insert("_");
			s_aRelief.Insert(0.0);
		}

		int probes;
		for (int mr = 0; mr < s_iRows; mr++)
		{
			for (int mc = 0; mc < s_iCols; mc++)
			{
				float cx0 = s_fX0 + mc * s_fCell;
				float cz1 = s_fZTop - mr * s_fCell;
				int water = 0;
				int land = 0;
				float lowY = 1000000;
				float highY = -1000000;
				for (int hz = 0; hz < HEIGHT_SAMPLES; hz++)
				{
					for (int hx = 0; hx < HEIGHT_SAMPLES; hx++)
					{
						float sx = cx0 + (hx + 0.5) / HEIGHT_SAMPLES * s_fCell;
						float sz = cz1 - (hz + 0.5) / HEIGHT_SAMPLES * s_fCell;
						float sy = world.GetSurfaceY(sx, sz);
						if (sy <= world.GetOceanHeight(sx, sz) + CLW_WarConfig.WATER_MARGIN_M)
						{
							water++;
							continue;
						}

						land++;
						lowY = Math.Min(lowY, sy);
						highY = Math.Max(highY, sy);
					}
				}

				float waterFrac = water / (HEIGHT_SAMPLES * HEIGHT_SAMPLES * 1.0);
				float relief = 0;
				if (land >= 2)
					relief = highY - lowY;

				int forestHits = 0;
				int forestTotal = 0;
				if (waterFrac < SEA_FRACTION)
				{
					for (int fz = 0; fz < FOREST_PROBES; fz++)
					{
						for (int fx = 0; fx < FOREST_PROBES; fx++)
						{
							float px = cx0 + (fx + 0.5) / FOREST_PROBES * s_fCell;
							float pz = cz1 - (fz + 0.5) / FOREST_PROBES * s_fCell;
							if (IsWaterAt(px, pz))
								continue;

							forestTotal++;
							probes++;
							if (CountTreesAround(px, pz, FOREST_RADIUS_M) >= FOREST_TREES)
								forestHits++;
						}
					}
				}

				float forestFrac = 0;
				if (forestTotal > 0)
					forestFrac = forestHits / (forestTotal * 1.0);

				string glyph = ".";
				if (waterFrac >= SEA_FRACTION)
					glyph = "~";
				else if (waterFrac >= COAST_FRACTION)
					glyph = "c";
				else if (relief >= MOUNTAIN_RELIEF_M)
					glyph = "M";
				else if (relief >= HILL_RELIEF_M)
					glyph = "h";
				else if (forestFrac >= FOREST_CELL_FRACTION)
					glyph = "f";

				int cellIndex = mr * s_iCols + mc;
				s_aTerrain[cellIndex] = glyph;
				s_aRelief[cellIndex] = relief;
			}
		}

		// towns and villages (named places) override the landscape of their cell, except open water
		array<ref CLW_Place> places = {};
		CLW_Places.GetAll(places);
		int townCells;
		foreach (CLW_Place place : places)
		{
			string kind = PlaceKind(place.m_sType);
			if (kind != "T" && kind != "v")
				continue;

			int pc;
			int pr;
			if (!CellIndexRaw(place.m_vPos, pc, pr))
				continue;

			int pIndex = pr * s_iCols + pc;
			if (s_aTerrain[pIndex] == "~" || s_aTerrain[pIndex] == "T")
				continue;

			if (kind == "v" && s_aTerrain[pIndex] == "v")
				continue;

			s_aTerrain[pIndex] = kind;
			townCells++;
		}

		// roads: one query for the whole world, each segment resampled into the cells it crosses
		int roadCount;
		RoadNetworkManager mgr = CLW_Placement.GetRoadManager();
		if (mgr)
		{
			array<BaseRoad> roads = {};
			mgr.GetRoadsInAABB(Vector(s_fX0, -10000, s_fZBottom), Vector(s_fX1, 10000, s_fZTop), roads);
			float step = Math.Max(50, s_fCell / 4);
			foreach (BaseRoad road : roads)
			{
				if (!road || road.GetWidth() < MIN_ROAD_WIDTH_M)
					continue;

				string roadGlyph = "-";
				if (road.GetWidth() >= MAIN_ROAD_WIDTH_M)
					roadGlyph = "=";

				array<vector> pts = {};
				road.GetPoints(pts);
				roadCount++;
				for (int ri = 0; ri < pts.Count() - 1; ri++)
				{
					float dx = pts[ri + 1][0] - pts[ri][0];
					float dz = pts[ri + 1][2] - pts[ri][2];
					float len = Math.Sqrt(dx * dx + dz * dz);
					if (len < 0.5)
						continue;

					for (float d = 0; d <= len; d += step)
					{
						vector rp = Vector(pts[ri][0] + dx * d / len, 0, pts[ri][2] + dz * d / len);
						int rc;
						int rr;
						if (!CellIndexRaw(rp, rc, rr))
							continue;

						int rIndex = rr * s_iCols + rc;
						if (s_aRoad[rIndex] != "=")
							s_aRoad[rIndex] = roadGlyph;
					}
				}
			}
		}

		s_bBuilt = true;
		s_sStaticJson = BuildStaticJson(places);
		Print(string.Format("CLW: fob map: macro digest built %1 x %2 cells of %3 m (world %4 x %5 m), %6 tree probes, %7 town cells, %8 roads, static part %9 chars (~%10 tokens), %11 ms", s_iCols, s_iRows, Math.Round(s_fCell), Math.Round(spanX), Math.Round(spanZ), probes, townCells, roadCount, s_sStaticJson.Length(), s_sStaticJson.Length() / 4, System.GetTickCount() - startMs), LogLevel.NORMAL);
		return true;
	}

	// Place kind from the map descriptor type text: "T" city / town, "v" village, "poi" military base / port / airport, "" other.
	protected static string PlaceKind(string typeText)
	{
		if (typeText.IsEmpty())
			return "";

		if (typeText.Contains("city") || typeText.Contains("town"))
			return "T";

		if (typeText.Contains("village"))
			return "v";

		if (typeText.Contains("base") || typeText.Contains("port"))
			return "poi";

		return "";
	}

	protected static string BuildStaticJson(notnull array<ref CLW_Place> places)
	{
		string cols;
		for (int c = 0; c < s_iCols; c++)
		{
			cols += COL_LETTERS.Substring(c, 1);
		}

		string rowsJson;
		for (int r = 0; r < s_iRows; r++)
		{
			string line;
			line = "";
			for (int k = 0; k < s_iCols; k++)
			{
				line += s_aTerrain[r * s_iCols + k] + s_aRoad[r * s_iCols + k];
			}

			if (r > 0)
				rowsJson += ",";
			rowsJson += "\"" + line + "\"";
		}

		string towns;
		int townCount;
		array<string> wantKinds = {"T", "v"};
		foreach (string wanted : wantKinds)
		{
			foreach (CLW_Place place : places)
			{
				if (townCount >= MAX_TOWNS)
					break;

				string wantType = place.m_sType;
				if (PlaceKind(wantType) != wanted)
					continue;

				string cell = CellOf(place.m_vPos);
				if (cell.IsEmpty())
					continue;

				if (townCount > 0)
					towns += ",";
				towns += "\"" + CLW_Situation.Escape(place.m_sName) + " " + cell + "\"";
				townCount++;
			}
		}

		string pois;
		int poiCount;
		foreach (CLW_Place poi : places)
		{
			if (poiCount >= MAX_POIS)
				break;

			string poiType = poi.m_sType;
			if (PlaceKind(poiType) != "poi")
				continue;

			string poiCell = CellOf(poi.m_vPos);
			if (poiCell.IsEmpty())
				continue;

			string poiWord = poiType;
			poiWord.Replace("name_", "");
			poiWord.Replace("_", " ");
			if (poiCount > 0)
				pois += ",";
			pois += "\"" + CLW_Situation.Escape(poi.m_sName) + " (" + CLW_Situation.Escape(poiWord) + ") " + poiCell + "\"";
			poiCount++;
		}

		string legend = "cell id = column letter + row number, row 1 = north, column A = west. Each cell is 2 glyphs, terrain then road. terrain: ~ sea/lake, c coast, T town/city, v village, M mountains, h hills, f forest, . open. road: = main road, - minor road, _ none.";
		string text = "{\"cell\":" + Math.Round(s_fCell) + ",\"x0\":" + Math.Round(s_fX0) + ",\"zTop\":" + Math.Round(s_fZTop) + ",\"cols\":\"" + cols + "\",\"n\":" + s_iRows;
		text += ",\"legend\":\"" + legend + "\",\"rows\":[" + rowsJson + "],\"towns\":[" + towns + "],\"pois\":[" + pois + "]}";
		return text;
	}

	// ---- macro digest (situation block `fobMap`) ----

	// The whole-map digest for one side: static terrain + objectives / front / HQs / own FOBs / known contacts by cell. "" when no map.
	// Logs its size (chars, ~tokens) when it changed by more than 15 % or every LOG_GAP_SEC.
	static string MacroJson(string side)
	{
		if (!Ensure() || s_sStaticJson.IsEmpty())
			return "";

		CLW_WarState war = CLW_GameMode.GetWarState();
		CLW_Bases bases = CLW_GameMode.GetBases();
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		string dyn = "";
		string objs = "";
		if (points)
		{
			for (int i = 0; i < points.Count(); i++)
			{
				CLW_ControlPoint point = points.Get(i);
				if (!point)
					continue;

				string owner = "none";
				if (!point.m_sOwner.IsEmpty())
					owner = point.m_sOwner;
				int contested;
				if (point.m_bContested)
					contested = 1;
				else
					contested = 0;

				if (!objs.IsEmpty())
					objs += ",";
				objs += "[\"" + CLW_Situation.Escape(point.m_sId) + "\",\"" + CLW_Situation.Escape(point.m_sName) + "\",\"" + CellOf(point.m_vPos) + "\",\"" + CLW_Situation.Escape(owner) + "\"," + contested + "]";
			}
		}

		string hqText = "";
		if (bases && war)
		{
			hqText = ",\"hq\":{\"own\":\"" + CellOf(bases.GetBasePos(side)) + "\",\"enemy\":\"" + CellOf(bases.GetBasePos(war.GetEnemyKey(side))) + "\"}";
		}

		string frontText = "";
		CLW_ControlPoint front = CLW_FobSites.FrontObjective(side);
		if (front)
			frontText = ",\"front\":\"" + CLW_Situation.Escape(front.m_sId) + "\",\"frontCell\":\"" + CellOf(front.m_vPos) + "\"";

		string fobCells = "";
		array<CLW_Fob> fobs = {};
		CLW_FobManager.GetFobs(side, fobs);
		foreach (CLW_Fob fob : fobs)
		{
			if (!fob || !(fob.IsLive() || fob.IsInProgress()))
				continue;

			if (!fobCells.IsEmpty())
				fobCells += ",";
			fobCells += "\"" + CellOf(fob.m_vPos) + "\"";
		}

		dyn = ",\"objs\":[" + objs + "]" + hqText + frontText + ",\"fobs\":[" + fobCells + "],\"contacts\":[" + ContactCells(side) + "]}";
		string json = CLW_Str.DropLast(s_sStaticJson) + dyn;
		LogSize("macro", side, json.Length());
		return json;
	}

	// Known enemy contacts aggregated by macro cell: [cell, count, "inf"|"veh"|"mix"].
	protected static string ContactCells(string side)
	{
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (!commander || !commander.GetRules() || !commander.GetRules().GetBook())
			return "";

		array<CLW_Contact> contacts = {};
		commander.GetRules().GetBook().GetContacts(contacts);
		array<string> cells = {};
		array<int> counts = {};
		array<int> vehicles = {};
		array<int> infantry = {};
		foreach (CLW_Contact contact : contacts)
		{
			string cell = CellOf(contact.m_vPos);
			if (cell.IsEmpty())
				continue;

			int at = cells.Find(cell);
			if (at < 0)
			{
				at = cells.Count();
				cells.Insert(cell);
				counts.Insert(0);
				vehicles.Insert(0);
				infantry.Insert(0);
			}

			counts[at] = counts[at] + 1;
			vehicles[at] = vehicles[at] + contact.m_iVehicles;
			infantry[at] = infantry[at] + contact.m_iInfantry;
		}

		string text = "";
		foreach (int n, string cellId : cells)
		{
			if (n >= MAX_CONTACT_CELLS)
				break;

			string kind = "inf";
			if (vehicles[n] > 0 && infantry[n] > 0)
				kind = "mix";
			else if (vehicles[n] > 0)
				kind = "veh";

			if (!text.IsEmpty())
				text += ",";
			text += "[\"" + cellId + "\"," + counts[n] + ",\"" + kind + "\"]";
		}

		return text;
	}

	protected static void LogSize(string what, string side, int chars)
	{
		string key = what + side;
		float now = CLW_Orders.Now();
		int lastChars;
		float lastAt = -1000;
		s_mLogChars.Find(key, lastChars);
		s_mLogAt.Find(key, lastAt);
		if (now < lastAt)
			lastAt = -1000;

		bool changed = lastChars <= 0 || Math.AbsFloat(chars - lastChars) > lastChars * 0.15;
		if (!changed && now - lastAt < LOG_GAP_SEC)
			return;

		s_mLogChars.Set(key, chars);
		s_mLogAt.Set(key, now);
		Print(string.Format("CLW: fob map %1 %2: %3 chars (~%4 tokens)", side, what, chars, chars / 4), LogLevel.NORMAL);
	}

	// ---- zoom digest (POST /fobzoom) ----

	// Sub-cell id for a position inside the area, "" when outside.
	protected static string SubOf(string area, vector pos)
	{
		float x0;
		float zTop;
		float size;
		if (!AreaBox(area, x0, zTop, size))
			return "";

		int n = ZoomN();
		float step = size / n;
		int sc = Math.Floor((pos[0] - x0) / step);
		int sr = Math.Floor((zTop - pos[2]) / step);
		if (sc < 0 || sc >= n || sr < 0 || sr >= n)
			return "";

		return SubId(sc, sr);
	}

	// The macro area at 100 m resolution for one side. "" with why when the area is invalid. Logs its size.
	static string ZoomJson(string side, string area, out string why)
	{
		float x0;
		float zTop;
		float size;
		if (!AreaBox(area, x0, zTop, size))
		{
			why = "unknown area '" + area + "'";
			return "";
		}

		string areaId = area;
		areaId.ToUpper();
		int startMs = System.GetTickCount();
		BaseWorld world = GetGame().GetWorld();
		int n = ZoomN();
		float step = size / n;
		int total = n * n;

		// rule keep-out: the same lists the placement validation uses
		array<vector> avoidPos = {};
		array<float> avoidDist = {};
		CLW_FobPlacer.BuildAvoid(side, avoidPos, avoidDist, true);

		// buildings per sub-cell: tiles of ZOOM_TILE_M, origin-inside-tile ownership
		CLW_BuildingBins bins = new CLW_BuildingBins();
		bins.m_fX0 = x0;
		bins.m_fZTop = zTop;
		bins.m_fSub = step;
		bins.m_iN = n;
		for (int bi = 0; bi < total; bi++)
		{
			bins.m_aBins.Insert(0);
		}

		for (float tz = 0; tz < size; tz += ZOOM_TILE_M)
		{
			for (float tx = 0; tx < size; tx += ZOOM_TILE_M)
			{
				bins.m_fTileX0 = x0 + tx;
				bins.m_fTileX1 = x0 + Math.Min(tx + ZOOM_TILE_M, size);
				bins.m_fTileZ1 = zTop - tz;
				bins.m_fTileZ0 = zTop - Math.Min(tz + ZOOM_TILE_M, size);
				world.QueryEntitiesByAABB(Vector(bins.m_fTileX0, -10000, bins.m_fTileZ0), Vector(bins.m_fTileX1, 10000, bins.m_fTileZ1), bins.OnEntity, null, EQueryEntitiesFlags.STATIC);
			}
		}

		// terrain per sub-cell
		array<float> heights = {};
		array<string> terrain = {};
		array<string> roadGlyphs = {};
		float lowH = 1000000;
		float highH = -1000000;
		for (int sr = 0; sr < n; sr++)
		{
			for (int sc = 0; sc < n; sc++)
			{
				vector center = Vector(x0 + (sc + 0.5) * step, 0, zTop - (sr + 0.5) * step);
				CLW_Placement.SurfaceAt(center);
				heights.Insert(center[1]);
				if (center[1] < lowH)
					lowH = center[1];
				if (center[1] > highH)
					highH = center[1];

				string glyph = ".";
				string blocked = "";
				bool keepOut;
				keepOut = CLW_FobPlacer.KeepOutIn(side, center, avoidPos, avoidDist, blocked);
				int buildings = bins.m_aBins[sr * n + sc];
				if (CLW_Placement.IsWater(center))
					glyph = "~";
				else if (keepOut)
					glyph = "x";
				else if (!CLW_Placement.SlopeOk(center))
					glyph = "^";
				else if (buildings >= ZOOM_TOWN_BUILDINGS)
					glyph = "T";
				else if (buildings > 0)
					glyph = "b";
				else
				{
					int trees = CountTreesAround(center[0] - step * 0.25, center[2] - step * 0.25, step * 0.3);
					trees += CountTreesAround(center[0] + step * 0.25, center[2] - step * 0.25, step * 0.3);
					trees += CountTreesAround(center[0] - step * 0.25, center[2] + step * 0.25, step * 0.3);
					trees += CountTreesAround(center[0] + step * 0.25, center[2] + step * 0.25, step * 0.3);
					if (trees >= ZOOM_FOREST_TREES)
						glyph = "f";
				}

				terrain.Insert(glyph);
				roadGlyphs.Insert("_");
			}
		}

		// roads inside the area, resampled every 25 m
		RoadNetworkManager mgr = CLW_Placement.GetRoadManager();
		if (mgr)
		{
			array<BaseRoad> roads = {};
			mgr.GetRoadsInAABB(Vector(x0, -10000, zTop - size), Vector(x0 + size, 10000, zTop), roads);
			foreach (BaseRoad road : roads)
			{
				if (!road || road.GetWidth() < MIN_ROAD_WIDTH_M)
					continue;

				string roadGlyph = "-";
				if (road.GetWidth() >= MAIN_ROAD_WIDTH_M)
					roadGlyph = "=";

				array<vector> pts = {};
				road.GetPoints(pts);
				for (int pi = 0; pi < pts.Count() - 1; pi++)
				{
					float dx = pts[pi + 1][0] - pts[pi][0];
					float dz = pts[pi + 1][2] - pts[pi][2];
					float len = Math.Sqrt(dx * dx + dz * dz);
					if (len < 0.5)
						continue;

					for (float d = 0; d <= len; d += 25)
					{
						float rx = pts[pi][0] + dx * d / len;
						float rz = pts[pi][2] + dz * d / len;
						int rc = Math.Floor((rx - x0) / step);
						int rr = Math.Floor((zTop - rz) / step);
						if (rc < 0 || rc >= n || rr < 0 || rr >= n)
							continue;

						int ri = rr * n + rc;
						if (roadGlyphs[ri] != "=")
							roadGlyphs[ri] = roadGlyph;
					}
				}
			}
		}

		// rows: elevation digit + terrain + road per sub-cell
		float range = Math.Max(highH - lowH, 1);
		string colsText;
		for (int cc = 0; cc < n; cc++)
		{
			colsText += SUB_LETTERS.Substring(cc, 1);
		}

		string rowsJson;
		for (int rowIndex = 0; rowIndex < n; rowIndex++)
		{
			string line;
			line = "";
			for (int colIndex = 0; colIndex < n; colIndex++)
			{
				int idx = rowIndex * n + colIndex;
				int digit = Math.Round((heights[idx] - lowH) / range * 9);
				digit = Math.Clamp(digit, 0, 9);
				line += digit.ToString() + terrain[idx] + roadGlyphs[idx];
			}

			if (rowIndex > 0)
				rowsJson += ",";
			rowsJson += "\"" + line + "\"";
		}

		string legend = "sub-cell id = column letter + row number, row 1 = north, column a = west, 100 m each. Each sub-cell is 3 glyphs: elevation digit (0 lowest .. 9 highest of this area), terrain, road. terrain: ~ water, x keep-out (game rule: too close to an HQ, an objective, another FOB or an enemy contact, or on the enemy half: cannot be used), ^ too steep, T many buildings, b a few buildings, f forest, . open. road: = main, - minor, _ none.";
		string json = "{\"area\":\"" + areaId + "\",\"x0\":" + Math.Round(x0) + ",\"zTop\":" + Math.Round(zTop) + ",\"sub\":" + Math.Round(step) + ",\"cols\":\"" + colsText + "\",\"n\":" + n;
		json += ",\"elev\":[" + Math.Round(lowH) + "," + Math.Round(highH) + "],\"legend\":\"" + legend + "\",\"rows\":[" + rowsJson + "]";
		json += ZoomLists(side, areaId, x0, zTop, size);
		json += "}";
		Print(string.Format("CLW: fob map %1 zoom %2: %3 sub-cells of %4 m, %5 chars (~%6 tokens), %7 ms", side, areaId, total, Math.Round(step), json.Length(), json.Length() / 4, System.GetTickCount() - startMs), LogLevel.NORMAL);
		return json;
	}

	// POIs, objectives (area + 1 km margin), known contacts (area + 500 m), own bases and the `toward` point of a zoom digest (string starts with a comma).
	protected static string ZoomLists(string side, string area, float x0, float zTop, float size)
	{
		float margin = 1000;
		string text = "";

		string pois = "";
		int poiCount;
		array<ref CLW_Place> places = {};
		CLW_Places.GetAll(places);
		foreach (CLW_Place place : places)
		{
			if (poiCount >= 12 || PlaceKind(place.m_sType).IsEmpty())
				continue;

			string sub = SubOf(area, place.m_vPos);
			if (sub.IsEmpty())
				continue;

			if (poiCount > 0)
				pois += ",";
			pois += "[\"" + CLW_Situation.Escape(place.m_sName) + "\",\"" + place.m_sType + "\",\"" + sub + "\"]";
			poiCount++;
		}

		text += ",\"pois\":[" + pois + "]";

		string objs = "";
		CLW_ControlPoints points = CLW_GameMode.GetControlPoints();
		if (points)
		{
			for (int i = 0; i < points.Count(); i++)
			{
				CLW_ControlPoint point = points.Get(i);
				if (!point)
					continue;

				if (point.m_vPos[0] < x0 - margin || point.m_vPos[0] > x0 + size + margin || point.m_vPos[2] > zTop + margin || point.m_vPos[2] < zTop - size - margin)
					continue;

				string owner = "none";
				if (!point.m_sOwner.IsEmpty())
					owner = point.m_sOwner;
				string whereSub = SubOf(area, point.m_vPos);
				if (whereSub.IsEmpty())
					whereSub = "outside";
				if (!objs.IsEmpty())
					objs += ",";
				objs += "[\"" + CLW_Situation.Escape(point.m_sId) + "\",\"" + CLW_Situation.Escape(point.m_sName) + "\",\"" + whereSub + "\"," + Math.Round(point.m_vPos[0]) + "," + Math.Round(point.m_vPos[2]) + "," + Math.Round(point.m_fRadius) + ",\"" + CLW_Situation.Escape(owner) + "\"]";
			}
		}

		text += ",\"objs\":[" + objs + "]";

		string contactText = "";
		CLW_RemoteCommander commander = CLW_GameMode.GetCommander(side);
		if (commander && commander.GetRules() && commander.GetRules().GetBook())
		{
			array<CLW_Contact> contacts = {};
			commander.GetRules().GetBook().GetContacts(contacts);
			float now = CLW_Orders.Now();
			int shown;
			foreach (CLW_Contact contact : contacts)
			{
				if (shown >= 12)
					break;

				if (contact.m_vPos[0] < x0 - 500 || contact.m_vPos[0] > x0 + size + 500 || contact.m_vPos[2] > zTop + 500 || contact.m_vPos[2] < zTop - size - 500)
					continue;

				string contactSub = SubOf(area, contact.m_vPos);
				if (contactSub.IsEmpty())
					contactSub = "outside";
				if (!contactText.IsEmpty())
					contactText += ",";
				contactText += "[\"" + contactSub + "\",\"" + contact.m_sKind + "\"," + Math.Max(contact.m_iCount, 1) + "," + Math.Round(Math.Max(0, now - contact.m_fLastSeen)) + "," + Math.Round(contact.m_vPos[0]) + "," + Math.Round(contact.m_vPos[2]) + "]";
				shown++;
			}
		}

		text += ",\"contacts\":[" + contactText + "]";

		string own = "";
		CLW_Bases bases = CLW_GameMode.GetBases();
		if (bases)
		{
			vector hq = bases.GetBasePos(side);
			string hqSub = SubOf(area, hq);
			if (!hqSub.IsEmpty())
				own += "[\"HQ\",\"" + hqSub + "\"]";
		}

		array<CLW_Fob> fobs = {};
		CLW_FobManager.GetFobs(side, fobs);
		foreach (CLW_Fob fob : fobs)
		{
			if (!fob || !(fob.IsLive() || fob.IsInProgress()))
				continue;

			string fobSub = SubOf(area, fob.m_vPos);
			if (fobSub.IsEmpty())
				continue;

			if (!own.IsEmpty())
				own += ",";
			own += "[\"" + CLW_Situation.Escape(fob.m_sId) + "\",\"" + fobSub + "\"]";
		}

		text += ",\"own\":[" + own + "]";

		CLW_ControlPoint front = CLW_FobSites.FrontObjective(side);
		if (front)
			text += ",\"toward\":[" + Math.Round(front.m_vPos[0]) + "," + Math.Round(front.m_vPos[2]) + "],\"towardId\":\"" + CLW_Situation.Escape(front.m_sId) + "\"";

		return text;
	}
}

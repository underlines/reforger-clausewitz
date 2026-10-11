// World info for the dashboard map (server only). Started from CLW_GameMode.OnGameStart.
//   POST /world            {worldFile, mapPrefab, mapOffset, mapSize, boundsMin, boundsMax}, every 60 s (covers sidecar restarts).
//                          Reply {ok, worldKey, needRoads, needLocations, needBuildings}: true while the sidecar has no cached vector data.
//   POST /world/roads      {worldKey, chunk, total (-1 until last), last, roads:[{w, p:[x,z,...]}]}, <= ~24 KB per body.
//   POST /world/locations  {worldKey, items:[{name, type, kind, faction, pos:[x,z]}]} from the MapEntity descriptors plus the
//                          vanilla SCR_MilitaryBaseSystem bases. kind = base|airport|port|town|village|hill|other.
//   POST /world/buildings  {worldKey, chunk, total, last, b:[[cx,cz,w,d,yawDeg,cat],...]}, cat 0 small, 1 large, 2 military/industrial.
//                          w = local X extent, d = local Z extent (m, scaled), yawDeg = entity yaw (Enforce heading: 0 = +Z/north, 90 = +X/east).
// Roads come from the AI world's RoadNetworkManager, queried per ~1 km tile (one tile per call, spread over frames).
// Buildings come from BaseWorld.QueryEntitiesByAABB per 512 m tile (one tile per call): Building-class entities (or prefab path
// ".../buildings/") with a footprint >= 12 m2 and longest side >= 3 m, fences/walls excluded; an entity belongs to the tile holding its origin.
//
// The map IMAGE is not handled here: the sidecar reads it at runtime from the user's own game install (never committed or
// redistributed; Bohemia content is APL-SA). For modded terrains put dev/tools/maps/<World>.png plus <World>.json
// ({"x0":..,"z0":..,"x1":..,"z1":..,"flipY":false}) next to the sidecar, e.g. exported with the World Editor
// "Export Map Data" tool (Rasterization, see dev/research/dashboard-map-imagery.md). Roads and names work without an image.

class CLW_WorldReply
{
	bool ok;
	string worldKey;
	bool needRoads;
	bool needLocations;
	bool needBuildings;
}

class CLW_WorldCallback : RestCallback
{
	protected CLW_WorldInfo m_Owner;	// weak: the owner holds this callback
	protected int m_iKind;
	bool m_bDone;

	void CLW_WorldCallback(CLW_WorldInfo owner, int kind)
	{
		m_Owner = owner;
		m_iKind = kind;
	}

	override void OnSuccess(string data, int dataSize)
	{
		m_bDone = true;
		if (m_Owner)
			m_Owner.OnReply(m_iKind, true, data, "");
	}

	override void OnError(int errorCode)
	{
		m_bDone = true;
		if (m_Owner)
			m_Owner.OnReply(m_iKind, false, "", string.Format("error code=%1 http=%2", errorCode, GetHttpCode()));
	}

	override void OnTimeout()
	{
		m_bDone = true;
		if (m_Owner)
			m_Owner.OnReply(m_iKind, false, "", "timeout");
	}
}

class CLW_WorldInfo
{
	static const string BASE_URL = "http://127.0.0.1:8788/";
	static const int KIND_WORLD = 0;
	static const int KIND_ROADS = 1;
	static const int KIND_LOCATIONS = 2;
	static const int KIND_BUILDINGS = 3;
	static const float BTILE_SIZE = 512;		// m, building query tile
	static const int MAX_BTILES_PER_AXIS = 40;
	static const int MAX_BUILDINGS = 60000;
	static const float MIN_BUILDING_AREA = 12;	// m2
	static const float MIN_BUILDING_SIDE = 3;	// m, longest side
	static const float MAX_BUILDING_SIDE = 150;	// m, larger is not a building footprint
	static const int RESEND_MS = 60000;
	static const float TILE_SIZE = 1024;		// m, road query tile
	static const int MAX_TILES_PER_AXIS = 20;
	static const int TILE_DELAY_MS = 40;
	static const int CHUNK_BYTES = 6000;	// string.Format truncates its result at ~8 KB: keep bodies below that and concatenate
	static const float MIN_POINT_DIST = 10;		// m, points closer than this to the previous kept one are dropped
	static const int MAX_LOCATIONS = 400;

	protected static ref CLW_WorldInfo s_Instance;

	protected ref array<ref CLW_WorldCallback> m_aCallbacks = {};	// must stay referenced until the reply arrives
	protected string m_sWorldKey;
	protected vector m_vMin;
	protected vector m_vMax;

	// road collection state
	protected bool m_bRoadsBusy;
	protected bool m_bLocationsBusy;
	protected RoadNetworkManager m_RoadMgr;
	protected ref array<BaseRoad> m_aSeen = {};
	protected float m_fTileSize;
	protected int m_iTilesX;
	protected int m_iTilesZ;
	protected int m_iTile;
	protected string m_sBuf;
	protected int m_iBufRoads;
	protected int m_iChunk;
	protected int m_iRoadsTotal;

	// building collection state
	protected bool m_bBuildingsBusy;
	protected BaseWorld m_BldWorld;
	protected float m_fBTile;
	protected int m_iBTilesX;
	protected int m_iBTilesZ;
	protected int m_iBTile;
	protected vector m_vQMin;
	protected vector m_vQMax;
	protected string m_sBBuf;
	protected int m_iBBufCount;
	protected int m_iBChunk;
	protected int m_iBTotal;
	protected int m_iBCat1;
	protected int m_iBCat2;

	// Entry point (CallLater from CLW_GameMode.OnGameStart).
	static void Start()
	{
		if (!Replication.IsServer() || s_Instance)
			return;
		s_Instance = new CLW_WorldInfo();
		s_Instance.SendWorld();
		GetGame().GetCallqueue().CallLater(s_Instance.SendWorld, RESEND_MS, true);
	}

	// The world's MapEntity, or null when the world has none.
	protected MapEntity FindMapEntity()
	{
		return SCR_MapEntity.GetMapInstance();
	}

	// "{GUID}Prefabs/x.et" -> "Prefabs/x.et"
	protected string StripGuid(string name)
	{
		int close = name.IndexOf("}");
		if (close < 0)
			return name;
		return name.Substring(close + 1, name.Length() - close - 1);
	}

	protected string Num(float v)
	{
		int i = Math.Round(v);
		return i.ToString();
	}

	void SendWorld()
	{
		BaseWorld world = GetGame().GetWorld();
		if (!world)
			return;

		world.GetBoundBox(m_vMin, m_vMax);

		string mapPrefab = "";
		string mapOffset = "[0,0]";
		string mapSize = "[0,0]";
		MapEntity mapEnt = FindMapEntity();
		if (mapEnt)
		{
			EntityPrefabData data = mapEnt.GetPrefabData();
			if (data)
				mapPrefab = StripGuid(data.GetPrefabName());
			vector offset = mapEnt.Offset();
			vector size = mapEnt.Size();
			mapOffset = "[" + Num(offset[0]) + "," + Num(offset[2]) + "]";
			mapSize = "[" + Num(size[0]) + "," + Num(size[2]) + "]";
		}

		string body = string.Format("{\"worldFile\":\"%1\",\"mapPrefab\":\"%2\",\"mapOffset\":%3,\"mapSize\":%4,", CLW_RuleCommander.JsonEscape(GetGame().GetWorldFile()), CLW_RuleCommander.JsonEscape(mapPrefab), mapOffset, mapSize);
		body += string.Format("\"boundsMin\":[%1,%2,%3],\"boundsMax\":[%4,%5,%6]}", Num(m_vMin[0]), Num(m_vMin[1]), Num(m_vMin[2]), Num(m_vMax[0]), Num(m_vMax[1]), Num(m_vMax[2]));
		Post("world", body, KIND_WORLD);
	}

	protected void Post(string path, string body, int kind)
	{
		for (int i = m_aCallbacks.Count() - 1; i >= 0; i--)
		{
			if (m_aCallbacks[i].m_bDone)
				m_aCallbacks.Remove(i);
		}

		RestContext ctx = GetGame().GetRestApi().GetContext(BASE_URL);
		if (!ctx)
			return;
		ctx.SetTimeout(10);
		ctx.SetHeaders("Content-Type,application/json");
		CLW_WorldCallback cb = new CLW_WorldCallback(this, kind);
		m_aCallbacks.Insert(cb);
		if (ctx.POST(cb, path, body) < 0)
			cb.m_bDone = true;
	}

	// Called by the REST callbacks (main thread).
	void OnReply(int kind, bool ok, string data, string failure)
	{
		if (!ok)
		{
			if (kind != KIND_WORLD)
				Print(string.Format("CLW: world data POST failed (kind %1): %2", kind, failure), LogLevel.WARNING);
			return;
		}

		if (kind != KIND_WORLD)
			return;

		SCR_JsonLoadContext loader = new SCR_JsonLoadContext();
		CLW_WorldReply reply = new CLW_WorldReply();
		if (!loader.ImportFromString(data) || !loader.ReadValue("", reply) || !reply.ok)
			return;

		m_sWorldKey = reply.worldKey;
		if (reply.needLocations && !m_bLocationsBusy)
		{
			m_bLocationsBusy = true;
			SendLocations();
		}
		if (reply.needRoads && !m_bRoadsBusy)
			StartRoads();
		if (reply.needBuildings && !m_bBuildingsBusy)
			StartBuildings();
	}

	// ---------- locations ----------

	// Map descriptor type key (e.g. "name_village", "airport") -> dashboard symbol kind.
	protected string KindOf(string typeKey)
	{
		if (typeKey.Contains("airport"))
			return "airport";
		if (typeKey.Contains("port"))
			return "port";
		if (typeKey.Contains("base"))
			return "base";
		if (typeKey.Contains("village") || typeKey.Contains("settlement"))
			return "village";
		if (typeKey.Contains("city") || typeKey.Contains("town"))
			return "town";
		if (typeKey.Contains("hill"))
			return "hill";
		return "other";
	}

	protected void SendLocations()
	{
		MapEntity mapEnt = FindMapEntity();
		if (!mapEnt)
		{
			Print("CLW: world locations skipped: no MapEntity in this world", LogLevel.WARNING);
			m_bLocationsBusy = false;
			return;
		}

		string items = "";
		int count = 0;
		array<string> seen = {};
		array<vector> basePos = {};
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
				if (!item || count >= MAX_LOCATIONS)
					continue;
				string name = WidgetManager.Translate(item.GetDisplayName());
				if (name.IsEmpty())
					continue;
				vector pos = item.GetPos();
				string key = name + Num(pos[0]) + "," + Num(pos[2]);
				if (seen.Contains(key))
					continue;
				seen.Insert(key);
				if (count > 0)
					items += ",";
				string kindName = KindOf(typeKey);
				items += string.Format("{\"name\":\"%1\",\"type\":\"%2\",\"kind\":\"%3\",\"pos\":[%4,%5]}", CLW_RuleCommander.JsonEscape(name), typeKey, kindName, Num(pos[0]), Num(pos[2]));
				if (kindName == "base")
					basePos.Insert(pos);
				count++;
			}
		}

		// vanilla military bases (Conflict etc.), skipping those already present as a MapItem base
		SCR_MilitaryBaseSystem baseSys = SCR_MilitaryBaseSystem.GetInstance();
		if (baseSys)
		{
			array<SCR_MilitaryBaseComponent> milBases = {};
			baseSys.GetBases(milBases);
			int added = 0;
			foreach (SCR_MilitaryBaseComponent milBase : milBases)
			{
				if (!milBase || count >= MAX_LOCATIONS)
					continue;
				IEntity baseOwner = milBase.GetOwner();
				if (!baseOwner)
					continue;
				vector basePosNow = baseOwner.GetOrigin();
				bool dupBase = false;
				foreach (vector known : basePos)
				{
					float ddx = known[0] - basePosNow[0];
					float ddz = known[2] - basePosNow[2];
					if (ddx * ddx + ddz * ddz < 150 * 150)
					{
						dupBase = true;
						break;
					}
				}
				if (dupBase)
					continue;
				string baseName = milBase.GetCallsignDisplayNameOnly();
				if (!baseName.IsEmpty())
					baseName = WidgetManager.Translate(baseName);
				if (baseName.IsEmpty())
					baseName = "Base";
				string factionKey = "";
				Faction baseFaction = milBase.GetFaction();
				if (baseFaction)
					factionKey = baseFaction.GetFactionKey();
				if (count > 0)
					items += ",";
				items += "{\"name\":\"" + CLW_RuleCommander.JsonEscape(baseName) + "\",\"type\":\"militarybase\",\"kind\":\"base\",\"faction\":\"" + CLW_RuleCommander.JsonEscape(factionKey) + "\",\"pos\":[" + Num(basePosNow[0]) + "," + Num(basePosNow[2]) + "]}";
				basePos.Insert(basePosNow);
				count++;
				added++;
			}
			Print(string.Format("CLW: world military bases total=%1 added=%2", milBases.Count(), added), LogLevel.NORMAL);
		}

		Print(string.Format("CLW: world locations found n=%1 (types 0..%2)", count, EMapDescriptorType.MDT_COUNT), LogLevel.NORMAL);
		Post("world/locations", "{\"worldKey\":\"" + m_sWorldKey + "\",\"items\":[" + items + "]}", KIND_LOCATIONS);
		m_bLocationsBusy = false;
	}

	// ---------- roads ----------

	protected void StartRoads()
	{
		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (aiWorld)
			m_RoadMgr = aiWorld.GetRoadNetworkManager();
		if (!m_RoadMgr)
		{
			Print("CLW: world roads skipped: no RoadNetworkManager", LogLevel.WARNING);
			return;
		}

		float spanX = Math.Max(m_vMax[0] - m_vMin[0], 1);
		float spanZ = Math.Max(m_vMax[2] - m_vMin[2], 1);
		m_fTileSize = TILE_SIZE;
		while (spanX / m_fTileSize > MAX_TILES_PER_AXIS || spanZ / m_fTileSize > MAX_TILES_PER_AXIS)
			m_fTileSize *= 2;
		m_iTilesX = Math.Ceil(spanX / m_fTileSize);
		m_iTilesZ = Math.Ceil(spanZ / m_fTileSize);
		m_iTile = 0;
		m_aSeen.Clear();
		m_sBuf = "";
		m_iBufRoads = 0;
		m_iChunk = 0;
		m_iRoadsTotal = 0;
		m_bRoadsBusy = true;
		GetGame().GetCallqueue().CallLater(RoadTile, TILE_DELAY_MS);
	}

	// One tile per call, so collecting never hitches a frame.
	protected void RoadTile()
	{
		int tiles = m_iTilesX * m_iTilesZ;
		if (m_iTile >= tiles)
		{
			FlushRoads(true);
			Print(string.Format("CLW: world roads sent n=%1 chunks=%2", m_iRoadsTotal, m_iChunk), LogLevel.NORMAL);
			m_aSeen.Clear();
			m_bRoadsBusy = false;
			return;
		}

		int tx = m_iTile % m_iTilesX;
		int tz = m_iTile / m_iTilesX;
		m_iTile++;

		vector mn = Vector(m_vMin[0] + tx * m_fTileSize, -10000, m_vMin[2] + tz * m_fTileSize);
		vector mx = Vector(mn[0] + m_fTileSize, 10000, mn[2] + m_fTileSize);
		array<BaseRoad> roads = {};
		m_RoadMgr.GetRoadsInAABB(mn, mx, roads);
		foreach (BaseRoad road : roads)
		{
			if (!road || m_aSeen.Contains(road))
				continue;
			m_aSeen.Insert(road);
			AddRoad(road);
		}

		GetGame().GetCallqueue().CallLater(RoadTile, TILE_DELAY_MS);
	}

	protected void AddRoad(BaseRoad road)
	{
		array<vector> pts = {};
		road.GetPoints(pts);
		if (pts.Count() < 2)
			return;

		// drop points closer than MIN_POINT_DIST to the last kept one; always keep the end point
		string p = "";
		int kept = 0;
		vector last = pts[0];
		p = Num(last[0]) + "," + Num(last[2]);
		kept++;
		int n = pts.Count();
		for (int i = 1; i < n; i++)
		{
			vector pt = pts[i];
			bool isEnd = (i == n - 1);
			float dx = pt[0] - last[0];
			float dz = pt[2] - last[2];
			if (!isEnd && dx * dx + dz * dz < MIN_POINT_DIST * MIN_POINT_DIST)
				continue;
			p += "," + Num(pt[0]) + "," + Num(pt[2]);
			last = pt;
			kept++;
		}
		if (kept < 2)
			return;

		if (m_iBufRoads > 0)
			m_sBuf += ",";
		m_sBuf += "{\"w\":" + Num(road.GetWidth()) + ",\"p\":[" + p + "]}";
		m_iBufRoads++;
		m_iRoadsTotal++;
		if (m_sBuf.Length() >= CHUNK_BYTES)
			FlushRoads(false);
	}

	protected void FlushRoads(bool last)
	{
		int total = -1;
		if (last)
			total = m_iChunk + 1;
		string lastTxt = "false";
		if (last)
			lastTxt = "true";
		string body = "{\"worldKey\":\"" + m_sWorldKey + "\",\"chunk\":" + m_iChunk + ",\"total\":" + total + ",\"last\":" + lastTxt + ",\"roads\":[" + m_sBuf + "]}";
		Post("world/roads", body, KIND_ROADS);
		m_iChunk++;
		m_sBuf = "";
		m_iBufRoads = 0;
	}

	// ---------- buildings ----------

	protected void StartBuildings()
	{
		m_BldWorld = GetGame().GetWorld();
		if (!m_BldWorld)
			return;

		float spanX = Math.Max(m_vMax[0] - m_vMin[0], 1);
		float spanZ = Math.Max(m_vMax[2] - m_vMin[2], 1);
		m_fBTile = BTILE_SIZE;
		while (spanX / m_fBTile > MAX_BTILES_PER_AXIS || spanZ / m_fBTile > MAX_BTILES_PER_AXIS)
			m_fBTile *= 2;
		m_iBTilesX = Math.Ceil(spanX / m_fBTile);
		m_iBTilesZ = Math.Ceil(spanZ / m_fBTile);
		m_iBTile = 0;
		m_sBBuf = "";
		m_iBBufCount = 0;
		m_iBChunk = 0;
		m_iBTotal = 0;
		m_iBCat1 = 0;
		m_iBCat2 = 0;
		m_bBuildingsBusy = true;
		GetGame().GetCallqueue().CallLater(BuildingTile, TILE_DELAY_MS);
	}

	// One tile per call, so collecting never hitches a frame much.
	protected void BuildingTile()
	{
		int tiles = m_iBTilesX * m_iBTilesZ;
		if (m_iBTile >= tiles || m_iBTotal >= MAX_BUILDINGS)
		{
			FlushBuildings(true);
			Print(string.Format("CLW: world buildings sent n=%1 chunks=%2 large=%3 military=%4 tile=%5m", m_iBTotal, m_iBChunk, m_iBCat1, m_iBCat2, m_fBTile), LogLevel.NORMAL);
			m_bBuildingsBusy = false;
			m_BldWorld = null;
			return;
		}

		int tx = m_iBTile % m_iBTilesX;
		int tz = m_iBTile / m_iBTilesX;
		m_iBTile++;

		m_vQMin = Vector(m_vMin[0] + tx * m_fBTile, -10000, m_vMin[2] + tz * m_fBTile);
		m_vQMax = Vector(m_vQMin[0] + m_fBTile, 10000, m_vQMin[2] + m_fBTile);
		m_BldWorld.QueryEntitiesByAABB(m_vQMin, m_vQMax, BuildingQuery, null, EQueryEntitiesFlags.ALL);

		GetGame().GetCallqueue().CallLater(BuildingTile, TILE_DELAY_MS);
	}

	// QueryEntitiesCallback: return true to keep iterating.
	protected bool BuildingQuery(IEntity ent)
	{
		if (!ent || m_iBTotal >= MAX_BUILDINGS)
			return true;
		if (ent.GetParent())
			return true;

		// an entity spanning several tiles is only taken by the tile that holds its origin (= dedupe without storage)
		vector origin = ent.GetOrigin();
		if (origin[0] < m_vQMin[0] || origin[0] >= m_vQMax[0] || origin[2] < m_vQMin[2] || origin[2] >= m_vQMax[2])
			return true;

		vector bmin, bmax;
		ent.GetBounds(bmin, bmax);
		float scl = ent.GetScale();
		float bw = (bmax[0] - bmin[0]) * scl;
		float bd = (bmax[2] - bmin[2]) * scl;
		float area = bw * bd;
		float longest = Math.Max(bw, bd);
		if (area < MIN_BUILDING_AREA || longest < MIN_BUILDING_SIDE || longest > MAX_BUILDING_SIDE)
			return true;

		string prefabName = "";
		EntityPrefabData prefabData = ent.GetPrefabData();
		if (prefabData)
			prefabName = prefabData.GetPrefabName();
		prefabName.ToLower();
		if (prefabName.Contains("fence") || prefabName.Contains("/walls/") || prefabName.Contains("/props/"))
			return true;
		if (!Building.Cast(ent) && !prefabName.Contains("/buildings/"))
			return true;

		int cat = 0;
		if (area > 250)
			cat = 1;
		if (prefabName.Contains("military") || prefabName.Contains("hangar") || prefabName.Contains("barrack") || prefabName.Contains("industrial") || prefabName.Contains("warehouse") || prefabName.Contains("factory"))
			cat = 2;

		// yaw: Enforce heading in degrees (0 = +Z, 90 = +X), rounded to 5 deg
		vector angles = ent.GetYawPitchRoll();
		float yaw = angles[0];
		while (yaw < 0)
			yaw += 360;
		while (yaw >= 360)
			yaw -= 360;
		int yawStep = Math.Round(yaw / 5) * 5;
		if (yawStep >= 360)
			yawStep = 0;

		// bbox centre (local) -> world: local X axis = (cos, -sin), local Z axis = (sin, cos) in (x, z)
		float rad = yaw * Math.DEG2RAD;
		float sn = Math.Sin(rad);
		float cs = Math.Cos(rad);
		float lcx = (bmin[0] + bmax[0]) * 0.5 * scl;
		float lcz = (bmin[2] + bmax[2]) * 0.5 * scl;
		float wx = origin[0] + lcx * cs + lcz * sn;
		float wz = origin[2] - lcx * sn + lcz * cs;

		if (m_iBBufCount > 0)
			m_sBBuf += ",";
		m_sBBuf += "[" + Num(wx) + "," + Num(wz) + "," + Num(bw) + "," + Num(bd) + "," + yawStep + "," + cat + "]";
		m_iBBufCount++;
		m_iBTotal++;
		if (cat == 1)
			m_iBCat1++;
		if (cat == 2)
			m_iBCat2++;
		if (m_sBBuf.Length() >= CHUNK_BYTES)
			FlushBuildings(false);
		return true;
	}

	protected void FlushBuildings(bool last)
	{
		int total = -1;
		string lastTxt = "false";
		if (last)
		{
			total = m_iBChunk + 1;
			lastTxt = "true";
		}
		string body = "{\"worldKey\":\"" + m_sWorldKey + "\",\"chunk\":" + m_iBChunk + ",\"total\":" + total + ",\"last\":" + lastTxt + ",\"b\":[" + m_sBBuf + "]}";
		Post("world/buildings", body, KIND_BUILDINGS);
		m_iBChunk++;
		m_sBBuf = "";
		m_iBBufCount = 0;
	}
}

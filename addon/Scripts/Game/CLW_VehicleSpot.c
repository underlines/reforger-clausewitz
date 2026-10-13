// Robust vehicle spawn spot (server): ONE entry point for every vehicle we spawn (AI purchases, start forces, supply / builder trucks,
// player requests). Nothing here names a prefab, a faction or a coordinate.
//
//   CLW_VehicleSpot.Find(prefab, anchor, side, maxRoadM, pos, yaw, how)   spot for a vehicle near the anchor (HQ, depot, FOB, requester)
//   CLW_VehicleSpot.Watch(vehicle, prefab, side, anchor, maxRoadM)      after the spawn: learns the real size, checks 1 s later, moves once
//
// Pipeline of Find:
//   1. size of the vehicle: real bounds learned from any earlier spawn / probe of the prefab (Learn), else a generous default box
//      (the engine API has no spawn-free mesh bounds); clearance around it CLEAR_M (also keeps 2 m from characters / vehicles).
//   2. candidates: road points within maxRoadM of the anchor (sampled along the road polylines, nearest first, centre line then the
//      edges, heading along the road), then the ring search of CLW_FreeSpot around the anchor, then a relaxed ring ("fallback").
//   3. every pose goes through CLW_FreeSpot.CheckPose (water, terrain range, solid entities by oriented-box query, cylinder traces) plus
//      CLW_VehicleExtra: never inside the footprint (+KEEP_PAD_M) of any placed service / structure (CLW_Services instances: command post,
//      arsenal, supply, depot, terminal, FOB kit), not on a spot reserved by a vehicle spawned in the last RESERVE_SEC seconds,
//      pitch / roll of the terrain <= 15 degrees.
//   4. the chosen spot is reserved; callers spawn at surface Y + LIFT_M.
//
// Watch: VERIFY_DELAY_MS after the spawn the vehicle is checked for tilt > 35 degrees and for solid entities inside its real box; if
// so it is moved ONCE to the next candidate (the bad spot is reserved so it is not picked again). Log prefix: "CLW: vehicle spot".

class CLW_VehicleSize
{
	float m_fHalfX = 1.7;		// default box of an unknown vehicle (half width, half length, height), generous on purpose
	float m_fHalfZ = 4.6;
	float m_fOffsetX;			// bounds centre relative to the entity origin
	float m_fOffsetZ;
	float m_fHeight = 3.2;
	bool m_bKnown;
	string m_sSource = "default";
}

class CLW_VehicleSpotConfig
{
	static const float CLEAR_M = 2.0;				// clearance around the vehicle box (objects, characters, other vehicles)
	static const float KEEP_PAD_M = 3.0;			// extra gap around the footprint of our own services
	static const float LIFT_M = 0.3;				// spawn height above the surface
	static const float MAX_SLOPE_TAN = 0.268;		// tan(15 degrees): pitch / roll of the ground under the box
	static const float RESERVE_SEC = 20;
	static const int VERIFY_DELAY_MS = 1000;
	static const float TILT_MIN_UP = 0.819;			// cos(35 degrees): the up axis of a spawned vehicle must stay above this
	static const float ROAD_STEP_M = 5;
	static const int ROAD_MAX_POINTS = 300;
	static const int ROAD_MAX_EVAL = 60;
	static const float ROAD_MIN_WIDTH_M = 3;
	static const float RING_MIN_M = 6;
	static const float RING_MAX_M = 60;
	static const float MAX_HALF_M = 12;				// learned half extents above this are not trusted
	static const float KEEP_MAX_PART_M = 60;		// hierarchy parts larger than this are terrain-like, ignored
}

// Footprints of our services, reserved spots and ground tilt on top of CLW_FreeSpot.CheckPose.
class CLW_VehicleExtra : CLW_SpotExtra
{
	ref array<float> m_aKeep = {};	// stride 5: centre x, centre z, half x, half z, yaw
	float m_fHalfX;
	float m_fHalfZ;
	float m_fOffsetX;
	float m_fOffsetZ;

	void AddKeep(vector center, float halfX, float halfZ, float yaw)
	{
		m_aKeep.Insert(center[0]);
		m_aKeep.Insert(center[2]);
		m_aKeep.Insert(halfX);
		m_aKeep.Insert(halfZ);
		m_aKeep.Insert(yaw);
	}

	override string Check(vector pos, float yaw)
	{
		vector mat[4];
		Math3D.AnglesToMatrix(Vector(yaw, 0, 0), mat);
		float cx = pos[0] + mat[0][0] * m_fOffsetX + mat[2][0] * m_fOffsetZ;
		float cz = pos[2] + mat[0][2] * m_fOffsetX + mat[2][2] * m_fOffsetZ;
		for (int i = 0; i + 4 < m_aKeep.Count(); i += 5)
		{
			if (CLW_VehicleSpot.RectsOverlap(cx, cz, yaw, m_fHalfX, m_fHalfZ, m_aKeep[i], m_aKeep[i + 1], m_aKeep[i + 4], m_aKeep[i + 2], m_aKeep[i + 3]))
				return "service";
		}

		if (CLW_VehicleSpot.ReservedNear(cx, cz, Math.Sqrt(m_fHalfX * m_fHalfX + m_fHalfZ * m_fHalfZ)))
			return "reserved";

		BaseWorld world = GetGame().GetWorld();
		float yFront = world.GetSurfaceY(cx + mat[2][0] * m_fHalfZ, cz + mat[2][2] * m_fHalfZ);
		float yBack = world.GetSurfaceY(cx - mat[2][0] * m_fHalfZ, cz - mat[2][2] * m_fHalfZ);
		float yRight = world.GetSurfaceY(cx + mat[0][0] * m_fHalfX, cz + mat[0][2] * m_fHalfX);
		float yLeft = world.GetSurfaceY(cx - mat[0][0] * m_fHalfX, cz - mat[0][2] * m_fHalfX);
		if (Math.AbsFloat(yFront - yBack) > 2 * m_fHalfZ * CLW_VehicleSpotConfig.MAX_SLOPE_TAN)
			return "tilt";

		if (Math.AbsFloat(yRight - yLeft) > 2 * m_fHalfX * CLW_VehicleSpotConfig.MAX_SLOPE_TAN)
			return "tilt";

		return "";
	}
}

class CLW_VehicleSpot
{
	protected static ref map<string, ref CLW_VehicleSize> s_mSizes = new map<string, ref CLW_VehicleSize>();
	protected static ref array<float> s_aResX = {};
	protected static ref array<float> s_aResZ = {};
	protected static ref array<float> s_aResR = {};
	protected static ref array<float> s_aResUntil = {};

	static void Reset()
	{
		s_mSizes.Clear();
		s_aResX.Clear();
		s_aResZ.Clear();
		s_aResR.Clear();
		s_aResUntil.Clear();
	}

	// ---- geometry ----

	// Oriented rectangles A and B (centre, yaw degrees, half extents) overlap in the XZ plane (separating axis test).
	static bool RectsOverlap(float ax, float az, float ayaw, float ahx, float ahz, float bx, float bz, float byaw, float bhx, float bhz)
	{
		vector ma[4];
		vector mb[4];
		Math3D.AnglesToMatrix(Vector(ayaw, 0, 0), ma);
		Math3D.AnglesToMatrix(Vector(byaw, 0, 0), mb);
		float dx = bx - ax;
		float dz = bz - az;
		for (int k = 0; k < 4; k++)
		{
			float ux;
			float uz;
			if (k == 0)
			{
				ux = ma[0][0];
				uz = ma[0][2];
			}
			else if (k == 1)
			{
				ux = ma[2][0];
				uz = ma[2][2];
			}
			else if (k == 2)
			{
				ux = mb[0][0];
				uz = mb[0][2];
			}
			else
			{
				ux = mb[2][0];
				uz = mb[2][2];
			}

			float ra = ahx * Math.AbsFloat(ma[0][0] * ux + ma[0][2] * uz) + ahz * Math.AbsFloat(ma[2][0] * ux + ma[2][2] * uz);
			float rb = bhx * Math.AbsFloat(mb[0][0] * ux + mb[0][2] * uz) + bhz * Math.AbsFloat(mb[2][0] * ux + mb[2][2] * uz);
			if (Math.AbsFloat(dx * ux + dz * uz) > ra + rb)
				return false;
		}

		return true;
	}

	// ---- size ----

	static CLW_VehicleSize SizeOf(ResourceName prefab)
	{
		CLW_VehicleSize size;
		string key = prefab.GetPath();
		if (!key.IsEmpty() && s_mSizes.Find(key, size) && size)
			return size;

		return new CLW_VehicleSize();
	}

	// Remembers the real bounds of a spawned vehicle for every later spawn of the same prefab.
	static void Learn(ResourceName prefab, IEntity vehicle)
	{
		string key = prefab.GetPath();
		if (key.IsEmpty() || !vehicle)
			return;

		vector mins, maxs;
		vehicle.GetBounds(mins, maxs);
		float halfX = (maxs[0] - mins[0]) * 0.5;
		float halfZ = (maxs[2] - mins[2]) * 0.5;
		if (halfX < 0.5 || halfZ < 0.5 || halfX > CLW_VehicleSpotConfig.MAX_HALF_M || halfZ > CLW_VehicleSpotConfig.MAX_HALF_M)
			return;

		CLW_VehicleSize size = new CLW_VehicleSize();
		size.m_fHalfX = halfX;
		size.m_fHalfZ = halfZ;
		size.m_fOffsetX = (maxs[0] + mins[0]) * 0.5;
		size.m_fOffsetZ = (maxs[2] + mins[2]) * 0.5;
		size.m_fHeight = Math.Clamp(maxs[1] - mins[1], 1.5, 6);
		size.m_bKnown = true;
		size.m_sSource = "bounds";
		s_mSizes.Set(key, size);
	}

	// ---- reservations ----

	static void Reserve(float x, float z, float radius, float seconds)
	{
		s_aResX.Insert(x);
		s_aResZ.Insert(z);
		s_aResR.Insert(radius);
		s_aResUntil.Insert(GetGame().GetWorld().GetWorldTime() + seconds * 1000);
	}

	static bool ReservedNear(float x, float z, float radius)
	{
		float now = GetGame().GetWorld().GetWorldTime();
		for (int i = s_aResX.Count() - 1; i >= 0; i--)
		{
			if (s_aResUntil[i] < now)
			{
				s_aResX.Remove(i);
				s_aResZ.Remove(i);
				s_aResR.Remove(i);
				s_aResUntil.Remove(i);
				continue;
			}

			float dx = x - s_aResX[i];
			float dz = z - s_aResZ[i];
			float need = (radius + s_aResR[i]) * 0.8;
			if (dx * dx + dz * dz < need * need)
				return true;
		}

		return false;
	}

	// ---- keep-out footprints ----

	protected static void CollectKeepouts(CLW_VehicleExtra extra)
	{
		array<CLW_ServiceInstance> list = {};
		CLW_Services.GetAllInstances(list);
		float pad = CLW_VehicleSpotConfig.KEEP_PAD_M;
		foreach (CLW_ServiceInstance inst : list)
		{
			if (inst.m_fHalfX > 0 && inst.m_fHalfZ > 0)
			{
				vector rectCenter = inst.m_vRectCenter;
				if (rectCenter == vector.Zero)
					rectCenter = inst.m_vPos;

				extra.AddKeep(rectCenter, inst.m_fHalfX + pad, inst.m_fHalfZ + pad, inst.m_fYaw);
			}
			else
			{
				float rad = Math.Max(inst.m_fRadius, 2);
				extra.AddKeep(inst.m_vPos, rad + pad, rad + pad, 0);
			}

			// the real geometry of the whole composition (children included)
			array<IEntity> tree = {};
			CLW_Services.CollectHierarchy(inst.m_Entity, tree);
			float loX = 1000000;
			float loZ = 1000000;
			float hiX = -1000000;
			float hiZ = -1000000;
			bool any;
			foreach (IEntity part : tree)
			{
				if (!part || (!part.GetVObject() && !part.GetPhysics()))
					continue;

				vector mn, mx;
				part.GetWorldBounds(mn, mx);
				if (mx[0] - mn[0] > CLW_VehicleSpotConfig.KEEP_MAX_PART_M || mx[2] - mn[2] > CLW_VehicleSpotConfig.KEEP_MAX_PART_M)
					continue;

				loX = Math.Min(loX, mn[0]);
				loZ = Math.Min(loZ, mn[2]);
				hiX = Math.Max(hiX, mx[0]);
				hiZ = Math.Max(hiZ, mx[2]);
				any = true;
			}

			if (any)
				extra.AddKeep(Vector((loX + hiX) * 0.5, 0, (loZ + hiZ) * 0.5), (hiX - loX) * 0.5 + pad, (hiZ - loZ) * 0.5 + pad, 0);
		}
	}

	protected static CLW_SpotParams MakeParams(string label, vector anchor, CLW_VehicleSize size, CLW_VehicleExtra extra, IEntity ignore)
	{
		CLW_SpotParams p = CLW_FreeSpot.VehicleParams(label, anchor);
		p.m_fHalfX = size.m_fHalfX;
		p.m_fHalfZ = size.m_fHalfZ;
		p.m_fOffsetX = size.m_fOffsetX;
		p.m_fOffsetZ = size.m_fOffsetZ;
		p.m_fHeight = size.m_fHeight;
		p.m_fMargin = CLW_VehicleSpotConfig.CLEAR_M;
		p.m_fMaxHeightRange = Math.Clamp(2 * size.m_fHalfZ * CLW_VehicleSpotConfig.MAX_SLOPE_TAN, 0.8, 3);
		p.m_Extra = extra;
		if (ignore)
			p.m_aIgnore.Insert(ignore);

		return p;
	}

	// ---- road candidates ----

	// Road points within maxRoadM of the anchor, nearest first: centre line, then the two edges; heading along the road away from the anchor.
	protected static bool RoadPass(CLW_SpotParams p, vector anchor, float maxRoadM, notnull CLW_SpotResult res)
	{
		RoadNetworkManager mgr = CLW_Placement.GetRoadManager();
		if (!mgr)
		{
			res.Reject("no-road-network");
			return false;
		}

		array<BaseRoad> roads = {};
		mgr.GetRoadsInAABB(Vector(anchor[0] - maxRoadM, -10000, anchor[2] - maxRoadM), Vector(anchor[0] + maxRoadM, 10000, anchor[2] + maxRoadM), roads);
		array<vector> pointPos = {};
		array<vector> pointDir = {};
		array<float> pointWidth = {};
		array<float> pointDist = {};
		foreach (BaseRoad road : roads)
		{
			if (!road || road.GetWidth() < CLW_VehicleSpotConfig.ROAD_MIN_WIDTH_M)
				continue;

			array<vector> pts = {};
			road.GetPoints(pts);
			for (int i = 0; i < pts.Count() - 1; i++)
			{
				float sx = pts[i + 1][0] - pts[i][0];
				float sz = pts[i + 1][2] - pts[i][2];
				float len = Math.Sqrt(sx * sx + sz * sz);
				if (len < 0.5)
					continue;

				for (float d = 0; d < len; d += CLW_VehicleSpotConfig.ROAD_STEP_M)
				{
					if (pointPos.Count() >= CLW_VehicleSpotConfig.ROAD_MAX_POINTS)
						break;

					vector rp = Vector(pts[i][0] + sx * d / len, 0, pts[i][2] + sz * d / len);
					float rd = vector.DistanceXZ(rp, anchor);
					if (rd > maxRoadM)
						continue;

					pointPos.Insert(rp);
					pointDir.Insert(Vector(sx / len, 0, sz / len));
					pointWidth.Insert(road.GetWidth());
					pointDist.Insert(rd);
				}
			}
		}

		if (pointPos.IsEmpty())
		{
			res.Reject("no-road");
			return false;
		}

		array<bool> used = {};
		used.Resize(pointPos.Count());
		float bestScore = 1000000000;
		int evals = Math.Min(CLW_VehicleSpotConfig.ROAD_MAX_EVAL, pointPos.Count());
		bool done;
		for (int e = 0; e < evals && !done; e++)
		{
			int pick = -1;
			float pickDist = 1000000000;
			for (int c = 0; c < pointPos.Count(); c++)
			{
				if (!used[c] && pointDist[c] < pickDist)
				{
					pick = c;
					pickDist = pointDist[c];
				}
			}

			if (pick < 0)
				break;

			used[pick] = true;
			res.m_iCandidates++;
			vector dir = pointDir[pick];
			vector normal = Vector(-dir[2], 0, dir[0]);
			float yawX = dir[0];
			float yawZ = dir[2];
			if (yawX * (pointPos[pick][0] - anchor[0]) + yawZ * (pointPos[pick][2] - anchor[2]) < 0)
			{
				yawX = -yawX;
				yawZ = -yawZ;
			}

			float yaw = CLW_Placement.YawFromDir(yawX, yawZ);
			float edge = pointWidth[pick] * 0.5 - p.m_fHalfX - 0.2;
			for (int opt = 0; opt < 3; opt++)
			{
				float lateral;
				float pen;
				if (opt == 0)
				{
					lateral = 0;
					pen = 0;
				}
				else if (edge < 0.5)
				{
					break;
				}
				else if (opt == 1)
				{
					lateral = edge;
					pen = 1.5;
				}
				else
				{
					lateral = -edge;
					pen = 1.5;
				}

				if (res.m_iTries >= p.m_iMaxTries)
				{
					done = true;
					break;
				}

				vector cand = pointPos[pick] + normal * lateral;
				res.m_iTries++;
				float range;
				float surfaceY;
				string reason = CLW_FreeSpot.CheckPose(p, cand, yaw, range, surfaceY);
				if (!reason.IsEmpty())
				{
					res.Reject(reason);
					continue;
				}

				float score = vector.DistanceXZ(cand, anchor) + pen + range * 6;
				res.m_iValid++;
				if (score < bestScore)
				{
					bestScore = score;
					res.m_vPos = Vector(cand[0], surfaceY, cand[2]);
					res.m_fYaw = yaw;
					res.m_fScore = score;
					res.m_fRange = range;
					res.m_fRoadDist = 0;
					res.m_sVia = "road";
				}

				if (res.m_iValid >= p.m_iValidTarget)
					done = true;

				break;	// the first clear option of this road point is its best
			}
		}

		return res.m_iValid > 0;
	}

	// ---- entry points ----

	// Spot for a vehicle of 'vehiclePrefab' near 'anchorPos' (HQ, depot, FOB, requester): road within maxRoadM first, then the ring
	// search (CLW_VehicleSpotConfig.RING_MIN_M .. RING_MAX_M), then a relaxed ring. how = road | ring | fallback | none. pos is the
	// surface point; spawn at pos + LIFT_M.
	static bool Find(ResourceName vehiclePrefab, vector anchorPos, string side, float maxRoadM, out vector pos, out float yaw, out string how)
	{
		return FindEx(vehiclePrefab, anchorPos, side, maxRoadM, CLW_VehicleSpotConfig.RING_MIN_M, CLW_VehicleSpotConfig.RING_MAX_M, null, pos, yaw, how);
	}

	// Find with explicit ring radii; 'ignore' (a vehicle that is being moved) never blocks.
	static bool FindEx(ResourceName vehiclePrefab, vector anchorPos, string side, float maxRoadM, float ringMin, float ringMax, IEntity ignore, out vector pos, out float yaw, out string how)
	{
		CLW_VehicleSize size = SizeOf(vehiclePrefab);
		CLW_VehicleExtra extra = new CLW_VehicleExtra();
		extra.m_fHalfX = size.m_fHalfX;
		extra.m_fHalfZ = size.m_fHalfZ;
		extra.m_fOffsetX = size.m_fOffsetX;
		extra.m_fOffsetZ = size.m_fOffsetZ;
		CollectKeepouts(extra);
		CLW_SpotParams p = MakeParams("vehicle spot", anchorPos, size, extra, ignore);
		p.m_iMaxTries = 150;
		CLW_SpotResult chosen;
		string rejects = "";
		int tried;

		if (maxRoadM > 0)
		{
			CLW_SpotResult roadRes = new CLW_SpotResult();
			bool roadOk = RoadPass(p, anchorPos, maxRoadM, roadRes);
			tried += roadRes.m_iTries;
			rejects += "road:" + roadRes.Summary();
			if (roadOk)
			{
				chosen = roadRes;
				how = "road";
			}
		}

		if (!chosen)
		{
			CLW_SpotParams ringParams = MakeParams("vehicle ring", anchorPos, size, extra, ignore);
			ringParams.m_fMinRadius = ringMin;
			ringParams.m_fMaxRadius = ringMax;
			ringParams.m_vFaceTarget = anchorPos;
			ringParams.m_bRoadYaw = true;
			ringParams.m_iMaxTries = 400;
			CLW_SpotResult ringRes = new CLW_SpotResult();
			bool ringOk = CLW_FreeSpot.FindFreeSpot(ringParams, ringRes);
			tried += ringRes.m_iTries;
			rejects += " ring:" + ringRes.Summary();
			if (ringOk)
			{
				chosen = ringRes;
				how = "ring";
			}
		}

		if (!chosen)
		{
			CLW_SpotParams wideParams = MakeParams("vehicle fallback", anchorPos, size, extra, ignore);
			wideParams.m_fMargin = 0.75;
			wideParams.m_fMaxHeightRange = wideParams.m_fMaxHeightRange * 1.5;
			wideParams.m_fMinRadius = ringMin;
			wideParams.m_fMaxRadius = ringMax * 1.5;
			wideParams.m_vFaceTarget = anchorPos;
			wideParams.m_bRoadYaw = true;
			wideParams.m_iMaxTries = 500;
			CLW_SpotResult wideRes = new CLW_SpotResult();
			bool wideOk = CLW_FreeSpot.FindFreeSpot(wideParams, wideRes);
			tried += wideRes.m_iTries;
			rejects += " fallback:" + wideRes.Summary();
			if (wideOk)
			{
				chosen = wideRes;
				how = "fallback";
			}
		}

		string prefabName = vehiclePrefab.GetPath();
		if (!chosen)
		{
			how = "none";
			pos = anchorPos;
			Print("CLW: vehicle spot " + prefabName + " for " + side + ": via=none FAILED around " + anchorPos.ToString() + " tried=" + tried + " rejects{" + rejects + "} size=" + size.m_sSource, LogLevel.WARNING);
			return false;
		}

		pos = chosen.m_vPos;
		yaw = chosen.m_fYaw;
		float reserveR = Math.Sqrt(size.m_fHalfX * size.m_fHalfX + size.m_fHalfZ * size.m_fHalfZ);
		Reserve(pos[0], pos[2], reserveR, CLW_VehicleSpotConfig.RESERVE_SEC);
		Print("CLW: vehicle spot " + prefabName + " for " + side + ": via=" + how + " at " + pos.ToString() + " dist=" + Math.Round(vector.DistanceXZ(pos, anchorPos)).ToString() + " tried=" + tried + " rejects{" + rejects + "} size=" + size.m_sSource + " " + (Math.Round(size.m_fHalfX * 20) / 10).ToString() + "x" + (Math.Round(size.m_fHalfZ * 20) / 10).ToString(), LogLevel.NORMAL);
		return true;
	}

	// ---- after the spawn ----

	// Call right after SpawnEntityPrefab: learns the real size and schedules the check.
	static void Watch(IEntity vehicle, ResourceName prefab, string side, vector anchor, float maxRoadM)
	{
		if (!vehicle)
			return;

		Learn(prefab, vehicle);
		GetGame().GetCallqueue().CallLater(Verify, CLW_VehicleSpotConfig.VERIFY_DELAY_MS, false, vehicle, prefab, side, anchor, maxRoadM, 0);
	}

	protected static string OverlapReason(IEntity vehicle)
	{
		vector mat[4];
		vehicle.GetWorldTransform(mat);
		vector lmin, lmax;
		vehicle.GetBounds(lmin, lmax);
		vector mins = Vector(lmin[0] + 0.15, lmin[1] + 0.4, lmin[2] + 0.15);
		vector maxs = Vector(lmax[0] - 0.15, lmax[1], lmax[2] - 0.15);
		if (maxs[0] <= mins[0] || maxs[2] <= mins[2] || maxs[1] <= mins[1])
			return "";

		CLW_SpotQuery query = new CLW_SpotQuery();
		array<IEntity> ignoreList = {};
		ignoreList.Insert(vehicle);
		query.m_aIgnore = ignoreList;
		query.m_bBlockTrees = true;
		query.m_bBlockCharacters = false;
		GetGame().GetWorld().QueryEntitiesByOBB(mins, maxs, mat, query.OnEntity, null, EQueryEntitiesFlags.STATIC | EQueryEntitiesFlags.DYNAMIC | EQueryEntitiesFlags.WITH_OBJECT);
		return query.m_sReason;
	}

	protected static void Verify(IEntity vehicle, ResourceName prefab, string side, vector anchor, float maxRoadM, int attempt)
	{
		if (!vehicle || vehicle.IsDeleted())
			return;

		vector mat[4];
		vehicle.GetWorldTransform(mat);
		string problem = "";
		if (mat[1][1] < CLW_VehicleSpotConfig.TILT_MIN_UP)
			problem = "tilt " + Math.Round(Math.Acos(Math.Clamp(mat[1][1], -1, 1)) * Math.RAD2DEG).ToString() + " deg";
		else
		{
			string overlap = OverlapReason(vehicle);
			if (!overlap.IsEmpty())
				problem = "intersects " + overlap;
		}

		if (problem.IsEmpty())
			return;

		vector here = vehicle.GetOrigin();
		if (attempt >= 1)
		{
			Print("CLW: vehicle spot " + prefab.GetPath() + " for " + side + ": still bad after the move (" + problem + ") at " + here.ToString(), LogLevel.WARNING);
			return;
		}

		CLW_VehicleSize size = SizeOf(prefab);
		Reserve(here[0], here[2], Math.Sqrt(size.m_fHalfX * size.m_fHalfX + size.m_fHalfZ * size.m_fHalfZ), 120);
		vector newPos;
		float newYaw;
		string how;
		if (!FindEx(prefab, anchor, side, maxRoadM, CLW_VehicleSpotConfig.RING_MIN_M, CLW_VehicleSpotConfig.RING_MAX_M, vehicle, newPos, newYaw, how))
		{
			Print("CLW: vehicle spot " + prefab.GetPath() + " for " + side + ": bad at " + here.ToString() + " (" + problem + ") and no other candidate, left in place", LogLevel.WARNING);
			return;
		}

		vector moved[4];
		Math3D.AnglesToMatrix(Vector(newYaw, 0, 0), moved);
		moved[3] = Vector(newPos[0], newPos[1] + CLW_VehicleSpotConfig.LIFT_M, newPos[2]);
		vehicle.SetTransform(moved);
		Physics physics = vehicle.GetPhysics();
		if (physics)
		{
			physics.SetVelocity(vector.Zero);
			physics.SetAngularVelocity(vector.Zero);
		}

		Print("CLW: vehicle spot " + prefab.GetPath() + " for " + side + ": moved once from " + here.ToString() + " (" + problem + ") to " + newPos.ToString() + " via=" + how, LogLevel.NORMAL);
		GetGame().GetCallqueue().CallLater(Verify, CLW_VehicleSpotConfig.VERIFY_DELAY_MS, false, vehicle, prefab, side, anchor, maxRoadM, 1);
	}
}

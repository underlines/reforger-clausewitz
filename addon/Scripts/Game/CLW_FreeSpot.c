// Collision-aware placement service (server). Finds a position + heading for something of a known footprint (a service composition, a
// parked vehicle, a terminal, a FOB site) that is dry, flat enough, clear of every solid world object (buildings, walls, fences, props,
// trees, vehicles) and of the positions the caller wants to keep free. Nothing here knows a prefab, a faction or a coordinate.
//
// Pieces:
//   CLW_SpotParams / CLW_SpotResult    request and answer (answer carries the reject counts per reason for the log)
//   CLW_FreeSpot.FindFreeSpot          ring search around a centre: several yaws per position, best scored candidate wins
//   CLW_FreeSpot.FindRoadside          vehicles: the nearest road point within a radius, parked on the road edge (aligned) or the shoulder
//   CLW_FreeSpot.FootprintOf           half extents of a composition derived from the children coordinates of its prefab (no spawn), data override
//   CLW_FreeSpot.NearestRoad           nearest road point + direction (engine road network)
//
// Clearance test of one pose (CheckPose): 3x3 terrain samples over the rotated footprint (water, height range), five short cylinder
// traces against solid entities, and QueryEntitiesByOBB over the footprint box (+ margin): an entity blocks when it has a visual object or
// physics, is at least MIN_BLOCK_H tall (flat decals, road meshes, clutter and grass do not block), is not terrain-sized, and is not a bush
// (SMALL_VEG_H). Trees and vehicles block; characters only when the caller asks (vehicles do).
//
// APIs: BaseWorld.QueryEntitiesByOBB / GetSurfaceY / GetOceanHeight / GetBoundBox, SCR_WorldTools.TraceCilinderUtil,
// ChimeraWorldUtils.TryGetWaterSurface, IEntity.GetWorldBounds / GetVObject / GetPhysics / GetRootParent, BaseTree, TreeEntity,
// RoadNetworkManager.GetRoadsInAABB (via CLW_Placement.GetRoadManager), BaseRoad.GetPoints / GetWidth,
// Resource.Load -> BaseResourceObject.ToEntitySource -> BaseContainer.GetNumChildren / GetChild / GetAncestor / Get("coords").
// Log prefix: "CLW: place".

class CLW_SpotConfig
{
	static const float MIN_BLOCK_H = 0.6;			// entities lower than this (decals, road meshes, clutter) never block
	static const float SMALL_VEG_H = 1.5;			// trees / bushes lower than this never block
	static const float MAX_ENTITY_M = 120;			// entities with a larger world box are terrain-like, ignored
	static const float TRACE_RADIUS = 0.4;
	static const float TRACE_LIFT = 0.5;			// the trace cylinders start this far above the ground
	static const float WORLD_MARGIN_M = 20;			// candidates closer than this to the world edge are rejected
	static const float FOOT_CHILD_MARGIN_M = 3;		// added around the children coordinates of a composition
	static const float FOOT_MIN_HALF_M = 2;
	static const float FOOT_MAX_HALF_M = 45;
	static const float FOOT_FALLBACK_FACTOR = 0.9;	// half extent = data radius * this when nothing else is known
	static const float COMPOSITION_HEIGHT_M = 4;
	static const float VEHICLE_HALF_X = 2.1;		// default vehicle footprint (half width, half length, height)
	static const float VEHICLE_HALF_Z = 4.8;
	static const float VEHICLE_HEIGHT = 3.2;
	static const float VEHICLE_MARGIN_M = 0.5;
	static const float VEHICLE_MAX_RANGE_M = 0.9;
	static const float ROAD_YAW_M = 25;				// a candidate within this of a road may take the road heading
	static const float ROAD_STEP_M = 6;				// road polyline resampling for the roadside search
	static const int ROAD_MAX_POINTS = 400;
	static const int ROAD_MAX_EVAL = 40;			// road points tried (nearest first)
	static const float ROAD_MIN_WIDTH_M = 3;
	static const float DEPOT_NEAR_M = 300;			// requester this close to a depot: the vehicle comes from the depot
	static const float REQUEST_ROAD_M = 50;			// roadside search radius around the requester
	static const float REQUEST_GROUND_M = 30;		// open ground search radius around the requester
	static const float REQUEST_GROUND_MIN_M = 5;
	static const float FOB_CLUSTER_HALF_M = 12;		// FOB site validation: room for the command post cluster
	static const float FOB_CLUSTER_HEIGHT_M = 4;
	static const float FOB_REFINE_RADIUS_M = 80;	// a FOB site may shift this far to reach clear ground (35 was too small on hilly Arland)
}

// Half extents of a footprint, local to the entity origin (the box centre is offset by m_fOffsetX / m_fOffsetZ).
class CLW_Footprint
{
	float m_fHalfX = 3;
	float m_fHalfZ = 3;
	float m_fOffsetX;
	float m_fOffsetZ;
	float m_fHeight = 3;
	string m_sSource = "default";

	float Radius()
	{
		return Math.Sqrt(m_fHalfX * m_fHalfX + m_fHalfZ * m_fHalfZ);
	}

	CLW_Footprint Dup()
	{
		CLW_Footprint other = new CLW_Footprint();
		other.m_fHalfX = m_fHalfX;
		other.m_fHalfZ = m_fHalfZ;
		other.m_fOffsetX = m_fOffsetX;
		other.m_fOffsetZ = m_fOffsetZ;
		other.m_fHeight = m_fHeight;
		other.m_sSource = m_sSource;
		return other;
	}
}

class CLW_SpotParams
{
	string m_sLabel = "spot";
	vector m_vCenter;
	float m_fMinRadius;
	float m_fMaxRadius = 30;
	float m_fHalfX = 3;
	float m_fHalfZ = 3;
	float m_fOffsetX;
	float m_fOffsetZ;
	float m_fHeight = 3;
	float m_fMargin = 1;				// extra clearance around the footprint for the entity query
	float m_fFixedYaw = -1;				// >= 0: only this heading
	vector m_vFaceTarget;				// yaw candidates face this point (zero = the centre)
	float m_fMaxHeightRange = 1.2;		// terrain height range over the footprint
	bool m_bBlockTrees = true;
	bool m_bBlockCharacters;
	float m_fRoadWeight;				// > 0: score += road distance * weight (no road within reach counts as 40 m)
	bool m_bRoadYaw;					// candidates near a road may take the road heading
	int m_iMaxTries = 600;				// pose checks
	int m_iValidTarget = 4;				// valid candidates collected before the best is taken
	ref array<vector> m_aAvoidPos = {};
	ref array<float> m_aAvoidDist = {};	// required distance from the CENTRE of the pose to the avoid position
	ref array<IEntity> m_aIgnore = {};	// entities (and their children) that do not block

	void SetFootprint(CLW_Footprint foot)
	{
		m_fHalfX = foot.m_fHalfX;
		m_fHalfZ = foot.m_fHalfZ;
		m_fOffsetX = foot.m_fOffsetX;
		m_fOffsetZ = foot.m_fOffsetZ;
		m_fHeight = foot.m_fHeight;
	}

	void SetVehicle()
	{
		m_fHalfX = CLW_SpotConfig.VEHICLE_HALF_X;
		m_fHalfZ = CLW_SpotConfig.VEHICLE_HALF_Z;
		m_fOffsetX = 0;
		m_fOffsetZ = 0;
		m_fHeight = CLW_SpotConfig.VEHICLE_HEIGHT;
		m_fMargin = CLW_SpotConfig.VEHICLE_MARGIN_M;
		m_fMaxHeightRange = CLW_SpotConfig.VEHICLE_MAX_RANGE_M;
		m_bBlockCharacters = true;
	}

	void AddAvoid(vector pos, float dist)
	{
		m_aAvoidPos.Insert(pos);
		m_aAvoidDist.Insert(dist);
	}

	float Radius()
	{
		return Math.Sqrt(m_fHalfX * m_fHalfX + m_fHalfZ * m_fHalfZ);
	}
}

class CLW_SpotResult
{
	vector m_vPos;
	float m_fYaw;
	float m_fScore = -1;
	float m_fRange;				// terrain height range under the footprint
	float m_fRoadDist = -1;
	int m_iTries;
	int m_iCandidates;
	int m_iValid;
	string m_sVia;
	ref map<string, int> m_mRejects = new map<string, int>();

	void Reject(string reason)
	{
		int n;
		m_mRejects.Find(reason, n);
		m_mRejects.Set(reason, n + 1);
	}

	string Summary()
	{
		string text = "";
		foreach (string reason, int count : m_mRejects)
		{
			if (!text.IsEmpty())
				text += " ";
			text += reason + "=" + count;
		}

		if (text.IsEmpty())
			return "none";

		return text;
	}
}

class CLW_RoadHit
{
	vector m_vPoint;
	vector m_vDir;		// unit, XZ
	float m_fWidth;
	float m_fDist;
}

// QueryEntitiesByOBB callback: remembers the reason of the first blocking entity.
class CLW_SpotQuery
{
	array<IEntity> m_aIgnore;
	bool m_bBlockTrees;
	bool m_bBlockCharacters;
	string m_sReason;

	bool OnEntity(IEntity entity)
	{
		if (!entity || !m_sReason.IsEmpty() || entity.IsDeleted())
			return true;

		if (m_aIgnore && !m_aIgnore.IsEmpty())
		{
			IEntity root = entity.GetRootParent();
			if (m_aIgnore.Contains(entity) || (root && m_aIgnore.Contains(root)))
				return true;
		}

		if (ChimeraCharacter.Cast(entity))
		{
			if (m_bBlockCharacters)
				m_sReason = "character";

			return true;
		}

		if (GenericTerrainEntity.Cast(entity))
			return true;

		if (!entity.GetVObject() && !entity.GetPhysics())
			return true;

		vector mins, maxs;
		entity.GetWorldBounds(mins, maxs);
		float sizeX = maxs[0] - mins[0];
		float sizeY = maxs[1] - mins[1];
		float sizeZ = maxs[2] - mins[2];
		if (sizeX > CLW_SpotConfig.MAX_ENTITY_M || sizeZ > CLW_SpotConfig.MAX_ENTITY_M)
			return true;

		bool isTree;
		if (BaseTree.Cast(entity) || TreeEntity.Cast(entity))
			isTree = true;

		if (isTree)
		{
			if (sizeY < CLW_SpotConfig.SMALL_VEG_H || !m_bBlockTrees)
				return true;

			m_sReason = "tree";
			return true;
		}

		if (sizeY < CLW_SpotConfig.MIN_BLOCK_H)
			return true;

		if (Vehicle.Cast(entity))
			m_sReason = "vehicle";
		else
			m_sReason = "object";

		return true;
	}
}

class CLW_FreeSpot
{
	protected static ref map<string, ref CLW_Footprint> s_mFootprints = new map<string, ref CLW_Footprint>();

	static void Reset()
	{
		s_mFootprints.Clear();
	}

	static float NormYaw(float yaw)
	{
		while (yaw < 0)
			yaw += 360;
		while (yaw >= 360)
			yaw -= 360;

		return yaw;
	}

	// ---- footprint of a prefab ----

	// Footprint of a composition prefab. Priority: the data override (data = half X, height, half Z; zero entries are not set), the
	// children coordinates of the prefab (extent of the child origins + CHILD_MARGIN, no spawn needed), a square from the data radius.
	static CLW_Footprint FootprintOf(ResourceName prefab, vector data, float radius, float defaultHeight)
	{
		CLW_Footprint foot = new CLW_Footprint();
		foot.m_fHeight = defaultHeight;
		if (data[0] > 0 && data[2] > 0)
		{
			foot.m_fHalfX = data[0];
			foot.m_fHalfZ = data[2];
			foot.m_sSource = "data";
			if (data[1] > 0)
				foot.m_fHeight = data[1];

			return foot;
		}

		string key = prefab.GetPath();
		CLW_Footprint cached;
		if (!key.IsEmpty() && s_mFootprints.Find(key, cached) && cached)
		{
			foot = cached.Dup();
		}
		else
		{
			CLW_Footprint derived = Derive(prefab);
			if (derived)
			{
				foot = derived;
				if (!key.IsEmpty())
					s_mFootprints.Set(key, derived.Dup());
			}
			else
			{
				float half = Math.Max(CLW_SpotConfig.FOOT_MIN_HALF_M, radius * CLW_SpotConfig.FOOT_FALLBACK_FACTOR);
				foot.m_fHalfX = half;
				foot.m_fHalfZ = half;
				foot.m_sSource = "radius";
			}
		}

		foot.m_fHeight = defaultHeight;
		if (data[1] > 0)
			foot.m_fHeight = data[1];

		return foot;
	}

	// Extent of the child origins of the prefab (nested children add their parent's coords; rotation is ignored). Null when the prefab
	// has no readable children.
	protected static CLW_Footprint Derive(ResourceName prefab)
	{
		if (prefab.IsEmpty())
			return null;

		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
			return null;

		BaseResourceObject resObject = resource.GetResource();
		if (!resObject)
			return null;

		BaseContainer source = resObject.ToEntitySource();
		if (!source)
			return null;

		float minX = 0;
		float maxX = 0;
		float minZ = 0;
		float maxZ = 0;
		int count = ScanChildren(source, 0, 0, 0, minX, maxX, minZ, maxZ);
		BaseContainer ancestor = source.GetAncestor();
		for (int guard = 0; count == 0 && ancestor && guard < 4; guard++)
		{
			count = ScanChildren(ancestor, 0, 0, 0, minX, maxX, minZ, maxZ);
			ancestor = ancestor.GetAncestor();
		}

		if (count == 0)
			return null;

		CLW_Footprint foot = new CLW_Footprint();
		foot.m_fHalfX = Math.Clamp((maxX - minX) * 0.5 + CLW_SpotConfig.FOOT_CHILD_MARGIN_M, CLW_SpotConfig.FOOT_MIN_HALF_M, CLW_SpotConfig.FOOT_MAX_HALF_M);
		foot.m_fHalfZ = Math.Clamp((maxZ - minZ) * 0.5 + CLW_SpotConfig.FOOT_CHILD_MARGIN_M, CLW_SpotConfig.FOOT_MIN_HALF_M, CLW_SpotConfig.FOOT_MAX_HALF_M);
		foot.m_fOffsetX = (maxX + minX) * 0.5;
		foot.m_fOffsetZ = (maxZ + minZ) * 0.5;
		foot.m_sSource = "children(" + count + ")";
		return foot;
	}

	protected static int ScanChildren(BaseContainer source, float baseX, float baseZ, int depth, inout float minX, inout float maxX, inout float minZ, inout float maxZ)
	{
		int count;
		int children = source.GetNumChildren();
		for (int i = 0; i < children; i++)
		{
			BaseContainer child = source.GetChild(i);
			if (!child)
				continue;

			vector coords;
			child.Get("coords", coords);
			float x = baseX + coords[0];
			float z = baseZ + coords[2];
			minX = Math.Min(minX, x);
			maxX = Math.Max(maxX, x);
			minZ = Math.Min(minZ, z);
			maxZ = Math.Max(maxZ, z);
			count++;
			if (depth < 3)
				count += ScanChildren(child, x, z, depth + 1, minX, maxX, minZ, maxZ);
		}

		return count;
	}

	// ---- roads ----

	// Nearest road point to pos within radius (roads narrower than minWidth are trails and skipped). dir = unit road direction in XZ.
	static bool NearestRoad(vector pos, float radius, float minWidth, out CLW_RoadHit hit)
	{
		RoadNetworkManager mgr = CLW_Placement.GetRoadManager();
		if (!mgr)
			return false;

		array<BaseRoad> roads = {};
		mgr.GetRoadsInAABB(Vector(pos[0] - radius, -10000, pos[2] - radius), Vector(pos[0] + radius, 10000, pos[2] + radius), roads);
		float bestSq = radius * radius;
		bool found;
		vector bestPoint;
		vector bestDir;
		float bestWidth;
		foreach (BaseRoad road : roads)
		{
			if (!road || road.GetWidth() < minWidth)
				continue;

			array<vector> pts = {};
			road.GetPoints(pts);
			for (int i = 0; i < pts.Count() - 1; i++)
			{
				float t;
				float sq = CLW_Placement.SegmentDistSq(pos[0], pos[2], pts[i][0], pts[i][2], pts[i + 1][0], pts[i + 1][2], t);
				if (sq >= bestSq)
					continue;

				float dx = pts[i + 1][0] - pts[i][0];
				float dz = pts[i + 1][2] - pts[i][2];
				float len = Math.Sqrt(dx * dx + dz * dz);
				if (len < 0.1)
					continue;

				bestSq = sq;
				bestPoint = Vector(pts[i][0] + dx * t, 0, pts[i][2] + dz * t);
				bestDir = Vector(dx / len, 0, dz / len);
				bestWidth = road.GetWidth();
				found = true;
			}
		}

		if (!found)
			return false;

		hit = new CLW_RoadHit();
		hit.m_vPoint = bestPoint;
		hit.m_vDir = bestDir;
		hit.m_fWidth = bestWidth;
		hit.m_fDist = Math.Sqrt(bestSq);
		return true;
	}

	// ---- one pose ----

	// "" when a footprint at pos with heading yaw is clear, else the reason: water | slope | trace | tree | vehicle | character | object.
	// range = terrain height range under the footprint, surfaceY = terrain height at pos.
	static string CheckPose(CLW_SpotParams p, vector pos, float yaw, out float range, out float surfaceY)
	{
		BaseWorld world = GetGame().GetWorld();
		vector mat[4];
		Math3D.AnglesToMatrix(Vector(yaw, 0, 0), mat);
		float minY = 1000000;
		float maxY = -1000000;
		for (int gx = 0; gx < 3; gx++)
		{
			float lx = p.m_fOffsetX + (gx - 1) * p.m_fHalfX;
			for (int gz = 0; gz < 3; gz++)
			{
				float lz = p.m_fOffsetZ + (gz - 1) * p.m_fHalfZ;
				vector sample = pos + mat[0] * lx + mat[2] * lz;
				float sy = world.GetSurfaceY(sample[0], sample[2]);
				if (sy <= world.GetOceanHeight(sample[0], sample[2]) + CLW_WarConfig.WATER_MARGIN_M)
					return "water";

				minY = Math.Min(minY, sy);
				maxY = Math.Max(maxY, sy);
			}
		}

		surfaceY = world.GetSurfaceY(pos[0], pos[2]);
		range = maxY - minY;
		if (range > p.m_fMaxHeightRange)
			return "slope";

		// inland water (lakes, rivers): the water surface at the centre lies above the terrain
		vector waterPoint;
		EWaterSurfaceType waterType;
		vector waterTransform[4];
		vector waterExtents;
		if (ChimeraWorldUtils.TryGetWaterSurface(world, Vector(pos[0], surfaceY, pos[2]), waterPoint, waterType, waterTransform, waterExtents) && waterPoint[1] > surfaceY - 0.05)
			return "water";

		// five short cylinder traces: centre and the four corners (pulled in), they catch colliders the entity query box misses
		float cylH = Math.Clamp(p.m_fHeight, 1.5, 3);
		float insetX = Math.Max(0, p.m_fHalfX - 0.6);
		float insetZ = Math.Max(0, p.m_fHalfZ - 0.6);
		for (int tp = 0; tp < 5; tp++)
		{
			float tx = p.m_fOffsetX;
			float tz = p.m_fOffsetZ;
			if (tp > 0)
			{
				if (tp % 2 == 1)
					tx = tx - insetX;
				else
					tx = tx + insetX;

				if (tp < 3)
					tz = tz - insetZ;
				else
					tz = tz + insetZ;
			}

			vector tw = pos + mat[0] * tx + mat[2] * tz;
			float ty = world.GetSurfaceY(tw[0], tw[2]) + CLW_SpotConfig.TRACE_LIFT + cylH * 0.5;
			if (!SCR_WorldTools.TraceCilinderUtil(Vector(tw[0], ty, tw[2]), CLW_SpotConfig.TRACE_RADIUS, cylH))
				return "trace";
		}

		// the footprint box (+ margin) against every entity that touches it
		vector box[4];
		Math3D.AnglesToMatrix(Vector(yaw, 0, 0), box);
		box[3] = Vector(pos[0], minY, pos[2]);
		vector mins = Vector(p.m_fOffsetX - p.m_fHalfX - p.m_fMargin, 0.25, p.m_fOffsetZ - p.m_fHalfZ - p.m_fMargin);
		vector maxs = Vector(p.m_fOffsetX + p.m_fHalfX + p.m_fMargin, (maxY - minY) + p.m_fHeight, p.m_fOffsetZ + p.m_fHalfZ + p.m_fMargin);
		CLW_SpotQuery query = new CLW_SpotQuery();
		query.m_aIgnore = p.m_aIgnore;
		query.m_bBlockTrees = p.m_bBlockTrees;
		query.m_bBlockCharacters = p.m_bBlockCharacters;
		world.QueryEntitiesByOBB(mins, maxs, box, query.OnEntity, null, EQueryEntitiesFlags.STATIC | EQueryEntitiesFlags.DYNAMIC | EQueryEntitiesFlags.WITH_OBJECT);
		return query.m_sReason;
	}

	// ---- ring search ----

	protected static void BuildYaws(CLW_SpotParams p, vector cand, notnull array<float> yaws, notnull array<float> pens)
	{
		yaws.Clear();
		pens.Clear();
		if (p.m_fFixedYaw >= 0)
		{
			yaws.Insert(NormYaw(p.m_fFixedYaw));
			pens.Insert(0);
			return;
		}

		vector target = p.m_vFaceTarget;
		if (target == vector.Zero)
			target = p.m_vCenter;

		if (p.m_bRoadYaw)
		{
			CLW_RoadHit hit;
			if (NearestRoad(cand, CLW_SpotConfig.ROAD_YAW_M, CLW_SpotConfig.ROAD_MIN_WIDTH_M, hit))
			{
				float dirX = hit.m_vDir[0];
				float dirZ = hit.m_vDir[2];
				if (dirX * (target[0] - cand[0]) + dirZ * (target[2] - cand[2]) < 0)
				{
					dirX = -dirX;
					dirZ = -dirZ;
				}

				float roadYaw = CLW_Placement.YawFromDir(dirX, dirZ);
				yaws.Insert(roadYaw);
				pens.Insert(0);
				yaws.Insert(NormYaw(roadYaw + 180));
				pens.Insert(0.5);
			}
		}

		float face = 0;
		if (vector.DistanceXZ(cand, target) > 0.5)
			face = CLW_Placement.YawFromDir(target[0] - cand[0], target[2] - cand[2]);

		yaws.Insert(face);
		pens.Insert(1);
		yaws.Insert(NormYaw(face + 90));
		pens.Insert(2.5);
		yaws.Insert(NormYaw(face + 270));
		pens.Insert(2.5);
		yaws.Insert(NormYaw(face + 180));
		pens.Insert(3.5);
	}

	// Ring search around p.m_vCenter between the radii, outward; per position the yaw candidates in preference order (the first clear one
	// counts). Collects p.m_iValidTarget clear candidates, returns the best score (lower = better): terrain height range * 6 + distance
	// beyond the inner radius * 0.2 + yaw preference + road distance * weight. Logs one line. False when nothing was clear (res.m_vPos =
	// the centre on the surface, NOT valid).
	static bool FindFreeSpot(CLW_SpotParams p, notnull CLW_SpotResult res)
	{
		BaseWorld world = GetGame().GetWorld();
		vector worldMin, worldMax;
		world.GetBoundBox(worldMin, worldMax);
		float margin = CLW_SpotConfig.WORLD_MARGIN_M;
		float step = Math.Clamp(Math.Min(p.m_fHalfX, p.m_fHalfZ) + 1, 3, 12);
		float baseAngle = Math.RandomFloat(0, Math.PI2);
		float maxRadius = Math.Max(p.m_fMaxRadius, p.m_fMinRadius);
		array<float> yaws = {};
		array<float> pens = {};
		float bestScore = 1000000000;
		bool done;
		int ring;
		for (float radius = p.m_fMinRadius; radius <= maxRadius + 0.01 && !done; radius += step)
		{
			int count = 1;
			if (radius > 0.5)
				count = Math.Clamp(Math.Ceil(Math.PI2 * radius / step), 6, 40);

			for (int a = 0; a < count && !done; a++)
			{
				float angle = baseAngle + Math.PI2 * a / count + ring * 0.7;
				vector cand = p.m_vCenter + Vector(Math.Cos(angle) * radius, 0, Math.Sin(angle) * radius);
				res.m_iCandidates++;
				if (cand[0] < worldMin[0] + margin || cand[0] > worldMax[0] - margin || cand[2] < worldMin[2] + margin || cand[2] > worldMax[2] - margin)
				{
					res.Reject("bounds");
					continue;
				}

				bool near;
				for (int ai = 0; ai < p.m_aAvoidPos.Count(); ai++)
				{
					if (vector.DistanceXZ(cand, p.m_aAvoidPos[ai]) < p.m_aAvoidDist[ai])
					{
						near = true;
						break;
					}
				}

				if (near)
				{
					res.Reject("avoid");
					continue;
				}

				BuildYaws(p, cand, yaws, pens);
				for (int yi = 0; yi < yaws.Count(); yi++)
				{
					if (res.m_iTries >= p.m_iMaxTries)
					{
						done = true;
						break;
					}

					res.m_iTries++;
					float range;
					float surfaceY;
					string reason = CheckPose(p, cand, yaws[yi], range, surfaceY);
					if (!reason.IsEmpty())
					{
						res.Reject(reason);
						continue;
					}

					float score = range * 6 + (radius - p.m_fMinRadius) * 0.2 + pens[yi];
					float roadDist = -1;
					CLW_RoadHit roadHit;
					if (p.m_fRoadWeight > 0)
					{
						if (NearestRoad(cand, 40, CLW_SpotConfig.ROAD_MIN_WIDTH_M, roadHit))
						{
							roadDist = roadHit.m_fDist;
							score += roadDist * p.m_fRoadWeight;
						}
						else
							score += 40 * p.m_fRoadWeight;
					}

					res.m_iValid++;
					if (score < bestScore)
					{
						bestScore = score;
						res.m_vPos = Vector(cand[0], surfaceY, cand[2]);
						res.m_fYaw = yaws[yi];
						res.m_fScore = score;
						res.m_fRange = range;
						res.m_fRoadDist = roadDist;
					}

					if (res.m_iValid >= p.m_iValidTarget)
						done = true;

					break;	// first clear yaw of this position
				}
			}

			ring++;
		}

		bool ok = res.m_iValid > 0;
		if (ok)
			res.m_sVia = "ring";
		else
		{
			res.m_vPos = p.m_vCenter;
			res.m_vPos[1] = world.GetSurfaceY(p.m_vCenter[0], p.m_vCenter[2]);
			res.m_sVia = "none";
		}

		LogResult(p, res, ok);
		return ok;
	}

	// ---- roadside (vehicles) ----

	// Parks a vehicle footprint on the road nearest to 'near' within roadRadius: first the road edge (vehicle flush with the edge, on the
	// side of the requester), then the shoulder just off the road, then the other side. The heading follows the road (pointing away from
	// 'near'). Candidates are road points nearest first. Score = distance to 'near' + side preference + height range * 6.
	static bool FindRoadside(CLW_SpotParams p, vector near, float roadRadius, notnull CLW_SpotResult res)
	{
		RoadNetworkManager mgr = CLW_Placement.GetRoadManager();
		if (!mgr)
		{
			res.Reject("no-road-network");
			LogResult(p, res, false);
			return false;
		}

		array<BaseRoad> roads = {};
		mgr.GetRoadsInAABB(Vector(near[0] - roadRadius, -10000, near[2] - roadRadius), Vector(near[0] + roadRadius, 10000, near[2] + roadRadius), roads);
		array<vector> pointPos = {};
		array<vector> pointDir = {};
		array<float> pointWidth = {};
		array<float> pointDist = {};
		foreach (BaseRoad road : roads)
		{
			if (!road || road.GetWidth() < CLW_SpotConfig.ROAD_MIN_WIDTH_M)
				continue;

			array<vector> pts = {};
			road.GetPoints(pts);
			for (int i = 0; i < pts.Count() - 1; i++)
			{
				float dx = pts[i + 1][0] - pts[i][0];
				float dz = pts[i + 1][2] - pts[i][2];
				float len = Math.Sqrt(dx * dx + dz * dz);
				if (len < 0.5)
					continue;

				for (float d = 0; d < len; d += CLW_SpotConfig.ROAD_STEP_M)
				{
					if (pointPos.Count() >= CLW_SpotConfig.ROAD_MAX_POINTS)
						break;

					vector rp = Vector(pts[i][0] + dx * d / len, 0, pts[i][2] + dz * d / len);
					float rd = vector.DistanceXZ(rp, near);
					if (rd > roadRadius)
						continue;

					pointPos.Insert(rp);
					pointDir.Insert(Vector(dx / len, 0, dz / len));
					pointWidth.Insert(road.GetWidth());
					pointDist.Insert(rd);
				}
			}
		}

		if (pointPos.IsEmpty())
		{
			res.Reject("no-road");
			LogResult(p, res, false);
			return false;
		}

		array<bool> used = {};
		used.Resize(pointPos.Count());
		float bestScore = 1000000000;
		int evals = Math.Min(CLW_SpotConfig.ROAD_MAX_EVAL, pointPos.Count());
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
			float side = 1;
			if (normal[0] * (near[0] - pointPos[pick][0]) + normal[2] * (near[2] - pointPos[pick][2]) < 0)
				side = -1;

			float yawX = dir[0];
			float yawZ = dir[2];
			if (yawX * (pointPos[pick][0] - near[0]) + yawZ * (pointPos[pick][2] - near[2]) < 0)
			{
				yawX = -yawX;
				yawZ = -yawZ;
			}

			float yaw = CLW_Placement.YawFromDir(yawX, yawZ);
			float halfRoad = pointWidth[pick] * 0.5;
			for (int opt = 0; opt < 4; opt++)
			{
				if (res.m_iTries >= p.m_iMaxTries)
				{
					done = true;
					break;
				}

				float lateral;
				float pen;
				string via;
				if (opt == 0)
				{
					lateral = Math.Max(0, halfRoad - p.m_fHalfX - 0.2) * side;
					pen = 0;
					via = "road-edge";
				}
				else if (opt == 1)
				{
					lateral = (halfRoad + p.m_fHalfX + 0.8) * side;
					pen = 6;
					via = "road-shoulder";
				}
				else if (opt == 2)
				{
					lateral = Math.Max(0, halfRoad - p.m_fHalfX - 0.2) * -side;
					pen = 3;
					via = "road-edge";
				}
				else
				{
					lateral = (halfRoad + p.m_fHalfX + 0.8) * -side;
					pen = 9;
					via = "road-shoulder";
				}

				vector cand = pointPos[pick] + normal * lateral;
				res.m_iTries++;
				float range;
				float surfaceY;
				string reason = CheckPose(p, cand, yaw, range, surfaceY);
				if (!reason.IsEmpty())
				{
					res.Reject(reason);
					continue;
				}

				float score = vector.DistanceXZ(cand, near) + pen + range * 6;
				res.m_iValid++;
				if (score < bestScore)
				{
					bestScore = score;
					res.m_vPos = Vector(cand[0], surfaceY, cand[2]);
					res.m_fYaw = yaw;
					res.m_fScore = score;
					res.m_fRange = range;
					res.m_fRoadDist = 0;
					res.m_sVia = via;
				}

				if (res.m_iValid >= p.m_iValidTarget)
					done = true;

				break;	// the first clear option of this road point is its best
			}
		}

		bool ok = res.m_iValid > 0;
		if (!ok)
		{
			res.m_vPos = near;
			res.m_sVia = "none";
		}

		LogResult(p, res, ok);
		return ok;
	}

	protected static void LogResult(CLW_SpotParams p, CLW_SpotResult res, bool ok)
	{
		if (ok)
		{
			Print(string.Format("CLW: place %1 at %2 grid %3 yaw=%4 via=%5 score=%6 range=%7 tries=%8 cand=%9", p.m_sLabel, res.m_vPos, CLW_Places.Grid(res.m_vPos), Math.Round(res.m_fYaw), res.m_sVia, Math.Round(res.m_fScore), Math.Round(res.m_fRange * 10) / 10, res.m_iTries, res.m_iCandidates) + " valid=" + res.m_iValid + " rejected: " + res.Summary(), LogLevel.NORMAL);
			return;
		}

		Print(string.Format("CLW: place %1 FAILED around %2 (r %3..%4 m, foot %5x%6) tries=%7 cand=%8", p.m_sLabel, p.m_vCenter, Math.Round(p.m_fMinRadius), Math.Round(p.m_fMaxRadius), Math.Round(p.m_fHalfX * 2), Math.Round(p.m_fHalfZ * 2), res.m_iTries, res.m_iCandidates) + " rejected: " + res.Summary(), LogLevel.WARNING);
	}

	// ---- ready-made searches ----

	// Vehicle parking parameters (footprint, clearance for characters too).
	static CLW_SpotParams VehicleParams(string label, vector center)
	{
		CLW_SpotParams p = new CLW_SpotParams();
		p.m_sLabel = label;
		p.m_vCenter = center;
		p.SetVehicle();
		return p;
	}

	// A small free spot (terminal, box, squad cluster): half = half extent of the square footprint.
	static bool FindSmall(string label, vector center, float minRadius, float maxRadius, float half, float height, out vector spot, out float yaw)
	{
		CLW_SpotParams p = new CLW_SpotParams();
		p.m_sLabel = label;
		p.m_vCenter = center;
		p.m_fMinRadius = minRadius;
		p.m_fMaxRadius = maxRadius;
		p.m_fHalfX = half;
		p.m_fHalfZ = half;
		p.m_fHeight = height;
		p.m_fMargin = 0.3;
		p.m_fMaxHeightRange = 0.6 + half * 0.1;
		p.m_iMaxTries = 250;
		p.m_iValidTarget = 3;
		CLW_SpotResult res = new CLW_SpotResult();
		if (!FindFreeSpot(p, res))
			return false;

		spot = res.m_vPos;
		yaw = res.m_fYaw;
		return true;
	}

	// Parking spot at a depot: ring around it, roads preferred, heading along the road or toward 'faceTarget'.
	static bool FindDepotSpot(string label, vector depotPos, float minRadius, float maxRadius, vector faceTarget, out vector spot, out float yaw, out string via)
	{
		CLW_SpotParams p = VehicleParams(label, depotPos);
		p.m_fMinRadius = minRadius;
		p.m_fMaxRadius = maxRadius;
		p.m_vFaceTarget = faceTarget;
		p.m_fRoadWeight = 1;
		p.m_bRoadYaw = true;
		CLW_SpotResult res = new CLW_SpotResult();
		if (!FindFreeSpot(p, res))
			return false;

		spot = res.m_vPos;
		yaw = res.m_fYaw;
		via = res.m_sVia;
		if (res.m_fRoadDist >= 0 && res.m_fRoadDist < 12)
			via = "depot-road";
		else
			via = "depot-ground";

		return true;
	}

	// Vehicle near a requester: nearest road within REQUEST_ROAD_M (road edge, aligned), else open ground within REQUEST_GROUND_M.
	static bool FindNearRequester(string label, vector requester, out vector spot, out float yaw, out string via)
	{
		CLW_SpotParams roadParams = VehicleParams(label + " roadside", requester);
		CLW_SpotResult roadRes = new CLW_SpotResult();
		if (FindRoadside(roadParams, requester, CLW_SpotConfig.REQUEST_ROAD_M, roadRes))
		{
			spot = roadRes.m_vPos;
			yaw = roadRes.m_fYaw;
			via = roadRes.m_sVia;
			return true;
		}

		CLW_SpotParams groundParams = VehicleParams(label + " ground", requester);
		groundParams.m_fMinRadius = CLW_SpotConfig.REQUEST_GROUND_MIN_M;
		groundParams.m_fMaxRadius = CLW_SpotConfig.REQUEST_GROUND_M;
		groundParams.m_vFaceTarget = requester;
		CLW_SpotResult groundRes = new CLW_SpotResult();
		if (!FindFreeSpot(groundParams, groundRes))
			return false;

		spot = groundRes.m_vPos;
		yaw = groundRes.m_fYaw;
		via = "open-ground";
		return true;
	}

	// FOB site / relocation: clear ground for the command post cluster within FOB_REFINE_RADIUS_M of 'site' (roads preferred). avoid /
	// avoidDist are the required CENTRE distances. False when none; otherwise 'site' is the refined position.
	static bool RefineSite(string label, inout vector site, notnull array<vector> avoid, notnull array<float> avoidDist)
	{
		CLW_SpotParams p = new CLW_SpotParams();
		p.m_sLabel = label;
		p.m_vCenter = site;
		p.m_fMinRadius = 0;
		p.m_fMaxRadius = CLW_SpotConfig.FOB_REFINE_RADIUS_M;
		p.m_fHalfX = CLW_SpotConfig.FOB_CLUSTER_HALF_M;
		p.m_fHalfZ = CLW_SpotConfig.FOB_CLUSTER_HALF_M;
		p.m_fHeight = CLW_SpotConfig.FOB_CLUSTER_HEIGHT_M;
		p.m_fMaxHeightRange = 2 * 1.414 * CLW_SpotConfig.FOB_CLUSTER_HALF_M * 0.14;	// 14 % grade over the cluster diagonal (~4.8 m); 2.5 m rejected every Arland site (DS e2e 2026-10-12)
		p.m_fRoadWeight = 0.5;
		p.m_iMaxTries = 300;
		p.m_iValidTarget = 3;
		for (int i = 0; i < avoid.Count(); i++)
		{
			p.AddAvoid(avoid[i], avoidDist[i]);
		}

		CLW_SpotResult res = new CLW_SpotResult();
		if (!FindFreeSpot(p, res))
			return false;

		site = res.m_vPos;
		return true;
	}
}

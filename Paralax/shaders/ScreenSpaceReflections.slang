import Common.Fullscreen;
import Common.SceneData;
import Common.SurfacePacking;
import Common.PBR;

ConstantBuffer<ViewConstants> g_View : register(b0);
struct ReflectionConstants
{
	float4 Settings; // maximum distance, thickness, roughness cutoff, iteration bound
	uint4 Dimensions; // width, height, hierarchy count, reflection-only output
	uint4 Probe; // internal pixel coordinates for optional inspection only
};
[[vk::push_constant]] ConstantBuffer<ReflectionConstants> g_Reflection;
Texture2D<float2> g_Level0 : register(t0);
Texture2D<float2> g_Level1 : register(t1);
Texture2D<float2> g_Level2 : register(t2);
Texture2D<float2> g_Level3 : register(t3);
Texture2D<float2> g_Level4 : register(t4);
Texture2D<float2> g_Level5 : register(t5);
Texture2D<float2> g_Level6 : register(t6);
Texture2D<float2> g_Level7 : register(t7);
Texture2D<float2> g_Level8 : register(t8);
Texture2D<float2> g_Level9 : register(t9);
Texture2D<float2> g_Level10 : register(t10);
Texture2D<float2> g_Level11 : register(t11);
Texture2D<float2> g_Level12 : register(t12);
Texture2D<float2> g_Level13 : register(t13);
Texture2D<float2> g_Level14 : register(t14);
Texture2D<float2> g_Level15 : register(t15);
Texture2D<float> g_Depth : register(t16);
Texture2D<float4> g_Normals : register(t17);
Texture2D<float4> g_Receiver : register(t18);
Texture2D<float4> g_Source : register(t19);
Texture2D<float4> g_BaseMetal : register(t20);
Texture2D<float4> g_Environment : register(t21);
Texture2D<float2> g_BRDF : register(t22);
Texture2D<float4> g_ReflectionColor : register(t23);
SamplerState g_Linear : register(s0);

float2 LoadInterval(uint level, int2 pixel)
{
	switch (level)
	{
	case 0: return g_Level0.Load(int3(pixel, 0));
	case 1: return g_Level1.Load(int3(pixel, 0));
	case 2: return g_Level2.Load(int3(pixel, 0));
	case 3: return g_Level3.Load(int3(pixel, 0));
	case 4: return g_Level4.Load(int3(pixel, 0));
	case 5: return g_Level5.Load(int3(pixel, 0));
	case 6: return g_Level6.Load(int3(pixel, 0));
	case 7: return g_Level7.Load(int3(pixel, 0));
	case 8: return g_Level8.Load(int3(pixel, 0));
	case 9: return g_Level9.Load(int3(pixel, 0));
	case 10: return g_Level10.Load(int3(pixel, 0));
	case 11: return g_Level11.Load(int3(pixel, 0));
	case 12: return g_Level12.Load(int3(pixel, 0));
	case 13: return g_Level13.Load(int3(pixel, 0));
	case 14: return g_Level14.Load(int3(pixel, 0));
	case 15: return g_Level15.Load(int3(pixel, 0));
	default: return float2(-1);
	}
}

[shader("vertex")] FullscreenVertex ReflectionVS(uint vertexID : SV_VertexID)
{
	return MakeFullscreenVertex(vertexID);
}

float3 WorldAt(float2 pixel, float depth)
{
	float2 ndc = pixel / float2(g_Reflection.Dimensions.xy) * float2(2, -2) + float2(-1, 1);
	float4 world = mul(g_View.InverseViewProjection, float4(ndc, depth, 1));
	return world.xyz / world.w;
}

float ViewDepth(float2 pixel, float depth)
{
	float2 ndc = pixel / float2(g_Reflection.Dimensions.xy) * float2(2, -2) + float2(-1, 1);
	float4 world = mul(g_View.InverseViewProjection, float4(ndc, depth, 1));
	float numerator = -mul(g_View.View, world).z;
	return world.w == 0 && numerator > 0 ? 3.402823466e+38 : numerator / world.w;
}

float3 OrientedReceiver(int2 pixel)
{
	float3 geometric = DecodeSurfaceNormal(g_Receiver.Load(int3(pixel, 0)).xy);
	float3 preMap = DecodeSurfaceNormal(g_Normals.Load(int3(pixel, 0)).zw);
	// Raster derivatives carry screen handedness. Orient this consumer's hemisphere using the pre-map
	// surface normal, without changing the stored geometric/shadow/mapped normal contracts.
	return dot(geometric, preMap) < 0 ? -geometric : geometric;
}

bool ClipPlane(float start, float end, inout float first, inout float last)
{
	if (start < 0 && end < 0)
		return false;
	if (start < 0 || end < 0)
	{
		float crossing = start / (start - end);
		if (start < 0) first = max(first, crossing);
		else last = min(last, crossing);
	}
	return first <= last;
}

// Clipping in homogeneous space handles rays toward the camera and near-plane crossings before division.
bool ClipRay(float4 start, float4 end, out float first, out float last)
{
	first = 0; last = 1;
	return all(isfinite(start)) && all(isfinite(end)) &&
		ClipPlane(start.w - 1e-5, end.w - 1e-5, first, last) &&
		ClipPlane(start.z, end.z, first, last) && ClipPlane(start.w - start.z, end.w - end.z, first, last) &&
		ClipPlane(start.w + start.x, end.w + end.x, first, last) && ClipPlane(start.w - start.x, end.w - end.x, first, last) &&
		ClipPlane(start.w + start.y, end.w + end.y, first, last) && ClipPlane(start.w - start.y, end.w - end.y, first, last);
}

// The hierarchy still bounds sampled depths, not entire tangent planes. This local reconstruction
// is usable only in a covered, visited base cell; it never jumps to its projected intersection.
float4 CandidatePlane(float3 origin, float3 direction, int2 texel)
{
	float unavailable = asfloat(0x7fc00000u);
	float3 plane = float3(unavailable);
	uint flags = 0;
	float3 center = WorldAt(float2(texel) + 0.5, g_Depth.Load(int3(texel, 0)));
	float3 normal = OrientedReceiver(texel);
	float denominator = dot(normal, direction);
	if (all(isfinite(center)) && all(isfinite(normal)) && isfinite(denominator) && abs(denominator) > 1e-6)
	{
		float distance = dot(center - origin, normal) / denominator;
		float3 point = origin + direction * distance;
		float4 clip = mul(g_View.ViewProjection, float4(point, 1));
		if (isfinite(distance) && distance > 0 && all(isfinite(clip)) && abs(clip.w) > 1e-6)
		{
			float2 pixel = (clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * float2(g_Reflection.Dimensions.xy);
			if (all(isfinite(pixel)))
			{
				plane = float3(pixel, distance); flags |= 1;
				if (clip.w > 0 && clip.z >= 0 && clip.z <= clip.w && all(abs(clip.xy) <= clip.w)) flags |= 2;
				if (distance > 0.001 && distance <= g_Reflection.Settings.x) flags |= 4;
				// Half-open texel ownership agrees with Load/floor and never borrows the outgoing neighbor.
				if (all(floor(pixel) == float2(texel))) flags |= 8;
			}
		}
	}
	return float4(plane, float(flags));
}

struct TraceResult
{
	float2 Pixel;
	int2 Texel;
	float3 World;
	float3 RayOrigin, RayDirection;
	int2 TerminationCell;
	uint Termination, RefinementCells;
	float Distance;
	float Residual, Facing, FilterWeight;
	uint Status; // 0 receiver rejection, 1 miss, 2 exhausted, 3 filter rejection, 4 no confidence, 5 hit, 6 approximate hit at iteration limit
	bool Candidate;
	bool Hit;
	bool PlaneVerified;
};

TraceResult Terminated(TraceResult result, uint reason, uint cells, int2 cell)
{
	result.Termination = reason; result.RefinementCells = cells; result.TerminationCell = cell;
	return result;
}

// A shared-boundary match is a local continuity heuristic, not proof of object identity.
// At most one secondary plane is frozen; the first approximate anchor never moves.
bool MatchingFacetBoundary(float2 pixel, float3 anchorPoint, float3 anchorNormal, float3 center, float3 normal, float3 direction)
{
	if (dot(anchorNormal, normal) <= 0 || dot(anchorNormal, direction) >= -0.01 || dot(normal, direction) >= -0.01) return false;
	float3 nearPoint = WorldAt(pixel, 1.0);
	float3 viewDirection = normalize(WorldAt(pixel, 0.5) - nearPoint);
	if (!all(isfinite(nearPoint)) || !all(isfinite(viewDirection))) return false;
	float2 denominator = float2(dot(viewDirection, anchorNormal), dot(viewDirection, normal));
	if (!all(isfinite(denominator)) || any(abs(denominator) <= 1e-6)) return false;
	float2 distance = float2(dot(anchorPoint - nearPoint, anchorNormal), dot(center - nearPoint, normal)) / denominator;
	if (!all(isfinite(distance)) || any(distance < 0)) return false;
	return abs(distance.x - distance.y) <= max(1e-4, g_Reflection.Settings.y * 0.05);
}

TraceResult Trace(float3 origin, float3 direction, int2 receiverPixel)
{
	TraceResult result = {};
	result.Texel = result.TerminationCell = int2(-1);
	result.RayOrigin = origin; result.RayDirection = direction;
	result.Status = 1;
	float distanceLimit = g_Reflection.Settings.x;
	float4 clip0 = mul(g_View.ViewProjection, float4(origin, 1));
	float4 clip1 = mul(g_View.ViewProjection, float4(origin + direction * distanceLimit, 1));
	float first, last;
	if (!ClipRay(clip0, clip1, first, last)) return result;
	float4 start = lerp(clip0, clip1, first), end = lerp(clip0, clip1, last);
	float3 ndc0 = start.xyz / start.w, ndc1 = end.xyz / end.w;
	float2 size = float2(g_Reflection.Dimensions.xy);
	float2 p0 = (ndc0.xy * float2(0.5, -0.5) + 0.5) * size;
	float2 p1 = (ndc1.xy * float2(0.5, -0.5) + 0.5) * size;
	float2 delta = p1 - p0;
	float extent = max(abs(delta.x), abs(delta.y));
	if (!all(isfinite(delta)) || extent < 1e-5) return result;
	float advance = 1e-3 / max(extent, 1.0);
	float cursor = advance;
	float cellEntry = 0;
	bool entryValid = false;
	TraceResult approximate = {};
	float3 anchorPoint = float3(0), anchorNormal = float3(0);
	float3 facetPoint = float3(0), facetNormal = float3(0);
	bool connectedFacet = false;
	int2 previousCell = int2(-1);
	uint refinementCells = 0;
	int2 terminationCell = int2(-1);
	uint level = g_Reflection.Dimensions.z - 1;
	uint iteration = 0;
	[loop] for (; iteration < uint(g_Reflection.Settings.w) && cursor < 1; ++iteration)
	{
		float2 pixel = p0 + delta * cursor;
		terminationCell = int2(floor(pixel));
		if (any(pixel < 0) || any(pixel >= size)) break;
		float span = float(1u << level);
		int2 cell = int2(floor(pixel / span));
		float2 boundary = float2(delta.x > 0 ? float(cell.x + 1) * span : float(cell.x) * span,
			delta.y > 0 ? float(cell.y + 1) * span : float(cell.y) * span);
		boundary = clamp(boundary, float2(0), size);
		float2 crossing = float2(abs(delta.x) > 1e-8 ? (boundary.x - p0.x) / delta.x : 2,
			abs(delta.y) > 1e-8 ? (boundary.y - p0.y) / delta.y : 2);
		float cellEnd = min(1.0, min(crossing.x, crossing.y));
		if (!isfinite(cellEnd) || cellEnd <= cursor)
		{
			cursor += advance;
			entryValid = false;
			continue;
		}
		float2 interval = LoadInterval(level, cell);
		if (approximate.Hit)
		{
			// Refinement walks contiguous base cells, anchored to the FIRST approximate surface.
			// One certified facet can use the remaining cell budget, with both plane anchors frozen.
			int2 priorCell = previousCell;
			if (any(cell != previousCell))
			{
				if (refinementCells >= 4) return Terminated(approximate, 5, refinementCells, cell);
				++refinementCells;
				previousCell = cell;
			}
			float3 center = WorldAt(float2(cell) + 0.5, interval.x);
			float3 normal = OrientedReceiver(cell);
			if (interval.x < 0 || all(cell == receiverPixel)) return Terminated(approximate, 2, refinementCells, cell);
			if (!all(isfinite(normal))) return Terminated(approximate, 3, refinementCells, cell);
			int2 separation = abs(cell - priorCell);
			bool adjacent = entryValid && separation.x + separation.y == 1;
			if (connectedFacet && (!adjacent || dot(normal, direction) >= -0.01))
				return Terminated(approximate, 3, refinementCells, cell);
			bool transition = false;
			if (dot(normal, connectedFacet ? facetNormal : anchorNormal) < 0.99)
			{
				if (connectedFacet || !adjacent ||
					!MatchingFacetBoundary(p0 + delta * cellEntry, anchorPoint, anchorNormal, center, normal, direction))
					return Terminated(approximate, 3, refinementCells, cell);
				transition = true;
			}
			float tolerance = max(1e-4, g_Reflection.Settings.y * 0.05);
			if (!all(isfinite(center)) || abs(dot(center - anchorPoint, anchorNormal)) > tolerance ||
				(connectedFacet && abs(dot(center - facetPoint, facetNormal)) > tolerance))
				return Terminated(approximate, transition ? 3 : 4, refinementCells, cell);
			if (transition)
			{
				facetPoint = center; facetNormal = normal; connectedFacet = true;
			}
		}
		float rayA = ViewDepth(pixel, lerp(ndc0.z, ndc1.z, cursor));
		float rayB = ViewDepth(p0 + delta * cellEnd, lerp(ndc0.z, ndc1.z, cellEnd));
		float nearDepth = interval.x >= 0 ? ViewDepth(pixel, interval.x) : 0;
		float farDepth = interval.x >= 0 ? ViewDepth(pixel, interval.y) : 0;
		bool overlap = interval.x >= 0 && all(isfinite(float4(rayA, rayB, nearDepth, farDepth))) &&
			max(rayA, rayB) >= nearDepth - g_Reflection.Settings.y && min(rayA, rayB) <= farDepth + g_Reflection.Settings.y;
		if (overlap && level > 0)
		{
			--level;
			continue;
		}
		if (overlap && any(cell != receiverPixel))
		{
			float zDelta = ndc1.z - ndc0.z;
			// Reversed-Z deltas scale with the near plane; a small nonzero delta can still span a real hit.
			float root = zDelta != 0 ? (interval.x - ndc0.z) / zDelta : cursor;
			float hitCursor = clamp(root, cursor, cellEnd);
			float2 hitPixel = p0 + delta * hitCursor;
			float rayDepth = ViewDepth(hitPixel, lerp(ndc0.z, ndc1.z, hitCursor));
			float3 world = WorldAt(hitPixel, interval.x);
			float3 rayPoint = WorldAt(hitPixel, lerp(ndc0.z, ndc1.z, hitCursor));
			float3 hitNormal = OrientedReceiver(cell);
			float alongRay = dot(rayPoint - origin, direction);
			result.Candidate = true; result.Texel = cell; result.Distance = alongRay;
			result.Residual = rayDepth - nearDepth; result.Facing = dot(hitNormal, direction);
			if (all(isfinite(world)) && all(isfinite(rayPoint)) && abs(rayDepth - nearDepth) <= g_Reflection.Settings.y &&
				alongRay > 0.001 && alongRay <= distanceLimit && dot(hitNormal, direction) < -0.01)
			{
				result.Pixel = hitPixel; result.Texel = cell; result.World = world; result.Distance = alongRay; result.Hit = true; result.Status = 5;
				float4 plane = CandidatePlane(origin, direction, cell);
				if (uint(plane.w) == 15)
				{
					float planeCursor = abs(delta.x) >= abs(delta.y) ? (plane.x - p0.x) / delta.x : (plane.y - p0.y) / delta.y;
					if (isfinite(planeCursor) && planeCursor >= cursor && planeCursor <= cellEnd)
					{
						result.Pixel = plane.xy;
						result.World = origin + direction * plane.z;
						result.Distance = plane.z;
						result.Residual = -mul(g_View.View, float4(result.World, 1)).z - ViewDepth(plane.xy, interval.x);
						result.PlaneVerified = true;
						return Terminated(result, 1, refinementCells, cell);
					}
				}
				// Unsupported planes keep the original first scalar candidate and confidence policy.
				if (!approximate.Hit)
				{
					approximate = result;
					anchorPoint = WorldAt(float2(cell) + 0.5, interval.x);
					anchorNormal = hitNormal;
					previousCell = cell;
					if (!all(isfinite(anchorPoint)) || !all(isfinite(anchorNormal))) return Terminated(approximate, 4, refinementCells, cell);
				}
			}
		}
		cellEntry = cellEnd;
		entryValid = true;
		cursor = cellEnd + advance;
		// An overlapping base cell already exhausted its candidates. Advance to the next cell
		// without spending the next iteration descending through the same coarse overlap.
		level = approximate.Hit || overlap ? 0 : min(level + 1, g_Reflection.Dimensions.z - 1);
	}
	if (approximate.Hit)
	{
		if (iteration >= uint(g_Reflection.Settings.w) && cursor < 1) approximate.Status = 6;
		return Terminated(approximate, approximate.Status == 6 ? 7 : 6, refinementCells, terminationCell);
	}
	if (iteration >= uint(g_Reflection.Settings.w) && cursor < 1) result.Status = 2;
	return Terminated(result, result.Status == 2 ? 7 : 6, 0, terminationCell);
}

struct ReflectionTargets
{
	float4 Color : SV_Target0;
	float4 Hit : SV_Target1;
};

ReflectionTargets EvaluateReflection(float2 samplePosition, out TraceResult diagnostic)
{
	ReflectionTargets output = {};
	diagnostic = {}; diagnostic.Texel = diagnostic.TerminationCell = int2(-1);
	int2 pixel = int2(samplePosition);
	float4 receiver = g_Receiver.Load(int3(pixel, 0));
	float roughness = receiver.z;
	if (roughness <= 0 || roughness >= g_Reflection.Settings.z || g_View.Diagnostics.x <= 0) return output;
	float depth = g_Depth.Load(int3(pixel, 0));
	float3 position = WorldAt(samplePosition, depth);
	if (!all(isfinite(position))) return output;
	float3 normal = DecodeSurfaceNormal(g_Normals.Load(int3(pixel, 0)).xy);
	float3 geometric = OrientedReceiver(pixel);
	bool orthographic = abs(g_View.Projection[3][3]) > 0.5;
	float3 view = orthographic ? normalize(g_View.View[2].xyz) : normalize(g_View.CameraPosition.xyz - position);
	float3 direction = reflect(-view, normal);
	if (!all(isfinite(direction)) || dot(direction, geometric) <= 0.01) return output;
	float epsilon = min(0.01, g_Reflection.Settings.y * 0.1);
	diagnostic = Trace(position + geometric * epsilon, direction, pixel);
	TraceResult hit = diagnostic;
	if (!hit.Hit) return output;
	int2 hitTexel = hit.Texel;
	float3 hitNormal = DecodeSurfaceNormal(g_Receiver.Load(int3(hitTexel, 0)).xy);
	float3 hitMapped = DecodeSurfaceNormal(g_Normals.Load(int3(hitTexel, 0)).xy);
	float hitDepth = ViewDepth(hit.Pixel, g_Depth.Load(int3(hitTexel, 0)));
	float footprint = min(8.0, roughness * roughness * hit.Distance * abs(g_View.Projection[1][1]) *
		float(g_Reflection.Dimensions.y) * 0.5 / (orthographic ? 1.0 : max(hitDepth, 0.001)));
	float3 radiance = float3(0);
	float weight = 0;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			int2 tap = int2(floor(float2(hitTexel) + 0.5 + float2(x, y) * footprint));
			if (any(tap < 0) || any(tap >= int2(g_Reflection.Dimensions.xy))) continue;
			float4 tapGuide = g_Receiver.Load(int3(tap, 0));
			float tapDepth = ViewDepth(float2(tap) + 0.5, g_Depth.Load(int3(tap, 0)));
			if (tapGuide.z <= 0 || !isfinite(tapDepth) || abs(tapDepth - hitDepth) > g_Reflection.Settings.y ||
				dot(DecodeSurfaceNormal(tapGuide.xy), hitNormal) < 0.9 ||
				dot(DecodeSurfaceNormal(g_Normals.Load(int3(tap, 0)).xy), hitMapped) < 0.9) continue;
			float w = float((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1));
			float3 value = g_Source.Load(int3(tap, 0)).rgb;
			if (!all(isfinite(value))) continue;
			radiance += value * w; weight += w;
		}
	diagnostic.FilterWeight = weight;
	if (weight <= 0) { diagnostic.Status = 3; return output; }
	radiance /= weight;
	float4 baseMetal = g_BaseMetal.Load(int3(pixel, 0));
	// Match the existing material-lighting view convention for the replaced split-sum term.
	float3 materialView = normalize(g_View.CameraPosition.xyz - position);
	float nDotV = max(dot(normal, materialView), 1e-4);
	float3 f0 = lerp(float3(0.04), baseMetal.rgb, baseMetal.a);
	float2 brdf = g_BRDF.SampleLevel(g_Linear, float2(nDotV, roughness), 0);
	float3 specular = EvaluateIndirectSpecular(radiance, f0, brdf, nDotV, roughness, receiver.w);
	float2 uv = hit.Pixel / float2(g_Reflection.Dimensions.xy);
	float edge = saturate(min(min(uv.x, uv.y), min(1 - uv.x, 1 - uv.y)) / 0.05);
	float distanceFade = saturate((g_Reflection.Settings.x - hit.Distance) / (0.1 * g_Reflection.Settings.x));
	float roughnessFade = 1 - smoothstep(g_Reflection.Settings.z * 0.5, g_Reflection.Settings.z, roughness);
	// A clamped depth-slab intersection is approximate. Fade continuously at the tolerance boundary;
	// an exact depth crossing retains the original confidence and reflected energy.
	float depthAgreement = hit.PlaneVerified ? 1.0 : 1 - smoothstep(0.0, g_Reflection.Settings.y, abs(hit.Residual));
	float confidence = edge * distanceFade * roughnessFade * depthAgreement;
	if (!all(isfinite(specular)) || confidence <= 0) { diagnostic.Status = 4; return output; }
	output.Color = float4(max(specular, float3(0)), confidence);
	output.Hit = float4(hit.World, 1);
	return output;
}

[shader("fragment")] ReflectionTargets TracePS(FullscreenVertex input)
{
	TraceResult diagnostic;
	return EvaluateReflection(input.Position.xy, diagnostic);
}

// Inspection reads texel-center geometry, not the ray intersection or a later refinement anchor.
bool InspectionGeometry(int2 texel, out float3 center, out float3 normal)
{
	center = normal = float3(asfloat(0x7fc00000u));
	if (any(texel < 0) || any(texel >= int2(g_Reflection.Dimensions.xy))) return false;
	float4 guide = g_Receiver.Load(int3(texel, 0));
	float depth = g_Depth.Load(int3(texel, 0));
	if (!all(isfinite(guide)) || guide.z <= 0 || !isfinite(depth) || depth < 0 || depth > 1) return false;
	float3 preMap = DecodeSurfaceNormal(g_Normals.Load(int3(texel, 0)).zw);
	if (!all(isfinite(preMap))) return false;
	float3 point = WorldAt(float2(texel) + 0.5, depth);
	float3 oriented = OrientedReceiver(texel);
	if (!all(isfinite(point)) || !all(isfinite(oriented))) return false;
	center = point; normal = oriented;
	return true;
}

[shader("fragment")] ReflectionTargets InspectionPS(FullscreenVertex input)
{
	TraceResult diagnostic;
	ReflectionTargets traced = EvaluateReflection(float2(g_Reflection.Probe.xy) + 0.5, diagnostic);
	float unavailable = asfloat(0x7fc00000u);
	ReflectionTargets output;
	if (g_Reflection.Probe.z == 2)
	{
		float3 candidateCenter, candidateNormal, terminationCenter, terminationNormal;
		bool candidateValid = InspectionGeometry(diagnostic.Candidate ? diagnostic.Texel : int2(-1), candidateCenter, candidateNormal);
		bool terminationValid = InspectionGeometry(diagnostic.TerminationCell, terminationCenter, terminationNormal);
		float normalDot = unavailable, planeOffset = unavailable;
		if (candidateValid && terminationValid)
		{
			normalDot = dot(candidateNormal, terminationNormal);
			float offset = dot(terminationCenter - candidateCenter, candidateNormal);
			if (isfinite(offset)) planeOffset = offset;
		}
		output.Color = float4(candidateNormal, normalDot);
		output.Hit = float4(terminationNormal, planeOffset);
		return output;
	}
	if (g_Reflection.Probe.z == 1)
	{
		// Hypothetical evidence remains separate from whether this plane was accepted by production tracing.
		float4 plane = float4(float3(unavailable), 0);
		if (diagnostic.Candidate) plane = CandidatePlane(diagnostic.RayOrigin, diagnostic.RayDirection, diagnostic.Texel);
		uint flags = uint(plane.w) | (diagnostic.Hit ? 16 : 0) | (diagnostic.PlaneVerified ? 32 : 0);
		output.Color = float4(plane.xyz, float(flags));
		output.Hit = float4(float(diagnostic.Termination), float(diagnostic.RefinementCells), float2(diagnostic.TerminationCell));
		return output;
	}
	output.Color = float4(diagnostic.Candidate ? float2(diagnostic.Texel) : float2(unavailable),
		diagnostic.Candidate ? diagnostic.Distance : unavailable, traced.Color.a);
	output.Hit = float4(diagnostic.Candidate ? diagnostic.Residual : unavailable,
		diagnostic.Candidate ? diagnostic.Facing : unavailable, float(diagnostic.Status),
		diagnostic.Hit ? diagnostic.FilterWeight : unavailable);
	return output;
}

[shader("fragment")] float4 ComposePS(FullscreenVertex input) : SV_Target
{
	int3 location = int3(int2(input.Position.xy), 0);
	float4 source = g_Source.Load(location);
	float4 reflection = g_ReflectionColor.Load(location);
	if (g_Reflection.Dimensions.w != 0) return float4(reflection.rgb * reflection.a, 1);
	if (reflection.a <= 0) return source;
	return float4(max(source.rgb + reflection.a * (reflection.rgb - g_Environment.Load(location).rgb), float3(0)), source.a);
}

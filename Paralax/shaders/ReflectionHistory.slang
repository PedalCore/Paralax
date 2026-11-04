import Common.Fullscreen;
import Common.SceneData;
import Common.SurfacePacking;

ConstantBuffer<ViewConstants> g_View : register(b0);
struct HistoryParameters
{
	float4x4 PreviousRaster;
	float4x4 PreviousView;
	float4 Settings; // weight, thickness, valid history, unused
	uint4 Dimensions;
};
StructuredBuffer<HistoryParameters> g_Parameters : register(t0);
Texture2D<float4> g_Current : register(t1);
Texture2D<float> g_Depth : register(t2);
Texture2D<float4> g_Normals : register(t3);
Texture2D<float4> g_Receiver : register(t4);
Texture2D<float4> g_Hit : register(t5);
Texture2D<float4> g_PreviousColor : register(t6);
Texture2D<float4> g_PreviousNormal : register(t7);
Texture2D<float> g_PreviousDepth : register(t8);
Texture2D<float4> g_PreviousHit : register(t9);

[shader("vertex")] FullscreenVertex HistoryVS(uint vertexID : SV_VertexID)
{
	return MakeFullscreenVertex(vertexID);
}

struct HistoryOutput
{
	float4 Color : SV_Target0;
	float4 Normal : SV_Target1;
	float Depth : SV_Target2;
	float4 Hit : SV_Target3;
};

// Reprojecting to a fractional pixel needs the same local surface evaluated there, not at its texel center.
// Both opposite neighbors must support a consistent plane; silhouettes and depth steps keep zero slope.
bool HistoryNeighbor(int2 pixel, uint2 size, float4 centerNormal, out float depth, out float3 hit)
{
	depth = 0; hit = float3(0);
	if (any(pixel < 0) || any(pixel >= int2(size))) return false;
	int3 location = int3(pixel, 0);
	float4 normal = g_PreviousNormal.Load(location);
	float4 point = g_PreviousHit.Load(location);
	float4 color = g_PreviousColor.Load(location);
	depth = g_PreviousDepth.Load(location); hit = point.xyz;
	return normal.a > 0 && point.a > 0 && color.a > 0 && all(isfinite(normal)) && all(isfinite(point)) && all(isfinite(color)) && isfinite(depth) &&
		abs(normal.z - centerNormal.z) <= 0.025 && dot(DecodeSurfaceNormal(normal.xy), DecodeSurfaceNormal(centerNormal.xy)) >= 0.95;
}

void CorrectPreviousSurface(int2 pixel, float2 offset, uint2 size, float4 normal, float tolerance, bool orthographic, inout float depth, inout float3 hit)
{
	if (!orthographic && depth <= 1e-6) return;
	float coordinate = orthographic ? depth : rcp(depth);
	float coordinateTolerance = orthographic ? tolerance : tolerance / max(depth * depth, 1e-12);
	float correction = 0;
	float3 hitCorrection = float3(0);
	[unroll] for (int axis = 0; axis < 2; ++axis)
	{
		int2 step = axis == 0 ? int2(1, 0) : int2(0, 1);
		float beforeDepth, afterDepth;
		float3 beforeHit, afterHit;
		if (!HistoryNeighbor(pixel - step, size, normal, beforeDepth, beforeHit) || !HistoryNeighbor(pixel + step, size, normal, afterDepth, afterHit)) continue;
		if (!orthographic && (beforeDepth <= 1e-6 || afterDepth <= 1e-6)) continue;
		float before = coordinate - (orthographic ? beforeDepth : rcp(beforeDepth));
		float after = (orthographic ? afterDepth : rcp(afterDepth)) - coordinate;
		float3 beforeHitSlope = hit - beforeHit, afterHitSlope = afterHit - hit;
		// An absolute thickness bound alone would mistake a shallow step for a plane.
		if (abs(before - after) > 2 * coordinateTolerance || abs(before - after) > 0.1 * max(abs(before), abs(after)) ||
			distance(beforeHitSlope, afterHitSlope) > 2 * tolerance ||
			distance(beforeHitSlope, afterHitSlope) > 0.1 * max(length(beforeHitSlope), length(afterHitSlope))) continue;
		correction += 0.5 * (before + after) * offset[axis];
		hitCorrection += 0.5 * (beforeHitSlope + afterHitSlope) * offset[axis];
	}
	float corrected = coordinate + correction;
	if (!isfinite(corrected) || (!orthographic && corrected <= 1e-20) || !all(isfinite(hit + hitCorrection))) return;
	float correctedDepth = orthographic ? corrected : rcp(corrected);
	if (!isfinite(correctedDepth)) return;
	depth = correctedDepth; hit += hitCorrection;
}

[shader("fragment")] HistoryOutput HistoryPS(FullscreenVertex input)
{
	HistoryOutput output = {};
	HistoryParameters parameters = g_Parameters[0];
	int2 pixel = int2(input.Position.xy);
	float4 current = g_Current.Load(int3(pixel, 0));
	float4 hit = g_Hit.Load(int3(pixel, 0));
	// History must never revive a current-frame miss, including uncovered and offscreen sources.
	if (current.a <= 0 || hit.a <= 0 || !all(isfinite(current)) || !all(isfinite(hit))) return output;
	float4 receiver = g_Receiver.Load(int3(pixel, 0));
	float2 normal = g_Normals.Load(int3(pixel, 0)).xy;
	if (!all(isfinite(receiver)) || receiver.z <= 0 || !all(isfinite(normal))) return output;
	float2 ndc = input.Position.xy / float2(parameters.Dimensions.xy) * float2(2, -2) + float2(-1, 1);
	float4 homogeneous = mul(g_View.InverseViewProjection, float4(ndc, g_Depth.Load(int3(pixel, 0)), 1));
	float3 world = homogeneous.xyz / homogeneous.w;
	if (!all(isfinite(world))) return output;
	output.Color = current;
	output.Normal = float4(normal, receiver.z, 1);
	output.Depth = -mul(g_View.View, float4(world, 1)).z;
	output.Hit = hit;
	if (parameters.Settings.z <= 0) return output;
	float4 previousClip = mul(parameters.PreviousRaster, float4(world, 1));
	if (!all(isfinite(previousClip)) || previousClip.w <= 0) return output;
	float3 previousNDC = previousClip.xyz / previousClip.w;
	float2 previousPixel = (previousNDC.xy * float2(0.5, -0.5) + 0.5) * float2(parameters.Dimensions.xy);
	if (any(previousPixel < 0) || any(previousPixel >= float2(parameters.Dimensions.xy)) || previousNDC.z < 0 || previousNDC.z > 1) return output;
	int3 location = int3(int2(previousPixel), 0);
	float4 oldColor = g_PreviousColor.Load(location);
	float4 oldNormal = g_PreviousNormal.Load(location);
	float4 oldHit = g_PreviousHit.Load(location);
	float oldDepth = g_PreviousDepth.Load(location);
	float expectedDepth = -mul(parameters.PreviousView, float4(world, 1)).z;
	float tolerance = max(0.02, parameters.Settings.y);
	if (oldColor.a <= 0 || oldNormal.a <= 0 || oldHit.a <= 0 || !all(isfinite(oldColor)) || !all(isfinite(oldNormal)) || !all(isfinite(oldHit)) || !isfinite(oldDepth)) return output;
	float3 correctedHit = oldHit.xyz;
	CorrectPreviousSurface(location.xy, previousPixel - (float2(location.xy) + 0.5), parameters.Dimensions.xy, oldNormal, tolerance,
		abs(g_View.Projection[3][3]) > 0.5, oldDepth, correctedHit);
	if (abs(oldDepth - expectedDepth) > tolerance || abs(oldNormal.z - receiver.z) > 0.025 ||
		dot(DecodeSurfaceNormal(oldNormal.xy), DecodeSurfaceNormal(normal)) < 0.95 || distance(correctedHit, hit.xyz) > tolerance * 2) return output;
	float3 low = current.rgb, high = current.rgb;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			int2 tap = pixel + int2(x, y);
			if (any(tap < 0) || any(tap >= int2(parameters.Dimensions.xy))) continue;
			float4 value = g_Current.Load(int3(tap, 0));
			if (value.a <= 0 || !all(isfinite(value))) continue;
			low = min(low, value.rgb); high = max(high, value.rgb);
		}
	output.Color.rgb = lerp(current.rgb, clamp(oldColor.rgb, low, high), parameters.Settings.x);
	return output;
}

Texture2D<float4> g_Source : register(t10);
Texture2D<float4> g_Environment : register(t11);
Texture2D<float4> g_Filtered : register(t12);
[[vk::push_constant]] ConstantBuffer<uint4> g_Compose;

[shader("fragment")] float4 ComposePS(FullscreenVertex input) : SV_Target
{
	int3 pixel = int3(int2(input.Position.xy), 0);
	float4 reflection = g_Filtered.Load(pixel);
	if (g_Compose.x != 0) return float4(reflection.rgb * reflection.a, 1);
	float4 source = g_Source.Load(pixel);
	if (reflection.a <= 0) return source;
	return float4(max(source.rgb + reflection.a * (reflection.rgb - g_Environment.Load(pixel).rgb), float3(0)), source.a);
}

import Common.Fullscreen;
import Common.SurfacePacking;
import Common.EdgeReconstruction;

struct ResolveConstants
{
	float4x4 InverseProjection;
	float4 JitterHistory;
	uint4 Size;
};
[[vk::push_constant]] ConstantBuffer<ResolveConstants> g_Resolve;
Texture2D<float4> g_Current : register(t0);
Texture2D<float> g_Depth : register(t1);
Texture2D<float2> g_Normal : register(t2);
Texture2D<uint> g_ID : register(t3);
Texture2D<float2> g_PreviousUV : register(t4);
Texture2D<float> g_PreviousDepth : register(t5);
Texture2D<float> g_Reactive : register(t6);
Texture2D<float4> g_HistoryColor : register(t7);
Texture2D<float4> g_HistoryMetadata : register(t8);
Texture2D<uint> g_HistoryID : register(t9);
Texture2D<float> g_HistoryDepth : register(t10);

[shader("vertex")] FullscreenVertex ResolveVS(uint vertex : SV_VertexID) {
	return MakeFullscreenVertex(vertex);
}

struct CurrentSample
{
	float3 Color;
	float3 Normal;
	float Depth;
	float2 PreviousUV;
	float PreviousDepth;
	uint ID;
	bool Reactive;
};

int2 Bounded(int2 pixel)
{
	return clamp(pixel, int2(0), int2(g_Resolve.Size.xy) - 1);
}

CurrentSample Current(int2 coordinate)
{
	int2 pixel = Bounded(coordinate);
	CurrentSample result;
	float3 color = g_Current.Load(int3(pixel, 0)).rgb;
	bool finiteColor = all(isfinite(color));
	result.Color = finiteColor ? color : float3(0);
	uint rawID = g_ID.Load(int3(pixel, 0));
	result.ID = rawID & 0x7fffffffu;
	result.Normal = DecodeSurfaceNormal(g_Normal.Load(int3(pixel, 0)));
	float2 uv = (float2(pixel) + 0.5) / float2(g_Resolve.Size.xy);
	float4 view = mul(g_Resolve.InverseProjection, float4(uv.x * 2 - 1, 1 - uv.y * 2, g_Depth.Load(int3(pixel, 0)), 1));
	bool validDepth = all(isfinite(view)) && abs(view.w) > 1e-8;
	result.Depth = validDepth ? -view.z / view.w : 0;
	result.PreviousUV = g_PreviousUV.Load(int3(pixel, 0));
	result.PreviousDepth = g_PreviousDepth.Load(int3(pixel, 0));
	result.Reactive = !finiteColor || !validDepth || (rawID & 0x80000000u) != 0 || g_Reactive.Load(int3(pixel, 0)) > 0;
	result.Reactive = result.Reactive || !all(isfinite(result.Normal)) || !all(isfinite(result.PreviousUV)) || !isfinite(result.PreviousDepth);
	return result;
}

bool CompatibleDepth(float a, float b)
{
	return isfinite(a) && isfinite(b) && abs(a - b) <= 0.01 + 0.02 * max(abs(a), abs(b));
}

bool SameSurface(CurrentSample a, CurrentSample b)
{
	return a.ID == b.ID && (a.ID == 0 || (CompatibleDepth(a.Depth, b.Depth) && dot(a.Normal, b.Normal) >= 0.9));
}

float3 ToYCoCg(float3 color)
{
	return float3(dot(color, float3(0.25, 0.5, 0.25)), (color.r - color.b) * 0.5, dot(color, float3(-0.25, 0.5, -0.25)));
}

float3 FromYCoCg(float3 color)
{
	return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

float3 ClipHistory(float3 current, float3 history, float3 low, float3 high)
{
	// Clip the segment from current to history rather than independent channels. Both endpoints
	// are valid RGB colors, so clipping cannot introduce negative RGB from a chroma-box corner.
	float3 delta = history - current;
	float extent = 1;
	[unroll] for (uint channel = 0; channel < 3; ++channel)
	{
		if (delta[channel] > 0)
			extent = min(extent, (high[channel] - current[channel]) / delta[channel]);
		else if (delta[channel] < 0)
			extent = min(extent, (low[channel] - current[channel]) / delta[channel]);
	}
	return current + delta * saturate(extent);
}

struct ResolvedTargets
{
	float4 Color : SV_Target0;
	float4 Metadata : SV_Target1;
	uint ID : SV_Target2;
	float Depth : SV_Target3;
};

[shader("fragment")] ResolvedTargets ResolvePS(FullscreenVertex input) {
	float2 raster = floor(input.Position.xy) + g_Resolve.JitterHistory.xy;
	int2 first = int2(floor(raster));
	float2 fraction = frac(raster);
	CurrentSample samples[4];
	float weights[4];
	uint representative = 0;
	float greatest = -1;
	float3 color = float3(0);
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		uint2 offset = uint2(i & 1, i >> 1);
		weights[i] = (offset.x != 0 ? fraction.x : 1 - fraction.x) * (offset.y != 0 ? fraction.y : 1 - fraction.y);
		samples[i] = Current(first + int2(offset));
		color += samples[i].Color * weights[i];
		if (weights[i] > greatest)
		{
			greatest = weights[i];
			representative = i;
		}
	}
	CurrentSample surface = samples[representative];
	bool reactive = surface.Reactive || surface.ID == 0;
	bool mixed = false;
	float3 normal = float3(0);
	float depth = 0, expectedDepth = 0;
	float2 previousUV = float2(0);
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		if (weights[i] <= 0)
			continue;
		mixed = mixed || !SameSurface(surface, samples[i]);
		reactive = reactive || samples[i].Reactive;
		normal += samples[i].Normal * weights[i];
		depth += samples[i].Depth * weights[i];
		previousUV += samples[i].PreviousUV * weights[i];
		expectedDepth += samples[i].PreviousDepth * weights[i];
	}
	if (!mixed && surface.ID != 0)
	{
		surface.Normal = normalize(normal);
		surface.Depth = depth;
		surface.PreviousUV = previousUV;
		surface.PreviousDepth = expectedDepth;
	}
	// Reconstruct residual silhouettes from this frame only. Interior material samples
	// retain temporal accumulation and its detail; the wider color footprint never
	// authorizes history from another object or a transparent/mixed edge.
	int2 centerPixel = first + int2(representative & 1, representative >> 1);
	bool boundary = mixed;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
			boundary = boundary || !SameSurface(surface, Current(centerPixel + int2(x, y)));
	if (boundary)
	{
		bool reconstructed;
		float3 currentEdge = ReconstructEdge(g_Current, raster, int2(g_Resolve.Size.xy), 0, reconstructed);
		if (reconstructed)
		{
			color = currentEdge;
			reactive = true;
		}
	}
	reactive = reactive || mixed;
	if (!reactive && g_Resolve.JitterHistory.w > 0.5)
	{
		float2 historyPixel = surface.PreviousUV * float2(g_Resolve.Size.xy) - 0.5;
		int2 historyFirst = int2(floor(historyPixel));
		float2 historyFraction = frac(historyPixel);
		float3 history = float3(0);
		float acceptedWeight = 0;
		[unroll] for (uint i = 0; i < 4; ++i)
		{
			uint2 offset = uint2(i & 1, i >> 1);
			int2 pixel = historyFirst + int2(offset);
			float weight = (offset.x != 0 ? historyFraction.x : 1 - historyFraction.x) * (offset.y != 0 ? historyFraction.y : 1 - historyFraction.y);
			if (weight <= 0 || any(pixel < 0) || any(pixel >= int2(g_Resolve.Size.xy)))
				continue;
			uint id = g_HistoryID.Load(int3(pixel, 0));
			float4 metadata = g_HistoryMetadata.Load(int3(pixel, 0));
			float oldDepth = g_HistoryDepth.Load(int3(pixel, 0));
			float3 oldColor = g_HistoryColor.Load(int3(pixel, 0)).rgb;
			// Every history tap is validated before interpolation, including stored mixed-edge reactivity.
			if (id != surface.ID || metadata.z > 0.5 || !all(isfinite(metadata)) || !all(isfinite(oldColor)) ||
				!CompatibleDepth(oldDepth, surface.PreviousDepth) || dot(DecodeSurfaceNormal(metadata.xy), surface.Normal) < 0.9)
				continue;
			history += oldColor * weight;
			acceptedWeight += weight;
		}
		if (acceptedWeight > 1e-6)
		{
			float3 currentYCoCg = ToYCoCg(color);
			float3 low = currentYCoCg, high = currentYCoCg;
			int2 center = first + int2(representative & 1, representative >> 1);
			[unroll] for (int y = -1; y <= 1; ++y)
				[unroll] for (int x = -1; x <= 1; ++x)
			{
				CurrentSample neighbor = Current(center + int2(x, y));
				if (SameSurface(surface, neighbor) && !neighbor.Reactive)
				{
					float3 neighborhood = ToYCoCg(neighbor.Color);
					low = min(low, neighborhood);
					high = max(high, neighborhood);
				}
			}
			float3 historyYCoCg = ToYCoCg(history / acceptedWeight);
			// Compare before clipping: a bright neighbor must not preserve an obsolete HDR flash.
			float luminanceScale = max(max(abs(currentYCoCg.x), abs(historyYCoCg.x)), 1e-4);
			float agreement = 1 - saturate(abs(historyYCoCg.x - currentYCoCg.x) / luminanceScale);
			float weight = g_Resolve.JitterHistory.z * agreement * agreement;
			color = lerp(color, FromYCoCg(ClipHistory(currentYCoCg, historyYCoCg, low, high)), weight);
		}
	}
	ResolvedTargets result;
	result.Color = float4(color, 1);
	result.Metadata = float4(EncodeSurfaceNormal(surface.Normal), reactive ? 1 : 0, 0);
	result.ID = surface.ID;
	result.Depth = surface.Depth;
	return result;
}

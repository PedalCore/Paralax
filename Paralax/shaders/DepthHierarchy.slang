import Common.Fullscreen;

// Input dimensions, explicit coverage component, reserved.
[[vk::push_constant]] ConstantBuffer<uint4> g_Settings;
Texture2D<float2> g_Input : register(t0);
Texture2D<float4> g_Coverage : register(t1);

[shader("vertex")] FullscreenVertex HierarchyVS(uint vertexID : SV_VertexID)
{
	return MakeFullscreenVertex(vertexID);
}

[shader("fragment")] float2 CopyPS(FullscreenVertex input) : SV_Target
{
	int3 location = int3(int2(input.Position.xy), 0);
	float depth = g_Input.Load(location).x;
	float coverage = g_Coverage.Load(location)[g_Settings.z];
	return isfinite(coverage) && coverage > 0 && isfinite(depth) && depth >= 0 && depth <= 1 ? float2(depth) : float2(-1);
}

[shader("fragment")] float2 ReducePS(FullscreenVertex input) : SV_Target
{
	uint2 origin = uint2(input.Position.xy) * 2;
	float2 interval = float2(-1);
	[unroll] for (uint y = 0; y < 2; ++y)
		[unroll] for (uint x = 0; x < 2; ++x)
		{
			uint2 pixel = origin + uint2(x, y);
			if (any(pixel >= g_Settings.xy))
				continue;
			float2 child = g_Input.Load(int3(pixel, 0));
			if (child.x < 0)
				continue;
			interval = interval.x < 0 ? child : float2(max(interval.x, child.x), min(interval.y, child.y));
		}
	return interval;
}

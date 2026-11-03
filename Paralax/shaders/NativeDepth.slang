import Common.Fullscreen;
import Common.AreaFootprint;

struct DepthDimensions
{
	uint4 Size; // Internal raster xy, native output zw. Both grids retain the same projection jitter.
};
[[vk::push_constant]] ConstantBuffer<DepthDimensions> g_DepthDimensions;
Texture2D<float> g_Depth : register(t0);

[shader("vertex")] FullscreenVertex DepthVS(uint vertex : SV_VertexID) {
	return MakeFullscreenVertex(vertex);
}

	[shader("fragment")] float DepthPS(FullscreenVertex input)
	: SV_Depth
{
	if (any(g_DepthDimensions.Size.xy > g_DepthDimensions.Size.zw))
	{
		uint2 pixel = uint2(input.Position.xy);
		uint2 first = AreaFirst(pixel, g_DepthDimensions.Size.xy, g_DepthDimensions.Size.zw);
		float closest = 0;
		[unroll] for (uint y = 0; y < 3; ++y)
			[unroll] for (uint x = 0; x < 3; ++x)
		{
			uint2 source = first + uint2(x, y);
			uint2 overlap = AreaOverlap(pixel, source, g_DepthDimensions.Size.xy, g_DepthDimensions.Size.zw);
			if (all(source < g_DepthDimensions.Size.xy) && all(overlap > 0))
				closest = max(closest, g_Depth.Load(int3(source, 0)));
		}
		return closest;
	}
	uint2 source = min(uint2(input.Position.xy * float2(g_DepthDimensions.Size.xy) / float2(g_DepthDimensions.Size.zw)), g_DepthDimensions.Size.xy - 1);
	return g_Depth.Load(int3(source, 0));
}

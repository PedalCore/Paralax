import Common.Fullscreen;

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
	uint2 source = min(uint2(input.Position.xy * float2(g_DepthDimensions.Size.xy) / float2(g_DepthDimensions.Size.zw)), g_DepthDimensions.Size.xy - 1);
	return g_Depth.Load(int3(source, 0));
}

import Common.Fullscreen;
import Common.EdgeReconstruction;

[[vk::push_constant]] ConstantBuffer<float4> g_Size;
Texture2D<float4> g_Color : register(t0);
SamplerState g_Linear : register(s0);

[shader("vertex")] FullscreenVertex SpatialVS(uint vertex : SV_VertexID) {
	return MakeFullscreenVertex(vertex);
}

	[shader("fragment")] float4 SpatialPS(FullscreenVertex input)
	: SV_Target
{
	float2 pixel = input.Position.xy - 0.5;
	bool reconstructed;
	float3 color = ReconstructEdge(g_Color, pixel, int2(g_Size.xy), 0.05, reconstructed);
	// Dither follows filtering and matches the ordinary tone-map output. Text/UI are composed later.
	float noise = frac(sin(dot(input.Position.xy, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
	color += noise / 255.0 * 2.2 * pow(max(color, 1e-6), float3(1.2 / 2.2));
	return float4(color, 1);
}

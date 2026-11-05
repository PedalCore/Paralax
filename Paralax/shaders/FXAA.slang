import Common.Fullscreen;
import Common.DisplayTransfer;
import Common.FXAA;

[[vk::push_constant]] ConstantBuffer<float4> g_Size;
Texture2D<float4> g_Color : register(t0);
SamplerState g_Linear : register(s0);

[shader("vertex")] FullscreenVertex FXAAVS(uint vertex : SV_VertexID) {
	return MakeFullscreenVertex(vertex);
}

	[shader("fragment")] float4 FXAAPS(FullscreenVertex input)
	: SV_Target
{
	float3 color = DecodeSRGB(FilterFXAA(g_Color, g_Linear, g_Size.xy, input.Position.xy));
	// Same final dither as Off/Spatial, applied once after filtering and before hardware encoding.
	float noise = frac(sin(dot(input.Position.xy, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
	color += noise / 255.0 * 2.2 * pow(max(color, 1e-6), float3(1.2 / 2.2));
	return float4(color, 1);
}

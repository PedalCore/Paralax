import Common.Fullscreen;
import Common.FXAA;
import Common.DisplayTransfer;

[[vk::push_constant]] ConstantBuffer<float4> g_Parameters;
Texture2D<float4> g_Color : register(t0);
SamplerState g_Linear : register(s0);

[shader("vertex")] FullscreenVertex TestVS(uint vertex : SV_VertexID) {
	return MakeFullscreenVertex(vertex);
}

	[shader("fragment")] float4 SearchPS(FullscreenVertex input)
	: SV_Target
{
	float2 origin = g_Parameters.zw;
	FXAAEndpoint a = FXAASearch(g_Color, g_Linear, g_Parameters.xy, origin, float2(-1, 0), 0.5, 0.25);
	FXAAEndpoint b = FXAASearch(g_Color, g_Linear, g_Parameters.xy, origin, float2(1, 0), 0.5, 0.25);
	return float4(FXAAOffset(a, b, 0.5, 1), a.Distance, b.Distance, (a.Found ? 1 : 0) + (b.Found ? 2 : 0));
}

[shader("fragment")] float4 TransferPS(FullscreenVertex input)
	: SV_Target
{
	float3 value = g_Color.Load(int3(int2(input.Position.xy), 0)).rgb;
	return float4(DecodeSRGB(value).r, EncodeSRGB(value).g, DecodeSRGB(EncodeSRGB(value)).b, 1);
}

[shader("fragment")] float4 HalfTexelPS(FullscreenVertex input)
	: SV_Target
{
	return float4(DecodeSRGB(g_Color.SampleLevel(g_Linear, float2(1.0, 0.5) / g_Parameters.xy, 0).rgb), 1);
}

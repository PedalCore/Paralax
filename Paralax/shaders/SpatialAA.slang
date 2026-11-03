import Common.Fullscreen;

[[vk::push_constant]] ConstantBuffer<float4> g_Size;
Texture2D<float4> g_Color : register(t0);
SamplerState g_Linear : register(s0);

[shader("vertex")] FullscreenVertex SpatialVS(uint vertex : SV_VertexID) {
	return MakeFullscreenVertex(vertex);
}

float Luma(float3 color)
{
	return sqrt(max(dot(color, float3(0.2126, 0.7152, 0.0722)), 0));
}

[shader("fragment")] float4 SpatialPS(FullscreenVertex input)
	: SV_Target
{
	float2 texel = 1 / g_Size.xy;
	float3 center = g_Color.SampleLevel(g_Linear, input.UV, 0).rgb;
	float m = Luma(center);
	float n = Luma(g_Color.SampleLevel(g_Linear, input.UV - float2(0, texel.y), 0).rgb);
	float s = Luma(g_Color.SampleLevel(g_Linear, input.UV + float2(0, texel.y), 0).rgb);
	float e = Luma(g_Color.SampleLevel(g_Linear, input.UV + float2(texel.x, 0), 0).rgb);
	float w = Luma(g_Color.SampleLevel(g_Linear, input.UV - float2(texel.x, 0), 0).rgb);
	float high = max(m, max(max(n, s), max(e, w)));
	float low = min(m, min(min(n, s), min(e, w)));
	float contrast = high - low;
	float3 color = center;
	if (contrast >= max(0.05, high * 0.125))
	{
		float2 normal = abs(n + s - 2 * m) >= abs(e + w - 2 * m) ? float2(0, texel.y) : float2(texel.x, 0);
		float3 filtered = (g_Color.SampleLevel(g_Linear, input.UV - normal * 0.5, 0).rgb + g_Color.SampleLevel(g_Linear, input.UV + normal * 0.5, 0).rgb) * 0.5;
		float subpixel = saturate(abs((n + s + e + w) * 0.25 - m) / contrast);
		color = lerp(center, filtered, min(subpixel * 2, 0.75));
	}
	// Dither follows filtering and matches the ordinary tone-map output. Text/UI are composed later.
	float noise = frac(sin(dot(input.Position.xy, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
	color += noise / 255.0 * 2.2 * pow(max(color, 1e-6), float3(1.2 / 2.2));
	return float4(color, 1);
}

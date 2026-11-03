// HDR -> display: exposure, AgX tone mapping (with a punchy look) and dithering. Output is linear; the sRGB back
// buffer applies the transfer function.
import Common.Fullscreen;

struct TonemapConstants
{
	float Exposure;
	uint Operator; // 0 AgX, 1 ACES (fitted), 2 none (clamp), 3 raw diagnostic (no dither)
	float BloomIntensity;
	float Upscale;
};
[[vk::push_constant]] ConstantBuffer<TonemapConstants> g_Tonemap;

Texture2D<float4> g_HDR : register(t0);
Texture2D<float4> g_Bloom : register(t1);
SamplerState g_LinearClamp : register(s1);
SamplerState g_PointClamp : register(s0);

// AgX (Troy Sobotka) via the polynomial fit by Benjamin Wrensch.
float3 AgXContrast(float3 x)
{
	float3 x2 = x * x;
	float3 x4 = x2 * x2;
	return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 AgX(float3 color)
{
	const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772,
									0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
	const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417,
									 -0.0980434501171241, -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
	const float minEv = -12.47393;
	const float maxEv = 4.026069;

	color = mul(color, inset);
	color = clamp(log2(max(color, 1e-10)), minEv, maxEv);
	color = (color - minEv) / (maxEv - minEv);
	color = AgXContrast(color);

	// "Punchy" look: slight saturation and contrast boost.
	float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
	color = luma + 1.25 * (pow(max(color, 0.0), float3(1.35)) - luma);

	color = mul(color, outset);
	// The fit produces sRGB-encoded values; return linear for the sRGB back buffer.
	return pow(max(color, 0.0), float3(2.2));
}

float3 ACESFitted(float3 color)
{
	const float3x3 input = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
	const float3x3 output = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276, 1.07602);
	color = mul(input, color);
	float3 a = color * (color + 0.0245786) - 0.000090537;
	float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
	return saturate(mul(output, a / b));
}

[shader("vertex")] FullscreenVertex TonemapVS(uint vertexID : SV_VertexID) {
	return MakeFullscreenVertex(vertexID);
}

	[shader("fragment")] float4 TonemapPS(FullscreenVertex input)
	: SV_Target
{
	float3 hdr = g_Tonemap.Upscale > 0.5 ? g_HDR.SampleLevel(g_LinearClamp, input.UV, 0.0).rgb * g_Tonemap.Exposure : g_HDR.SampleLevel(g_PointClamp, input.UV, 0.0).rgb * g_Tonemap.Exposure;
	if (g_Tonemap.BloomIntensity > 0)
		hdr += g_Bloom.SampleLevel(g_LinearClamp, input.UV, 0).rgb * g_Tonemap.BloomIntensity * g_Tonemap.Exposure;
	if ((g_Tonemap.Operator & 0xffu) == 3)
		return float4(hdr, 1.0);
	float3 color;
	if ((g_Tonemap.Operator & 0xffu) == 0)
		color = AgX(hdr);
	else if ((g_Tonemap.Operator & 0xffu) == 1)
		color = ACESFitted(hdr);
	else
		color = saturate(hdr);

	if ((g_Tonemap.Operator & 0x100u) != 0)
		return float4(color, 1);

	// Dither of about one 8-bit step in the *encoded* (sRGB) domain, converted to linear: d(linear)/d(encoded) is
	// roughly 2.2 * encoded^1.2, so darks (where banding is most visible) get proportionally less linear noise.
	float noise = frac(sin(dot(input.Position.xy, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
	color += noise / 255.0 * 2.2 * pow(max(color, 1e-6), float3(1.2 / 2.2));
	return float4(color, 1.0);
}

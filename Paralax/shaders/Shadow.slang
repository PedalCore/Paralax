// Depth-only rendering into a shadow cascade. Alpha-masked materials are tested against their base color alpha.
import Common.SceneData;

ConstantBuffer<MaterialConstants> g_Material : register(b0, space1);
Texture2D<float4> g_BaseColorTexture : register(t0, space1);
SamplerState g_LinearWrap : register(s0, space0);

// Light view-projection pre-multiplied with the object's world matrix (fits the 128-byte push constant minimum).
struct ShadowConstants
{
	float4x4 WorldLightViewProjection;
};
[[vk::push_constant]] ConstantBuffer<ShadowConstants> g_Shadow;

struct VertexInput
{
	float3 Position : POSITION;
	float2 UV : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
	float2 ClipDepth : TEXCOORD1;
};

VertexOutput TransformShadow(VertexInput input)
{
	VertexOutput output;
	output.Position = mul(g_Shadow.WorldLightViewProjection, float4(input.Position, 1.0));
	output.UV = input.UV;
	output.ClipDepth = output.Position.zw;
	return output;
}

// The vertex clamp preserves coverage; original depth must survive for per-fragment clamping.
// Clamping only vertices would move the rest of a triangle that crosses the near plane.
// The pinned Vulkan backend does not map depthClipEnable to core depthClamp.
[shader("vertex")]
VertexOutput ShadowVS(VertexInput input)
{
	VertexOutput output = TransformShadow(input);
	output.Position.z = max(output.Position.z, 0.0);
	return output;
}

[shader("vertex")]
VertexOutput LocalShadowVS(VertexInput input)
{
	VertexOutput output = TransformShadow(input);
	output.Position.z = min(output.Position.z, output.Position.w);
	return output;
}

float ShadowDepth(VertexOutput input, bool reversed)
{
	// Perspective interpolation of both original clip coordinates preserves the true projected depth.
	float depth = input.ClipDepth.x / input.ClipDepth.y;
	float slope = max(abs(ddx(depth)), abs(ddy(depth)));
	depth += (reversed ? -1.5 : 1.5) * slope;
	// One representable D32 step, followed by clamping, mirrors raster constant/slope bias.
	float positive = max(depth, 0.0);
	uint bits = asuint(positive);
	float biased = reversed ? asfloat(max(bits, 1u) - 1u) : asfloat(bits + 1u);
	return saturate(biased);
}

void TestShadowAlpha(VertexOutput input)
{
	float alpha = g_Material.BaseColorFactor.a;
	if ((g_Material.TextureFlags & 1u) != 0)
		alpha *= g_BaseColorTexture.Sample(g_LinearWrap, input.UV).a;
	if (alpha < g_Material.AlphaCutoff)
		discard;
}

[shader("fragment")]
float ShadowPS(VertexOutput input) : SV_Depth
{
	return ShadowDepth(input, false);
}

[shader("fragment")]
float ShadowMaskPS(VertexOutput input) : SV_Depth
{
	float depth = ShadowDepth(input, false);
	TestShadowAlpha(input);
	return depth;
}

[shader("fragment")]
float LocalShadowPS(VertexOutput input) : SV_Depth
{
	return ShadowDepth(input, true);
}

[shader("fragment")]
float LocalShadowMaskPS(VertexOutput input) : SV_Depth
{
	float depth = ShadowDepth(input, true);
	TestShadowAlpha(input);
	return depth;
}

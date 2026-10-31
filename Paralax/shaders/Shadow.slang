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
};

[shader("vertex")]
VertexOutput ShadowVS(VertexInput input)
{
	VertexOutput output;
	output.Position = mul(g_Shadow.WorldLightViewProjection, float4(input.Position, 1.0));
	output.UV = input.UV;
	return output;
}

[shader("fragment")]
void ShadowMaskPS(VertexOutput input)
{
	float alpha = g_Material.BaseColorFactor.a;
	if ((g_Material.TextureFlags & 1u) != 0)
		alpha *= g_BaseColorTexture.Sample(g_LinearWrap, input.UV).a;
	if (alpha < g_Material.AlphaCutoff)
		discard;
}

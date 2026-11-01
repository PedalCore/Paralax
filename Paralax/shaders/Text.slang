// Text from a signed-distance-field glyph atlas, drawn after tone mapping into the sRGB target. Vertex colours
// are linear; coverage comes from the distance field, anti-aliased over one pixel at any size.
struct TextConstants
{
	float4x4 Transform; // Identity for screen-space text (positions are clip space); view-projection for world-space.
};
[[vk::push_constant]] ConstantBuffer<TextConstants> g_Text;

Texture2D<float4> g_Atlas : register(t0);
SamplerState g_Sampler : register(s0);

struct VertexInput
{
	float3 Position : POSITION;
	float2 UV : TEXCOORD0;
	float4 Color : COLOR0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
	float4 Color : COLOR0;
};

[shader("vertex")]
VertexOutput TextVS(VertexInput input)
{
	VertexOutput output;
	output.Position = mul(g_Text.Transform, float4(input.Position, 1.0));
	output.UV = input.UV;
	output.Color = input.Color;
	return output;
}

[shader("fragment")]
float4 TextPS(VertexOutput input) : SV_Target
{
	// 0.5 is the glyph edge (128 in the atlas); the field grows inwards.
	float distance = g_Atlas.Sample(g_Sampler, input.UV).r - 0.5;
	float width = max(fwidth(distance), 1e-4);
	float coverage = smoothstep(-width, width, distance);
	return float4(input.Color.rgb, input.Color.a * coverage);
}

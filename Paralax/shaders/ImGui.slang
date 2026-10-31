// Dear ImGui rendering. Vertex colors are sRGB-encoded (ImGui convention); the back buffer is an sRGB format that
// encodes linear output, so colors are linearized here. Textures marked sRGB are decoded by the hardware.
struct ImGuiConstants
{
	float2 Scale;
	float2 Translate;
};
[[vk::push_constant]] ConstantBuffer<ImGuiConstants> g_ImGui;

Texture2D<float4> g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

struct VertexInput
{
	float2 Position : POSITION;
	float2 UV : TEXCOORD0;
	float4 Color : COLOR0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
	float4 Color : COLOR0;
};

float3 SRGBToLinear(float3 c)
{
	return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

[shader("vertex")]
VertexOutput ImGuiVS(VertexInput input)
{
	VertexOutput output;
	// ImGui uses pixel coordinates with +Y down; clip space is D3D-style (+Y up).
	float2 ndc = input.Position * g_ImGui.Scale + g_ImGui.Translate;
	output.Position = float4(ndc, 0.0, 1.0);
	output.UV = input.UV;
	output.Color = float4(SRGBToLinear(input.Color.rgb), input.Color.a);
	return output;
}

[shader("fragment")]
float4 ImGuiPS(VertexOutput input) : SV_Target
{
	return input.Color * g_Texture.Sample(g_Sampler, input.UV);
}

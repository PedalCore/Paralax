// Simple shaders used to verify the rendering pipeline end to end (device, pipelines, bindings, readback).
import Common.Fullscreen;

struct ColorVertex
{
	float4 Position : SV_Position;
	float3 Color : COLOR0;
};

// A triangle with red, green and blue corners: top (0, 0.5) red, bottom-right (0.5, -0.5) green,
// bottom-left (-0.5, -0.5) blue. Paralax uses D3D-style clip space (+Y up); NVRHI flips the Vulkan viewport.
[shader("vertex")]
ColorVertex TriangleVS(uint vertexID: SV_VertexID)
{
	const float2 positions[3] = { float2(0.0, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5) };
	const float3 colors[3] = { float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0) };

	ColorVertex output;
	output.Position = float4(positions[vertexID], 0.0, 1.0);
	output.Color = colors[vertexID];
	return output;
}

[shader("fragment")]
float4 TrianglePS(ColorVertex input) : SV_Target
{
	return float4(input.Color, 1.0);
}

cbuffer SolidColorConstants : register(b0)
{
	float4 g_SolidColor;
};

[shader("vertex")]
FullscreenVertex FullscreenVS(uint vertexID: SV_VertexID)
{
	return MakeFullscreenVertex(vertexID);
}

[shader("fragment")]
float4 SolidColorPS(FullscreenVertex input) : SV_Target
{
	return g_SolidColor;
}

// Writes the interpolated UV to red/green; verifies the fullscreen-triangle UV orientation.
[shader("fragment")]
float4 UVGradientPS(FullscreenVertex input) : SV_Target
{
	return float4(input.UV, 0.0, 1.0);
}

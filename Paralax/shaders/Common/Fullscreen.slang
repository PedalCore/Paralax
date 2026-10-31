// Fullscreen-triangle helpers shared by post-processing passes.
module Fullscreen;

public struct FullscreenVertex
{
	public float4 Position : SV_Position;
	public float2 UV : TEXCOORD0;
};

// Generates a triangle covering the whole viewport from SV_VertexID 0..2 (no vertex buffer).
// UV (0,0) is the top-left of the render target.
public FullscreenVertex MakeFullscreenVertex(uint vertexID)
{
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	FullscreenVertex output;
	// Clip space is D3D-style (+Y up), so UV y = 0 maps to the top edge.
	output.Position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	output.UV = uv;
	return output;
}

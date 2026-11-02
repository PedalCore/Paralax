// Portable vertex pulling and reconstruction; no primitive ID or hardware barycentric feature is required.
module VisibilityGeometry;
import Common.SceneBindings;
import Common.SurfaceVertex;

public
ByteAddressBuffer g_VisibilityVertices : register(t0, space3);
public
ByteAddressBuffer g_VisibilityIndices : register(t1, space3);
public
Texture2D<uint> g_Visibility : register(t2, space3);

public
VertexInput PullVertex(uint index)
{
	uint vertex = g_VisibilityIndices.Load((g_Draw.Geometry.x + index) * 4) + g_Draw.Geometry.y;
	uint address = vertex * 48;
	VertexInput result;
	result.Position = asfloat(g_VisibilityVertices.Load3(address));
	result.Normal = asfloat(g_VisibilityVertices.Load3(address + 12));
	result.Tangent = asfloat(g_VisibilityVertices.Load4(address + 24));
	result.UV = asfloat(g_VisibilityVertices.Load2(address + 40));
	return result;
}

float Cross2(float2 a, float2 b)
{
	return a.x * b.y - a.y * b.x;
}

public
struct ReconstructedSurface
{
public
	VertexOutput Vertex;
public
	bool FrontFace;
};

// Solve directly in homogeneous clip coordinates. This also handles a vertex on or behind the eye plane.
// Each quad lane reconstructs the same candidate, so native derivatives later have same-primitive helpers.
public
ReconstructedSurface ReconstructSurface(uint triangle, float2 pixel)
{
	VertexOutput a = TransformVertex(PullVertex(triangle * 3));
	VertexOutput b = TransformVertex(PullVertex(triangle * 3 + 1));
	VertexOutput c = TransformVertex(PullVertex(triangle * 3 + 2));
	float2 ndc = pixel * float2(2.0, -2.0) / g_View.ViewportSize + float2(-1.0, 1.0);
	float2 h0 = a.Position.xy - ndc * a.Position.w;
	float2 h1 = b.Position.xy - ndc * b.Position.w;
	float2 h2 = c.Position.xy - ndc * c.Position.w;
	float3 numerator = float3(Cross2(h1, h2), Cross2(h2, h0), Cross2(h0, h1));
	float denominator = numerator.x + numerator.y + numerator.z;
	ReconstructedSurface result = {};
	// A helper may lie at a projective pole outside coverage. Finite distant UVs force a conservative native
	// footprint without skipping that lane; valid covered lanes still retain their own reconstructed attributes.
	result.Vertex = a;
	result.Vertex.Position = float4(pixel, 0.0, 1.0);
	result.Vertex.UV = pixel * 65536.0;
	float winding = dot(float3(a.Position.xy, a.Position.w), cross(float3(b.Position.xy, b.Position.w), float3(c.Position.xy, c.Position.w)));
	result.FrontFace = (winding > 0.0) != ((g_Draw.Geometry.w & 0x80000000u) != 0);
	if (!isfinite(denominator) || abs(denominator) < 1e-30)
		return result;
	float3 bary = numerator / denominator;
	VertexOutput vertex;
	vertex.Position = result.Vertex.Position;
	vertex.WorldPosition = a.WorldPosition * bary.x + b.WorldPosition * bary.y + c.WorldPosition * bary.z;
	vertex.Normal = a.Normal * bary.x + b.Normal * bary.y + c.Normal * bary.z;
	vertex.Tangent = a.Tangent * bary.x + b.Tangent * bary.y + c.Tangent * bary.z;
	vertex.UV = a.UV * bary.x + b.UV * bary.y + c.UV * bary.z;
	vertex.ViewDepth = a.ViewDepth * bary.x + b.ViewDepth * bary.y + c.ViewDepth * bary.z;
	if (all(isfinite(vertex.WorldPosition)) && all(isfinite(vertex.Normal)) && all(isfinite(vertex.Tangent)) && all(isfinite(vertex.UV)) && isfinite(vertex.ViewDepth))
		result.Vertex = vertex;
	return result;
}

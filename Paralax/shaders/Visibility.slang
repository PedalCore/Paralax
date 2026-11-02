import Common.SceneBindings;
import Common.MaterialBindings;
import Common.SurfaceVertex;
import Common.SurfaceMaterial;
import Common.SurfaceContract;
import Common.SurfacePacking;
import Common.VisibilityGeometry;

struct VisibilityVertex
{
	VertexOutput Surface;
	nointerpolation uint Triangle : TEXCOORD2;
};

[shader("vertex")] VisibilityVertex VisibilityVS(uint vertexID : SV_VertexID) {
	VisibilityVertex output;
	output.Surface = TransformVertex(PullVertex(vertexID));
	output.Triangle = vertexID / 3;
	return output;
}

	[shader("fragment")] uint VisibilityPS(VisibilityVertex input, bool isFrontFace : SV_IsFrontFace)
	: SV_Target
{
	// Raster interpolation and native material sampling make the ID pass's mask decision authoritative.
	if (g_Material.AlphaMode == 1)
		EvaluateMaterial(input.Surface, isFrontFace);
	return g_Draw.Geometry.z + input.Triangle + 1;
}

[shader("vertex")] float4 MaterialResolveVS(uint vertexID : SV_VertexID)
	: SV_Position
{
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

uint LoadID(uint2 pixel)
{
	return all(pixel < uint2(g_View.ViewportSize)) ? g_Visibility.Load(int3(pixel, 0)) : 0;
}

[shader("fragment")] SurfaceTargets MaterialResolvePS(float4 pixel : SV_Position) {
	uint2 location = uint2(pixel.xy);
	uint2 origin = location & ~uint2(1, 1);
	uint ownID = LoadID(location);
	MaterialSurface selected = {};
	bool matched = false;
	// All lanes load the same four IDs in the same order. Each candidate therefore evaluates one primitive
	// across the entire quad, including helpers. Native implicit texture filtering sees its post-POM footprint.
	[unroll] for (uint lane = 0; lane < 4; ++lane)
	{
		uint candidate = LoadID(origin + uint2(lane & 1, lane >> 1));
		uint triangle = candidate - g_Draw.Geometry.z - 1;
		if (candidate != 0 && triangle < (g_Draw.Geometry.w & 0x7fffffffu))
		{
			ReconstructedSurface surface = ReconstructSurface(triangle, pixel.xy);
			float2 uvDx = ddx(surface.Vertex.UV), uvDy = ddy(surface.Vertex.UV);
			float3 positionDx = ddx(surface.Vertex.WorldPosition), positionDy = ddy(surface.Vertex.WorldPosition);
			MaterialSurface material = EvaluateMaterialWithFootprint(surface.Vertex, surface.FrontFace, uvDx, uvDy, positionDx, positionDy, false);
			if (candidate == ownID)
			{
				selected = material;
				matched = true;
			}
		}
	}
	// The winning raster ID already applied alpha masking. Retesting here could drop a covered surface.
	if (!matched)
		discard;
	return PackSurface(selected, g_View.DebugView);
}

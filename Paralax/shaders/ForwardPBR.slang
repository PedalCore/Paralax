// Forward remains the parity oracle; opaque and transparent surfaces share the material/lighting contract.
import Common.SurfaceVertex;
import Common.SurfaceMaterial;
import Common.MaterialBindings;
import Common.SurfaceLighting;

[shader("vertex")] VertexOutput MainVS(VertexInput input) {
	return TransformVertex(input);
}

	[shader("fragment")] float4 MainPS(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
	: SV_Target
{
	return ShadeSurface(EvaluateMaterial(input, isFrontFace), input.Position.xy, input.ViewDepth, g_Material.AlphaMode != 2);
}

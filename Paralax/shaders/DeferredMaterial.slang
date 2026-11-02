import Common.SceneBindings;
import Common.SurfaceVertex;
import Common.SurfaceMaterial;
import Common.SurfacePacking;

[shader("vertex")] VertexOutput MaterialVS(VertexInput input) {
	return TransformVertex(input);
}

	[shader("fragment")] SurfaceTargets MaterialPS(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	return PackSurface(EvaluateMaterial(input, isFrontFace), g_View.DebugView);
}

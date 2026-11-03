import Common.SurfaceVertex;
import Common.SurfaceMaterial;
import Common.SurfacePacking;
import Common.SurfaceContract;

[shader("vertex")] VertexOutput GuideVS(VertexInput input) {
	return TransformVertex(input);
}

struct GuideTargets
{
	float2 Normal : SV_Target0;
	float Coverage : SV_Target1;
};

[shader("fragment")] GuideTargets GuidePS(VertexOutput input, bool isFrontFace : SV_IsFrontFace) {
	MaterialSurface material = EvaluateMaterial(input, isFrontFace);
	GuideTargets result;
	result.Normal = EncodeSurfaceNormal(material.ReceiverNormal);
	result.Coverage = 1;
	return result;
}

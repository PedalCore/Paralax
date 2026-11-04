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

struct ReflectionGuideTargets
{
	float4 BaseMetal : SV_Target0;
	float4 Normals : SV_Target1;
	float4 ReceiverRoughAO : SV_Target2;
};

[shader("fragment")] ReflectionGuideTargets ReflectionGuidePS(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	SurfaceTargets surface = PackSurface(EvaluateMaterial(input, isFrontFace), 0);
	ReflectionGuideTargets result;
	result.BaseMetal = surface.BaseMetal;
	result.Normals = surface.Normals;
	result.ReceiverRoughAO = surface.ReceiverRoughAO;
	return result;
}

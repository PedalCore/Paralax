import Common.SurfaceLighting;
import Common.SceneBindings;

RWStructuredBuffer<float> g_Result : register(u0, space0);

[shader("compute")]
	[numthreads(1, 1, 1)] void
	SampleCascadeBoundary(uint3 thread : SV_DispatchThreadID) {
		uint boundary = thread.x / 65;
		uint sample = thread.x % 65;
		float end = g_View.CascadeSplits[boundary];
		float start = boundary == 0 ? g_View.CascadeNear : g_View.CascadeSplits[boundary - 1];
		// Sampling positions stay fixed when blending is disabled, so the hard-jump control retains its stimulus.
		float width = (end - start) * 0.3;
		float viewDepth = end - width + width * (float(sample) / 64.0);
		g_Result[thread.x] = SunShadow(float3(0, 0, 0.5), float3(0, 0, 1), float3(0, 0, 1), viewDepth, float2x2(1, 0, 0, 1));
	}

import Common.SceneData;
import Common.SurfacePacking;

ConstantBuffer<ViewConstants> g_View : register(b0, space0);
struct AOConstants
{
	float4x4 InverseProjection;
	float4 Size;     // full width/height, half width/height
	float4 Settings; // world radius, relative bias, strength, sample count
	float4 Pass;     // coverage channel, filter axis, orthographic, unused
	float4 Padding;
};
[[vk::push_constant]] ConstantBuffer<AOConstants> g_AO;
Texture2D<float> g_Depth : register(t0, space1);
Texture2D<float4> g_Normal : register(t1, space1);
Texture2D<float4> g_Coverage : register(t2, space1);
Texture2D<float> g_HalfDepth : register(t3, space1);
Texture2D<float4> g_Metadata : register(t4, space1);
Texture2D<float> g_InputAO : register(t5, space1);

[shader("vertex")] float4 FullscreenVS(uint vertex : SV_VertexID)
	: SV_Position
{
	float2 uv = float2((vertex << 1) & 2, vertex & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 Unproject(float2 pixel, float depth)
{
	float2 ndc = (pixel + 0.5) / g_AO.Size.xy * float2(2, -2) + float2(-1, 1);
	float4 point = mul(g_AO.InverseProjection, float4(ndc, depth, 1));
	return point.xyz / point.w;
}

float3 PositionAt(float2 pixel, float signedDepth)
{
	float3 nearPoint = Unproject(pixel, 1);
	float3 farPoint = Unproject(pixel, 0);
	float3 ray = farPoint - nearPoint;
	return nearPoint + ray * ((-signedDepth - nearPoint.z) / ray.z);
}

bool Covered(int2 pixel)
{
	if (any(pixel < 0) || any(pixel >= int2(g_AO.Size.xy)))
		return false;
	float4 value = g_Coverage.Load(int3(pixel, 0));
	return (g_AO.Pass.x > 0.5 ? value.z : value.x) > 0;
}

float3 ViewNormal(int2 pixel)
{
	return normalize(mul((float3x3)g_View.View, DecodeSurfaceNormal(g_Normal.Load(int3(pixel, 0)).xy)));
}

float2 Representative(int2 pixel, float4 metadata)
{
	uint offset = uint(metadata.z);
	return float2(pixel * 2 + int2(offset & 1, offset >> 1));
}

struct GuideTargets
{
	float Depth : SV_Target0;
	float4 Metadata : SV_Target1;
};

[shader("fragment")] GuideTargets GuidePS(float4 position : SV_Position) {
	int2 origin = int2(position.xy) * 2;
	float closest = -1;
	int2 selected = origin;
	uint offset = 0;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		int2 pixel = origin + int2(i & 1, i >> 1);
		if (!Covered(pixel))
			continue;
		float depth = g_Depth.Load(int3(pixel, 0));
		if (depth > closest)
		{
			closest = depth;
			selected = pixel;
			offset = i;
		}
	}
	GuideTargets result = {};
	if (closest >= 0)
	{
		result.Depth = -Unproject(float2(selected), closest).z;
		result.Metadata = float4(EncodeSurfaceNormal(ViewNormal(selected)), float(offset), 1);
	}
	return result;
}

	[shader("fragment")] float OcclusionPS(float4 position : SV_Position)
	: SV_Target
{
	int2 pixel = int2(position.xy);
	float4 metadata = g_Metadata.Load(int3(pixel, 0));
	if (metadata.w < 0.5)
		return 1;
	float3 point = PositionAt(Representative(pixel, metadata), g_HalfDepth.Load(int3(pixel, 0)));
	float3 normal = DecodeSurfaceNormal(metadata.xy);
	float3 toCamera = g_AO.Pass.z > 0.5 ? float3(0, 0, 1) : normalize(-point);
	if (dot(normal, toCamera) < 0)
		normal = -normal;
	float3 tangent = normalize(cross(normal, abs(normal.z) < 0.99 ? float3(0, 0, 1) : float3(0, 1, 0)));
	float3 bitangent = cross(normal, tangent);
	float radius = g_AO.Settings.x;
	float bias = max(radius * g_AO.Settings.y, 1e-5);
	uint count = uint(g_AO.Settings.w);
	float obscurance = 0;
	// Fixed rotations avoid temporal noise while decorrelating neighbouring pixels.
	float rotation = frac(dot(float2(pixel), float2(0.754877666, 0.569840296))) * 6.28318530718;
	for (uint i = 0; i < count; ++i)
	{
		float fraction = (float(i) + 0.5) / float(count);
		float angle = float(i) * 2.39996322973 + rotation;
		float z = sqrt(1 - fraction);
		float radial = sqrt(fraction);
		float3 direction = tangent * cos(angle) * radial + bitangent * sin(angle) * radial + normal * z;
		float3 samplePoint = point + direction * radius * (0.15 + 0.85 * fraction);
		float4 clip = mul(g_View.Projection, float4(samplePoint, 1));
		if (clip.w <= 1e-6 || !all(isfinite(clip)))
			continue;
		float3 ndc = clip.xyz / clip.w;
		float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
		if (any(uv < 0) || any(uv >= 1) || ndc.z < 0 || ndc.z > 1)
			continue;
		int2 fullPixel = int2(uv * g_AO.Size.xy);
		if (!Covered(fullPixel))
			continue;
		float3 hit = Unproject(float2(fullPixel), g_Depth.Load(int3(fullPixel, 0)));
		float3 delta = hit - point;
		float distance = length(delta);
		// Reject the receiver's own plane, including sloped surfaces and signed orthographic depths.
		if (hit.z > samplePoint.z + bias && dot(delta, normal) > bias && distance < radius)
			obscurance += 1 - distance / radius;
	}
	return pow(saturate(1 - obscurance / float(count)), g_AO.Settings.z);
}

float GuideWeight(float depth, float3 normal, float otherDepth, float3 otherNormal)
{
	float depthWeight = exp(-abs(depth - otherDepth) / max(g_AO.Settings.x * 0.1, 1e-5));
	return depthWeight * pow(saturate(dot(normal, otherNormal)), 32);
}

[shader("fragment")] float FilterPS(float4 position : SV_Position)
	: SV_Target
{
	int2 pixel = int2(position.xy);
	float4 center = g_Metadata.Load(int3(pixel, 0));
	if (center.w < 0.5)
		return 1;
	float depth = g_HalfDepth.Load(int3(pixel, 0));
	float3 normal = DecodeSurfaceNormal(center.xy);
	float sum = 0, total = 0;
	for (int i = -2; i <= 2; ++i)
	{
		int2 tap = pixel + (g_AO.Pass.y < 0.5 ? int2(i, 0) : int2(0, i));
		if (any(tap < 0) || any(tap >= int2(g_AO.Size.zw)))
			continue;
		float4 metadata = g_Metadata.Load(int3(tap, 0));
		if (metadata.w < 0.5)
			continue;
		float weight = exp(-float(i * i) * 0.5) * GuideWeight(depth, normal, g_HalfDepth.Load(int3(tap, 0)), DecodeSurfaceNormal(metadata.xy));
		sum += g_InputAO.Load(int3(tap, 0)) * weight;
		total += weight;
	}
	return total > 1e-6 ? sum / total : g_InputAO.Load(int3(pixel, 0));
}

[shader("fragment")] float UpsamplePS(float4 position : SV_Position)
	: SV_Target
{
	int2 pixel = int2(position.xy);
	if (!Covered(pixel))
		return 1;
	float depth = -Unproject(float2(pixel), g_Depth.Load(int3(pixel, 0))).z;
	float3 normal = ViewNormal(pixel);
	float2 halfPosition = (float2(pixel) + 0.5) * 0.5 - 0.5;
	int2 origin = int2(floor(halfPosition));
	float2 fraction = frac(halfPosition);
	float sum = 0, total = 0;
	[unroll] for (int i = 0; i < 4; ++i)
	{
		int2 offset = int2(i & 1, i >> 1);
		int2 tap = clamp(origin + offset, int2(0), int2(g_AO.Size.zw) - 1);
		float4 metadata = g_Metadata.Load(int3(tap, 0));
		if (metadata.w < 0.5)
			continue;
		float2 bilinear = lerp(1 - fraction, fraction, float2(offset));
		float weight = bilinear.x * bilinear.y * GuideWeight(depth, normal, g_HalfDepth.Load(int3(tap, 0)), DecodeSurfaceNormal(metadata.xy));
		sum += g_InputAO.Load(int3(tap, 0)) * weight;
		total += weight;
	}
	return total > 1e-6 ? saturate(sum / total) : 1;
}

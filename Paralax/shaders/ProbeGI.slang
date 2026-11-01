import Common.ProbeGI;
import Common.SceneData;
import Common.PBR;

struct TraceNode
{
	float4 Minimum;
	float4 Maximum;
	uint4 Range;
};
struct TraceTriangle
{
	float4 Position[3];
	float4 Normal[3];
	float4 UV01;
	float4 UV2;
	uint4 Material;
};
struct TraceMaterial
{
	float4 BaseColor;
	float4 Emissive;
	float4 Parameters;
	uint4 Textures[3];
};

ConstantBuffer<ProbeGIConstants> g_Probe : register(b0);
ConstantBuffer<ViewConstants> g_View : register(b1);
StructuredBuffer<TraceNode> g_Nodes : register(t0);
StructuredBuffer<TraceTriangle> g_Triangles : register(t1);
StructuredBuffer<TraceMaterial> g_Materials : register(t2);
StructuredBuffer<float4> g_Texels : register(t3);
StructuredBuffer<GPULight> g_Lights : register(t4);
TextureCube<float4> g_Environment : register(t5);
Texture2DArray<float4> g_PreviousIrradiance : register(t6);
Texture2DArray<float4> g_PreviousDistance : register(t7);
SamplerState g_LinearClamp : register(s0);
RWStructuredBuffer<float4> g_Rays : register(u0);
RWTexture2DArray<float4> g_Irradiance : register(u1);
RWTexture2DArray<float4> g_Distance : register(u2);

float4 MaterialTexel(uint4 texture, int2 pixel)
{
	int2 size = int2(texture.yz);
	pixel = (pixel % size + size) % size;
	return g_Texels[texture.x + uint(pixel.y) * texture.y + uint(pixel.x)];
}

float4 SampleMaterial(uint4 texture, float2 uv)
{
	float2 pixel = frac(uv) * float2(texture.yz) - 0.5;
	int2 base = int2(floor(pixel));
	float2 alpha = frac(pixel);
	return lerp(lerp(MaterialTexel(texture, base), MaterialTexel(texture, base + int2(1, 0)), alpha.x),
		lerp(MaterialTexel(texture, base + int2(0, 1)), MaterialTexel(texture, base + int2(1, 1)), alpha.x), alpha.y);
}

bool IntersectBox(float3 origin, float3 direction, TraceNode node, float maximum)
{
	float nearDistance = 0.0;
	float farDistance = maximum;
	for (uint axis = 0; axis < 3; ++axis)
	{
		if (abs(direction[axis]) < 1e-10)
		{
			if (origin[axis] < node.Minimum[axis] || origin[axis] > node.Maximum[axis])
				return false;
		}
		else
		{
			float a = (node.Minimum[axis] - origin[axis]) / direction[axis];
			float b = (node.Maximum[axis] - origin[axis]) / direction[axis];
			nearDistance = max(nearDistance, min(a, b));
			farDistance = min(farDistance, max(a, b));
		}
	}
	return nearDistance <= farDistance;
}

struct TraceHit
{
	float Distance;
	uint Triangle;
	float2 Barycentric;
};

TraceHit Trace(float3 origin, float3 direction, float maximum, bool anyHit)
{
	TraceHit hit;
	hit.Distance = maximum;
	hit.Triangle = 0xffffffff;
	hit.Barycentric = float2(0.0);
	uint nodeIndex = 0;
	while (nodeIndex < g_Probe.Scene.x)
	{
		TraceNode node = g_Nodes[nodeIndex];
		if (!IntersectBox(origin, direction, node, hit.Distance))
		{
			nodeIndex = node.Range.z;
			continue;
		}
		for (uint i = 0; i < node.Range.y; ++i)
		{
			uint index = node.Range.x + i;
			TraceTriangle triangle = g_Triangles[index];
			float3 edge1 = triangle.Position[1].xyz - triangle.Position[0].xyz;
			float3 edge2 = triangle.Position[2].xyz - triangle.Position[0].xyz;
			float3 p = cross(direction, edge2);
			float determinant = dot(edge1, p);
			if (!isfinite(determinant) || abs(determinant) < 1e-10)
				continue;
			float3 offset = origin - triangle.Position[0].xyz;
			float u = dot(offset, p) / determinant;
			float3 q = cross(offset, edge1);
			float v = dot(direction, q) / determinant;
			float distance = dot(edge2, q) / determinant;
			if (!all(isfinite(float3(u, v, distance))) || u < 0.0 || v < 0.0 || u + v > 1.0 || distance < 1e-4 || distance >= hit.Distance)
				continue;
			TraceMaterial material = g_Materials[triangle.Material.x];
			if (material.Parameters.w == 1.0)
			{
				float2 uv = triangle.UV01.xy * (1.0 - u - v) + triangle.UV01.zw * u + triangle.UV2.xy * v;
				if (SampleMaterial(material.Textures[0], uv).a * material.BaseColor.a < material.Parameters.z)
					continue;
			}
			hit.Distance = distance;
			hit.Triangle = index;
			hit.Barycentric = float2(u, v);
			if (anyHit)
				return hit;
		}
		++nodeIndex;
	}
	return hit;
}

float3 RayDirection(uint ray)
{
	float y = 1.0 - 2.0 * (float(ray) + 0.5) / float(g_Probe.Update.z);
	float radius = sqrt(max(1.0 - y * y, 0.0));
	float angle = 2.399963229728653 * float(ray) + 0.754877666 * float(g_Probe.Update.w % 4096);
	return float3(cos(angle) * radius, y, sin(angle) * radius);
}

bool Unoccluded(float3 origin, float3 direction, float maximum)
{
	return Trace(origin, direction, maximum, true).Triangle == 0xffffffff;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TraceProbes(uint3 dispatch : SV_DispatchThreadID)
{
	uint ray = dispatch.x;
	uint slot = dispatch.y;
	if (ray >= g_Probe.Update.z || slot >= g_Probe.Update.y)
		return;
	uint probeIndex = (g_Probe.Update.x + slot) % g_Probe.Grid.w;
	float3 origin = ProbePosition(probeIndex, g_Probe);
	float3 direction = RayDirection(ray);
	TraceHit hit = Trace(origin, direction, g_Probe.Spacing.w, false);
	float distance = min(hit.Distance, g_Probe.Tuning.w);
	float3 radiance = float3(0.0);
	if (hit.Triangle == 0xffffffff)
	{
		float s = sin(g_View.EnvironmentParams.z), c = cos(g_View.EnvironmentParams.z);
		float3 rotated = float3(c * direction.x + s * direction.z, direction.y, -s * direction.x + c * direction.z);
		radiance = g_Environment.SampleLevel(g_LinearClamp, rotated, 0.0).rgb * g_View.EnvironmentParams.x;
	}
	else
	{
		TraceTriangle triangle = g_Triangles[hit.Triangle];
		TraceMaterial material = g_Materials[triangle.Material.x];
		float3 geometric = normalize(cross(triangle.Position[1].xyz - triangle.Position[0].xyz,
			triangle.Position[2].xyz - triangle.Position[0].xyz));
		bool backFace = dot(geometric, direction) > 0.0;
		if (backFace && material.Emissive.w == 0.0)
			distance = -distance;
		else
		{
			float3 barycentric = float3(1.0 - hit.Barycentric.x - hit.Barycentric.y, hit.Barycentric);
			float2 uv = triangle.UV01.xy * barycentric.x + triangle.UV01.zw * barycentric.y + triangle.UV2.xy * barycentric.z;
			float3 normal = triangle.Normal[0].xyz * barycentric.x + triangle.Normal[1].xyz * barycentric.y + triangle.Normal[2].xyz * barycentric.z;
			normal = dot(normal, normal) > 1e-10 ? normalize(normal) : geometric;
			if (backFace)
			{
				geometric = -geometric;
				normal = -normal;
			}
			float3 position = origin + direction * hit.Distance;
			float3 rayOrigin = position + geometric * 0.001;
			float3 baseColor = saturate(material.BaseColor.rgb * SampleMaterial(material.Textures[0], uv).rgb);
			float metallic = saturate(material.Parameters.x * SampleMaterial(material.Textures[2], uv).b);
			float3 diffuse = baseColor * (1.0 - metallic) * 0.96;
			radiance = max(material.Emissive.rgb * SampleMaterial(material.Textures[1], uv).rgb, float3(0.0));
			if (g_View.SunDirection.w > 0.5 && dot(normal, -g_View.SunDirection.xyz) > 0.0 &&
				Unoccluded(rayOrigin, -g_View.SunDirection.xyz, g_Probe.Spacing.w))
				radiance += diffuse * g_View.SunColor.rgb * saturate(dot(normal, -g_View.SunDirection.xyz)) / PI;
			for (uint i = 0; i < g_Probe.Scene.y; ++i)
			{
				GPULight light = g_Lights[i];
				float3 delta = light.PositionRange.xyz - position;
				float distanceSquared = dot(delta, delta);
				if (distanceSquared > light.PositionRange.w * light.PositionRange.w || distanceSquared < 1e-8)
					continue;
				float lightDistance = sqrt(distanceSquared);
				float3 L = delta / lightDistance;
				float attenuation = DistanceAttenuation(distanceSquared, light.PositionRange.w);
				if (light.ColorIntensity.w > 0.5)
				{
					float cone = saturate((dot(-L, light.DirectionCos.xyz) - light.DirectionCos.w) / max(light.Params.x - light.DirectionCos.w, 1e-4));
					attenuation *= cone * cone;
				}
				if (dot(normal, L) > 0.0 && Unoccluded(rayOrigin, L, max(lightDistance - 0.002, 0.0)))
					radiance += diffuse * light.ColorIntensity.rgb * attenuation * saturate(dot(normal, L)) / PI;
			}
			float4 indirect = SampleProbeGI(position, normal, -direction, g_Probe, g_PreviousIrradiance, g_PreviousDistance);
			radiance += diffuse * indirect.rgb;
		}
	}
	// Defensive boundary for extreme lighting supplied directly by clients. Bounded radiance also prevents
	// a single pathological light from poisoning every subsequent frame through cache feedback.
	radiance = all(isfinite(radiance)) ? clamp(radiance, float3(0.0), float3(1e12)) : float3(0.0);
	g_Rays[slot * g_Probe.Update.z + ray] = float4(radiance, distance);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void FilterProbes(uint3 thread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
	uint probeIndex = group.x;
	if (probeIndex >= g_Probe.Grid.w)
		return;
	uint slot = (probeIndex + g_Probe.Grid.w - g_Probe.Update.x) % g_Probe.Grid.w;
	if (slot >= g_Probe.Update.y)
	{
		uint3 pixel = uint3(thread.xy, probeIndex);
		g_Irradiance[pixel] = g_PreviousIrradiance.Load(int4(pixel, 0));
		g_Distance[pixel] = g_PreviousDistance.Load(int4(pixel, 0));
		return;
	}
	// Mirror the octahedral seams into a one-texel border, including the diagonal corner copies.
	uint2 interior = thread.xy;
	if (interior.x == 0 || interior.x == 7)
		interior = uint2(interior.x == 0 ? 1 : 6, 7 - interior.y);
	if (interior.y == 0 || interior.y == 7)
		interior = uint2(7 - interior.x, interior.y == 0 ? 1 : 6);
	float3 direction = ProbeOctDecode((float2(interior) - 0.5) / 6.0);
	float3 irradiance = float3(0.0);
	float2 moments = float2(0.0);
	float irradianceWeight = 0.0, distanceWeight = 0.0;
	uint backFaces = 0;
	for (uint ray = 0; ray < g_Probe.Update.z; ++ray)
	{
		float4 sample = g_Rays[slot * g_Probe.Update.z + ray];
		backFaces += sample.w < 0.0 ? 1 : 0;
		float cosine = saturate(dot(direction, RayDirection(ray)));
		irradiance += sample.rgb * cosine;
		irradianceWeight += cosine;
		float weight = pow(cosine, 32.0);
		float distance = abs(sample.w);
		moments += float2(distance, distance * distance) * weight;
		distanceWeight += weight;
	}
	float active = backFaces * 4 < g_Probe.Update.z ? 1.0 : 0.0;
	float3 nextIrradiance = irradiance / max(irradianceWeight, 1e-6);
	float2 nextDistance = moments / max(distanceWeight, 1e-6);
	uint3 pixel = uint3(thread.xy, probeIndex);
	float4 previousIrradiance = g_PreviousIrradiance.Load(int4(pixel, 0));
	float4 previousDistance = g_PreviousDistance.Load(int4(pixel, 0));
	float history = previousDistance.w > 0.5 && active > 0.5 ? g_Probe.Tuning.z : 0.0;
	g_Irradiance[pixel] = float4(lerp(nextIrradiance, previousIrradiance.rgb, history), active);
	g_Distance[pixel] = float4(lerp(nextDistance, previousDistance.xy, history), 0.0, active);
}

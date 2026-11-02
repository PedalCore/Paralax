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

bool IntersectBox(float3 origin, float3 direction, TraceNode node, float maximum, float3 originOffset)
{
	precise float3 minimum = (node.Minimum.xyz - origin) - originOffset;
	precise float3 maximumPoint = (node.Maximum.xyz - origin) - originOffset;
	float nearDistance = 0.0;
	float farDistance = maximum;
	for (uint axis = 0; axis < 3; ++axis)
	{
		if (abs(direction[axis]) < 1e-10)
		{
			if (minimum[axis] > 0.0 || maximumPoint[axis] < 0.0)
				return false;
		}
		else
		{
			float a = minimum[axis] / direction[axis];
			float b = maximumPoint[axis] / direction[axis];
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

TraceHit Trace(float3 origin, float3 direction, float maximum, bool shadowRay, float3 originOffset = float3(0.0))
{
	TraceHit hit;
	hit.Distance = maximum;
	hit.Triangle = 0xffffffff;
	hit.Barycentric = float2(0.0);
	uint nodeIndex = 0;
	while (nodeIndex < g_Probe.Scene.x)
	{
		TraceNode node = g_Nodes[nodeIndex];
		if (!IntersectBox(origin, direction, node, hit.Distance, originOffset))
		{
			nodeIndex = node.Range.z;
			continue;
		}
		for (uint i = 0; i < node.Range.y; ++i)
		{
			uint index = node.Range.x + i;
			TraceTriangle triangle = g_Triangles[index];
			// Non-shadowing emissive proxies still participate in radiance, classification and visibility.
			if (shadowRay && triangle.Material.y != 0)
				continue;
			float3 edge1 = triangle.Position[1].xyz - triangle.Position[0].xyz;
			float3 edge2 = triangle.Position[2].xyz - triangle.Position[0].xyz;
			float3 p = cross(direction, edge2);
			float determinant = dot(edge1, p);
			if (!isfinite(determinant) || abs(determinant) < 1e-10)
				continue;
			precise float3 offset = (origin - triangle.Position[0].xyz) + originOffset;
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
			if (shadowRay)
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

bool Unoccluded(float3 origin, float3 direction, float maximum, float3 originOffset)
{
	return Trace(origin, direction, maximum, true, originOffset).Triangle == 0xffffffff;
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
		// Each ray represents a finite solid angle. Sampling mip zero aliases small bright sky
		// features into rare spikes as the ray pattern rotates; filter to the average ray footprint.
		uint width, height, mipLevels;
		g_Environment.GetDimensions(0, width, height, mipLevels);
		// Cube texels cover different solid angles at a face centre and at its corners.
		float major = max(abs(rotated.x), max(abs(rotated.y), abs(rotated.z)));
		float texelSolidAngle = 4.0 * major * major * major / (float(width) * float(height));
		float raySolidAngle = 4.0 * PI / float(g_Probe.Update.z);
		float mip = clamp(0.5 * log2(raySolidAngle / texelSolidAngle), 0.0, float(mipLevels - 1));
		radiance = g_Environment.SampleLevel(g_LinearClamp, rotated, mip).rgb * g_View.EnvironmentParams.x;
	}
	else
	{
		TraceTriangle triangle = g_Triangles[hit.Triangle];
		TraceMaterial material = g_Materials[triangle.Material.x];
		float3 geometric = normalize(cross(triangle.Position[1].xyz - triangle.Position[0].xyz,
										   triangle.Position[2].xyz - triangle.Position[0].xyz));
		bool backFace = dot(geometric, direction) > 0.0;
		// Geometric validity is independent of whether the material shades its back side. Otherwise an
		// embedded probe becomes valid merely by enabling DoubleSided and can leak exterior sky into a solid.
		if (backFace)
			distance = -distance;
		if (!backFace || material.Emissive.w != 0.0)
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
			// Keep secondary origins relative to a triangle vertex. Adding a millimetre offset to
			// a world position near 1e5 otherwise rounds it away; enlarging that offset skips close
			// blockers. Subtract the anchor before adding this local part during traversal.
			float3 anchor = triangle.Position[0].xyz;
			float3 edge1 = triangle.Position[1].xyz - anchor;
			float3 edge2 = triangle.Position[2].xyz - anchor;
			precise float3 localPosition = edge1 * hit.Barycentric.x + edge2 * hit.Barycentric.y;
			float3 position = origin + direction * hit.Distance;
			float3 localError = 4.8e-7 * (abs(edge1 * hit.Barycentric.x) + abs(edge2 * hit.Barycentric.y));
			float epsilon = max(0.001, dot(abs(geometric), localError));
			precise float3 rayOffset = localPosition + geometric * epsilon;
			float3 baseColor = saturate(material.BaseColor.rgb * SampleMaterial(material.Textures[0], uv).rgb);
			float metallic = saturate(material.Parameters.x * SampleMaterial(material.Textures[2], uv).b);
			float3 diffuse = baseColor * (1.0 - metallic) * 0.96;
			radiance = max(material.Emissive.rgb * SampleMaterial(material.Textures[1], uv).rgb, float3(0.0));
			if (g_View.SunDirection.w > 0.5 && dot(normal, -g_View.SunDirection.xyz) > 0.0 &&
				Unoccluded(anchor, -g_View.SunDirection.xyz, g_Probe.Spacing.w, rayOffset))
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
				precise float3 shadowDelta = (light.PositionRange.xyz - anchor) - rayOffset;
				float shadowDistance = length(shadowDelta);
				float3 shadowDirection = shadowDelta / max(shadowDistance, 1e-8);
				// The light is a point, not a surface requiring an exclusion interval before it.
				if (dot(normal, L) > 0.0 && Unoccluded(anchor, shadowDirection, shadowDistance, rayOffset))
					radiance += diffuse * light.ColorIntensity.rgb * attenuation * saturate(dot(normal, L)) / PI;
			}
			float4 indirect = SampleProbeGI(position, normal, -direction, g_Probe, g_PreviousIrradiance, g_PreviousDistance, hit.Distance);
			radiance += diffuse * indirect.rgb * g_Probe.Diagnostics.z;
		}
	}
	// Defensive boundary for extreme lighting supplied directly by clients. Bounded radiance also prevents
	// a single pathological light from poisoning every subsequent frame through cache feedback.
	radiance = all(isfinite(radiance)) ? clamp(radiance, float3(0.0), float3(1e12)) : float3(0.0);
	g_Rays[slot * g_Probe.Update.z + ray] = float4(radiance, distance);
}

// The mirrored border must use the resolution of its own atlas: irradiance and visibility have
// different angular bandwidths. Keeping the visibility tile coarse mixes unrelated wall/open directions.
float3 FilterDirection(uint2 pixel, uint size)
{
	uint2 interior = pixel;
	if (interior.x == 0 || interior.x == size - 1)
		interior = uint2(interior.x == 0 ? 1 : size - 2, size - 1 - interior.y);
	if (interior.y == 0 || interior.y == size - 1)
		interior = uint2(size - 1 - interior.x, interior.y == 0 ? 1 : size - 2);
	return ProbeOctDecode((float2(interior) - 0.5) / float(size - 2));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void FilterProbes(uint3 dispatch : SV_DispatchThreadID)
{
	uint probeIndex = dispatch.z;
	uint distanceSize = g_Probe.Scene.z;
	if (probeIndex >= g_Probe.Grid.w || any(dispatch.xy >= distanceSize))
		return;
	uint3 pixel = uint3(dispatch.xy, probeIndex);
	bool writesIrradiance = all(dispatch.xy < 8);
	uint slot = (probeIndex + g_Probe.Grid.w - g_Probe.Update.x) % g_Probe.Grid.w;
	if (slot >= g_Probe.Update.y)
	{
		if (writesIrradiance)
			g_Irradiance[pixel] = g_PreviousIrradiance.Load(int4(pixel, 0));
		g_Distance[pixel] = g_PreviousDistance.Load(int4(pixel, 0));
		return;
	}
	float3 distanceDirection = FilterDirection(dispatch.xy, distanceSize);
	float3 irradianceDirection = writesIrradiance ? FilterDirection(dispatch.xy, 8) : float3(0.0);
	// Match the lobe's half-power radius to a cone covering the average interior texel solid angle.
	float interiorSize = float(distanceSize - 2);
	float exponent = log(0.5) / log(1.0 - 2.0 / (interiorSize * interiorSize));
	float3 irradiance = float3(0.0);
	float2 moments = float2(0.0);
	float irradianceWeight = 0.0, distanceWeight = 0.0;
	uint backFaces = 0;
	for (uint ray = 0; ray < g_Probe.Update.z; ++ray)
	{
		float4 sample = g_Rays[slot * g_Probe.Update.z + ray];
		backFaces += sample.w < 0.0 ? 1 : 0;
		float3 rayDirection = RayDirection(ray);
		float cosine = saturate(dot(irradianceDirection, rayDirection));
		irradiance += sample.rgb * cosine;
		irradianceWeight += cosine;
		float weight = pow(saturate(dot(distanceDirection, rayDirection)), exponent);
		float distance = abs(sample.w);
		moments += float2(distance, distance * distance) * weight;
		distanceWeight += weight;
	}
	float active = backFaces * 4 < g_Probe.Update.z ? 1.0 : 0.0;
	// Retain kernel support through history and bilinear reconstruction. Averaging per-frame
	// normalized moments gives a barely sampled texel the same influence as a well-sampled one.
	float3 nextDistance = float3(moments, distanceWeight) / float(g_Probe.Update.z);
	float4 previousDistance = g_PreviousDistance.Load(int4(pixel, 0));
	float history = previousDistance.w > 0.5 && active > 0.5 ? g_Probe.Tuning.z : 0.0;
	g_Distance[pixel] = float4(lerp(nextDistance, previousDistance.xyz, history), active);
	if (writesIrradiance)
	{
		float3 nextIrradiance = irradiance / max(irradianceWeight, 1e-6);
		float4 previousIrradiance = g_PreviousIrradiance.Load(int4(pixel, 0));
		g_Irradiance[pixel] = float4(lerp(nextIrradiance, previousIrradiance.rgb, history), active);
	}
}

// On-demand diagnostics do not update probe history.
struct InspectionConstants
{
	ProbeGIConstants Probe;
	float4 Position;
	float4 Normal;
	float4 View;
};
struct ProbeVisibilityRay
{
	float4 OriginLength;
	float4 EndOffset;
	float4 HitPositionDistance;
	uint4 Metadata;
};
struct NeighbourInspection
{
	float4 PositionDistance;
	float4 Moments;
	float4 Weights;
	float4 Sampling;
	float4 IrradianceWeight;
	uint4 Metadata;
	ProbeVisibilityRay SurfaceRay;
	ProbeVisibilityRay BiasedRay;
};
struct Inspection
{
	float4 Irradiance;
	float4 Summary;
	float4 SurfaceRejected, SurfaceExact, BiasedRejected, BiasedExact;
	NeighbourInspection Neighbours[8];
};

ConstantBuffer<InspectionConstants> g_Query : register(b2);
RWStructuredBuffer<Inspection> g_Result : register(u3);

// Diagnostic segments use the production tracer, including alpha masks and both triangle sides.
// Trace both starts: the tiny geometric offset avoids self hits; the shading bias can cross a wall.
ProbeVisibilityRay InspectVisibility(float3 receiver, float3 normal, float3 probe)
{
	ProbeVisibilityRay result = {};
	float epsilon = 1e-4;
	result.Metadata = uint4(0xffffffff, 0xffffffff, 0, 0);
	result.OriginLength = float4(receiver, 0.0);
	result.EndOffset = float4(receiver, epsilon);
	result.HitPositionDistance = float4(receiver, 0.0);
	if (all(probe == receiver))
		return result;
	float3 offset = normal * epsilon;
	float3 origin = receiver + offset;
	float3 roundingStep = abs(asfloat(asuint(receiver) + uint3(1)) - receiver);
	// A diagnostic must not report a clear ray when rounding or endpoint trimming makes it untestable.
	// Check each component so a large unrelated world coordinate cannot inflate the offset.
	if (length(probe - receiver) <= 2.0 * epsilon || any((abs(offset) > 1e-7) & (roundingStep > abs(offset))))
	{
		result.Metadata.w = 2; // indeterminate; bit zero remains the backface flag
		return result;
	}
	float3 direction = normalize(probe - origin);
	float maximum = max(length(probe - origin) - epsilon, 0.0);
	if (maximum <= 1e-4)
	{
		result.Metadata.w = 2;
		return result;
	}
	float3 end = origin + direction * maximum;
	TraceHit hit = Trace(origin, direction, maximum, false);
	result.OriginLength = float4(origin, maximum);
	result.EndOffset = float4(end, epsilon);
	result.HitPositionDistance = float4(origin + direction * hit.Distance, hit.Distance);
	if (hit.Triangle != 0xffffffff)
	{
		TraceTriangle triangle = g_Triangles[hit.Triangle];
		float3 geometric = cross(triangle.Position[1].xyz - triangle.Position[0].xyz, triangle.Position[2].xyz - triangle.Position[0].xyz);
		result.Metadata = uint4(hit.Triangle, triangle.Material.x, 1, dot(geometric, direction) > 0.0 ? 1 : 0);
	}
	return result;
}

[shader("compute")]
	[numthreads(1, 1, 1)]
void InspectProbeGI()
{
	Inspection result = {};
	ProbeSamplingPoint point = PrepareProbeSample(g_Query.Position.xyz, g_Query.Normal.xyz, g_Query.View.xyz, g_Query.Probe, g_Query.View.w);
	result.Irradiance = SampleProbeGI(g_Query.Position.xyz, g_Query.Normal.xyz, g_Query.View.xyz, g_Query.Probe, g_PreviousIrradiance, g_PreviousDistance, g_Query.View.w);
	if (point.Inside)
	{
		for (uint corner = 0; corner < 8; ++corner)
		{
			ProbeNeighbour neighbour = GetProbeNeighbour(point, corner, g_Query.Probe, g_PreviousDistance);
			result.Neighbours[corner].PositionDistance = float4(neighbour.Position, neighbour.Distance);
			result.Neighbours[corner].Moments = float4(neighbour.Moments.xy, neighbour.RawVariance, neighbour.Variance);
			result.Neighbours[corner].Sampling = float4(neighbour.Moments.z, neighbour.ExpectedMass, neighbour.Confidence, 0.0);
			result.Neighbours[corner].Weights = float4(neighbour.Trilinear, neighbour.Facing, neighbour.Visibility, neighbour.Weight);
			result.Neighbours[corner].IrradianceWeight = float4(ProbeDirectionalIrradiance(point.Normal, neighbour.Index, g_PreviousIrradiance), neighbour.Weight);
			result.Neighbours[corner].Metadata = uint4(neighbour.Index, neighbour.Moments.w >= 0.5 ? 1 : 0, 0, 0);
			ProbeVisibilityRay surface = InspectVisibility(g_Query.Position.xyz, point.Normal, neighbour.Position);
			ProbeVisibilityRay biased = InspectVisibility(point.Biased, point.Normal, neighbour.Position);
			result.Neighbours[corner].SurfaceRay = surface;
			result.Neighbours[corner].BiasedRay = biased;
			float4 irradiance = float4(result.Neighbours[corner].IrradianceWeight.xyz, 1.0);
			result.SurfaceRejected += irradiance * ((surface.Metadata.z == 0 && (surface.Metadata.w & 2) == 0) ? neighbour.Weight : 0.0);
			result.SurfaceExact += irradiance * ((surface.Metadata.z == 0 && (surface.Metadata.w & 2) == 0) ? neighbour.EligibleSupport : 0.0);
			result.BiasedRejected += irradiance * ((biased.Metadata.z == 0 && (biased.Metadata.w & 2) == 0) ? neighbour.Weight : 0.0);
			result.BiasedExact += irradiance * ((biased.Metadata.z == 0 && (biased.Metadata.w & 2) == 0) ? neighbour.EligibleSupport : 0.0);
			result.Summary.x += neighbour.Weight;
			result.Summary.z += neighbour.EligibleSupport;
			result.Summary.w += neighbour.Support;
			result.Summary.y += neighbour.Moments.w >= 0.5 ? 1.0 : 0.0;
		}
		for (uint corner = 0; corner < 8; ++corner)
			result.Neighbours[corner].IrradianceWeight.w *= ProbeNormalization(result.Summary.x, result.Summary.z, result.Summary.w);
		result.SurfaceRejected.xyz *= ProbeNormalization(result.SurfaceRejected.w, result.Summary.z, result.Summary.w);
		result.SurfaceExact.xyz *= ProbeNormalization(result.SurfaceExact.w, result.Summary.z, result.Summary.w);
		result.BiasedRejected.xyz *= ProbeNormalization(result.BiasedRejected.w, result.Summary.z, result.Summary.w);
		result.BiasedExact.xyz *= ProbeNormalization(result.BiasedExact.w, result.Summary.z, result.Summary.w);
	}
	g_Result[0] = result;
}

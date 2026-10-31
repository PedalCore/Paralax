// Cubemap face/texel <-> direction helpers (Vulkan/D3D cube face order: +X, -X, +Y, -Y, +Z, -Z).
module CubeMap;

// Direction through the centre of texel (x, y) of a cube face of the given size.
public float3 CubeTexelDirection(uint face, float2 texel, float size)
{
	float2 uv = (texel + 0.5) / size * 2.0 - 1.0; // [-1, 1], v down
	switch (face)
	{
		case 0: return normalize(float3(1.0, -uv.y, -uv.x));
		case 1: return normalize(float3(-1.0, -uv.y, uv.x));
		case 2: return normalize(float3(uv.x, 1.0, uv.y));
		case 3: return normalize(float3(uv.x, -1.0, -uv.y));
		case 4: return normalize(float3(uv.x, -uv.y, 1.0));
		default: return normalize(float3(-uv.x, -uv.y, -1.0));
	}
}

// Equirectangular (latitude-longitude) UV for a direction; +Y up, u = 0.5 at -Z.
public float2 DirectionToEquirect(float3 direction)
{
	float u = atan2(direction.x, -direction.z) / (2.0 * 3.14159265358979) + 0.5;
	float v = acos(clamp(direction.y, -1.0, 1.0)) / 3.14159265358979;
	return float2(u, v);
}

// Van der Corput / Hammersley low-discrepancy sequence.
public float2 Hammersley(uint i, uint count)
{
	uint bits = reversebits(i);
	return float2(float(i) / float(count), float(bits) * 2.3283064365386963e-10);
}

public float3 ImportanceSampleGGX(float2 xi, float3 N, float roughness)
{
	float a = roughness * roughness;
	float phi = 2.0 * 3.14159265358979 * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

	float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	float3 tangent = normalize(cross(up, N));
	float3 bitangent = cross(N, tangent);
	return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

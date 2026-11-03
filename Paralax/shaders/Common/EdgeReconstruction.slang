module EdgeReconstruction;

// Current-frame directional reconstruction. Coordinates name texel centers (0,0 is
// the first center), so temporal jitter and spatial output use the same footprint.
public
float3 EdgeLoad(Texture2D<float4> source, int2 pixel, int2 size)
{
	float3 color = source.Load(int3(clamp(pixel, int2(0), size - 1), 0)).rgb;
	return all(isfinite(color)) ? color : float3(0);
}

public
float3 EdgeSample(Texture2D<float4> source, float2 pixel, int2 size)
{
	int2 first = int2(floor(pixel));
	float2 weight = frac(pixel);
	return lerp(lerp(EdgeLoad(source, first, size), EdgeLoad(source, first + int2(1, 0), size), weight.x),
				lerp(EdgeLoad(source, first + int2(0, 1), size), EdgeLoad(source, first + int2(1, 1), size), weight.x), weight.y);
}

public
float EdgeLuma(float3 color)
{
	return sqrt(max(dot(color, float3(0.2126, 0.7152, 0.0722)), 0));
}

// The directional/endpoint idea follows Lottes' FXAA whitepaper; this is a custom
// bounded filter, not the FXAA reference implementation or its quality presets.
// https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf
public
float3 ReconstructEdge(Texture2D<float4> source, float2 position, int2 size, float minimumContrast, out bool reconstructed)
{
	reconstructed = false;
	float2 pixel = floor(position + 0.5);
	float3 original = EdgeSample(source, position, size);
	float m = EdgeLuma(EdgeLoad(source, int2(pixel), size));
	float n = EdgeLuma(EdgeLoad(source, int2(pixel + float2(0, -1)), size));
	float s = EdgeLuma(EdgeLoad(source, int2(pixel + float2(0, 1)), size));
	float w = EdgeLuma(EdgeLoad(source, int2(pixel + float2(-1, 0)), size));
	float e = EdgeLuma(EdgeLoad(source, int2(pixel + float2(1, 0)), size));
	float high = max(m, max(max(n, s), max(w, e)));
	float low = min(m, min(min(n, s), min(w, e)));
	float contrast = high - low;
	if (contrast < max(minimumContrast, high * 0.125))
		return original;
	float nw = EdgeLuma(EdgeLoad(source, int2(pixel + float2(-1, -1)), size));
	float ne = EdgeLuma(EdgeLoad(source, int2(pixel + float2(1, -1)), size));
	float sw = EdgeLuma(EdgeLoad(source, int2(pixel + float2(-1, 1)), size));
	float se = EdgeLuma(EdgeLoad(source, int2(pixel + float2(1, 1)), size));
	float horizontal = abs(nw + sw - 2 * w) + 2 * abs(n + s - 2 * m) + abs(ne + se - 2 * e);
	float vertical = abs(nw + ne - 2 * n) + 2 * abs(w + e - 2 * m) + abs(sw + se - 2 * s);
	bool alongX = horizontal >= vertical;
	float minus = alongX ? n : w;
	float plus = alongX ? s : e;
	bool towardMinus = abs(minus - m) >= abs(plus - m);
	float neighbor = towardMinus ? minus : plus;
	float gradient = abs(neighbor - m);
	if (gradient <= 0)
		return original;
	float2 normal = alongX ? float2(0, 1) : float2(1, 0);
	normal *= towardMinus ? -1 : 1;
	float2 tangent = alongX ? float2(1, 0) : float2(0, 1);
	// Fractional tangential footprints need both source columns/rows. The
	// coverage model is safe only for two locally constant colors: collinearity
	// alone also admits varying same-hue shading, which it would discard.
	if (abs(dot(position - pixel, tangent)) > 0)
	{
		float3 centerColor = EdgeLoad(source, int2(pixel), size);
		float3 neighborColor = EdgeLoad(source, int2(pixel + normal), size);
		float colorScale = max(max(max(abs(centerColor.x), abs(centerColor.y)), abs(centerColor.z)),
							   max(max(abs(neighborColor.x), abs(neighborColor.y)), abs(neighborColor.z)));
		[unroll] for (int y = -1; y <= 1; ++y)
			[unroll] for (int x = -1; x <= 1; ++x)
		{
			float3 sample = EdgeLoad(source, int2(pixel) + int2(x, y), size);
			float3 fromCenter = abs(sample - centerColor), fromNeighbor = abs(sample - neighborColor);
			float residual = min(max(max(fromCenter.x, fromCenter.y), fromCenter.z),
								 max(max(fromNeighbor.x, fromNeighbor.y), fromNeighbor.z));
			if (residual > colorScale * 1e-5)
				return original;
		}
	}
	float pair = (m + neighbor) * 0.5;
	float distanceMinus = 1, distancePlus = 1;
	float endMinus = 0, endPlus = 0;
	bool doneMinus = false, donePlus = false;
	// Unit steps retain the one-pixel turns in thin pipes and repeating grout lines.
	// Search up to 64 texels for shallow pipes; endpoint colors never enter the output kernel.
	[loop] for (uint step = 0; step < 64; ++step)
	{
		if (!doneMinus)
		{
			float2 p = pixel - tangent * distanceMinus;
			endMinus = (EdgeLuma(EdgeLoad(source, int2(p), size)) + EdgeLuma(EdgeLoad(source, int2(p + normal), size))) * 0.5 - pair;
			doneMinus = abs(endMinus) >= gradient * 0.25;
		}
		if (!donePlus)
		{
			float2 p = pixel + tangent * distancePlus;
			endPlus = (EdgeLuma(EdgeLoad(source, int2(p), size)) + EdgeLuma(EdgeLoad(source, int2(p + normal), size))) * 0.5 - pair;
			donePlus = abs(endPlus) >= gradient * 0.25;
		}
		if (doneMinus && donePlus)
			break;
		if (!doneMinus)
			distanceMinus += 1;
		if (!donePlus)
			distancePlus += 1;
	}
	// No complete span means the feature may be an already resolved axis edge,
	// corner, or repeating high-frequency material detail. Preserve its bilinear
	// footprint instead of inventing coverage or erasing tangential contributions.
	if (!doneMinus || !donePlus)
		return original;
	if ((endMinus < 0) == (endPlus < 0))
	{
		// A curved contour has two endpoints on the same side. Require a plateau
		// on both sides of the normal before correcting it: impulses, one-pixel
		// ridges and checker detail do not describe a separated surface edge.
		float behind = EdgeLuma(EdgeLoad(source, int2(pixel - normal), size));
		float beyond = EdgeLuma(EdgeLoad(source, int2(pixel + normal * 2), size));
		if (abs(behind - m) > gradient * 0.25 || abs(beyond - neighbor) > gradient * 0.25)
			return original;
		// A short rectangular detail has plateaus too, but its endpoint stays
		// outside on the next row. Require the contour to widen inward at both
		// endpoints before interpreting a closed span as curvature.
		bool centerBounded = ((endMinus < 0) != (m - pair < 0));
		float2 inward = centerBounded ? -normal : normal * 2;
		float interior = centerBounded ? m : neighbor;
		float extensionMinus = EdgeLuma(EdgeLoad(source, int2(pixel - tangent * distanceMinus + inward), size));
		float extensionPlus = EdgeLuma(EdgeLoad(source, int2(pixel + tangent * distancePlus + inward), size));
		if (abs(extensionMinus - interior) > gradient * 0.25 || abs(extensionPlus - interior) > gradient * 0.25)
			return original;
		float average = (2 * (n + s + w + e) + nw + ne + sw + se) / 12;
		float subpixel = smoothstep(0, 1, saturate(abs(average - m) / contrast));
		float offset = min(0.5, 0.75 * subpixel * subpixel);
		reconstructed = offset > 0;
		return EdgeSample(source, position + normal * offset, size);
	}
	float coverage;
	{
		// Endpoints straddle a coherent staircase. Their half-texel crossings
		// define a local line; integrate it at the unjittered output footprint.
		float span = distanceMinus + distancePlus - 1;
		bool increasing = (endMinus > 0) == (neighbor > m);
		float slope = (increasing ? 1 : -1) / span;
		float intercept = increasing ? (distanceMinus - 0.5) / span : (distancePlus - 0.5) / span;
		float2 delta = position - pixel;
		float a = 0.5 + dot(delta, normal) - intercept - slope * dot(delta, tangent);
		float halfSlope = abs(slope) * 0.5;
		float lo = a - halfSlope, hi = a + halfSlope;
		float integralLo = 0.5 * (max(lo, 0) * max(lo, 0) - max(lo - 1, 0) * max(lo - 1, 0));
		float integralHi = 0.5 * (max(hi, 0) * max(hi, 0) - max(hi - 1, 0) * max(hi - 1, 0));
		coverage = saturate((integralHi - integralLo) / abs(slope));
	}
	reconstructed = true;
	return lerp(EdgeLoad(source, int2(pixel), size), EdgeLoad(source, int2(pixel + normal), size), coverage);
}

module FXAA;

float FXAALuma(float3 color)
{
	return dot(color, float3(0.299, 0.587, 0.114));
}

float3 FXAASample(Texture2D<float4> color, SamplerState linearSampler, float2 size, float2 center)
{
	return color.SampleLevel(linearSampler, center / size, 0).rgb;
}

public
struct FXAAEndpoint
{
public
	float Distance;
public
	float Luminance;
public
	bool Found;
};

public
FXAAEndpoint FXAASearch(Texture2D<float4> color, SamplerState linearSampler, float2 size, float2 origin, float2 direction, float mean, float threshold)
{
	FXAAEndpoint result = {0, mean, false};
	// A missing endpoint contributes only the span actually tested, including at image borders.
	[loop] for (int distance = 1; distance <= 16; ++distance)
	{
		float2 position = origin + direction * distance;
		float tangentPosition = dot(position, abs(direction));
		float tangentExtent = dot(size, abs(direction));
		if (tangentPosition < 0.5 || tangentPosition > tangentExtent - 0.5)
			break;
		result.Distance = float(distance);
		result.Luminance = FXAALuma(FXAASample(color, linearSampler, size, position));
		if (abs(result.Luminance - mean) >= threshold)
		{
			result.Found = true;
			break;
		}
	}
	return result;
}

public
float FXAAOffset(FXAAEndpoint first, FXAAEndpoint second, float pairMean, float centerLuma)
{
	if (!first.Found && !second.Found)
		return 0;
	FXAAEndpoint nearest = first.Found && (!second.Found || first.Distance <= second.Distance) ? first : second;
	float span = first.Distance + second.Distance;
	if (span > 0 && (nearest.Luminance - pairMean) * (centerLuma - pairMean) < 0)
		return clamp(0.5 - nearest.Distance / span, 0.0, 0.5);
	return 0;
}

// Independent bounded implementation of the 2011 FXAA whitepaper's directional search and
// subpixel blend. Full RGB luma and a fixed 16-sample search are this engine's deliberate preset.
public
float3 FilterFXAA(Texture2D<float4> color, SamplerState linearSampler, float2 size, float2 position)
{
	float3 colors[9];
	float luma[9];
	float3 meanRGB = 0;
	[unroll] for (int y = 0; y < 3; ++y)
		[unroll] for (int x = 0; x < 3; ++x)
	{
		int index = y * 3 + x;
		colors[index] = FXAASample(color, linearSampler, size, position + float2(x - 1, y - 1));
		luma[index] = FXAALuma(colors[index]);
		meanRGB += colors[index] / 9.0;
	}
	float minimum = min(luma[4], min(min(luma[1], luma[7]), min(luma[3], luma[5])));
	float maximum = max(luma[4], max(max(luma[1], luma[7]), max(luma[3], luma[5])));
	float range = maximum - minimum;
	if (range < max(1.0 / 16.0, maximum / 8.0))
		return colors[4];

	float vertical = abs(luma[0] - 2 * luma[1] + luma[2]) + 2 * abs(luma[3] - 2 * luma[4] + luma[5]) + abs(luma[6] - 2 * luma[7] + luma[8]);
	float horizontal = abs(luma[0] - 2 * luma[3] + luma[6]) + 2 * abs(luma[1] - 2 * luma[4] + luma[7]) + abs(luma[2] - 2 * luma[5] + luma[8]);
	bool horizontalEdge = horizontal >= vertical;
	float negative = horizontalEdge ? luma[1] : luma[3];
	float positive = horizontalEdge ? luma[7] : luma[5];
	bool negativePair = abs(negative - luma[4]) >= abs(positive - luma[4]);
	float neighbor = negativePair ? negative : positive;
	float2 perpendicular = (horizontalEdge ? float2(0, 1) : float2(1, 0)) * (negativePair ? -1.0 : 1.0);
	float2 tangent = horizontalEdge ? float2(1, 0) : float2(0, 1);
	float pairMean = (neighbor + luma[4]) * 0.5;
	float threshold = abs(neighbor - luma[4]) * 0.25;
	float2 origin = position + perpendicular * 0.5;
	FXAAEndpoint first = FXAASearch(color, linearSampler, size, origin, -tangent, pairMean, threshold);
	FXAAEndpoint second = FXAASearch(color, linearSampler, size, origin, tangent, pairMean, threshold);
	float offset = FXAAOffset(first, second, pairMean, luma[4]);
	float3 edge = FXAASample(color, linearSampler, size, position + perpendicular * offset);
	float cardinalMean = (luma[1] + luma[3] + luma[5] + luma[7]) * 0.25;
	float subpixel = clamp((abs(cardinalMean - luma[4]) / range - 0.25) / 0.75, 0.0, 0.75);
	return lerp(edge, meanRGB, subpixel);
}

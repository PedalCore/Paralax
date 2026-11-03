module AreaFootprint;

// Exact integer overlap of pixel boxes on two grids. Renderer dimensions are bounded
// to 8192 for reduction, so products fit uint. A scale of at most two touches at most
// three source texels per axis, including fractional boundaries at odd dimensions.
public
uint2 AreaFirst(uint2 pixel, uint2 sourceSize, uint2 outputSize)
{
	return pixel * sourceSize / outputSize;
}

public
uint2 AreaOverlap(uint2 pixel, uint2 source, uint2 sourceSize, uint2 outputSize)
{
	uint2 low = max(pixel * sourceSize, source * outputSize);
	uint2 high = min((pixel + 1) * sourceSize, (source + 1) * outputSize);
	return high - min(low, high);
}

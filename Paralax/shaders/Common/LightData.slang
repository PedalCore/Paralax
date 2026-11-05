module LightData;
import Common.SceneData;

// Same four float4 records as the structured ProbeGI view. Masks never pass through floats.
public GPULight LoadRawLight(ByteAddressBuffer data, uint index)
{
	GPULight result;
	uint offset = index * 64;
	result.PositionRange = asfloat(data.Load4(offset));
	result.ColorIntensity = asfloat(data.Load4(offset + 16));
	result.DirectionCos = asfloat(data.Load4(offset + 32));
	result.Params = asfloat(data.Load4(offset + 48));
	return result;
}

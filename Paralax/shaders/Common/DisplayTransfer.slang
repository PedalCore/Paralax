module DisplayTransfer;

// Exact sRGB transfer for ordinary float intermediates: these textures have no hardware decoding.
public
float3 EncodeSRGB(float3 linear)
{
	linear = saturate(linear);
	return select(linear <= 0.0031308, linear * 12.92, 1.055 * pow(linear, float3(1.0 / 2.4)) - 0.055);
}

public
float3 DecodeSRGB(float3 encoded)
{
	encoded = saturate(encoded);
	return select(encoded <= 0.04045, encoded / 12.92, pow((encoded + 0.055) / 1.055, float3(2.4)));
}

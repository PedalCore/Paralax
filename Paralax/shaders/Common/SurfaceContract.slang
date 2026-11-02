// All opaque paths preserve the mapped, shadow-offset and receiver-plane normals independently.
module SurfaceContract;
import Common.PBR;

public
struct MaterialSurface
{
public
	SurfaceData Surface;
	// Before normal mapping, oriented to the visible face: offsets shadow receivers.
public
	float3 ShadowNormal;
	// Triangle plane from world-position derivatives: corrects shadow filter depth.
public
	float3 ReceiverNormal;
};

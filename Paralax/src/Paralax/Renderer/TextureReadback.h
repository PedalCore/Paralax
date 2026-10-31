#pragma once

#include "Paralax/Renderer/Image.h"

#include <nvrhi/nvrhi.h>

namespace Paralax {

	// Copies mip 0 / slice 0 of a GPU texture into a CPU image, blocking until the copy completes. Supports
	// RGBA8 (UNORM/SRGB) and BGRA8 (UNORM/SRGB) textures. Returns an empty image (with an error logged) otherwise.
	// The texture must be single-sampled 2D with a known state (keepInitialState, or a permanent state).
	// Intended for captures, tests and agent screenshots, not per-frame use.
	Image ReadbackTexture(nvrhi::IDevice* device, nvrhi::ITexture* texture);

}

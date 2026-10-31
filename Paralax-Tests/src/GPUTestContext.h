#pragma once

#include <Paralax/Renderer/GraphicsDevice.h>

#include <filesystem>

namespace Paralax::Testing {

	// Shared headless graphics device for GPU tests, created on first use with validation enabled.
	//
	// Returns nullptr when no Vulkan device is available. GPU test cases then pass with a warning, unless the
	// PX_REQUIRE_GPU environment variable is set (CI with a Vulkan driver), in which case the absence fails.
	GraphicsDevice* GetTestGraphicsDevice();

	// Destroys the shared device (it is recreated on next use). Required before a test creates its own device,
	// because only one GraphicsDevice may exist at a time.
	void ReleaseTestGraphicsDevice();

	// True if GPU tests should run; otherwise records a warning or failure as described above.
	bool RequireTestGraphicsDevice();

	// Directory for images written by tests (inspected by humans and CI artifacts).
	std::filesystem::path GetTestOutputDirectory();

}

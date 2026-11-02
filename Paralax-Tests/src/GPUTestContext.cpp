#include "GPUTestContext.h"

#include <doctest/doctest.h>

#include <cstdlib>

namespace Paralax::Testing {

	namespace {

		struct DeviceHolder
		{
			Scope<GraphicsDevice> Device;
			bool Attempted = false;
		};

		DeviceHolder& GetHolder()
		{
			static DeviceHolder s_Holder;
			return s_Holder;
		}

	}

	GraphicsDevice* GetTestGraphicsDevice()
	{
		DeviceHolder& holder = GetHolder();
		if (!holder.Attempted)
		{
			holder.Attempted = true;
			GraphicsDeviceSpecification specification;
			specification.Headless = true;
			specification.EnableValidation = true;
			holder.Device = GraphicsDevice::Create(specification);
		}
		return holder.Device.get();
	}

	void ReleaseTestGraphicsDevice()
	{
		DeviceHolder& holder = GetHolder();
		holder.Device.reset();
		holder.Attempted = false;
	}

	bool RequireTestGraphicsDevice()
	{
		if (GetTestGraphicsDevice())
			return true;

		const char* required = std::getenv("PX_REQUIRE_GPU");
		if (required && *required && std::string(required) != "0")
			FAIL("No Vulkan device available but PX_REQUIRE_GPU is set");
		else
			WARN_MESSAGE(false, "No Vulkan device available; GPU test skipped");
		return false;
	}

	bool RequireSynchronizationValidation()
	{
		if (!RequireTestGraphicsDevice())
			return false;
		if (GetTestGraphicsDevice()->GetInfo().SynchronizationValidation)
			return true;

		const char* required = std::getenv("PX_REQUIRE_VALIDATION_LAYER");
		if (required && *required && std::string(required) != "0")
			FAIL("Synchronization validation is unavailable but PX_REQUIRE_VALIDATION_LAYER is set");
		else
			WARN_MESSAGE(false, "Synchronization validation is unavailable (no validation layer); test skipped");
		return false;
	}

	std::filesystem::path GetTestOutputDirectory()
	{
		std::filesystem::path directory = std::filesystem::current_path() / "TestOutput";
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		return directory;
	}

}

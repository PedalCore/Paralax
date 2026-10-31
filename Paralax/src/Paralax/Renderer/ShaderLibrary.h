#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Paralax {

	// Access to the SPIR-V shader modules compiled from Paralax/shaders at build time. A module is named after its
	// source file ("Fullscreen" for Fullscreen.slang) and may contain several entry points.
	class ShaderLibrary
	{
	public:
		// Returns the module's SPIR-V, or an empty span if no module has that name.
		static std::span<const uint8_t> GetModule(std::string_view name);
		static std::vector<std::string_view> GetModuleNames();

		// Creates a shader for one entry point of an embedded module. Returns nullptr (with an error logged) if the
		// module does not exist or the device rejects it.
		static nvrhi::ShaderHandle CreateShader(nvrhi::IDevice* device, std::string_view module, const char* entryPoint, nvrhi::ShaderType type);
	};

}

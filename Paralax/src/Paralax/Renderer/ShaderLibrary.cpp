#include "pxpch.h"
#include "Paralax/Renderer/ShaderLibrary.h"

#include "Paralax/Renderer/EmbeddedShaderTable.h"

namespace Paralax {

	std::span<const uint8_t> ShaderLibrary::GetModule(std::string_view name)
	{
		for (size_t i = 0; i < EmbeddedShaders::g_EntryCount; i++)
		{
			const EmbeddedShaders::Entry& entry = EmbeddedShaders::g_Entries[i];
			if (name == entry.Name)
				return {entry.Data, entry.Size};
		}
		return {};
	}

	std::vector<std::string_view> ShaderLibrary::GetModuleNames()
	{
		std::vector<std::string_view> names;
		names.reserve(EmbeddedShaders::g_EntryCount);
		for (size_t i = 0; i < EmbeddedShaders::g_EntryCount; i++)
			names.emplace_back(EmbeddedShaders::g_Entries[i].Name);
		return names;
	}

	nvrhi::ShaderHandle ShaderLibrary::CreateShader(nvrhi::IDevice* device, std::string_view module, const char* entryPoint, nvrhi::ShaderType type)
	{
		PX_CORE_VERIFY(device, "CreateShader requires a device");

		std::span<const uint8_t> code = GetModule(module);
		if (code.empty())
		{
			PX_CORE_ERROR("Shader module '{}' is not embedded in this build", module);
			return nullptr;
		}

		nvrhi::ShaderDesc desc;
		desc.shaderType = type;
		desc.entryName = entryPoint;
		desc.debugName = fmt::format("{}:{}", module, entryPoint);

		nvrhi::ShaderHandle shader = device->createShader(desc, code.data(), code.size());
		if (!shader)
			PX_CORE_ERROR("Failed to create shader {}", desc.debugName);
		return shader;
	}

}

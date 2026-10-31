#include "pxpch.h"
#include "Paralax/Core/DynamicLibrary.h"

#include <spdlog/fmt/ranges.h>

#if !defined(PX_PLATFORM_WINDOWS)

	#include <dlfcn.h>

namespace Paralax {

	DynamicLibrary::~DynamicLibrary()
	{
		Unload();
	}

	DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
		: m_Handle(std::exchange(other.m_Handle, nullptr))
		, m_LoadedName(std::move(other.m_LoadedName))
	{
	}

	DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
	{
		if (this != &other)
		{
			Unload();
			m_Handle = std::exchange(other.m_Handle, nullptr);
			m_LoadedName = std::move(other.m_LoadedName);
		}
		return *this;
	}

	bool DynamicLibrary::Load(const std::vector<std::string>& candidateNames)
	{
		Unload();

		for (const std::string& name : candidateNames)
		{
			m_Handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
			if (m_Handle)
			{
				m_LoadedName = name;
				return true;
			}

			const char* error = dlerror();
			PX_CORE_TRACE("Could not load '{}': {}", name, error ? error : "unknown error");
		}

		PX_CORE_ERROR("Failed to load any of: {}", fmt::join(candidateNames, ", "));
		return false;
	}

	void DynamicLibrary::Unload()
	{
		if (m_Handle)
		{
			dlclose(m_Handle);
			m_Handle = nullptr;
			m_LoadedName.clear();
		}
	}

	void* DynamicLibrary::GetSymbol(const char* name) const
	{
		return m_Handle ? dlsym(m_Handle, name) : nullptr;
	}

}

#endif

#include "pxpch.h"
#include "Paralax/Core/DynamicLibrary.h"

#include <spdlog/fmt/ranges.h>

#if defined(PX_PLATFORM_WINDOWS)

	#include <Windows.h>

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
			HMODULE module = LoadLibraryA(name.c_str());
			if (module)
			{
				m_Handle = reinterpret_cast<void*>(module);
				m_LoadedName = name;
				return true;
			}
			PX_CORE_TRACE("Could not load '{}': error {}", name, GetLastError());
		}

		PX_CORE_ERROR("Failed to load any of: {}", fmt::join(candidateNames, ", "));
		return false;
	}

	void DynamicLibrary::Unload()
	{
		if (m_Handle)
		{
			FreeLibrary(reinterpret_cast<HMODULE>(m_Handle));
			m_Handle = nullptr;
			m_LoadedName.clear();
		}
	}

	void* DynamicLibrary::GetSymbol(const char* name) const
	{
		if (!m_Handle)
			return nullptr;
		return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(m_Handle), name));
	}

}

#endif

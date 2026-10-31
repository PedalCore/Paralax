#pragma once

#include "Paralax/Core/Base.h"

#include <string>
#include <vector>

namespace Paralax {

	// A loaded shared library (.so / .dylib / .dll). Unloaded on destruction. Not copyable; movable.
	class DynamicLibrary
	{
	public:
		DynamicLibrary() = default;
		~DynamicLibrary();

		DynamicLibrary(const DynamicLibrary&) = delete;
		DynamicLibrary& operator=(const DynamicLibrary&) = delete;
		DynamicLibrary(DynamicLibrary&& other) noexcept;
		DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

		// Tries each name in order (as passed to the platform loader, so search paths apply) and keeps the first
		// that loads. Returns false and logs every attempt's error if none load.
		bool Load(const std::vector<std::string>& candidateNames);
		void Unload();

		bool IsLoaded() const { return m_Handle != nullptr; }
		const std::string& GetLoadedName() const { return m_LoadedName; }

		// Returns the address of an exported symbol, or nullptr if the library is not loaded or lacks it.
		void* GetSymbol(const char* name) const;

		template<typename T>
		T GetFunction(const char* name) const
		{
			return reinterpret_cast<T>(GetSymbol(name));
		}

	private:
		void* m_Handle = nullptr;
		std::string m_LoadedName;
	};

}

#include "pxpch.h"
#include "Paralax/Core/EngineResources.h"

#if defined(PX_PLATFORM_WINDOWS)

	#include <Windows.h>

	#include <vector>

namespace Paralax {

	std::filesystem::path GetExecutablePath()
	{
		// Grow the buffer until the path fits (long paths can exceed MAX_PATH).
		std::vector<wchar_t> buffer(MAX_PATH);
		for (;;)
		{
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0)
				return {};
			if (length < buffer.size())
				return std::filesystem::path(std::wstring(buffer.data(), length));
			if (buffer.size() >= 32768)
				return {};
			buffer.resize(buffer.size() * 2);
		}
	}

}

#endif

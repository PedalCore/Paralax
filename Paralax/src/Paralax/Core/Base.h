#pragma once

#include "Paralax/Core/Config.h"

#include <memory>
#include <utility>

#define PX_EXPAND_MACRO(x) x
#define PX_STRINGIFY_MACRO(x) #x

#define PX_BIT(x) (1u << (x))

// Binds a member function as an event callback for the current object.
#define PX_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

namespace Paralax {

	// Owning pointer with a single owner.
	template<typename T>
	using Scope = std::unique_ptr<T>;

	template<typename T, typename... Args>
	constexpr Scope<T> CreateScope(Args&&... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	// Shared, reference-counted owning pointer.
	template<typename T>
	using Ref = std::shared_ptr<T>;

	template<typename T, typename... Args>
	constexpr Ref<T> CreateRef(Args&&... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}

}

#include "Paralax/Core/Log.h"
#include "Paralax/Core/Assert.h"

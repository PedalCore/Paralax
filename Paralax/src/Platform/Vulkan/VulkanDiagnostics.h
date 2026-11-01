#pragma once

namespace Paralax::VulkanDiagnostics {

	// Temporary main-thread setup after NVRHI device creation. No-op unless PX_TRACE_BRDF_BARRIERS=1.
	// Installs thread-safe, pass-through logging hooks; returns false with an error if required entry points are missing.
	bool EnableBRDFTrace();

}

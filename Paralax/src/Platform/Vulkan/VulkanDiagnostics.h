#pragma once

namespace Paralax::VulkanDiagnostics {

	// Temporary main-thread setup after NVRHI device creation. PX_TRACE_BRDF_BARRIERS=1 records thread-safe logs;
	// "passthrough" installs silent forwarding hooks without tracking or locking; other values leave dispatch unchanged.
	// Returns false with an error if required entry points are missing.
	bool EnableBRDFTrace();

}

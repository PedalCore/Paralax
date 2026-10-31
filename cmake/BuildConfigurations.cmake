# Paralax build configurations (mirroring Hazel):
#   Debug   - no optimisation, asserts and logging on
#   Release - optimised, asserts and logging on
#   Dist    - optimised, asserts compiled out, editor-free distribution builds

get_property(PX_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(PX_IS_MULTI_CONFIG)
	set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Dist" CACHE STRING "Available build configurations" FORCE)
else()
	if(NOT CMAKE_BUILD_TYPE)
		set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build configuration" FORCE)
	endif()
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "Dist")
	if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release|Dist)$")
		message(FATAL_ERROR "Unsupported CMAKE_BUILD_TYPE '${CMAKE_BUILD_TYPE}'. Use Debug, Release or Dist.")
	endif()
endif()

foreach(lang C CXX)
	set(CMAKE_${lang}_FLAGS_DIST "${CMAKE_${lang}_FLAGS_RELEASE}" CACHE STRING "Flags used by the ${lang} compiler for Dist builds" FORCE)
endforeach()
foreach(kind EXE SHARED MODULE STATIC)
	set(CMAKE_${kind}_LINKER_FLAGS_DIST "${CMAKE_${kind}_LINKER_FLAGS_RELEASE}" CACHE STRING "Linker flags for Dist builds" FORCE)
endforeach()
mark_as_advanced(CMAKE_C_FLAGS_DIST CMAKE_CXX_FLAGS_DIST CMAKE_EXE_LINKER_FLAGS_DIST
	CMAKE_SHARED_LINKER_FLAGS_DIST CMAKE_MODULE_LINKER_FLAGS_DIST CMAKE_STATIC_LINKER_FLAGS_DIST)

# Third-party packages only know Debug/Release; map Dist onto Release for imported targets.
set(CMAKE_MAP_IMPORTED_CONFIG_DIST Release)

# Use the static MSVC runtime consistently (/MT, /MTd) across the engine and all dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

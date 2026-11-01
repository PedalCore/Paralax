cmake_minimum_required(VERSION 3.25)

project(Paralax
	VERSION 0.1.0
	DESCRIPTION "Paralax 3D game engine"
	LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset <name>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(PX_BUILD_TESTS "Build the Paralax unit tests" ON)
option(PX_BUILD_EDITOR "Build the Paralax editor" ON)
option(PX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
set(PX_SANITIZERS "" CACHE STRING "Sanitizers for Paralax targets (GCC/Clang): empty, \"address,undefined\" or \"thread\"")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(BuildConfigurations)
include(CompilerOptions)
include(Dependencies)
include(Shaders)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

add_subdirectory(Paralax)
add_subdirectory(Paralax-Runtime)

if(PX_BUILD_EDITOR)
	add_subdirectory(Paralax-Editor)
endif()

if(PX_BUILD_TESTS)
	enable_testing()
	add_subdirectory(Paralax-Tests)
endif()

# All executables share this output directory, with a configuration subdirectory on MSVC/Xcode.
# Run on every build so deleting Resources is repaired even when no executable needs relinking.
set(PX_RESOURCE_OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(CMAKE_CONFIGURATION_TYPES)
	string(APPEND PX_RESOURCE_OUTPUT "/$<CONFIG>")
endif()
add_custom_target(Paralax-Resources ALL
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${PX_RESOURCE_OUTPUT}/Resources/Environments"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${PX_ENVIRONMENTS} "${PX_RESOURCE_OUTPUT}/Resources/Environments"
	DEPENDS ${PX_ENVIRONMENTS}
	VERBATIM)
foreach(executable Paralax-Runtime Paralax-Editor Paralax-Tests Paralax-WindowedTests)
	if(TARGET ${executable})
		add_dependencies(${executable} Paralax-Resources)
	endif()
endforeach()

# Third-party dependencies, pinned to exact versions via CPM.cmake (vendored in cmake/CPM.cmake).
# Set CPM_SOURCE_CACHE (e.g. ~/.cache/CPM) to share downloads between build trees and worktrees.
#
# Policy: every dependency is pinned to an exact release tag. Bumping a version is a reviewed change
# that must keep the full test suite green on every CI platform.

include(CPM)

# --- GLFW (windowing and input) -----------------------------------------------------------------
CPMAddPackage(
	NAME glfw
	GITHUB_REPOSITORY glfw/glfw
	GIT_TAG 3.5.1
	OPTIONS
		"GLFW_BUILD_DOCS OFF"
		"GLFW_BUILD_TESTS OFF"
		"GLFW_BUILD_EXAMPLES OFF"
		"GLFW_INSTALL OFF"
		"BUILD_SHARED_LIBS OFF")

# --- glm (math) ---------------------------------------------------------------------------------
CPMAddPackage(
	NAME glm
	GITHUB_REPOSITORY g-truc/glm
	GIT_TAG 1.0.1
	OPTIONS
		"GLM_BUILD_TESTS OFF"
		"GLM_BUILD_LIBRARY OFF"
		"GLM_ENABLE_CXX_20 ON")

# --- spdlog (logging) ---------------------------------------------------------------------------
CPMAddPackage(
	NAME spdlog
	GITHUB_REPOSITORY gabime/spdlog
	VERSION 1.15.3
	OPTIONS
		"SPDLOG_BUILD_EXAMPLE OFF"
		"SPDLOG_BUILD_TESTS OFF"
		"SPDLOG_INSTALL OFF"
		"SPDLOG_FMT_EXTERNAL OFF")

# --- doctest (unit tests) -----------------------------------------------------------------------
if(PX_BUILD_TESTS)
	CPMAddPackage(
		NAME doctest
		GITHUB_REPOSITORY doctest/doctest
		VERSION 2.4.12
		DOWNLOAD_ONLY YES)
	if(doctest_ADDED)
		add_library(doctest INTERFACE)
		add_library(doctest::doctest ALIAS doctest)
		target_include_directories(doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
	endif()
endif()

# Treat third-party headers as system headers so Paralax's strict warnings don't apply to them,
# and group third-party targets in IDEs.
foreach(dep glfw glm glm-header-only spdlog)
	if(TARGET ${dep})
		set_target_properties(${dep} PROPERTIES SYSTEM TRUE FOLDER "Dependencies")
	endif()
endforeach()

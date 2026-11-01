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

# --- Vulkan-Headers (Vulkan API headers incl. vulkan.hpp) -------------------------------------
# The Vulkan loader is not linked: NVRHI and Paralax load it at runtime, so no SDK is needed to build.
CPMAddPackage(
	NAME VulkanHeaders
	GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
	GIT_TAG v1.4.352
	OPTIONS
		"VULKAN_HEADERS_ENABLE_TESTS OFF"
		"VULKAN_HEADERS_ENABLE_INSTALL OFF")

# --- NVRHI (rendering hardware interface) -------------------------------------------------------
# NVRHI publishes no release tags; pinned to a reviewed commit on main (2026-10-05).
CPMAddPackage(
	NAME nvrhi
	GITHUB_REPOSITORY NVIDIA-RTX/NVRHI
	GIT_TAG 6b96fb03e07539f08327aea76c56d55f1de9d906
	OPTIONS
		"NVRHI_WITH_VULKAN ON"
		"NVRHI_WITH_VALIDATION ON"
		"NVRHI_WITH_DX11 OFF"
		"NVRHI_WITH_DX12 OFF"
		"NVRHI_WITH_RTXMU OFF"
		"NVRHI_WITH_AFTERMATH OFF"
		"NVRHI_BUILD_SHARED OFF"
		"NVRHI_INSTALL OFF"
		"NVRHI_FETCH_VULKAN_HEADERS OFF")
# The static Vulkan backend uses symbols from the core library but does not declare it; make link order correct.
target_link_libraries(nvrhi_vk PUBLIC nvrhi)

# --- Slang (shader compiler; prebuilt release binaries) -----------------------------------------
# Only the host compiler is used at build time; shaders are compiled to SPIR-V and embedded.
set(PX_SLANG_VERSION "2026.18.3")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
	set(PX_SLANG_ASSET "windows-x86_64.zip")
	set(PX_SLANG_SHA256 "1339d7b3050ae680c11152b8ccff977173591ce8e9c1d2a5cff2eb904d52e50d")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
	set(PX_SLANG_ASSET "macos-aarch64.zip")
	set(PX_SLANG_SHA256 "d06187a6f90aaee68a5ceef453cf973016e2d3adfc61ce51b1cd6a0d204cfca2")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
	set(PX_SLANG_ASSET "linux-x86_64.tar.gz")
	set(PX_SLANG_SHA256 "4d664ca905124ae68204ee05170f5acd8dc53956baf850b2e7b23215cfb78f7b")
else()
	message(FATAL_ERROR "No pinned Slang binaries for host ${CMAKE_HOST_SYSTEM_NAME}")
endif()
CPMAddPackage(
	NAME slang
	URL "https://github.com/shader-slang/slang/releases/download/v${PX_SLANG_VERSION}/slang-${PX_SLANG_VERSION}-${PX_SLANG_ASSET}"
	URL_HASH "SHA256=${PX_SLANG_SHA256}"
	DOWNLOAD_ONLY YES)
# A plain variable (not find_program's cache entry) so a version bump never keeps using a stale compiler.
set(PX_SLANGC "${slang_SOURCE_DIR}/bin/slangc${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${PX_SLANGC}")
	message(FATAL_ERROR "slangc not found at ${PX_SLANGC}")
endif()

# --- stb (image writing) ------------------------------------------------------------------------
# stb publishes no release tags; pinned to a reviewed commit (2026-08-02).
CPMAddPackage(
	NAME stb
	GITHUB_REPOSITORY nothings/stb
	GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
	DOWNLOAD_ONLY YES)
if(stb_ADDED)
	add_library(stb INTERFACE)
	add_library(stb::stb ALIAS stb)
	target_include_directories(stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
endif()

# --- EnTT (entity-component-system) ------------------------------------------------------------
CPMAddPackage(
	NAME EnTT
	GITHUB_REPOSITORY skypjack/entt
	VERSION 3.16.0
	DOWNLOAD_ONLY YES)
if(EnTT_ADDED)
	add_library(EnTT INTERFACE)
	add_library(EnTT::EnTT ALIAS EnTT)
	target_include_directories(EnTT SYSTEM INTERFACE "${EnTT_SOURCE_DIR}/src")
endif()

# --- nlohmann/json (scene, asset and agent-protocol serialization) -----------------------------
CPMAddPackage(
	NAME nlohmann_json
	GITHUB_REPOSITORY nlohmann/json
	VERSION 3.12.0
	OPTIONS
		"JSON_BuildTests OFF"
		"JSON_Install OFF")

# --- simdjson (JSON parser used by fastgltf) ----------------------------------------------------
# Provided here so fastgltf does not download its own (unhashed) copy at configure time.
CPMAddPackage(
	NAME simdjson
	GITHUB_REPOSITORY simdjson/simdjson
	GIT_TAG v4.6.11
	OPTIONS
		"SIMDJSON_DEVELOPER_MODE OFF"
		"BUILD_SHARED_LIBS OFF")

# --- fastgltf (glTF 2.0 import) ----------------------------------------------------------------
CPMAddPackage(
	NAME fastgltf
	GITHUB_REPOSITORY spnda/fastgltf
	GIT_TAG v0.9.1
	OPTIONS
		"FASTGLTF_ENABLE_TESTS OFF"
		"FASTGLTF_ENABLE_EXAMPLES OFF"
		"FASTGLTF_ENABLE_DOCS OFF"
		"FASTGLTF_COMPILE_AS_CPP20 ON"
		"FASTGLTF_ENABLE_INSTALL OFF")

# --- Dear ImGui (editor UI; docking branch) + ImGuizmo (transform gizmos) --------------------------
CPMAddPackage(
	NAME imgui
	GITHUB_REPOSITORY ocornut/imgui
	GIT_TAG v1.93.0-docking
	DOWNLOAD_ONLY YES)
# ImGuizmo publishes no recent tags; pinned to a reviewed commit (2026-10-10) compatible with imgui 1.93.
CPMAddPackage(
	NAME ImGuizmo
	GITHUB_REPOSITORY CedricGuillemet/ImGuizmo
	GIT_TAG 105e4fde45e4e74aae8973d98750820ef7aca7e2
	DOWNLOAD_ONLY YES)
if(imgui_ADDED AND NOT TARGET imgui)
	add_library(imgui STATIC
		"${imgui_SOURCE_DIR}/imgui.cpp"
		"${imgui_SOURCE_DIR}/imgui_draw.cpp"
		"${imgui_SOURCE_DIR}/imgui_tables.cpp"
		"${imgui_SOURCE_DIR}/imgui_widgets.cpp"
		"${imgui_SOURCE_DIR}/imgui_demo.cpp"
		"${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
		"${ImGuizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
	add_library(imgui::imgui ALIAS imgui)
	target_include_directories(imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/misc/cpp" "${ImGuizmo_SOURCE_DIR}/src")
	target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DEFINE_MATH_OPERATORS)
	set_target_properties(imgui PROPERTIES FOLDER "Dependencies")
endif()

# --- Jolt Physics (rigid body physics) ----------------------------------------------------------
# Cross-platform determinism is required: simulation must reproduce exactly on every platform (AGENTS.md rule 6).
CPMAddPackage(
	NAME JoltPhysics
	GITHUB_REPOSITORY jrouwe/JoltPhysics
	GIT_TAG v5.6.0
	SOURCE_SUBDIR Build
	OPTIONS
		"CROSS_PLATFORM_DETERMINISTIC ON"
		"DOUBLE_PRECISION OFF"
		"TARGET_UNIT_TESTS OFF"
		"TARGET_HELLO_WORLD OFF"
		"TARGET_PERFORMANCE_TEST OFF"
		"TARGET_SAMPLES OFF"
		"TARGET_VIEWER OFF"
		"ENABLE_ALL_WARNINGS OFF"
		"INTERPROCEDURAL_OPTIMIZATION OFF"
		"USE_STATIC_MSVC_RUNTIME_LIBRARY OFF"
		"DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF"
		"PROFILER_IN_DEBUG_AND_RELEASE OFF"
		"OVERRIDE_CXX_FLAGS OFF"
		"ENABLE_INSTALL OFF"
		"ENABLE_OBJECT_STREAM OFF"
		# The engine is built with RTTI (and UBSan's vptr checks need Jolt's type information).
		"CPP_RTTI_ENABLED ON"
		"FLOATING_POINT_EXCEPTIONS_ENABLED OFF"
		# Jolt's own graphics backends belong to its samples and viewer.
		"JPH_USE_DX12 OFF"
		"JPH_USE_VK OFF"
		"JPH_USE_MTL OFF"
		# A portable x86-64 baseline (SSE4.2) so shipped games run on older CPUs; ARM uses NEON regardless.
		"USE_AVX OFF"
		"USE_AVX2 OFF"
		"USE_F16C OFF"
		"USE_FMADD OFF"
		"USE_LZCNT OFF"
		"USE_TZCNT OFF")
if(TARGET Jolt)
	# Jolt's headers are third-party: keep our warnings-as-errors policy away from them.
	get_target_property(PX_JOLT_INCLUDES Jolt INTERFACE_INCLUDE_DIRECTORIES)
	set_target_properties(Jolt PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PX_JOLT_INCLUDES}" FOLDER "Dependencies")
	# Jolt hands work to its own worker threads through atomics. ThreadSanitizer only sees synchronisation in
	# instrumented code, so in TSan builds Jolt is instrumented too; otherwise every hand-off is a false race.
	if(PX_SANITIZERS MATCHES "thread")
		target_compile_options(Jolt PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
	endif()
endif()

# --- Lua (scripting language) --------------------------------------------------------------------
# Lua's repository has no CMake build; the library is built here from its C sources. It is compiled as C++
# so a Lua error raised inside a C++ binding unwinds with destructors run (luai_throw uses C++ exceptions),
# instead of longjmp-ing over C++ frames.
CPMAddPackage(
	NAME lua
	GITHUB_REPOSITORY lua/lua
	GIT_TAG v5.4.9
	DOWNLOAD_ONLY YES)
if(lua_ADDED AND NOT TARGET lua)
	set(PX_LUA_SOURCES
		lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c
		linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c
		lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c)
	list(TRANSFORM PX_LUA_SOURCES PREPEND "${lua_SOURCE_DIR}/")
	set_source_files_properties(${PX_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
	add_library(lua STATIC ${PX_LUA_SOURCES})
	add_library(lua::lua ALIAS lua)
	target_include_directories(lua SYSTEM PUBLIC "${lua_SOURCE_DIR}")
	# Lua's sources are C written to also compile as C++; their warnings are not ours to fix.
	if(MSVC)
		target_compile_options(lua PRIVATE /w /TP)
	else()
		target_compile_options(lua PRIVATE -w -x c++)
	endif()
	# POSIX APIs (e.g. mkstemp instead of the deprecated tmpnam); dynamic library loading stays disabled.
	if(UNIX)
		target_compile_definitions(lua PRIVATE LUA_USE_POSIX)
	endif()
	# Deterministic string hashing (pairs order), see cmake/LuaDeterminism.h.
	if(MSVC)
		target_compile_options(lua PRIVATE "/FI${CMAKE_CURRENT_LIST_DIR}/LuaDeterminism.h")
	else()
		target_compile_options(lua PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/LuaDeterminism.h")
	endif()
	set_target_properties(lua PROPERTIES FOLDER "Dependencies" LINKER_LANGUAGE CXX)
endif()

# --- sol2 (C++ bindings for Lua, header-only) ---------------------------------------------------
CPMAddPackage(
	NAME sol2
	GITHUB_REPOSITORY ThePhD/sol2
	GIT_TAG v3.5.0
	DOWNLOAD_ONLY YES)
if(sol2_ADDED AND NOT TARGET sol2)
	add_library(sol2 INTERFACE)
	add_library(sol2::sol2 ALIAS sol2)
	target_include_directories(sol2 SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
	# Lua is built as C++ (see above); sol2 must know so it does not wrap the headers in extern "C".
	# Errors raised in bindings become Lua errors that the engine reports; sol2 need not also print them.
	target_compile_definitions(sol2 INTERFACE SOL_USING_CXX_LUA=1 SOL_ALL_SAFETIES_ON=1 SOL_PRINT_ERRORS=0)
	target_link_libraries(sol2 INTERFACE lua::lua)
endif()

# --- Inter (UI font, SIL Open Font License) ------------------------------------------------------
CPMAddPackage(
	NAME InterFont
	URL "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
	URL_HASH "SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e"
	DOWNLOAD_ONLY YES)
set(PX_UI_FONTS
	"${InterFont_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf"
	"${InterFont_SOURCE_DIR}/extras/ttf/Inter-SemiBold.ttf")

# --- Poly Haven environments (CC0, raw 2k Radiance files) --------------------------------------
# Large engine resources stay out of source control and binaries. Hashes pin the exact downloads.
CPMAddPackage(
	NAME ParalaxSkyHDR
	URL "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_48d_partly_cloudy_puresky_2k.hdr"
	URL_HASH "SHA256=5244534e9cf5b606f2ff513aa00ddb161b0a4826ffd88a0d3bd03ac29247d198"
	DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT TRUE)
CPMAddPackage(
	NAME ParalaxStudioHDR
	URL "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/studio_small_09_2k.hdr"
	URL_HASH "SHA256=36724313c0fc66dab126ff90f081b85a0b1ef47be65eda1417bd20b033f0573f"
	DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT TRUE)
set(PX_ENVIRONMENTS
	"${ParalaxSkyHDR_SOURCE_DIR}/kloofendal_48d_partly_cloudy_puresky_2k.hdr"
	"${ParalaxStudioHDR_SOURCE_DIR}/studio_small_09_2k.hdr")

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
foreach(dep glfw glm glm-header-only spdlog nvrhi nvrhi_vk Vulkan-Headers nlohmann_json fastgltf simdjson)
	if(TARGET ${dep})
		set_target_properties(${dep} PROPERTIES SYSTEM TRUE FOLDER "Dependencies")
	endif()
endforeach()

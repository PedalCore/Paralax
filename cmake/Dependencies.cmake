# Third-party dependencies, pinned to exact versions via CPM.cmake (vendored in cmake/CPM.cmake).
# Set CPM_SOURCE_CACHE (e.g. ~/.cache/CPM) to share downloads between build trees and worktrees.
#
# Policy: every dependency is pinned to an exact release tag. Bumping a version is a reviewed change
# that must keep the full test suite green on every CI platform.

include(CPM)

# CPM keys its source cache on a package's arguments, which name a patch but not its content, so an edited patch would
# silently reuse a tree patched with the old one. Patched packages are cached under a key that includes the patch hash.
# The key hashes every patch (in order), so editing or adding one re-fetches and re-patches the package.
function(px_patched_cache_key outVar version patch)
	set(combined "")
	foreach(file IN LISTS patch ARGN)
		file(SHA256 "${file}" fileHash)
		string(APPEND combined "${fileHash}")
	endforeach()
	string(SHA256 patchHash "${combined}")
	string(SUBSTRING "${patchHash}" 0 16 patchHash)
	set(${outVar} "${version}-patch-${patchHash}" PARENT_SCOPE)
endfunction()

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

# --- stb (image loading and writing) -------------------------------------------------------------
# stb publishes no release tags; pinned to a reviewed commit (2026-08-02). Patched: stbi__getn copied zero bytes from a
# null buffer for an empty first IDAT chunk in a PNG (undefined behaviour; found by the loader fuzz tests).
px_patched_cache_key(PX_STB_CACHE_KEY 2c980bb5 "${CMAKE_CURRENT_LIST_DIR}/patches/stb-zero-length-read.patch")
CPMAddPackage(
	NAME stb
	GITHUB_REPOSITORY nothings/stb
	GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
	PATCHES "${CMAKE_CURRENT_LIST_DIR}/patches/stb-zero-length-read.patch"
	CUSTOM_CACHE_KEY "${PX_STB_CACHE_KEY}"
	DOWNLOAD_ONLY YES)
if(stb_ADDED)
	file(READ "${stb_SOURCE_DIR}/stb_image.h" PX_STB_IMAGE)
	if(NOT PX_STB_IMAGE MATCHES "Paralax patch: nothing to read")
		message(FATAL_ERROR "stb in ${stb_SOURCE_DIR} lacks cmake/patches/stb-zero-length-read.patch; reconfigure this build tree with cmake --fresh")
	endif()
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
# Patched for untrusted files (found by Paralax-Tests' loader fuzzing and its review; see the patch for details):
# - getAccessorType indexed an accessor's "type" at [0] and [3] without checking its length ("V", "" read out of
#   bounds). The patch compares whole names, as upstream does from v1.0.0 (2026-10-31, too new to pin).
# - Base64 data URIs whose length is not a multiple of four, or with characters outside the base64 alphabet, made
#   the decoders read out of bounds; they are now rejected as invalid URIs.
# - Percent-decoding read past the end of a URI ending in "%" or "%A", and could decode "%00" into a NUL; such
#   sequences are now left as they are.
# - With an extras callback set (Paralax reads material extras), extras that are not JSON objects failed the whole
#   file, though glTF allows any type there; they are now ignored.
# - Indices from the file used without a range check (an animation channel's sampler and the sampler's accessors in
#   validate, sparse accessors' buffer views, and a POSITION accessor in GenerateMeshIndices) are now checked, and so
#   is that accessor's count there (zero wrapped around; a huge one allocated without limit).
# Drop the parts upstream has fixed when upgrading.
px_patched_cache_key(PX_FASTGLTF_CACHE_KEY v0.9.1 "${CMAKE_CURRENT_LIST_DIR}/patches/fastgltf-v0.9.1-untrusted-input.patch")
CPMAddPackage(
	NAME fastgltf
	GITHUB_REPOSITORY spnda/fastgltf
	GIT_TAG v0.9.1
	PATCHES "${CMAKE_CURRENT_LIST_DIR}/patches/fastgltf-v0.9.1-untrusted-input.patch"
	CUSTOM_CACHE_KEY "${PX_FASTGLTF_CACHE_KEY}"
	OPTIONS
		"FASTGLTF_ENABLE_TESTS OFF"
		"FASTGLTF_ENABLE_EXAMPLES OFF"
		"FASTGLTF_ENABLE_DOCS OFF"
		"FASTGLTF_COMPILE_AS_CPP20 ON"
		"FASTGLTF_ENABLE_INSTALL OFF")
if(fastgltf_ADDED)
	# Like Jolt's patch: a build tree configured before the patch keeps unpatched sources; say so.
	file(READ "${fastgltf_SOURCE_DIR}/src/fastgltf.cpp" PX_FASTGLTF_SOURCE)
	if(NOT PX_FASTGLTF_SOURCE MATCHES "from the base64 alphabet" OR NOT PX_FASTGLTF_SOURCE MATCHES "never into a NUL" OR NOT PX_FASTGLTF_SOURCE MATCHES "glTF allows extras of any type" OR NOT PX_FASTGLTF_SOURCE MATCHES "out-of-range sampler was read out of bounds" OR NOT PX_FASTGLTF_SOURCE MATCHES "a huge one allocated that many indices")
		message(FATAL_ERROR "fastgltf in ${fastgltf_SOURCE_DIR} lacks cmake/patches/fastgltf-v0.9.1-untrusted-input.patch; reconfigure this build tree with cmake --fresh")
	endif()
endif()

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
# Two patches, both found by ThreadSanitizer; drop each when upgrading to a release that has it:
# - jolt-v5.6.0-atomic-mutex-owner.patch: upstream commit 1b2e77e (2026-10-14, "Changed Mutex::mLockedThreadID to an
#   atomic"). In debug builds Jolt's mutexes track their owner for asserts, and v5.6.0 reads that field while another
#   thread writes it.
# - jolt-v5.6.0-barrier-done-ordering.patch (ours): a job that finishes before PhysicsSystem::Update adds it to the
#   update barrier was observed with relaxed ordering, so the caller never synchronised with that job's writes.
px_patched_cache_key(PX_JOLTPHYSICS_CACHE_KEY v5.6.0
	"${CMAKE_CURRENT_LIST_DIR}/patches/jolt-v5.6.0-atomic-mutex-owner.patch"
	"${CMAKE_CURRENT_LIST_DIR}/patches/jolt-v5.6.0-barrier-done-ordering.patch")
CPMAddPackage(
	NAME JoltPhysics
	GITHUB_REPOSITORY jrouwe/JoltPhysics
	GIT_TAG v5.6.0
	PATCHES
		"${CMAKE_CURRENT_LIST_DIR}/patches/jolt-v5.6.0-atomic-mutex-owner.patch"
		"${CMAKE_CURRENT_LIST_DIR}/patches/jolt-v5.6.0-barrier-done-ordering.patch"
	CUSTOM_CACHE_KEY "${PX_JOLTPHYSICS_CACHE_KEY}"
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
	# A build tree configured before a patch was added keeps using the unpatched sources; say so instead of
	# building them.
	file(READ "${JoltPhysics_SOURCE_DIR}/Jolt/Core/Mutex.h" PX_JOLT_MUTEX)
	if(NOT PX_JOLT_MUTEX MATCHES "atomic<thread::id>")
		message(FATAL_ERROR "Jolt in ${JoltPhysics_SOURCE_DIR} lacks cmake/patches/jolt-v5.6.0-atomic-mutex-owner.patch; reconfigure this build tree with cmake --fresh")
	endif()
	file(READ "${JoltPhysics_SOURCE_DIR}/Jolt/Core/JobSystem.h" PX_JOLT_JOBSYSTEM)
	if(NOT PX_JOLT_JOBSYSTEM MATCHES "Paralax patch: a job created without dependencies")
		message(FATAL_ERROR "Jolt in ${JoltPhysics_SOURCE_DIR} lacks cmake/patches/jolt-v5.6.0-barrier-done-ordering.patch; reconfigure this build tree with cmake --fresh")
	endif()
	# Jolt's headers are third-party: keep our warnings-as-errors policy away from them.
	get_target_property(PX_JOLT_INCLUDES Jolt INTERFACE_INCLUDE_DIRECTORIES)
	set_target_properties(Jolt PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PX_JOLT_INCLUDES}" FOLDER "Dependencies")
	# Jolt hands work to its own worker threads through atomics. ThreadSanitizer only sees synchronisation in
	# instrumented code, so in TSan builds Jolt is instrumented too; otherwise every hand-off is a false race.
	if(PX_SANITIZERS MATCHES "thread")
		target_compile_options(Jolt PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
	endif()
endif()

# --- miniaudio (audio playback and decoding) -------------------------------------------------------
# A single-file library: its implementation (miniaudio.c) is compiled once here, and only Paralax/src/Paralax/Audio
# includes its header.
# 0.11.26 (2026-10-31) is too new; 0.11.25 was released 2026-03-03.
CPMAddPackage(
	NAME miniaudio
	GITHUB_REPOSITORY mackron/miniaudio
	GIT_TAG 0.11.25
	DOWNLOAD_ONLY YES)
if(miniaudio_ADDED AND NOT TARGET miniaudio)
	add_library(miniaudio STATIC "${miniaudio_SOURCE_DIR}/miniaudio.c")
	target_include_directories(miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
	# Paralax only decodes and plays. Encoders are left out, and so is the resource manager: clips are decoded by
	# AudioClip, and the manager would start a job thread. Backends are loaded at run time (no audio SDK is linked),
	# so a machine without an audio device or server still runs (the engine then mixes offline).
	target_compile_definitions(miniaudio PUBLIC MA_NO_ENCODING MA_NO_RESOURCE_MANAGER)
	if(MSVC)
		target_compile_options(miniaudio PRIVATE /w)
	else()
		# The decoders' sample arithmetic (FLAC prediction, for one) overflows signed integers on corrupt files, which
		# the loader fuzz tests found under UBSan. -fwrapv makes that overflow defined wrapping, so a hostile file
		# only decodes to noise. MSVC has no equivalent flag; there it stays undefined, as it is upstream.
		target_compile_options(miniaudio PRIVATE -w -fwrapv)
	endif()
	find_package(Threads REQUIRED)
	target_link_libraries(miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
	if(UNIX AND NOT APPLE)
		target_link_libraries(miniaudio PUBLIC m)
	endif()
	if(APPLE)
		target_link_libraries(miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
	endif()
	# In TSan builds miniaudio's mixing thread must be instrumented too (see Jolt above).
	if(PX_SANITIZERS MATCHES "thread")
		target_compile_options(miniaudio PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
	endif()
	set_target_properties(miniaudio PROPERTIES FOLDER "Dependencies")
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

# In ASan/UBSan builds the third-party code that reads untrusted files (glTF and its JSON parser, audio, scripts)
# is instrumented too, so the loader fuzz tests catch out-of-bounds reads and undefined behaviour inside it, not
# only in Paralax code. (TSan builds instrument the threaded libraries instead; see Jolt and miniaudio above.)
if(PX_SANITIZERS MATCHES "address|undefined")
	foreach(dep fastgltf simdjson miniaudio lua)
		if(TARGET ${dep})
			target_compile_options(${dep} PRIVATE -fsanitize=${PX_SANITIZERS} -fno-omit-frame-pointer -fno-sanitize-recover=all)
		endif()
	endforeach()
endif()

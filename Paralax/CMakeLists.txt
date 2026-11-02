file(GLOB_RECURSE PX_ENGINE_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

add_library(Paralax STATIC ${PX_ENGINE_SOURCES})
add_library(Paralax::Paralax ALIAS Paralax)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${PX_ENGINE_SOURCES})

target_include_directories(Paralax PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(Paralax
	PUBLIC
		glm::glm
		spdlog::spdlog
		nvrhi
		EnTT::EnTT
		nlohmann_json::nlohmann_json
		imgui::imgui
	PRIVATE
		glfw
		nvrhi_vk
		Vulkan::Headers
		stb::stb
		fastgltf::fastgltf
		simdjson::simdjson
		sol2::sol2
		Jolt
		miniaudio)
if(WIN32)
	# Winsock, for the loopback agent server (Network/TcpSocket), and Shell32, for CommandLineToArgvW (the UTF-16
	# command line, Platform/Windows/WindowsCommandLine).
	target_link_libraries(Paralax PRIVATE ws2_32 shell32)
endif()
if(MSVC)
	# The Lua bindings (sol2 templates) exceed the default COFF section limit (C1128). Set for the whole target so
	# every source agrees with the precompiled header.
	target_compile_options(Paralax PRIVATE /bigobj)
endif()

target_compile_definitions(Paralax PUBLIC
	GLM_FORCE_DEPTH_ZERO_TO_ONE
	GLM_FORCE_RADIANS
	GLM_ENABLE_EXPERIMENTAL)

px_add_embedded_shaders(Paralax DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
px_embed_files(Paralax NAMESPACE EmbeddedFonts FILES ${PX_UI_FONTS})

target_precompile_headers(Paralax PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pxpch.h")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/Paralax/Renderer/StbImplementation.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

px_configure_target(Paralax)

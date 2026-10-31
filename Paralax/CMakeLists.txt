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
	PRIVATE
		glfw
		nvrhi_vk
		Vulkan::Headers
		stb::stb)

target_compile_definitions(Paralax PUBLIC
	GLM_FORCE_DEPTH_ZERO_TO_ONE
	GLM_FORCE_RADIANS
	GLM_ENABLE_EXPERIMENTAL)

px_add_embedded_shaders(Paralax DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

target_precompile_headers(Paralax PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pxpch.h")

px_configure_target(Paralax)

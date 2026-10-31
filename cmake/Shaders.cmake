# px_add_embedded_shaders(<target> DIRECTORY <dir>)
#
# Compiles every <dir>/*.slang file (each holding one or more [shader("...")] entry points) to a single SPIR-V
# module with the pinned slangc, then embeds all modules into <target> as a generated source file. Files in
# subdirectories are treated as imported modules only (added to the include path, never compiled on their own).
#
# Embedded shaders are looked up at runtime by file stem, e.g. "Triangle" for Triangle.slang (see
# Paralax/Renderer/ShaderLibrary.h). Bindings use HLSL register syntax and are shifted to NVRHI's default
# Vulkan binding offsets (t: 0, s: 128, b: 256, u: 384).
function(px_add_embedded_shaders target)
	cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DIRECTORY" "")
	if(NOT ARG_DIRECTORY)
		message(FATAL_ERROR "px_add_embedded_shaders: DIRECTORY is required")
	endif()

	file(GLOB shader_sources CONFIGURE_DEPENDS "${ARG_DIRECTORY}/*.slang")
	file(GLOB_RECURSE shader_all_files CONFIGURE_DEPENDS "${ARG_DIRECTORY}/*.slang")

	set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
	set(slang_flags
		-target spirv
		-profile spirv_1_5
		-fvk-use-entrypoint-name
		-matrix-layout-column-major
		-fvk-t-shift 0 all
		-fvk-s-shift 128 all
		-fvk-b-shift 256 all
		-fvk-u-shift 384 all
		-warnings-as-errors all
		-O2
		-I "${ARG_DIRECTORY}")

	set(spirv_files "")
	foreach(source IN LISTS shader_sources)
		get_filename_component(name "${source}" NAME_WE)
		set(spirv "${output_dir}/${name}.spv")
		add_custom_command(
			OUTPUT "${spirv}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
			COMMAND "${PX_SLANGC}" "${source}" ${slang_flags} -o "${spirv}"
			# Conservative but always correct: any change to any shader file recompiles every module.
			DEPENDS "${source}" ${shader_all_files} "${PX_SLANGC}"
			COMMENT "Compiling shader ${name}.slang"
			VERBATIM)
		list(APPEND spirv_files "${spirv}")
	endforeach()

	px_embed_files(${target} NAMESPACE EmbeddedShaders FILES ${spirv_files})
	source_group("Shaders" FILES ${shader_all_files})
	target_sources(${target} PRIVATE ${shader_all_files})
	set_source_files_properties(${shader_all_files} PROPERTIES HEADER_FILE_ONLY ON)
endfunction()

# px_embed_files(<target> NAMESPACE <ns> FILES <file>...)
#
# Compiles the files into <target> as a table of Paralax::EmbeddedFile in namespace Paralax::<ns> (g_Entries,
# g_EntryCount), named by file stem. Regenerated whenever an input changes.
function(px_embed_files target)
	cmake_parse_arguments(PARSE_ARGV 1 ARG "" "NAMESPACE" "FILES")
	if(NOT ARG_NAMESPACE)
		message(FATAL_ERROR "px_embed_files: NAMESPACE is required")
	endif()
	set(embedded_source "${CMAKE_CURRENT_BINARY_DIR}/Embedded_${ARG_NAMESPACE}.cpp")
	add_custom_command(
		OUTPUT "${embedded_source}"
		COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${embedded_source}" "-DINPUTS=${ARG_FILES}" "-DNAMESPACE=${ARG_NAMESPACE}" -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
		DEPENDS ${ARG_FILES} "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
		COMMENT "Embedding ${ARG_NAMESPACE}"
		VERBATIM)
	target_sources(${target} PRIVATE "${embedded_source}")
	set_source_files_properties("${embedded_source}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endfunction()

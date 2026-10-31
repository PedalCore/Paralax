# px_configure_target(<target>)
# Applies Paralax compile definitions, warnings and sanitizer settings to a first-party target.
function(px_configure_target target)
	target_compile_features(${target} PUBLIC cxx_std_20)

	target_compile_definitions(${target} PUBLIC
		$<$<CONFIG:Debug>:PX_DEBUG>
		$<$<CONFIG:Release>:PX_RELEASE>
		$<$<CONFIG:Dist>:PX_DIST>)

	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /MP)
		# Required by consumers too: Paralax macros rely on a conforming preprocessor (__VA_OPT__).
		target_compile_options(${target} PUBLIC /utf-8 /Zc:__cplusplus /Zc:preprocessor)
		if(PX_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE /WX)
		endif()
		target_compile_definitions(${target} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	else()
		target_compile_options(${target} PRIVATE
			-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
			-Woverloaded-virtual -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
		# GCC's -Wnull-dereference reports false positives inside libstdc++ at -O2/-O3; Clang's is reliable.
		if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
			target_compile_options(${target} PRIVATE -Wnull-dereference)
		endif()
		if(PX_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
		if(PX_SANITIZERS)
			target_compile_options(${target} PUBLIC -fsanitize=${PX_SANITIZERS} -fno-omit-frame-pointer -fno-sanitize-recover=all)
			target_link_options(${target} PUBLIC -fsanitize=${PX_SANITIZERS})
		endif()
	endif()
endfunction()

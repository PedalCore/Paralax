// The single translation unit that compiles stb_image and stb_image_write. Built without the precompiled header
// (see Paralax/CMakeLists.txt): with GCC's PCH active, stb's SIMD configuration is not applied consistently.

#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wold-style-cast"
	#pragma GCC diagnostic ignored "-Wcast-align"
	#pragma GCC diagnostic ignored "-Wdouble-promotion"
	#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
	#pragma GCC diagnostic ignored "-Wshadow"
	#pragma GCC diagnostic ignored "-Wsign-compare"
	#pragma GCC diagnostic ignored "-Wunused-function"
	#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#elif defined(_MSC_VER)
	#pragma warning(push, 0)
#endif
#if defined(__GNUC__) && !defined(__clang__) && (!defined(__OPTIMIZE__) || defined(__SANITIZE_ADDRESS__))
	// GCC cannot compile stb_image's SSE2 intrinsics without optimisation (non-immediate shift operands).
	#define STBI_NO_SIMD
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
#include <stb_image.h>
#include <stb_image_write.h>
#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
	#pragma warning(pop)
#endif

// The single translation unit that compiles stb_image, stb_image_write and stb_truetype. Built without the precompiled header
// (see Paralax/CMakeLists.txt): with GCC's PCH active, stb's SIMD configuration is not applied consistently.

// stb's implementation compiles in this translation unit, but its code is in stb's headers, which are on a SYSTEM
// include path (cmake/Dependencies.cmake). GCC and Clang report no warning located in a system header, so stb needs no
// pragmas for them: without any, none of our warnings fires in it, at any optimisation level or with any sanitizer.
// MSVC's equivalent, /external:W0, leaves out the warnings its code generator reports (C47xx), so for MSVC stb is
// compiled at warning level 0.
#if defined(_MSC_VER)
	#pragma warning(push, 0)
#endif
#if defined(__GNUC__) && !defined(__clang__) && (!defined(__OPTIMIZE__) || defined(__SANITIZE_ADDRESS__))
	// GCC cannot compile stb_image's SSE2 intrinsics without optimisation (non-immediate shift operands).
	#define STBI_NO_SIMD
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_truetype.h>
#if defined(_MSC_VER)
	#pragma warning(pop)
#endif

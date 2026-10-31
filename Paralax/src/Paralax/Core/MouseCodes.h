#pragma once

#include <cstdint>

namespace Paralax {

	using MouseCode = uint16_t;

	// Mouse button codes. Values match GLFW's mouse button tokens.
	namespace Mouse {

		enum : MouseCode
		{
			Button0 = 0,
			Button1 = 1,
			Button2 = 2,
			Button3 = 3,
			Button4 = 4,
			Button5 = 5,
			Button6 = 6,
			Button7 = 7,

			ButtonLeft = Button0,
			ButtonRight = Button1,
			ButtonMiddle = Button2
		};

		// One past the largest valid mouse button code; used to size button state tables.
		inline constexpr MouseCode Count = 8;

	}

}

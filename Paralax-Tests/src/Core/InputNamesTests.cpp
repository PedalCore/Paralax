#include <doctest/doctest.h>

#include <Paralax/Core/InputNames.h>

#include <set>
#include <string>

using namespace Paralax;

TEST_CASE("InputNames: key and mouse button names map both ways without duplicates")
{
	CHECK(KeyFromName("Space") == Key::Space);
	CHECK(KeyFromName("A") == Key::A);
	CHECK(KeyFromName("F12") == Key::F12);
	CHECK(KeyFromName("LeftShift") == Key::LeftShift);
	CHECK_FALSE(KeyFromName("space")); // Case-sensitive, as in scripts.
	CHECK_FALSE(KeyFromName(""));
	CHECK(MouseButtonFromName("Left") == Mouse::ButtonLeft);
	CHECK(MouseButtonFromName("Middle") == Mouse::ButtonMiddle);
	CHECK_FALSE(MouseButtonFromName("Button4"));

	std::set<std::string> names;
	std::set<KeyCode> codes;
	for (const InputName<KeyCode>& key : GetKeyNames())
	{
		CHECK(names.insert(key.Name).second);
		CHECK(codes.insert(key.Code).second);
		CHECK(key.Code < Key::Count);
		CHECK(KeyFromName(key.Name) == key.Code);
	}
	// Every letter, digit and function key has a name.
	for (char c = 'A'; c <= 'Z'; c++)
		CHECK(KeyFromName(std::string(1, c)) == static_cast<KeyCode>(Key::A + (c - 'A')));
	for (int d = 0; d <= 9; d++)
		CHECK(KeyFromName("D" + std::to_string(d)) == static_cast<KeyCode>(Key::D0 + d));
	for (int f = 1; f <= 12; f++)
		CHECK(KeyFromName("F" + std::to_string(f)) == static_cast<KeyCode>(Key::F1 + f - 1));
	CHECK(GetMouseButtonNames().size() == 3);
}

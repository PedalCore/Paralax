#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace TraceReference {

	// Test-only signed fixed-point integer, scaled by 2^-512. All operations are exact; asserted overflow
	// and discarded-bit checks make the range explicit. The reference inputs need at most three binary32
	// factors (minimum exponent -447); 1024 magnitude bits also cover every admitted positive exponent.
	// This deliberately does not use host floating arithmetic to decide rounding or intersections.
	struct Exact
	{
		std::array<uint32_t, 32> Words{};
		bool Negative = false;

		explicit Exact(float value = 0)
		{
			const uint32_t bits = std::bit_cast<uint32_t>(value);
			const uint32_t exponent = (bits >> 23) & 255;
			REQUIRE(exponent != 255);
			Negative = (bits >> 31) != 0;
			const uint32_t significand = (bits & 0x7fffff) | (exponent ? 0x800000 : 0);
			const int shift = 512 + (exponent ? int(exponent) - 150 : -149);
			const uint64_t shifted = uint64_t(significand) << (shift % 32);
			Words[shift / 32] = uint32_t(shifted);
			Words[shift / 32 + 1] = uint32_t(shifted >> 32);
			Canonicalize();
		}

		bool Zero() const
		{
			for (auto word : Words)
				if (word)
					return false;
			return true;
		}
		void Canonicalize() { Negative = Negative && !Zero(); }
		int CompareMagnitude(const Exact& other) const
		{
			for (int i = 31; i >= 0; --i)
				if (Words[i] != other.Words[i])
					return Words[i] < other.Words[i] ? -1 : 1;
			return 0;
		}
		int Compare(const Exact& other) const
		{
			if (Negative != other.Negative)
				return Negative ? -1 : 1;
			return (Negative ? -1 : 1) * CompareMagnitude(other);
		}
		Exact operator-() const
		{
			Exact result = *this;
			result.Negative = !result.Negative;
			result.Canonicalize();
			return result;
		}
		Exact operator+(const Exact& other) const
		{
			Exact result;
			if (Negative == other.Negative)
			{
				uint64_t carry = 0;
				for (size_t i = 0; i < Words.size(); ++i)
				{
					const uint64_t sum = uint64_t(Words[i]) + other.Words[i] + carry;
					result.Words[i] = uint32_t(sum);
					carry = sum >> 32;
				}
				REQUIRE(carry == 0);
				result.Negative = Negative;
			}
			else
			{
				const bool firstLarger = CompareMagnitude(other) >= 0;
				const auto& large = firstLarger ? *this : other;
				const auto& small = firstLarger ? other : *this;
				uint64_t borrow = 0;
				for (size_t i = 0; i < Words.size(); ++i)
				{
					const uint64_t sub = uint64_t(small.Words[i]) + borrow;
					result.Words[i] = uint32_t(uint64_t(large.Words[i]) - sub);
					borrow = uint64_t(large.Words[i]) < sub;
				}
				REQUIRE(borrow == 0);
				result.Negative = large.Negative;
			}
			result.Canonicalize();
			return result;
		}
		Exact operator-(const Exact& other) const { return *this + -other; }
		Exact operator*(const Exact& other) const
		{
			std::array<uint32_t, 64> product{};
			for (size_t i = 0; i < Words.size(); ++i)
			{
				uint64_t carry = 0;
				for (size_t j = 0; j < Words.size(); ++j)
				{
					const uint64_t value = uint64_t(Words[i]) * other.Words[j] + product[i + j] + carry;
					product[i + j] = uint32_t(value);
					carry = value >> 32;
				}
				product[i + Words.size()] = uint32_t(carry);
			}
			Exact result;
			for (size_t i = 0; i < 16; ++i)
			{
				REQUIRE(product[i] == 0);
				REQUIRE(product[i + 48] == 0);
			}
			for (size_t i = 0; i < 32; ++i)
				result.Words[i] = product[i + 16];
			result.Negative = Negative != other.Negative;
			result.Canonicalize();
			return result;
		}
		bool Bit(int bit) const { return (Words[bit / 32] & (1u << (bit % 32))) != 0; }
		float Rounded() const
		{
			if (Zero())
				return 0;
			int high = 1023;
			while (!Bit(high))
				--high;
			int shift = std::max(363, high - 23); // binary32's smallest quantum is 2^-149.
			uint32_t significand = 0;
			for (int bit = high; bit >= shift; --bit)
				significand = (significand << 1) | uint32_t(Bit(bit));
			bool sticky = false;
			for (int bit = 0; bit < shift - 1; ++bit)
				sticky |= Bit(bit);
			if (Bit(shift - 1) && (sticky || (significand & 1)))
				++significand;
			if (significand == 0x1000000)
			{
				significand >>= 1;
				++shift;
			}
			const int exponent = shift - 512 + 23 + 127;
			REQUIRE(exponent < 255);
			uint32_t bits = significand < 0x800000 ? significand : (uint32_t(exponent) << 23) | (significand & 0x7fffff);
			bits |= Negative ? 0x80000000u : 0;
			return std::bit_cast<float>(bits);
		}
	};

	struct Interval
	{
		float Lo, Hi;
	};
	inline Interval Wrap(float value)
	{
		const auto bits = std::bit_cast<uint32_t>(value);
		if ((bits & 0x7fffffffu) <= 0x00800000u)
			return {std::bit_cast<float>(0x80800001u), std::bit_cast<float>(0x00800001u)};
		const bool negative = bits >> 31;
		return {std::bit_cast<float>(negative ? bits + 1 : bits - 1), std::bit_cast<float>(negative ? bits - 1 : bits + 1)};
	}
	inline Interval Add(Interval a, Interval b)
	{
		return {Wrap((Exact(a.Lo) + Exact(b.Lo)).Rounded()).Lo, Wrap((Exact(a.Hi) + Exact(b.Hi)).Rounded()).Hi};
	}
	inline Interval Subtract(Interval a, Interval b)
	{
		return {Wrap((Exact(a.Lo) - Exact(b.Hi)).Rounded()).Lo, Wrap((Exact(a.Hi) - Exact(b.Lo)).Rounded()).Hi};
	}
	inline Interval Multiply(Interval a, Interval b)
	{
		Interval result = Wrap((Exact(a.Lo) * Exact(b.Lo)).Rounded());
		for (float x : {a.Lo, a.Hi})
			for (float y : {b.Lo, b.Hi})
			{
				const auto product = Wrap((Exact(x) * Exact(y)).Rounded());
				result.Lo = std::min(result.Lo, product.Lo);
				result.Hi = std::max(result.Hi, product.Hi);
			}
		return result;
	}

	// Independent slab oracle: compare exact rational endpoints by cross multiplication. There is no
	// shared separating-axis implementation, rounded origin addition, division, or epsilon.
	inline bool Intersects(const std::array<Interval, 3>& box, const std::array<float, 3>& anchor, const std::array<float, 3>& offset, const std::array<float, 3>& direction, float maximum)
	{
		Exact low(0), lowDenominator(1), high(maximum), highDenominator(1);
		for (size_t i = 0; i < 3; ++i)
		{
			Exact lo = Exact(box[i].Lo) - Exact(anchor[i]) - Exact(offset[i]);
			Exact hi = Exact(box[i].Hi) - Exact(anchor[i]) - Exact(offset[i]);
			Exact denominator(direction[i]);
			if (denominator.Zero())
			{
				if (lo.Compare(Exact()) > 0 || hi.Compare(Exact()) < 0)
					return false;
				continue;
			}
			if (denominator.Negative)
			{
				denominator = -denominator;
				const Exact oldLo = lo;
				lo = -hi;
				hi = -oldLo;
			}
			if ((lo * lowDenominator).Compare(low * denominator) > 0)
			{
				low = lo;
				lowDenominator = denominator;
			}
			if ((hi * highDenominator).Compare(high * denominator) < 0)
			{
				high = hi;
				highDenominator = denominator;
			}
			if ((low * highDenominator).Compare(high * lowDenominator) > 0)
				return false;
		}
		return true;
	}
}

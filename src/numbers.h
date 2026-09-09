// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef numbers_h
#define numbers_h

#include "atom.h"
#include "platform.h"
#include <cstdint>

JET_ALWAYS_INLINE inline bool as_uint64(Atom atom, uint64_t& value)
{
	if (atom.bits == 0) [[unlikely]]
	{
		value = 0;
	}
	else
	{
		uint64_t shift{(1023 + 63) - (atom.bits >> 52)};
		if (shift > 63) [[unlikely]]
		{
			return false;
		}
		uint64_t significand{(atom.bits << 11) | (1ULL << 63)};
		value = significand >> shift;
		if ((value << shift) != significand) [[unlikely]]
		{
			return false;
		}
	}
	return true;
}

JET_ALWAYS_INLINE inline bool as_uint8(Atom atom, uint8_t& value)
{
	uint64_t integer;
	if (!as_uint64(atom, integer) || integer > 255) [[unlikely]]
	{
		return false;
	}
	value = static_cast<uint8_t>(integer);
	return true;
}

template <>
inline decltype(auto) slow_unbox<uint64_t>(VmState& state, Atom atom)
{
	type_check(state, atom, jet::Type::Number);
	uint64_t value;
	JET_DIE_UNLESS(&state, as_uint64(atom, value), "expected exact integer in uint64 range, got {}",
	               unbox<Number>(atom));
	return value;
}

inline uint8_t as_uint8_or_die(VmState& state, Atom atom)
{
	type_check(state, atom, jet::Type::Number);
	uint8_t value;
	JET_DIE_UNLESS(&state, as_uint8(atom, value), "expected exact integer in uint8 range, got {}",
	               unbox<Number>(atom));
	return value;
}

JET_ALWAYS_INLINE inline Atom truncate_number(Atom value)
{
	uint64_t exponent{(value.bits >> 52) & 2047};

	if (exponent < 1023) [[unlikely]]
	{
		return Atom::from_bits(0);
	}
	if (exponent >= 1075) [[unlikely]]
	{
		return value;
	}

	value.bits &= ~((uint64_t{1} << (1075 - exponent)) - 1);
	return value;
}

#endif

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef atom_h
#define atom_h

#include "debug.h"
#include "error.h"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#define JET_IMM_TYPES(X)                  \
	X(Boolean,   boolean,    bool)        \
	X(Character, character,  Character)   \
	X(EmptyList, empty_list, EmptyList)   \
	X(Symbol,     symbol,      Symbol)

#define JET_HEAP_TYPES(X)                  \
	X(Pair,       pair,        Cons)       \
	X(Procedure,  procedure,   Lambda)     \
	X(Primitive,  primitive,   Prim)       \
	X(String,     string,      String)     \
	X(Vector,     vector,      Vec)        \
	X(ByteVector, bytevector, ByteVector)  \
	X(Port,       port,       Port)        \
	X(Slot,       slot,        Slot)       \
	X(StructType, struct_type, StructType) \
	X(Struct,     struct_,     Struct)

#define JET_ALL_TYPES(X)                  \
	X(Number,    "number")                \
	X(Boolean,   "boolean")               \
	X(Character, "character")             \
	X(EmptyList, "empty list")            \
	X(Eof,       "eof")                   \
	X(Pair,      "pair")                  \
	X(Procedure, "procedure")             \
	X(Primitive, "primitive")             \
	X(Symbol,    "symbol")                \
	X(String,    "string")                \
	X(Vector,    "vector")                \
	X(ByteVector,"bytevector")            \
	X(Port,     "port")                  \
	X(Slot,      "slot")                  \
	X(StructType,"struct type")           \
	X(Struct,    "struct")                \
	X(Unknown,   "unknown")

namespace jet
{
	enum class Type : uint8_t
	{
#define X(name, _str) name,
		JET_ALL_TYPES(X)
#undef X
		TypeMax,
	};
} // namespace jet

template <>
struct std::formatter<jet::Type> : std::formatter<std::string_view>
{
	std::format_context::iterator format(jet::Type type, std::format_context& context) const
	{
		switch (type)
		{
#define X(name, text) case jet::Type::name: return std::formatter<std::string_view>::format(#name, context);
		JET_ALL_TYPES(X)
#undef X
			case jet::Type::TypeMax:
				break;
		}
		JET_DIE(nullptr, "invalid jet::Type {}", static_cast<unsigned>(type));
	}
};

using Character = uint8_t;
using String = std::string;
using Symbol = const std::string*;
using ByteVector = std::vector<uint8_t>;

// NaN boxing layout:
//
// If (bits & QNAN_TAG) != QNAN_TAG -> it's a double (number).
// Otherwise, bits 50..48 = type tag low 3 bits,
// bit 63 = type tag high bit,
// bits 47..0 = payload (pointer or immediate value).

constexpr uint64_t QNAN_TAG = 0x7FF8'0000'0000'0000ULL;
constexpr uint64_t TAG_MASK = 0x0007'0000'0000'0000ULL;
constexpr uint64_t SIGN_BIT = 0x8000'0000'0000'0000ULL;
constexpr uint64_t PAYLOAD_MASK = 0x0000'FFFF'FFFF'FFFFULL;
constexpr uint64_t CANONICAL_NAN = 0x7FF0'0000'0000'0001ULL;
constexpr uint64_t HOLE_BITS = QNAN_TAG | 1;

struct Number
{
	double value;

	Number() = delete;

	static Number from_ieee(double value)
	{
		if (std::bit_cast<uint64_t>(value) == SIGN_BIT) [[unlikely]]
		{
			return Number{0.0};
		}
		if (value != value) [[unlikely]]
		{
			return nan();
		}
		return Number{value};
	}

	static Number from_sum(double value)
	{
		if (value != value) [[unlikely]]
		{
			return nan();
		}
		return trusted(value);
	}

	static Number nan()
	{
		uint64_t nan_bits = CANONICAL_NAN;
		// the `asm` forces a real branch by making nan_bits opaque. without it clang
		// if-converts the branch to branchless code, and the NaN fold lands on the FP
		// dependency chain of every arithmetic op in the vm.
		asm ("" : "+r" (nan_bits));
		return Number{std::bit_cast<double>(nan_bits)};
	}

	static Number trusted(double value)
	{
#ifdef JET_DEBUG
		uint64_t canon = std::bit_cast<uint64_t>(from_ieee(value).value);
		JET_DIE_UNLESS(nullptr, std::bit_cast<uint64_t>(value) == canon, "non-canonical number {}", value);
#endif
		return Number{value};
	}

	private:
		explicit Number(double value) : value{value} {}
};

namespace jet_tag
{
	enum : int
	{
		none = 0,
#define X(_enum, name, _cpp) name,
		JET_IMM_TYPES(X)
#undef X
		eof_tag,  // Marker tag (singleton, no C++ value type).
#define X(_enum, name, _cpp) name,
		JET_HEAP_TYPES(X)
#undef X
		HEAP_END,
		TAG_MAX = HEAP_END,
	};
} // namespace jet_tag

class Atom
{
public:
	uint64_t bits;

	Atom() : bits{QNAN_TAG} {}

	static Atom from_bits(uint64_t b)
	{
		Atom a;
		a.bits = b;
		return a;
	}

	static Atom from_double(double d)
	{
		Atom a;
		memcpy(&a.bits, &d, sizeof(double));
		return a;
	}

	double as_double()
	{
		double d;
		memcpy(&d, &bits, sizeof(double));
		return d;
	}

	bool is_number() { return (bits & QNAN_TAG) != QNAN_TAG; }
	bool is_tagged() { return (bits & QNAN_TAG) == QNAN_TAG; }

	template <int tag_value>
	bool tag_is()
	{
		static_assert(tag_value >= 0 && tag_value < 16);
		constexpr uint16_t encoded = static_cast<uint16_t>(
			(QNAN_TAG >> 48) | (tag_value & 0x7) | ((tag_value & 0x8) << 12));
		return static_cast<uint16_t>(bits >> 48) == encoded;
	}

	int tag() { return static_cast<int>(((bits >> 48) & 0x7) | ((bits >> 60) & 0x8)); }

	void* as_ptr() { return std::bit_cast<void*>(bits & PAYLOAD_MASK); }

	uint64_t as_payload() { return bits & PAYLOAD_MASK; }

	static Atom make_tagged(int tag, const void* ptr)
	{
		uint64_t p = std::bit_cast<uint64_t>(ptr) & PAYLOAD_MASK;
		return from_bits(QNAN_TAG | (static_cast<uint64_t>(tag & 0x7) << 48) |
		                 (static_cast<uint64_t>((tag >> 3) & 0x1) << 63) | p);
	}

	static Atom make_immediate(int tag, uint64_t payload = 0)
	{
		return from_bits(QNAN_TAG | (static_cast<uint64_t>(tag & 0x7) << 48) |
		                 (static_cast<uint64_t>((tag >> 3) & 0x1) << 63) | (payload & PAYLOAD_MASK));
	}

	jet::Type type();

	bool is_heap()
	{
		if (!is_tagged())
		{
			return false;
		}
		int t = tag();
		return t > jet_tag::eof_tag && t < jet_tag::HEAP_END;
	}
};

inline Atom hole()
{
	return Atom::from_bits(HOLE_BITS);
}

inline bool is_hole(Atom a)
{
	return a.bits == HOLE_BITS;
}

inline jet::Type Atom::type()
{
	if (is_number())
	{
		return jet::Type::Number;
	}
	switch (tag())
	{
#define X(name, tag, _cpp) case jet_tag::tag: return jet::Type::name;
	JET_IMM_TYPES(X)
	JET_HEAP_TYPES(X)
#undef X
		case jet_tag::eof_tag:
			return jet::Type::Eof;
		default:
			return jet::Type::Unknown;
	}
}

template <jet::Type type>
bool is_type(Atom x)
{
	return type == x.type();
}

inline uint16_t type_bits(Atom a)
{
	return static_cast<uint16_t>(a.bits >> 48);
}

struct VectorCursor;

struct Vec
{
private:
	std::vector<Atom> values;
	size_t count{};

public:
	std::vector<VectorCursor*> cursors;
	std::vector<size_t> cursor_indices;

	Vec() = default;
	template <typename It>
	Vec(It first, It last) : values(first, last), count{values.size()} {}
	Vec(size_t size, Atom fill) : values(size, fill), count{size} {}
	Vec(const Vec& other) : values(other.values), count{other.count} {}
	Vec(Vec&& other) noexcept : values(std::move(other.values)), count{other.count}
	{
		other.count = 0;
	}
	~Vec();

	using iterator = std::vector<Atom>::iterator;

	bool empty() const { return count == 0; }
	size_t size() const { return count; }
	Atom* data() { return values.data(); }
	Atom& front() { return values.front(); }
	Atom& back() { return values.back(); }
	Atom& operator[](size_t index) { return values[index]; }
	iterator begin() { return values.begin(); }
	iterator end() { return values.end(); }
	void reserve(size_t capacity) { values.reserve(capacity); }
	void push_back(Atom value)
	{
		values.push_back(value);
		++count;
	}
	void pop_back()
	{
		values.pop_back();
		--count;
	}
	iterator erase(iterator position)
	{
		iterator next{values.erase(position)};
		--count;
		return next;
	}
};

struct EmptyList
{
};

struct Cons;
struct Lambda;
struct Prim;
class IPort;
class OPort;
class Port;
class IPortFile;
class OPortFile;
struct Slot;
class StructType;
struct Struct;

template <typename T>
struct dynamic_type;

template <>
struct dynamic_type<Number>
{
	static constexpr jet::Type id = jet::Type::Number;
};

#define X(name, tag_name, cpp) \
	template <> struct dynamic_type<cpp> \
	{ \
		static constexpr jet::Type id = jet::Type::name; \
		static constexpr int tag = jet_tag::tag_name; \
	};
JET_IMM_TYPES(X)
JET_HEAP_TYPES(X)
#undef X

template <>
struct dynamic_type<IPort>
{
	static constexpr jet::Type id = jet::Type::Port;
};
template <>
struct dynamic_type<OPort>
{
	static constexpr jet::Type id = jet::Type::Port;
};
template <>
struct dynamic_type<IPortFile>
{
	static constexpr jet::Type id = jet::Type::Port;
};
template <>
struct dynamic_type<OPortFile>
{
	static constexpr jet::Type id = jet::Type::Port;
};

template <typename T>
struct box_unbox_t;

template <>
struct box_unbox_t<Number>
{
	static Atom box(Number number) { return Atom::from_double(number.value); }

	static double unbox(Atom atom) { return atom.as_double(); }
};

template <>
struct box_unbox_t<bool>
{
	static Atom box(bool v) { return Atom::make_immediate(jet_tag::boolean, v ? 1 : 0); }

	static bool unbox(Atom x) { return x.as_payload() != 0; }
};

template <>
struct box_unbox_t<Character>
{
	static Atom box(Character v) { return Atom::make_immediate(jet_tag::character, v); }

	static Character unbox(Atom x) { return static_cast<Character>(x.as_payload()); }
};

template <>
struct box_unbox_t<EmptyList>
{
	static Atom box(EmptyList = {}) { return Atom::make_immediate(jet_tag::empty_list); }

	static EmptyList unbox(Atom&) { return {}; }
};

std::string_view type_name(jet::Type type);

[[noreturn]] void die_type_mismatch(VmState& s, Atom a, jet::Type t);

inline void type_check(VmState& s, Atom a, jet::Type t)
{
	if (t != a.type()) [[unlikely]]
	{
		die_type_mismatch(s, a, t);
	}
}

inline Atom box(Atom value)
{
	return value;
}

template <typename T>
Atom box(T&& init)
{
	return box_unbox_t<typename std::remove_reference<T>::type>::box(static_cast<T&&>(init));
}

template <typename T, typename... Args>
Atom box(Args&&... args)
{
	return box_unbox_t<T>::box(static_cast<Args&&>(args)...);
}

template <typename T>
decltype(auto) unbox(Atom a)
{
	// Unchecked. Caller has proven the type.
	return box_unbox_t<T>::unbox(a);
}

template <typename T>
decltype(auto) slow_unbox(VmState& s, Atom a)
{
	type_check(s, a, dynamic_type<T>::id);
	return box_unbox_t<T>::unbox(a);
}

#endif

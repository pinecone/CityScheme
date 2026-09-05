// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef vm_h
#define vm_h

#include "atom.h"
#include "debug.h"
#include "opcodes.h"
#include "platform.h"
#include <ankerl/unordered_dense.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Arity
{
	enum
	{
		Exactly,
		AtLeast,
		NAry
	} how;
	size_t expected;
};

constexpr Arity exactly(size_t expected)
{
	return {Arity::Exactly, expected};
}
constexpr Arity at_least(size_t expected)
{
	return {Arity::AtLeast, expected};
}
constexpr Arity n_ary()
{
	return {Arity::NAry, 0};
}
constexpr bool is_nary(Arity& a)
{
	return Arity::NAry == a.how;
}

struct Struct;
struct Cursor;
using StructDestructor = void (*)(Struct*);

inline void set_bit(uint64_t* bits, size_t i)
{
	bits[i / 64] |= 1ULL << (i % 64);
}

inline void set_bits(uint64_t* bits, size_t start, size_t n)
{
	size_t word = start / 64;
	size_t offset = start % 64;
	size_t head = 64 - offset;
	if (n <= head) [[likely]]
	{
		bits[word] |= (n == 64 ? ~0ULL : (1ULL << n) - 1) << offset;
		return;
	}
	bits[word] |= ~0ULL << offset;
	for (size_t rest = n - head; rest != 0;)
	{
		size_t take = rest < 64 ? rest : 64;
		bits[++word] |= take == 64 ? ~0ULL : (1ULL << take) - 1;
		rest -= take;
	}
}

inline void clear_bit(uint64_t* bits, size_t i)
{
	bits[i / 64] &= ~(1ULL << (i % 64));
}

inline void clear_bits(uint64_t* bits, size_t start, size_t n)
{
	size_t word = start / 64;
	size_t offset = start % 64;
	size_t head = 64 - offset;
	if (n <= head) [[likely]]
	{
		bits[word] &= ~((n == 64 ? ~0ULL : (1ULL << n) - 1) << offset);
		return;
	}
	bits[word] &= ~(~0ULL << offset);
	for (size_t rest = n - head; rest != 0;)
	{
		size_t take = rest < 64 ? rest : 64;
		bits[++word] &= take == 64 ? 0ULL : ~((1ULL << take) - 1);
		rest -= take;
	}
}

inline bool test_bit(const uint64_t* bits, size_t i)
{
	return (bits[i / 64] >> (i % 64)) & 1ULL;
}

inline void* checked_malloc(VmState* vm, size_t bytes)
{
	void* mem = std::malloc(bytes);
	JET_DIE_UNLESS(vm, mem != nullptr, "gc: out of memory allocating %zu bytes", bytes);
	return mem;
}

struct Gc
{
	struct ObjEntry
	{
		uint32_t cell_idx;
		uint32_t n_cells;
		uint16_t destructor_id;
		uint8_t tag;
	};
	static_assert(sizeof(ObjEntry) == 12);

	struct HugeEntry
	{
		uint32_t n_cells;
		uint16_t destructor_id;
		uint8_t tag;
		bool marked;
	};

	static constexpr size_t CELL_SIZE = 16;
	static constexpr size_t ARENA_SIZE = 1ULL << 30;
	static constexpr size_t TOTAL_CELLS = ARENA_SIZE / CELL_SIZE;
	static constexpr size_t BITMAP_WORDS = TOTAL_CELLS / 64;
	static constexpr size_t N_BUCKETS = 256;
	static constexpr size_t MAX_BUCKET_BYTES = (N_BUCKETS - 1) * CELL_SIZE;

	// Allocations permitted per live object before the next collection. GC work per
	// allocation falls as 1 + 2/k, while the arena high-water mark grows as (1+k)*live.
	static constexpr size_t HEAP_GROWTH_FACTOR = 4;
	static constexpr uint32_t MIN_GC_THRESHOLD = 256;

	uint32_t alloc_since_gc = 0;
	uint32_t gc_threshold = 256;
	uint32_t epoch = 0;
	char* arena_base;
	size_t bump_cells = 0;
	ObjEntry* objects = nullptr;
	ObjEntry* objects_end = nullptr;
	ObjEntry* objects_cap = nullptr;
	uint64_t* live_bits;
	uint64_t* mark_bits;
	std::vector<StructDestructor> struct_destructors{nullptr};
	void* freelist[jet_tag::TAG_MAX][N_BUCKETS] = {};
	void* raw_freelist[N_BUCKETS] = {};
	ankerl::unordered_dense::map<void*, HugeEntry> huge;

	Gc();
	~Gc();

	Gc(const Gc&) = delete;
	Gc& operator=(const Gc&) = delete;

	uint16_t register_struct_destructor(VmState& s, StructDestructor destructor);
	JET_NOINLINE void* alloc_slow(VmState& s, size_t n, int tag, uint16_t destructor_id);
	JET_NOINLINE JET_COLD void* alloc_huge(VmState& s, size_t n, int tag, uint16_t destructor_id);
	JET_NOINLINE JET_COLD void mark_huge(void* ptr);
	JET_NOINLINE void grow_objects(VmState& s);
	void sweep();
	void mark_atom(uint64_t bits);
	void mark_object(void* ptr, int tag);
	void mark_lambda(struct Lambda* la);

	JET_ALWAYS_INLINE void* alloc(VmState& s, size_t obj_size, int tag, uint16_t destructor_id)
	{
		size_t n = (obj_size + CELL_SIZE - 1) / CELL_SIZE;
		void* mem = n < N_BUCKETS ? freelist[tag][n] : nullptr;
		if (mem == nullptr || objects_end == objects_cap) [[unlikely]]
		{
			return alloc_slow(s, n, tag, destructor_id);
		}
		freelist[tag][n] = next_free(mem);
		uint32_t start = static_cast<uint32_t>((static_cast<char*>(mem) - arena_base) / CELL_SIZE);
		set_bits(live_bits, start, n);
		*objects_end++ = {start, static_cast<uint32_t>(n), destructor_id, static_cast<uint8_t>(tag)};
		++alloc_since_gc;
		return mem;
	}

	JET_ALWAYS_INLINE void* alloc_raw_small(VmState& s, size_t n)
	{
		void* mem = raw_freelist[n];
		if (mem)
		{
			raw_freelist[n] = next_free(mem);
			return mem;
		}
		JET_DIE_UNLESS(&s, bump_cells + n <= TOTAL_CELLS, "gc: arena exhausted");
		mem = arena_base + bump_cells * CELL_SIZE;
		bump_cells += n;
		return mem;
	}

	JET_ALWAYS_INLINE void* alloc_raw(VmState& s, size_t bytes)
	{
		if (bytes > MAX_BUCKET_BYTES) [[unlikely]]
		{
			return checked_malloc(&s, bytes);
		}
		size_t n = (bytes + CELL_SIZE - 1) / CELL_SIZE;
		return alloc_raw_small(s, n);
	}

	JET_ALWAYS_INLINE void free_raw_small(void* mem, size_t n)
	{
		link_free(mem, raw_freelist[n]);
		raw_freelist[n] = mem;
	}

	JET_ALWAYS_INLINE void free_raw(void* mem, size_t bytes)
	{
		if (bytes > MAX_BUCKET_BYTES) [[unlikely]]
		{
			std::free(mem);
			return;
		}
		size_t n = (bytes + CELL_SIZE - 1) / CELL_SIZE;
		free_raw_small(mem, n);
	}

	JET_ALWAYS_INLINE static void link_free(void* mem, void* next) { std::memcpy(mem, &next, sizeof(next)); }

	JET_ALWAYS_INLINE static void* next_free(void* mem)
	{
		void* next;
		// the link shares storage with a dead object of another type;
		// byte copies avoid the aliasing violation (UB)
		std::memcpy(&next, mem, sizeof(next));
		return next;
	}

	bool should_collect() { return alloc_since_gc > gc_threshold; }

	template <typename T, typename... Args>
	Atom alloc_tagged(VmState& s, Args&&... args);
};

template <typename T>
constexpr StructDestructor struct_destructor()
{
	if constexpr (std::is_trivially_destructible_v<T>)
	{
		return nullptr;
	}
	else
	{
		return [](Struct* instance) { static_cast<T*>(instance)->~T(); };
	}
}

constexpr int type_to_tag(jet::Type t)
{
	switch (t)
	{
#define X(name, tag, _cpp) case jet::Type::name: return jet_tag::tag;
	JET_IMM_TYPES(X)
	JET_HEAP_TYPES(X)
#undef X
		case jet::Type::Eof:
			return jet_tag::eof_tag;
		default:
			return 0;
	}
}

template <typename T>
void gc_destroy(void* p)
{
	// No free -- GC owns the memory.
	static_cast<T*>(p)->~T();
}

template <typename T>
struct box_unbox_t
{
	static constexpr int tag = type_to_tag(dynamic_type<T>::id);

	static T* unbox(Atom x) { return static_cast<T*>(x.as_ptr()); }
};

template <typename T, typename... Args>
JET_ALWAYS_INLINE Atom Gc::alloc_tagged(VmState& s, Args&&... args)
{
	constexpr int tag = box_unbox_t<T>::tag;
	void* mem = alloc(s, sizeof(T), tag, 0);
	return Atom::make_tagged(tag, new (mem) T(static_cast<Args&&>(args)...));
}

template <>
struct box_unbox_t<Symbol>
{
	static Atom box(Symbol symbol)
	{
		return Atom::make_tagged(jet_tag::symbol, symbol);
	}

	static Symbol unbox(Atom x) { return static_cast<Symbol>(x.as_ptr()); }
};

class InternedSymbols
{
public:
	Symbol intern(std::string_view value)
	{
		if (auto found = values_.find(value); found != values_.end())
		{
			return &*found;
		}
		auto entry = values_.emplace(value).first;
		return &*entry;
	}

private:
	struct StringHash : std::hash<std::string_view>
	{
		using is_transparent = void;
	};

	std::unordered_set<std::string, StringHash, std::equal_to<>> values_;
};

inline uint64_t g_slot_version_counter = 0;
inline uint64_t next_slot_version()
{
	return ++g_slot_version_counter;
}

struct Slot
{
	Atom value;
	// Globally unique, stamped on every allocation and every mutation, so an
	// IC keying on (Slot*, version) cannot ABA when GC reuses a slot's memory.
	uint64_t version;

	Slot() : value{}, version{next_slot_version()} {}
	explicit Slot(Atom v) : value{v}, version{next_slot_version()} {}
};

using Code = uint8_t;

enum class CaptureSource : uint8_t
{
	Local = 0,
	Upvalue = 1
};

enum class ConstTag : uint8_t
{
	Number,
	Boolean,
	Character,
	String,
	Symbol,
	EmptyList,
	Unknown,
	GlobalName,
	// Pool entry encoding a lambda body. Decodes to a template Lambda atom
	// (captures empty); clos clones it with a populated captures vec.
	// A zero-upvalue lambda is reached via ldc -- the template is the closure.
	Lambda
};

struct Lambda;
struct Coro;

struct Frame
{
	Code* code;
	Lambda* closure;
	size_t base;
	size_t top;
};

template <typename T>
class Stack
{
public:
	size_t size() const { return active_; }
	bool empty() const { return active_ == 0; }
	bool can_push() const { return active_ < storage_.size(); }
	void grow() { storage_.resize(storage_.empty() ? 8 : storage_.size() * 2); }

	T& push()
	{
		if (!can_push()) [[unlikely]]
		{
			grow();
		}
		return push_unchecked();
	}

	T& push(const T& value)
	{
		T& item = push();
		item = value;
		return item;
	}

	T& push_unchecked() { return storage_[active_++]; }

	void pop() { --active_; }
	void truncate(size_t n) { active_ = n; }
	T& back() { return storage_[active_ - 1]; }
	T& operator[](size_t i) { return storage_[i]; }
	T* begin() { return storage_.data(); }
	T* end() { return storage_.data() + active_; }

private:
	std::vector<T> storage_;
	size_t active_{};
};


class Env
{
public:
	void bind(std::string_view name, Atom atom)
	{
		items_[std::string{name}] = atom;
		JET_PROFILE_BIND(name, atom);
	}

	Atom* lookup(std::string_view name)
	{
		auto x = items_.find(std::string{name});
		return x == items_.end() ? nullptr : &x->second;
	}

	template <typename F>
	void scan(F&& f)
	{
		for (auto& [k, v] : items_)
		{
			f(v);
		}
	}

private:
	using items_t = std::unordered_map<std::string, Atom>;
	items_t items_;
};

struct Lambda
{
	Code* code;
	Arity arity;
	uint16_t n_locals;
	uint16_t n_captures;
	Atom captures[];

	Lambda(Code* c, Arity a, uint16_t nl, uint16_t n) : code{c}, arity{a}, n_locals{nl}, n_captures{n} {}

	static Atom alloc(VmState& s, Code* code, Arity arity, uint16_t n_locals, uint16_t n_captures);

	Lambda(const Lambda&) = delete;
	Lambda& operator=(const Lambda&) = delete;
};

inline bool operator==(Lambda& l1, Lambda& l2)
{
	return l1.code == l2.code;
}

template <>
struct box_unbox_t<Lambda>
{
	static Lambda* unbox(Atom x) { return static_cast<Lambda*>(x.as_ptr()); }
};

struct LambdaDebug
{
	struct Line
	{
		uint32_t off;
		uint32_t line;
		uint32_t file;
	};

	std::string name;
	std::vector<Line> lines;
	size_t code_size = 0;

	const Line* find_line(size_t off) const;
	const Line* find_line(size_t off, size_t code_size) const;
};

struct ProgramDebug
{
	std::vector<std::string> files{};
	ankerl::unordered_dense::map<const Code*, LambdaDebug> code{};
};

constexpr size_t STACK_CAPACITY = 1 << 20;
// apply's list splat writes above the frame before its overflow check runs;
// the slack below the true end absorbs the overshoot.
constexpr size_t STACK_SLACK = 4096;

struct SavedStack
{
	std::unique_ptr<Atom[]> storage;
	Atom* base{};
	Atom* end{};
	Atom* top{};
	Atom* watermark{};
	Stack<Frame> frames{};
};

struct VmState
{
	Gc gc{};
	Env& env;
	InternedSymbols symbols{};
	Stack<Frame> frames{};
	std::unique_ptr<Atom[]> stack{new Atom[STACK_CAPACITY]};
	Code halt_code[OPCODE_SIZE]{};
	Atom* stack_base{};
	Atom* stack_end{};
	Atom* stack_top{};
	// High-water mark of stack_top since the last collect. Slots above it hold no
	// heap atoms; collect() re-zeroes everything between the scanned region and the
	// watermark so the mark scan never reads an atom whose referent a sweep freed
	// while the slot sat unscanned.
	Atom* stack_watermark{};
	Atom* constants{};
	size_t n_constants{};
	uint64_t host_token{};
	uint64_t next_host_token{};
	// The running coroutines, innermost last. The main stack is a permanent coroutine
	// at `running[0]` (`eval`), so `running.back()` always owns the running stack.
	// `x` is a member when `running[x->running_index] == x` with a bounds check;
	// after an escape removes `x`, this stays false forever.
	Stack<Coro*> running{};
	ProgramDebug debug{};
	const Code* toplevel_code{};
	size_t toplevel_code_size{};

	void switch_stack(SavedStack& other)
	{
		stack.swap(other.storage);
		std::swap(stack_base, other.base);
		std::swap(stack_end, other.end);
		std::swap(stack_top, other.top);
		std::swap(stack_watermark, other.watermark);
		std::swap(frames, other.frames);
	}

	void take_stack(SavedStack& other)
	{
		stack = std::move(other.storage);
		stack_base = other.base;
		stack_end = other.end;
		stack_top = other.top;
		stack_watermark = other.watermark;
		frames = std::move(other.frames);
		other.frames.truncate(0);
		other.base = other.end = other.top = other.watermark = nullptr;
	}
};

#define VM_OP_PARAMS                                                                                         \
	VmState& s, Frame* frame, Code* pc, Atom* stack_top, Atom callee, Atom* args, Atom* stack_base,           \
	Atom* frame_regs
using VmOp = void (*)(VM_OP_PARAMS) JET_PRESERVE_NONE;
static_assert(sizeof(VmOp) == VM_OP_SLOT_SIZE);

JET_ALWAYS_INLINE inline VmOp decode_op(const Code* code)
{
	VmOp op;
	std::memcpy(&op, code, sizeof(op));
	return op;
}

#define VM_OP_ARGS s, frame, pc, stack_top, callee, args, stack_base, frame_regs

void collect(VmState& s);

struct ObjShape
{
	VmOp ldf_handler;
	VmOp stf_handler;
	VmOp ldfk_handler;
	VmOp stfk_handler;
	VmOp ldfh_handler;
	VmOp ldfkh_handler;
	VmOp ldfo_handler;
	VmOp ldfko_handler;
	Atom (*ref_or_die)(VmState&, Atom, Atom);
	Atom (*ref_or_hole)(VmState&, Atom, Atom);
	Cursor* (*iter)(VmState&, Atom);
};

extern ObjShape g_shape_by_tag[jet_tag::HEAP_END];

#define DISPATCH()                                                                                           \
	do                                                                                                       \
	{                                                                                                        \
		VmOp h = decode_op(pc);                                                                                \
		pc += OPCODE_SIZE;                                                                                   \
		JET_PROFILE_OP(pc - OPCODE_SIZE);                                                                  \
		JET_TRACE_STEP(s, frame, pc, stack_top);                                                            \
		JET_MUSTTAIL return h(VM_OP_ARGS);                                                                  \
	} while (0)

#define JET_GC_CHECK()                                                                                      \
	do                                                                                                       \
	{                                                                                                        \
		if (s.gc.should_collect()) [[unlikely]]                                                             \
		{                                                                                                    \
			JET_MUSTTAIL return op_gc_slow(VM_OP_ARGS);                                                     \
		}                                                                                                    \
	} while (0)

[[noreturn]] void vm_exit(VmState& vm, int status);

[[noreturn]] void eval(VmState& vm, Frame& init_frame, Atom* constants, size_t n_constants,
                       size_t initial_stack_size);

Atom jet_enter_vm(VmState& vm, Atom proc, Atom* args, size_t n_args);

struct LoadedProgram
{
	Code* code;
	uint32_t n_toplevel_slots;
	std::vector<Atom> constants;
};

struct CodeImage
{
	Code* bytes;
	size_t size;

	CodeImage(const Code* data, size_t n_bytes)
		: bytes{static_cast<Code*>(checked_malloc(nullptr, n_bytes))}, size{n_bytes}
	{
		std::memcpy(bytes, data, n_bytes);
	}
	~CodeImage()
	{
		std::free(bytes);
	}

	CodeImage(const CodeImage&) = delete;
	CodeImage& operator=(const CodeImage&) = delete;
};

// Bytecode layout: [debug section][u32 n_toplevel_slots][u32 n_pool_entries][pool entries][toplevel code]
// Debug source maps: lambda source maps in constant-pool order, toplevel source map last.
Code* parse_debug_section(VmState* s, Code* p, Code* end, std::vector<std::string>& files,
                          std::vector<LambdaDebug>& source_maps);
const Code* frame_code_start(const VmState& s, const Frame& f, const Code* instruction);
LoadedProgram load_program(VmState& s, Code* bytecode, size_t n_bytes);

inline Atom Lambda::alloc(VmState& s, Code* code, Arity arity, uint16_t n_locals, uint16_t n_captures)
{
	size_t total = sizeof(Lambda) + static_cast<size_t>(n_captures) * sizeof(Atom);
	void* mem = s.gc.alloc(s, total, jet_tag::procedure, 0);
	return Atom::make_tagged(jet_tag::procedure, new (mem) Lambda{code, arity, n_locals, n_captures});
}

#endif

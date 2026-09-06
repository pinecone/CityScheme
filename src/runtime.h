// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef runtime_h
#define runtime_h

#include "atom.h"
#include "error.h"
#include "platform.h"
#include "vm.h"
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct Cons
{
	Cons(Atom car_, Atom cdr_) : car{car_}, cdr{cdr_} {}
	mutable Atom car, cdr;
};

bool operator==(Cons& p1, Cons& p2);

JET_ALWAYS_INLINE inline Atom cons(VmState& s, Atom obj1, Atom obj2)
{
	return s.gc.alloc_tagged<Cons>(s, obj1, obj2);
}

inline Atom car(VmState& s, Atom a)
{
	return slow_unbox<Cons>(s, a)->car;
}

inline Atom cdr(VmState& s, Atom a)
{
	return slow_unbox<Cons>(s, a)->cdr;
}

Atom is_list(VmState& s, Atom a);

void init_lists(VmState& s);

template <typename Out>
Out list_to_args(VmState& s, Atom list, Out out)
{
	for (Atom x = list; !is_type<jet::Type::EmptyList>(x); x = cdr(s, x))
	{
		*out++ = car(s, x);
	}
	return out;
}

inline const std::string& symbol_to_string(Symbol symbol) { return *symbol; }
Atom string_to_symbol(VmState& s, Atom a);

void init_symbols(VmState& s);

bool operator==(Vec& v1, Vec& v2);

Atom vector_ctor(VmState& s, Atom* first, Atom* last);
Atom make_vector(VmState& s, Atom n, Atom f);
Atom vector_ref(VmState& s, Atom v, Atom i);
Atom vector_length(VmState& s, Atom v);
void init_vecs(VmState& s);

Atom string_ref(VmState& s, Atom str, Atom k);

Atom bytevector_u8_ref(VmState& s, Atom bv, Atom k);
void init_bytevectors(VmState& s);

struct EqualContext;
struct Struct;
class StructType;
using EqualRecur = bool (*)(EqualContext&, Atom, Atom);

enum class StructKind : uint8_t
{
	Scheme,
	Tuple,
	HashSet,
	HashMap,
	Cursor,
	Escape,
	Coro,
	Yield,
};

struct StructOps
{
	StructKind kind;
	VmOp constructor;
	ObjShape shape;
	StructDestructor destroy;
	bool (*equal)(EqualContext&, Struct*, Struct*, EqualRecur);
	void (*display)(VmState&, Struct*, std::string&);
	void (*write)(VmState&, Struct*, std::string&);
};

class StructType
{
public:
	StructType(VmState& s, Atom name, std::vector<Atom> field_names, Arity arity, const StructOps& ops)
		: name_{name}, field_names_{std::move(field_names)}, arity_{arity},
		destructor_id_{s.gc.register_struct_destructor(s, ops.destroy)}, kind_{ops.kind}, ops_{&ops}
	{
	}

	Atom name() const { return name_; }
	Arity arity() const { return arity_; }
	uint16_t destructor_id() const { return destructor_id_; }
	StructKind kind() const { return kind_; }
	const StructOps& ops() const { return *ops_; }

	int find(Atom key) const
	{
		for (size_t i = 0; i < field_names_.size(); ++i)
		{
			if (field_names_[i].bits == key.bits)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

private:
	Atom name_;
	std::vector<Atom> field_names_;
	Arity arity_;
	uint16_t destructor_id_;
	StructKind kind_;
	const StructOps* ops_;
};

inline Atom make_struct_type(VmState& s, Atom name, std::vector<Atom> field_names, Arity arity,
                             const StructOps& ops)
{
	return s.gc.alloc_tagged<StructType>(s, s, name, std::move(field_names), arity, ops);
}

inline bool operator==(StructType& a, StructType& b)
{
	return &a == &b;
}

struct Struct
{
	StructType* type;

	explicit Struct(StructType* type_) : type{type_} {}

	Struct(const Struct&) = delete;
	Struct& operator=(const Struct&) = delete;
};

struct Escape : Struct
{
	inline static Atom type_atom{};

	Code* resume_pc;
	// The coroutine whose stack holds the `let/ec` frame.
	Coro* owner;
	uint64_t host_token;
	uint32_t n_frames;
	uint16_t dst;

	// One instruction, [handler][tag][Escape*], installed as the return address of
	// frame n_frames - 1 for exactly as long as the extent lives.
	Code retk_code[OPCODE_SIZE + sizeof(Struct*)];

	Escape(StructType* type_, Code* resume_pc_, Coro* owner_, uint64_t host_token_, uint32_t n_frames_,
	       uint16_t dst_)
		: Struct{type_}, resume_pc{resume_pc_}, owner{owner_}, host_token{host_token_}, n_frames{n_frames_},
	dst{dst_}, retk_code{} {}
};

void init_escapes(VmState& s);

enum class CoroState : uint8_t
{
	Suspended,
	Running,
	Completed,
	Dead,
};

struct Coro : Struct
{
	inline static Atom type_atom{};

	// Holds this coroutine's own stack while suspended and the parent's saved stack
	// while running; `switch_stack` keeps every storage owned by exactly one holder.
	SavedStack stack;
	Code* consequent_pc{};
	uint32_t running_index{};
	uint16_t dst{};
	CoroState state{CoroState::Suspended};

	explicit Coro(StructType* type_) : Struct{type_} {}

	void trace(Gc& gc)
	{
		if (!stack.storage)
		{
			return;
		}
		Atom* frontier = stack.top;
		for (Frame& frame : stack.frames)
		{
			if (stack.base + frame.top > frontier)
			{
				frontier = stack.base + frame.top;
			}
			if (frame.closure)
			{
				gc.mark_atom(Atom::make_tagged(jet_tag::procedure, frame.closure).bits);
			}
		}
		for (Atom* p = stack.base; p < frontier; ++p)
		{
			gc.mark_atom(p->bits);
		}
		// Same stale-slot invariant as `VmState::stack_watermark`: this scan does not
		// reach above the frontier, so that region must be zero before the sweep.
		std::memset(frontier, 0, static_cast<size_t>(stack.watermark - frontier) * sizeof(Atom));
		stack.watermark = frontier;
	}
};

struct Yield : Struct
{
	inline static Atom type_atom{};

	Struct* target;

	Yield(StructType* type_, Struct* target_) : Struct{type_}, target{target_} {}

	void trace(Gc& gc) { gc.mark_atom(Atom::make_tagged(jet_tag::struct_, target).bits); }
};

void init_coroutines(VmState& s);

struct CursorOps
{
	VmOp next1;
	VmOp next2;
};

struct Cursor : Struct
{
	Atom target;
	const CursorOps* ops;

	Cursor(StructType* type, Atom target_, const CursorOps* ops_) : Struct{type}, target{target_},
	ops{ops_} {}

	void trace(Gc& gc) { gc.mark_atom(target.bits); }
};

struct VectorCursor : Cursor
{
	inline static Atom type_atom{};
	Vec* vector;
	size_t slot;

	VectorCursor(StructType* type, Atom target, const CursorOps* ops)
		: Cursor{type, target, ops}, vector{unbox<Vec>(target)}, slot{vector->cursors.size()}
	{
		vector->cursors.push_back(this);
		vector->cursor_indices.push_back(0);
	}

	~VectorCursor() {
		detach();
	}

	void detach()
	{
		if (!vector)
		{
			return;
		}
		VectorCursor* moved = vector->cursors.back();
		vector->cursors[slot] = moved;
		vector->cursor_indices[slot] = vector->cursor_indices.back();
		moved->slot = slot;
		vector->cursors.pop_back();
		vector->cursor_indices.pop_back();
		vector = nullptr;
	}
};

inline Vec::~Vec()
{
	for (VectorCursor* cursor : cursors)
	{
		cursor->vector = nullptr;
	}
}

struct SchemeStruct : Struct
{
	uint32_t n_fields;
	Atom values[];

	SchemeStruct(StructType* type, uint32_t n) : Struct{type}, n_fields{n} {}

	static SchemeStruct* alloc(VmState& s, StructType* type, uint32_t n_fields)
	{
		size_t total = sizeof(SchemeStruct) + static_cast<size_t>(n_fields) * sizeof(Atom);
		void* mem = s.gc.alloc(s, total, jet_tag::struct_, type->destructor_id());
		return new (mem) SchemeStruct{type, n_fields};
	}

	void trace(Gc& gc)
	{
		for (uint32_t i = 0; i < n_fields; ++i)
		{
			gc.mark_atom(values[i].bits);
		}
	}

	SchemeStruct(const SchemeStruct&) = delete;
	SchemeStruct& operator=(const SchemeStruct&) = delete;
};

struct Tuple : Struct
{
	static constexpr uint32_t hash_unset = 0;
	static constexpr uint32_t hash_illegal = 1;

	uint32_t size;
	uint32_t hash;
	Atom elements[];

	Tuple(StructType* type, uint32_t size_) : Struct{type}, size{size_}, hash{hash_unset} {}

	static Tuple* alloc(VmState& s, StructType* type, uint32_t size)
	{
		size_t total = sizeof(Tuple) + static_cast<size_t>(size) * sizeof(Atom);
		void* mem = s.gc.alloc(s, total, jet_tag::struct_, type->destructor_id());
		return new (mem) Tuple{type, size};
	}

	void trace(Gc& gc)
	{
		for (uint32_t i = 0; i < size; ++i)
		{
			gc.mark_atom(elements[i].bits);
		}
	}

	Tuple(const Tuple&) = delete;
	Tuple& operator=(const Tuple&) = delete;
};

static_assert(sizeof(Tuple) == 16);

bool is_eqv(VmState& s, Atom a, Atom b);

// Capture-free lambdas share one pool template, so identity would expose optimization choices.
inline bool is_eq(Atom a, Atom b)
{
	return a.bits == b.bits && !is_type<jet::Type::Procedure>(a);
}

// Hashing a value and proving it a legal key are one walk, so the verdict travels with the key.
struct TableKey
{
	Atom atom;
	uint64_t hash;
};

struct HashMapEntry
{
	TableKey key;
	Atom value;
};

enum class FastKeyKind : uint8_t
{
	Bits,
	Tuple,
};

struct FastKey
{
	TableKey key;
	FastKeyKind kind;
};

struct KeyHash
{
	using is_avalanching = void;
	using is_transparent = void;
	size_t operator()(const TableKey& key) const { return key.hash; }
	size_t operator()(const FastKey& key) const { return key.key.hash; }
};

bool equal_key(VmState& vm, const TableKey& first, const TableKey& second);

struct KeyEqual
{
	using is_transparent = void;

	VmState& vm;

	explicit KeyEqual(VmState& vm_) : vm{vm_} {}
	bool operator()(const TableKey& first, const TableKey& second) const
	{
		return equal_key(vm, first, second);
	}
	bool operator()(const FastKey& first, const TableKey& second) const
	{
		if (first.key.hash != second.hash)
		{
			return false;
		}
		if (first.key.atom.bits == second.atom.bits)
		{
			return true;
		}
		if (first.kind == FastKeyKind::Tuple)
		{
			return equal_key(vm, first.key, second);
		}
		return false;
	}
};

template <typename T>
struct GcAllocator
{
	using value_type = T;
	using is_always_equal = std::true_type;

	VmState* vm;

	GcAllocator() = delete;
	explicit GcAllocator(VmState& s) : vm{&s} {}
	template <typename U>
	GcAllocator(const GcAllocator<U>& other) : vm{other.vm} {}

	[[nodiscard]] T* allocate(size_t n)
	{
		static_assert(alignof(T) <= Gc::CELL_SIZE);
		return static_cast<T*>(vm->gc.alloc_raw(*vm, n * sizeof(T)));
	}

	void deallocate(T* p, size_t n)
	{
		vm->gc.free_raw(p, n * sizeof(T));
	}
};

template <typename T, typename U>
bool operator==(const GcAllocator<T>&, const GcAllocator<U>&)
{
	return true;
}

template <typename T, typename U>
bool operator!=(const GcAllocator<T>&, const GcAllocator<U>&)
{
	return false;
}

using TableIndexAllocator = GcAllocator<std::pair<TableKey, size_t>>;
using TableIndex = ankerl::unordered_dense::map<TableKey, size_t, KeyHash, KeyEqual, TableIndexAllocator>;

template <typename T, typename Allocator>
class Ring
{
static_assert(std::is_trivially_copyable_v<T>);

public:
	explicit Ring(const Allocator& allocator) : allocator_{allocator} {}
	~Ring()
	{
		if (values_)
		{
			std::allocator_traits<Allocator>::deallocate(allocator_, values_, mask_ + 1);
		}
	}

	bool empty() const { return count_ == 0; }
	size_t size() const { return count_; }
	T& operator[](size_t index) { return values_[physical(index)]; }
	const T& operator[](size_t index) const { return values_[physical(index)]; }

	void push_back(T value)
	{
		if (!values_ || count_ > mask_)
		{
			grow();
		}
		values_[physical(count_)] = std::move(value);
		++count_;
	}

	void pop_front()
	{
		head_ = (head_ + 1) & mask_;
		--count_;
	}

	void pop_back()
	{
		--count_;
	}

	void clear()
	{
		head_ = 0;
		count_ = 0;
	}

private:
	static constexpr size_t initial_capacity = 4;
	[[no_unique_address]] Allocator allocator_;
	T* values_{};
	size_t mask_{};
	size_t head_{};
	size_t count_{};

	size_t physical(size_t index) const { return (head_ + index) & mask_; }

	void grow()
	{
		size_t old_capacity = values_ ? mask_ + 1 : 0;
		size_t capacity = old_capacity ? old_capacity * 2 : initial_capacity;
		T* values = std::allocator_traits<Allocator>::allocate(allocator_, capacity);
		for (size_t i = 0; i < count_; ++i)
		{
			values[i] = std::move((*this)[i]);
		}
		if (values_)
		{
			std::allocator_traits<Allocator>::deallocate(allocator_, values_, old_capacity);
		}
		values_ = values;
		mask_ = capacity - 1;
		head_ = 0;
	}
};

inline const TableKey& table_entry_key(const TableKey& entry)
{
	return entry;
}

inline const TableKey& table_entry_key(const HashMapEntry& entry)
{
	return entry.key;
}

inline void trace_table_entry(Gc& gc, const TableKey& entry)
{
	gc.mark_atom(entry.atom.bits);
}

inline void trace_table_entry(Gc& gc, const HashMapEntry& entry)
{
	gc.mark_atom(entry.key.atom.bits);
	gc.mark_atom(entry.value.bits);
}

template <typename Entry>
struct TableCursor;

template <typename Entry>
struct Table : Struct
{
	using CursorType = TableCursor<Entry>;

	TableIndex index;
	Ring<Entry, GcAllocator<Entry>> entries;
	Ring<uint64_t, GcAllocator<uint64_t>> live_words;
	std::vector<CursorType*, GcAllocator<CursorType*>> cursors;
	std::vector<size_t, GcAllocator<size_t>> cursor_positions;
	size_t first{};
	size_t last{};

	Table(VmState& s, StructType* type)
		: Struct{type}, index{0, KeyHash{}, KeyEqual{s}, GcAllocator<std::pair<TableKey, size_t>>{s}},
	entries{GcAllocator<Entry>{s}},
	live_words{GcAllocator<uint64_t>{s}}, cursors{GcAllocator<CursorType*>{s}},
	cursor_positions{GcAllocator<size_t>{s}}
	{
	}
	~Table();

	static Table* alloc(VmState& s, StructType* type)
	{
		void* mem = s.gc.alloc(s, sizeof(Table), jet_tag::struct_, type->destructor_id());
		return new (mem) Table{s, type};
	}

	Entry& entry(size_t position) { return entries[position - first]; }
	const Entry& entry(size_t position) const { return entries[position - first]; }

	std::pair<size_t, bool> try_insert(Entry entry)
	{
		size_t position = last;
		auto [it, inserted] = index.try_emplace(table_entry_key(entry), position);
		if (!inserted)
		{
			return {it->second, false};
		}
		size_t word = position / 64;
		if (word == first / 64 + live_words.size())
		{
			live_words.push_back(0);
		}
		entries.push_back(entry);
		set_bit(&live_words[word - first / 64], position % 64);
		++last;
		return {position, true};
	}

	void erase(const TableKey& key)
	{
		auto it = index.find(key);
		if (it == index.end())
		{
			return;
		}
		size_t position = it->second;
		index.erase(it);
		clear_bit(&live_words[position / 64 - first / 64], position % 64);
		entry(position) = {};
		trim();
	}

	bool is_live(size_t position) const
	{
		size_t word = position / 64;
		return test_bit(&live_words[word - first / 64], position % 64);
	}

	size_t next_live(size_t position) const
	{
		position = std::max(position, first);
		if (position >= last)
		{
			return last;
		}
		size_t word = position / 64;
		size_t final_word = (last - 1) / 64;
		uint64_t bits = live_words[word - first / 64] & (~uint64_t{0} << (position % 64));
		while (true)
		{
			if (bits != 0)
			{
				return word * 64 + std::countr_zero(bits);
			}
			if (word == final_word)
			{
				return last;
			}
			++word;
			bits = live_words[word - first / 64];
		}
	}

	void trace(Gc& gc)
	{
		for (size_t position = next_live(first); position < last; position = next_live(position + 1))
		{
			trace_table_entry(gc, entry(position));
		}
	}

	Table(const Table&) = delete;
	Table& operator=(const Table&) = delete;

	void trim()
	{
		size_t old_last = last;
		while (first < last && !is_live(first))
		{
			entries.pop_front();
			++first;
			if (first % 64 == 0)
			{
				live_words.pop_front();
			}
		}
		while (first < last && !is_live(last - 1))
		{
			entries.pop_back();
			--last;
		}
		if (first == last)
		{
			live_words.clear();
		}
		else
		{
			size_t final_word = (last - 1) / 64;
			while (first / 64 + live_words.size() - 1 > final_word)
			{
				live_words.pop_back();
			}
		}
		if (last < old_last)
		{
			for (size_t& position : cursor_positions)
			{
				position = std::min(position, last);
			}
		}
	}
};

template <typename Entry>
struct TableCursor : Cursor
{
	inline static Atom type_atom{};
	Table<Entry>* table;
	size_t slot;

	TableCursor(StructType* type, Atom target, const CursorOps* ops)
		: Cursor{type, target, ops}, table{static_cast<Table<Entry>*>(target.as_ptr())},
	slot{table->cursors.size()}
	{
		table->cursors.push_back(this);
		table->cursor_positions.push_back(table->first);
	}

	~TableCursor()
	{
		detach();
	}

	void detach()
	{
		if (!table)
		{
			return;
		}
		TableCursor* moved = table->cursors.back();
		table->cursors[slot] = moved;
		table->cursor_positions[slot] = table->cursor_positions.back();
		moved->slot = slot;
		table->cursors.pop_back();
		table->cursor_positions.pop_back();
		table = nullptr;
	}
};

template <typename Entry>
inline Table<Entry>::~Table()
{
	for (CursorType* cursor : cursors)
	{
		cursor->table = nullptr;
	}
}

using HashSet = Table<TableKey>;
using HashSetCursor = TableCursor<TableKey>;
using HashMap = Table<HashMapEntry>;
using HashMapCursor = TableCursor<HashMapEntry>;

Cursor* make_hashset_cursor(VmState& s, Atom target);
Cursor* make_hashmap_cursor(VmState& s, Atom target);

inline bool operator==(Struct& a, Struct& b)
{
	return &a == &b;
}

template <>
struct box_unbox_t<Struct>
{
	static Struct* unbox(Atom x) { return static_cast<Struct*>(x.as_ptr()); }
};

inline const ObjShape* shape_of(Atom object)
{
	if (!object.is_heap())
	{
		return nullptr;
	}
	if (object.tag() == jet_tag::struct_)
	{
		return &unbox<Struct>(object)->type->ops().shape;
	}
	const ObjShape* shape = &g_shape_by_tag[object.tag()];
	return shape->ldf_handler || shape->iter ? shape : nullptr;
}

template <auto Construct>
JET_PRESERVE_NONE void struct_constructor_handler(VM_OP_PARAMS)
{
	StructType* type = unbox<StructType>(callee);
	Struct* instance = Construct(s, type, args, stack_top);
	*args = Atom::make_tagged(jet_tag::struct_, instance);
	stack_top = stack_base + frame->top;
	DISPATCH();
}

template <auto Resolve, auto Load>
Atom struct_ref(VmState& s, Atom object, Atom key)
{
	Struct* instance = unbox<Struct>(object);
	return Load(instance, Resolve(s, instance, key));
}

Atom display_to(VmState& s, Atom value, std::string& out);
Atom write_to(VmState& s, Atom value, std::string& out);

void init_structs(VmState& s);
Atom construct_struct(VmState& s, StructType* type, Atom* first, Atom* last);

struct Prim
{
	using Fun = Atom (*)(VmState&, Atom*, Atom*);

	VmOp stub;
	Arity arity;
#ifdef JET_PROFILE
	PrimitiveProfile* profile{&g_profile.primitives.emplace_back(PrimitiveProfile{"<primitive>"})};
#endif
};

inline bool operator==(Prim& p1, Prim& p2)
{
	return p1.stub == p2.stub;
}

inline void check_arity(VmState& s, Arity a, size_t actual)
{
	if (Arity::Exactly == a.how)
	{
		JET_DIE_UNLESS(&s, actual == a.expected,
		               "procedure expects exactly %zu argument(s), given %zu", a.expected, actual);
	}
	else if (Arity::AtLeast == a.how)
	{
		JET_DIE_UNLESS(&s, actual >= a.expected,
		               "procedure expects at least %zu argument(s), given %zu", a.expected, actual);
	}
}

template <typename F>
struct PrimTraits;

template <typename R, typename... A>
struct PrimTraits<R (*)(VmState&, A...)>
{
	static constexpr size_t arity = sizeof...(A);
};

template <auto fn>
JET_PRESERVE_NONE inline void prim_stub_varargs(VM_OP_PARAMS)
{
	JET_PROFILE_PRIM;
	Atom result = fn(s, args, stack_top);
	*args = result;
	stack_top = stack_base + frame->top;
	DISPATCH();
}

template <auto fn>
JET_PRESERVE_NONE inline void prim_stub_typed(VM_OP_PARAMS)
{
	JET_PROFILE_PRIM;
	using T = PrimTraits<decltype(fn)>;
	Atom result = [&]<size_t... Is>(std::index_sequence<Is...>)
	{
		return box(fn(s, args[Is] ...));
	}(std::make_index_sequence<T::arity>{});
	*args = result;
	stack_top = stack_base + frame->top;
	DISPATCH();
}

template <auto fn>
Atom make_prim(VmState& s, Arity arity)
{
	if constexpr (std::is_same_v<decltype(fn), Prim::Fun>)
	{
		return s.gc.alloc_tagged<Prim>(s, Prim{&prim_stub_varargs<fn>, arity});
	}
	else
	{
		return s.gc.alloc_tagged<Prim>(s, Prim{&prim_stub_typed<fn>, arity});
	}
}

template <auto fn>
Atom make_prim(VmState& s)
{
	if constexpr (std::is_same_v<decltype(fn), Prim::Fun>)
	{
		return make_prim<fn>(s, n_ary());
	}
	else
	{
		return make_prim<fn>(s, exactly(PrimTraits<decltype(fn)>::arity));
	}
}

inline bool is_exact(double x)
{
	return trunc(x) == x;
}

inline bool is_integer(double x)
{
	return is_exact(x);
}

inline bool is_nonnegative_integer(VmState& s, Atom num)
{
	double n = slow_unbox<Number>(s, num);
	return is_integer(n) && n >= 0;
}

inline bool is_byte(VmState& s, Atom a)
{
	return is_nonnegative_integer(s, a) && unbox<Number>(a) <= 255;
}

void init_number(VmState& s);

constexpr uint64_t FIELD_IC_NONE = ~static_cast<uint64_t>(0);

enum class FieldAccess : uint8_t
{
	Load,
	Store,
};

enum class FieldKeySource : uint8_t
{
	Register,
	Constant,
};

template <FieldAccess access, FieldKeySource key_source>
using FieldOp =
	std::conditional_t<access == FieldAccess::Store,
	                   std::conditional_t<key_source == FieldKeySource::Constant, OP_stfk, OP_stf>,
	                   std::conditional_t<key_source == FieldKeySource::Constant, OP_ldfk, OP_ldf>>;

enum class FieldMiss
{
	Die,
	Hole,
	Default,
};

template <FieldAccess access, FieldKeySource key_source, FieldMiss miss = FieldMiss::Die>
constexpr Opcode field_opcode =
	access == FieldAccess::Store
	? (key_source == FieldKeySource::Constant ? Opcode::stfk : Opcode::stf)
	: miss == FieldMiss::Hole ? (key_source == FieldKeySource::Constant ? Opcode::ldfkh : Opcode::ldfh)
	: miss == FieldMiss::Default ? (key_source == FieldKeySource::Constant ? Opcode::ldfko : Opcode::ldfo)
	: (key_source == FieldKeySource::Constant ? Opcode::ldfk : Opcode::ldf);

template <FieldMiss miss, FieldKeySource key_source>
using FieldLoadOp =
	std::conditional_t<miss == FieldMiss::Default,
	                   std::conditional_t<key_source == FieldKeySource::Constant, OP_ldfko, OP_ldfo>,
	                   std::conditional_t<key_source == FieldKeySource::Constant, OP_ldfk, OP_ldf>>;

template <FieldMiss miss, FieldKeySource key_source>
JET_ALWAYS_INLINE Atom field_miss_value(FieldLoadOp<miss, key_source>* op, Atom* frame_regs)
{
	if constexpr (miss == FieldMiss::Default)
	{
		return frame_regs[op->dfl];
	}
	else
	{
		return hole();
	}
}

template <FieldKeySource key_source, typename Op>
JET_ALWAYS_INLINE Atom field_key(VmState& s, const Op* op, Atom* frame_regs)
{
	if constexpr (key_source == FieldKeySource::Constant)
	{
		return s.constants[op->key_idx];
	}
	else
	{
		return frame_regs[op->key];
	}
}

template <FieldAccess access>
[[noreturn]] JET_NOINLINE inline void die_field_index(VmState& s, Atom key)
{
	const char* op = access == FieldAccess::Store ? "setf!" : "ref";
	if (!is_type<jet::Type::Number>(key))
	{
		JET_DIE(&s, "%s: expected a non-negative integer index", op);
	}
	double n = unbox<Number>(key);
	if (!is_integer(n) || n < 0)
	{
		JET_DIE(&s, "%s: expected a non-negative integer index", op);
	}
	JET_DIE(&s, "%s: index out of bounds", op);
}

template <FieldKeySource key_source>
JET_ALWAYS_INLINE bool index_of_key(size_t size, Atom key, FieldIc& ic, size_t& index)
{
	if constexpr (key_source == FieldKeySource::Constant)
	{
		if (ic.cached_index < size) [[likely]]
		{
			index = ic.cached_index;
			return true;
		}
		JET_PROFILE_FIELD_KEY_MISS();
	}
	if (!is_type<jet::Type::Number>(key)) [[unlikely]]
	{
		return false;
	}
	double n = unbox<Number>(key);
	if (!is_integer(n) || n < 0) [[unlikely]]
	{
		return false;
	}
	index = static_cast<size_t>(n);
	if (index >= size) [[unlikely]]
	{
		return false;
	}
	if constexpr (key_source == FieldKeySource::Constant)
	{
		ic.cached_index = index;
	}
	return true;
}

template <typename T>
JET_ALWAYS_INLINE Atom container_load(T& container, size_t index)
{
	if constexpr (std::is_same_v<T, String>)
	{
		return box(static_cast<Character>(static_cast<uint8_t>(container[index])));
	}
	else if constexpr (std::is_same_v<T, ByteVector>)
	{
		return box(Number::trusted(container[index]));
	}
	else
	{
		return container[index];
	}
}

template <typename T>
JET_ALWAYS_INLINE bool container_store(T& container, size_t index, Atom value)
{
	if constexpr (std::is_same_v<T, ByteVector>)
	{
		if (!is_type<jet::Type::Number>(value)) [[unlikely]]
		{
			return false;
		}
		double n = unbox<Number>(value);
		if (!is_integer(n) || n < 0 || n > 255) [[unlikely]]
		{
			return false;
		}
		container[index] = static_cast<uint8_t>(n);
	}
	else
	{
		container[index] = value;
	}
	return true;
}

template <typename T>
struct ContainerAccess
{
	static constexpr bool is_struct = false;
	static constexpr bool caches_keys = false;

	template <FieldKeySource key_source, typename Op>
	JET_ALWAYS_INLINE static bool load_fast(VmState& s, Op* op, Atom* frame_regs)
	{
		T& container = *unbox<T>(frame_regs[op->obj]);
		size_t index;
		Atom key = field_key<key_source>(s, op, frame_regs);
		if (!index_of_key<key_source>(container.size(), key, op->ic, index)) [[unlikely]]
		{
			return false;
		}
		frame_regs[op->dst] = container_load(container, index);
		return true;
	}

	static Atom load_or_hole(VmState& s, Atom object, Atom key)
	{
		T& container = *unbox<T>(object);
		if (!is_type<jet::Type::Number>(key))
		{
			die_field_index<FieldAccess::Load>(s, key);
		}
		double n = unbox<Number>(key);
		if (!is_integer(n) || n < 0)
		{
			die_field_index<FieldAccess::Load>(s, key);
		}
		size_t index{static_cast<size_t>(n)};
		if (index >= container.size())
		{
			return hole();
		}
		return container_load(container, index);
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_load_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Load,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Load, key_source>*>(pc);
		die_field_index<FieldAccess::Load>(s, field_key<key_source>(s, op, frame_regs));
	}

	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(VmState& s, FieldOp<FieldAccess::Store, key_source>* op,
	                                         Atom* frame_regs)
	{
		T& container = *unbox<T>(frame_regs[op->obj]);
		size_t index;
		Atom key = field_key<key_source>(s, op, frame_regs);
		if (!index_of_key<key_source>(container.size(), key, op->ic, index)) [[unlikely]]
		{
			return false;
		}
		return container_store(container, index, frame_regs[op->val]);
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		FieldOp<FieldAccess::Store,
		        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Store, key_source>*>(pc);
		T& container = *unbox<T>(frame_regs[op->obj]);
		size_t index;
		Atom key = field_key<key_source>(s, op, frame_regs);
		if (!index_of_key<key_source>(container.size(), key, op->ic, index))
		{
			die_field_index<FieldAccess::Store>(s, key);
		}
		JET_DIE(&s, "setf!: expected a byte value");
	}
};

struct StringAccess : ContainerAccess<String>
{
	template <FieldKeySource key_source>
	JET_ALWAYS_INLINE static bool store_fast(VmState&, FieldOp<FieldAccess::Store, key_source>*, Atom*)
	{
		return false;
	}

	template <FieldKeySource key_source>
	JET_NOINLINE JET_PRESERVE_NONE static void op_store_slow(VM_OP_PARAMS)
	{
		JET_DIE(&s, "setf!: strings are immutable");
	}
};

template <typename Access>
JET_ALWAYS_INLINE bool field_receiver_matches(Atom object, uint64_t dispatch_key)
{
	if constexpr (Access::is_struct)
	{
		return object.tag_is<jet_tag::struct_>() &&
		       std::bit_cast<uint64_t>(unbox<Struct>(object)->type) == dispatch_key;
	}
	else
	{
		return type_bits(object) == dispatch_key;
	}
}

template <FieldAccess access>
JET_NOINLINE JET_PRESERVE_NONE void die_field_receiver(VM_OP_PARAMS)
{
	JET_DIE(&s, "%s: unsupported receiver type", access == FieldAccess::Store ? "setf!" : "ref");
}

template <typename Access, FieldMiss miss, FieldKeySource key_source>
JET_NOINLINE JET_PRESERVE_NONE void op_field_load_miss(VM_OP_PARAMS)
{
	FieldLoadOp<miss, key_source>* op = reinterpret_cast<FieldLoadOp<miss, key_source>*>(pc);
	Atom value = Access::load_or_hole(s, frame_regs[op->obj], field_key<key_source>(s, op, frame_regs));
	frame_regs[op->dst] = is_hole(value) ? field_miss_value<miss, key_source>(op, frame_regs) : value;
	pc += sizeof(*op);
	DISPATCH();
}

template <FieldMiss miss, FieldKeySource key_source>
JET_NOINLINE JET_PRESERVE_NONE void op_field_hole_receiver(VM_OP_PARAMS)
{
	FieldLoadOp<miss, key_source>* op = reinterpret_cast<FieldLoadOp<miss, key_source>*>(pc);
	frame_regs[op->dst] = field_miss_value<miss, key_source>(op, frame_regs);
	pc += sizeof(*op);
	DISPATCH();
}

template <FieldAccess access, FieldKeySource key_source, FieldMiss miss = FieldMiss::Die>
JET_PRESERVE_NONE void op_field_impl(VM_OP_PARAMS)
{
	using Op = std::conditional_t<access == FieldAccess::Store, FieldOp<access, key_source>,
	                              FieldLoadOp<miss, key_source>>;
	Op* op = reinterpret_cast<Op*>(pc);
	Atom object = frame_regs[op->obj];
	if constexpr (access == FieldAccess::Load && miss != FieldMiss::Die)
	{
		if (is_hole(object)) [[unlikely]]
		{
			JET_MUSTTAIL return op_field_hole_receiver<miss, key_source>(VM_OP_ARGS);
		}
	}
	const ObjShape* shape = shape_of(object);
	VmOp handler = nullptr;
	if (shape)
	{
		if constexpr (access == FieldAccess::Store)
		{
			handler = key_source == FieldKeySource::Constant ? shape->stfk_handler : shape->stf_handler;
		}
		else if constexpr (miss == FieldMiss::Die)
		{
			handler = key_source == FieldKeySource::Constant ? shape->ldfk_handler : shape->ldf_handler;
		}
		else if constexpr (miss == FieldMiss::Hole)
		{
			handler = key_source == FieldKeySource::Constant ? shape->ldfkh_handler : shape->ldfh_handler;
		}
		else
		{
			handler = key_source == FieldKeySource::Constant ? shape->ldfko_handler : shape->ldfo_handler;
		}
	}
	if (!handler) [[unlikely]]
	{
		JET_MUSTTAIL return die_field_receiver<access>(VM_OP_ARGS);
	}
	JET_PROFILE_FIELD_DISPATCH((field_opcode<access, key_source>), profile_field_receiver(object), false);
	op->ic.dispatch_key = object.tag_is<jet_tag::struct_>()
	                      ? std::bit_cast<uint64_t>(unbox<Struct>(object)->type)
	                      : type_bits(object);
	op->ic.cached_index = FIELD_IC_NONE;
	op->ic.cached_key = FIELD_IC_NONE;
	std::memcpy(pc - OPCODE_SIZE, &handler, sizeof(handler));
	JET_MUSTTAIL return handler(VM_OP_ARGS);
}

template <typename Access, FieldKeySource key_source, FieldMiss miss = FieldMiss::Die>
JET_PRESERVE_NONE void op_field_load_fast(VM_OP_PARAMS)
{
	FieldLoadOp<miss, key_source>* op = reinterpret_cast<FieldLoadOp<miss, key_source>*>(pc);
	Atom object = frame_regs[op->obj];
	if (!field_receiver_matches<Access>(object, op->ic.dispatch_key)) [[unlikely]]
	{
		JET_MUSTTAIL return op_field_impl<FieldAccess::Load, key_source, miss>(VM_OP_ARGS);
	}
	JET_PROFILE_FIELD_DISPATCH((field_opcode<FieldAccess::Load, key_source, miss>),
	                           profile_field_receiver(object), true);
	if (!Access::template load_fast<key_source>(s, op, frame_regs)) [[unlikely]]
	{
		if constexpr (miss == FieldMiss::Die)
		{
			JET_MUSTTAIL return Access::template op_load_slow<key_source>(VM_OP_ARGS);
		}
		else if constexpr (Access::caches_keys)
		{
			JET_MUSTTAIL return Access::template op_load_miss<key_source, miss>(VM_OP_ARGS);
		}
		else
		{
			JET_MUSTTAIL return op_field_load_miss<Access, miss, key_source>(VM_OP_ARGS);
		}
	}
	pc += sizeof(*op);
	DISPATCH();
}

template <typename Access, FieldKeySource key_source>
JET_PRESERVE_NONE void op_field_store_fast(VM_OP_PARAMS)
{
	FieldOp<FieldAccess::Store,
	        key_source>* op = reinterpret_cast<FieldOp<FieldAccess::Store, key_source>*>(pc);
	Atom object = frame_regs[op->obj];
	if (!field_receiver_matches<Access>(object, op->ic.dispatch_key)) [[unlikely]]
	{
		JET_MUSTTAIL return op_field_impl<FieldAccess::Store, key_source>(VM_OP_ARGS);
	}
	JET_PROFILE_FIELD_DISPATCH((field_opcode<FieldAccess::Store, key_source>), profile_field_receiver(object),
	                           true);
	if (!Access::template store_fast<key_source>(s, op, frame_regs)) [[unlikely]]
	{
		JET_MUSTTAIL return Access::template op_store_slow<key_source>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	DISPATCH();
}

template <typename Access>
constexpr ObjShape make_field_shape(Atom (*ref_or_die)(VmState&, Atom, Atom),
                                    Cursor* (*iter)(VmState&, Atom))
{
	return {op_field_load_fast<Access, FieldKeySource::Register>,
	        op_field_store_fast<Access, FieldKeySource::Register>,
	        op_field_load_fast<Access, FieldKeySource::Constant>,
	        op_field_store_fast<Access, FieldKeySource::Constant>,
	        op_field_load_fast<Access, FieldKeySource::Register, FieldMiss::Hole>,
	        op_field_load_fast<Access, FieldKeySource::Constant, FieldMiss::Hole>,
	        op_field_load_fast<Access, FieldKeySource::Register, FieldMiss::Default>,
	        op_field_load_fast<Access, FieldKeySource::Constant, FieldMiss::Default>,
	        ref_or_die, Access::load_or_hole, iter};
}

template <typename op_t, Number (*to_number)(double)>
JET_ALWAYS_INLINE Atom fold(VmState& s, Atom* first, Atom* last, double result)
{
	while (first != last)
	{
		double next = slow_unbox<Number>(s, *first++);
		result = op_t()(s, result, next);
	}
	return box(to_number(result));
}

template <typename op_t, Number (*to_number)(double) = Number::from_ieee>
JET_ALWAYS_INLINE Atom folding_op(VmState& s, Atom* first, Atom* last)
{
	double result = slow_unbox<Number>(s, *first++);
	return fold<op_t, to_number>(s, first, last, result);
}

template <typename op_t, int init, Number (*to_number)(double) = Number::from_ieee>
JET_ALWAYS_INLINE Atom folding_op(VmState& s, Atom* first, Atom* last)
{
	double result = last - first < 2 ? static_cast<double>(init) : slow_unbox<Number>(s, *first++);
	return fold<op_t, to_number>(s, first, last, result);
}

template <typename op_t>
JET_ALWAYS_INLINE Atom folding_pred(VmState& s, Atom* first, Atom* last)
{
	bool result = true;
	double prev = slow_unbox<Number>(s, *first++);
	while (first != last)
	{
		double cur = slow_unbox<Number>(s, *first++);
		result = result && op_t()(prev, cur);
		prev = cur;
	}

	return box(result);
}

template <typename T, T (*op)()>
Atom arith_nullary_fun(VmState&, Atom*, Atom*)
{
	return box(Number::trusted(static_cast<double>(op())));
}

JET_ALWAYS_INLINE inline Number truncate_number(double value)
{
	uint64_t bits{std::bit_cast<uint64_t>(value)};
	uint64_t exponent{(bits >> 52) & 2047};

	if (exponent < 1023) [[unlikely]]
	{
		return Number::trusted(0.0);
	}
	if (exponent >= 1075) [[unlikely]]
	{
		return Number::trusted(value);
	}

	bits &= ~((uint64_t{1} << (1075 - exponent)) - 1);
	return Number::trusted(std::bit_cast<double>(bits));
}

template <typename T, T (*op)(T)>
Atom arith_unary_fun(VmState& s, Atom* first, Atom*)
{
	return box(Number::from_ieee(op(slow_unbox<Number>(s, *first))));
}

template <typename T, T (*op)(VmState&, T), Number (*to_number)(double) = Number::from_ieee>
Atom arith_unary_fun(VmState& s, Atom* first, Atom*)
{
	return box(to_number(op(s, slow_unbox<Number>(s, *first))));
}

template <typename T, bool (*op)(VmState&, T)>
Atom arith_unary_pred(VmState& s, Atom* first, Atom*)
{
	return box(op(s, slow_unbox<Number>(s, *first)));
}

template <typename T, T (*op)(T, T)>
Atom arith_binary_fun(VmState& s, Atom* first, Atom*)
{
	return box(Number::from_ieee(op(slow_unbox<Number>(s, first[0]), slow_unbox<Number>(s, first[1]))));
}

template <typename T, T (*op)(VmState&, T, T), Number (*to_number)(double) = Number::from_ieee>
Atom arith_binary_fun(VmState& s, Atom* first, Atom*)
{
	return box(to_number(op(s, slow_unbox<Number>(s, first[0]), slow_unbox<Number>(s, first[1]))));
}

template <typename T>
bool compare_objects(Atom obj1, Atom obj2)
{
	decltype(box_unbox_t<T>::unbox(obj1)) a = unbox<T>(obj1);
	decltype(box_unbox_t<T>::unbox(obj2)) b = unbox<T>(obj2);
	if constexpr (std::is_pointer_v<decltype(a)>)
	{
		return *a == *b;
	}
	else
	{
		return a == b;
	}
}

void init_equivalence(VmState& s);

Atom display(VmState& s, Atom* first, Atom* last);
Atom write_to(VmState& s, Atom value, std::string& out);
void init_display_primitives(VmState& s);

void init_strings(VmState& s);
void init_chars(VmState& s);

inline bool is_true(Atom a)
{
	return is_type<jet::Type::Boolean>(a) ? unbox<bool>(a) : true;
}

class Port
{
public:
	enum class Mode : uint8_t { Input, Output };

	Port(Mode m) : mode_{m} {}
	virtual void close() = 0;
	virtual ~Port() = default;

	bool is_input() const { return mode_ == Mode::Input; }
	bool is_output() const { return mode_ == Mode::Output; }

private:
	Mode mode_;
};

class IPort : public Port
{
public:
	IPort() : Port{Mode::Input} {}
	virtual char read_byte() = 0;
	virtual char peek_byte() = 0;
	virtual size_t read_bytes(char* p, size_t n) = 0;
	virtual bool eof() = 0;
};

class OPort : public Port
{
public:
	OPort() : Port{Mode::Output} {}
	virtual void write_bytes(const char* data, size_t size) = 0;

	void write_byte(char value)
	{
		write_bytes(&value, 1);
	}
};

void init_port(VmState& s);

Atom make_eof();

class IPortFile : public IPort
{
public:
	explicit IPortFile(FILE* file) : f_{file} {}
	~IPortFile() override;

	char read_byte() override;
	char peek_byte() override;
	size_t read_bytes(char* p, size_t n) override;

	void close() override;
	bool eof() override;

private:
	FILE* f_;
};

class IPortMem : public IPort
{
public:
	explicit IPortMem(std::string_view src) : src_{src} {}

	char read_byte() override;
	char peek_byte() override;
	size_t read_bytes(char* p, size_t n) override;

	void close() override {}
	bool eof() override { return pos_ >= src_.size(); }

private:
	std::string_view src_;
	size_t pos_ = 0;
};

class OPortFile : public OPort
{
public:
	explicit OPortFile(VmState& s, std::string_view name);
	~OPortFile() override;

	void write_bytes(const char* data, size_t size) override;
	void close() override;

private:
	FILE* f_;
};

Atom read_char(VmState& s, Atom p);
void init_port_file(VmState& s);

void init_runtime(VmState& s);
void init_cmdline(VmState& s, int argc, char* argv[]);

#endif

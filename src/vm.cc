// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "vm.h"

#include "error.h"
#include "platform.h"
#include "runtime.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <type_traits>
#include <utility>

using GcDestructor = void (*)(void*);

template <typename T>
static constexpr GcDestructor gc_destructor_of()
{
	if constexpr (std::is_same_v<T, Struct>)
	{
		return nullptr;
	}
	else
	{
		return gc_destroy<T>;
	}
}

static constexpr GcDestructor gc_destructor_table[jet_tag::TAG_MAX]{
	nullptr,
#define X(_enum, _name, _cpp) nullptr,
	JET_IMM_TYPES(X)
#undef X
	nullptr,
#define X(_name, _tag, cpp) gc_destructor_of<cpp>(),
	JET_HEAP_TYPES(X)
#undef X
};
static_assert(gc_destructor_table[jet_tag::pair] == gc_destroy<Cons>);
static_assert(gc_destructor_table[jet_tag::struct_] == nullptr);
static VmOp dispatch_table[256];

uint16_t Gc::register_struct_destructor(VmState& s, StructDestructor destructor)
{
	if (!destructor)
	{
		return 0;
	}
	for (size_t i = 1; i < struct_destructors.size(); ++i)
	{
		if (struct_destructors[i] == destructor)
		{
			return static_cast<uint16_t>(i);
		}
	}
	JET_DIE_WHEN(&s, struct_destructors.size() > UINT16_MAX, "too many native struct destructors");
	struct_destructors.push_back(destructor);
	return static_cast<uint16_t>(struct_destructors.size() - 1);
}

JET_ALWAYS_INLINE static void destroy_object(
	uint8_t tag, uint16_t destructor_id, void* object, const StructDestructor* struct_destructor_table)
{
	if (tag == jet_tag::struct_)
	{
		if (StructDestructor destructor = struct_destructor_table[destructor_id]; destructor)
		{
			destructor(static_cast<Struct*>(object));
		}
		return;
	}
	if (GcDestructor destructor = gc_destructor_table[tag]; destructor)
	{
		destructor(object);
	}
}

Gc::Gc()
{
	void* p = ::mmap(nullptr, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	JET_DIE_UNLESS(nullptr, p != MAP_FAILED, "gc: mmap %zu bytes failed", ARENA_SIZE);
	arena_base = static_cast<char*>(p);

	size_t bm_bytes = BITMAP_WORDS * sizeof(uint64_t);
	void* lb = ::mmap(nullptr, bm_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	JET_DIE_UNLESS(nullptr, lb != MAP_FAILED, "gc: mmap live_bits failed");
	live_bits = static_cast<uint64_t*>(lb);

	void* mb = ::mmap(nullptr, bm_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	JET_DIE_UNLESS(nullptr, mb != MAP_FAILED, "gc: mmap mark_bits failed");
	mark_bits = static_cast<uint64_t*>(mb);
}

Gc::~Gc()
{
	const StructDestructor* struct_destructor_table = struct_destructors.data();
	for (ObjEntry* e = objects; e != objects_end; ++e)
	{
		void* object = arena_base + static_cast<size_t>(e->cell_idx) * CELL_SIZE;
		destroy_object(e->tag, e->destructor_id, object, struct_destructor_table);
	}
	for (const auto& [object, entry] : huge)
	{
		destroy_object(entry.tag, entry.destructor_id, object, struct_destructor_table);
		std::free(object);
	}
	::free(objects);
	::munmap(arena_base, ARENA_SIZE);
	::munmap(live_bits, BITMAP_WORDS * sizeof(uint64_t));
	::munmap(mark_bits, BITMAP_WORDS * sizeof(uint64_t));
}

void* Gc::alloc_slow(VmState& s, size_t n, int tag, uint16_t destructor_id)
{
	if (n >= N_BUCKETS) [[unlikely]]
	{
		return alloc_huge(s, n, tag, destructor_id);
	}

	void* mem;
	uint32_t start;

	if (freelist[tag][n])
	{
		mem = freelist[tag][n];
		freelist[tag][n] = next_free(mem);
		start = static_cast<uint32_t>((static_cast<char*>(mem) - arena_base) / CELL_SIZE);
	}
	else
	{
		JET_DIE_UNLESS(&s, bump_cells + n <= TOTAL_CELLS, "gc: arena exhausted");
		start = static_cast<uint32_t>(bump_cells);
		bump_cells += n;
		mem = arena_base + static_cast<size_t>(start) * CELL_SIZE;
	}

	set_bits(live_bits, start, n);

	if (objects_end == objects_cap)
	{
		grow_objects(s);
	}
	*objects_end++ = {start, static_cast<uint32_t>(n), destructor_id, static_cast<uint8_t>(tag)};
	++alloc_since_gc;
	return mem;
}

void* Gc::alloc_huge(VmState& s, size_t n, int tag, uint16_t destructor_id)
{
	void* mem = checked_malloc(&s, n * CELL_SIZE);
	huge.emplace(mem, HugeEntry{static_cast<uint32_t>(n), destructor_id, static_cast<uint8_t>(tag), false});
	++alloc_since_gc;
	return mem;
}

void Gc::grow_objects(VmState& s)
{
	size_t size = objects_end - objects;
	size_t cap = objects_cap - objects;
	size_t next = cap ? 2 * cap : 1024;
	ObjEntry* fresh = static_cast<ObjEntry*>(std::realloc(objects, next * sizeof(ObjEntry)));
	JET_DIE_UNLESS(&s, fresh != nullptr, "gc: out of memory growing the object table");
	objects = fresh;
	objects_end = fresh + size;
	objects_cap = fresh + next;
}

void Gc::mark_atom(uint64_t bits)
{
	Atom a = Atom::from_bits(bits);
	if (!a.is_heap())
	{
		return;
	}

	size_t cell = (static_cast<char*>(a.as_ptr()) - arena_base) / CELL_SIZE;
	if (cell >= TOTAL_CELLS) [[unlikely]]
	{
		mark_huge(a.as_ptr());
		return;
	}
	if (!test_bit(live_bits, cell))
	{
		return;
	}
	if (test_bit(mark_bits, cell))
	{
		return;
	}

	set_bit(mark_bits, cell);
	mark_object(a.as_ptr(), a.tag());
}

// Traces by the recorded tag, never the referring Atom's: malloc can hand a freed huge
// object's address to one of another type, and a stale stack slot may still name it.
void Gc::mark_huge(void* ptr)
{
	auto entry = huge.find(ptr);
	if (entry == huge.end() || entry->second.marked)
	{
		return;
	}
	entry->second.marked = true;
	mark_object(ptr, entry->second.tag);
}

void Gc::mark_lambda(Lambda* la)
{
	for (uint16_t i = 0; i < la->n_captures; ++i)
	{
		mark_atom(la->captures[i].bits);
	}
}

void Gc::mark_object(void* ptr, int tag)
{
	switch (tag)
	{
		case jet_tag::pair:
		{
			Cons* c = static_cast<Cons*>(ptr);
			mark_atom(c->car.bits);
			mark_atom(c->cdr.bits);
			break;
		}
		case jet_tag::procedure:
		{
			mark_lambda(static_cast<Lambda*>(ptr));
			break;
		}
		case jet_tag::vector:
		{
			Vec* v = static_cast<Vec*>(ptr);
			for (Atom elem : *v)
			{
				mark_atom(elem.bits);
			}
			break;
		}
		case jet_tag::slot:
		{
			Slot* sl = static_cast<Slot*>(ptr);
			mark_atom(sl->value.bits);
			break;
		}
		case jet_tag::struct_:
		{
			Struct* s = static_cast<Struct*>(ptr);
			mark_atom(Atom::make_tagged(jet_tag::struct_type, s->type).bits);
			switch (s->type->kind())
			{
				case StructKind::Scheme:
					static_cast<SchemeStruct*>(s)->trace(*this);
					break;
				case StructKind::Tuple:
					static_cast<Tuple*>(s)->trace(*this);
					break;
				case StructKind::HashSet:
					static_cast<HashSet*>(s)->trace(*this);
					break;
				case StructKind::HashMap:
					static_cast<HashMap*>(s)->trace(*this);
					break;
				case StructKind::Cursor:
					static_cast<Cursor*>(s)->trace(*this);
					break;
				case StructKind::Escape:
					break;
				case StructKind::Coro:
					static_cast<Coro*>(s)->trace(*this);
					break;
				case StructKind::Yield:
					static_cast<Yield*>(s)->trace(*this);
					break;
			}
			break;
		}
		case jet_tag::struct_type:
			break;
		default:
			break;
	}
}

void Gc::sweep()
{
	++epoch;
	const StructDestructor* struct_destructor_table = struct_destructors.data();
	ObjEntry* out = objects;
	for (ObjEntry* e = objects; e != objects_end; ++e)
	{
		if (test_bit(mark_bits, e->cell_idx))
		{
			clear_bit(mark_bits, e->cell_idx);
			*out++ = *e;
		}
		else
		{
			void* obj = arena_base + static_cast<size_t>(e->cell_idx) * CELL_SIZE;
			destroy_object(e->tag, e->destructor_id, obj, struct_destructor_table);
			clear_bits(live_bits, e->cell_idx, e->n_cells);
			link_free(obj, freelist[e->tag][e->n_cells]);
			freelist[e->tag][e->n_cells] = obj;
		}
	}
	objects_end = out;

	for (auto entry = huge.begin(); entry != huge.end();)
	{
		if (entry->second.marked)
		{
			entry->second.marked = false;
			++entry;
			continue;
		}
		destroy_object(entry->second.tag, entry->second.destructor_id, entry->first,
		               struct_destructor_table);
		std::free(entry->first);
		entry = huge.erase(entry);
	}

	size_t next = HEAP_GROWTH_FACTOR * (objects_end - objects + huge.size());
	alloc_since_gc = 0;
	gc_threshold = next < MIN_GC_THRESHOLD ? MIN_GC_THRESHOLD : static_cast<uint32_t>(next);
}

void collect(VmState& s)
{
	JET_PROFILE_GC;
	JET_PROFILE_GC_TIMER;
	Gc& gc = s.gc;

	// Scan every frame-claimed slot, not just up to stack_top: enclosing frames
	// reach above the innermost extent, and marking their stale slots keeps the
	// referents alive so no slot below the frontier ever dangles.
	Atom* scan_frontier = s.stack_top;
	for (Frame& frame : s.frames)
	{
		if (s.stack_base + frame.top > scan_frontier)
		{
			scan_frontier = s.stack_base + frame.top;
		}
	}

	for (Atom* p = s.stack_base; p < scan_frontier; ++p)
	{
		gc.mark_atom(p->bits);
	}

	for (Frame& frame : s.frames)
	{
		if (frame.closure)
		{
			Atom proc_atom = Atom::make_tagged(jet_tag::procedure, frame.closure);
			gc.mark_atom(proc_atom.bits);
		}
	}

	for (size_t i = 0; i < s.n_constants; ++i)
	{
		gc.mark_atom(s.constants[i].bits);
	}

	s.env.scan([&gc](Atom& value) { gc.mark_atom(value.bits); });

	for (Coro* coro : s.running)
	{
		gc.mark_atom(Atom::make_tagged(jet_tag::struct_, coro).bits);
	}

	gc.sweep();

	std::memset(scan_frontier, 0, static_cast<size_t>(s.stack_watermark - scan_frontier) * sizeof(Atom));
	s.stack_watermark = scan_frontier;
}

const LambdaDebug::Line* LambdaDebug::find_line(size_t off) const
{
	return find_line(off, code_size);
}

const LambdaDebug::Line* LambdaDebug::find_line(size_t off, size_t code_size_limit) const
{
	if (lines.empty() || off >= code_size_limit)
	{
		return nullptr;
	}
	auto it = std::upper_bound(lines.begin(), lines.end(), off,
	                           [](size_t offset, const Line& line) { return offset < line.off; });
	return it == lines.begin() ? nullptr : &*--it;
}

const Code* frame_code_start(const VmState& s, const Frame& f, const Code* instruction)
{
	if (f.closure != nullptr)
	{
		return f.closure->code;
	}
	if (s.toplevel_code != nullptr && instruction >= s.toplevel_code
	    && instruction < s.toplevel_code + s.toplevel_code_size)
	{
		return s.toplevel_code;
	}
	return nullptr;
}

JET_COLD static void print_stack_frame(VmState& s, Frame& f)
{
	const Code* key = frame_code_start(s, f, f.code);
	if (key == nullptr)
	{
		return;
	}
	auto found = s.debug.code.find(key);
	if (found == s.debug.code.end())
	{
		return;
	}
	const LambdaDebug& debug = found->second;
	std::string_view name{debug.name};
	if (name.empty())
	{
		name = f.closure == nullptr ? "<toplevel>" : "<lambda>";
	}
	const LambdaDebug::Line* line = nullptr;
	if (f.code > key)
	{
		line = debug.find_line(static_cast<size_t>(f.code - 1 - key));
	}
	if (line == nullptr || line->file >= s.debug.files.size())
	{
		std::fprintf(stderr, "  %.*s\n", static_cast<int>(name.size()), name.data());
		return;
	}
	const std::string& file = s.debug.files[line->file];
	std::fprintf(stderr, "  %.*s at %.*s:%u\n", static_cast<int>(name.size()), name.data(),
	             static_cast<int>(file.size()), file.data(), line->line);
}

[[noreturn]] JET_NOINLINE void die_type_mismatch(VmState& s, Atom a, jet::Type t)
{
	std::string_view want = type_name(t);
	std::string_view got = type_name(a.type());
	JET_DIE(&s, "expected <%.*s>, got <%.*s>", static_cast<int>(want.size()), want.data(),
	        static_cast<int>(got.size()), got.data());
}

JET_COLD void print_stack_trace(VmState* vm)
{
	if (vm == nullptr)
	{
		return;
	}
	std::fputs("stack trace (innermost first):\n", stderr);
	for (size_t ci = vm->running.size(); ci-- > 0;)
	{
		// A running coroutine's frames live in the next inner coroutine's SavedStack:
		// switch_stack exchanges them, so outer frames are parked one level down.
		Stack<Frame>& frames = ci + 1 == vm->running.size() ? vm->frames : vm->running[ci + 1]->stack.frames;
		size_t i = frames.size();
		while (i-- > 0)
		{
			print_stack_frame(*vm, frames[i]);
		}
	}
}

Code* parse_debug_section(VmState* s, Code* p, Code* end, std::vector<std::string>& files,
                          std::vector<LambdaDebug>& source_maps)
{
	auto&& require = [&](size_t n_bytes_needed)
	{
		JET_DIE_UNLESS(s, static_cast<size_t>(end - p) >= n_bytes_needed, "invalid debug section");
	};
	uint32_t n_files;
	require(sizeof(n_files));
	memcpy(&n_files, p, sizeof(n_files));
	p += sizeof(n_files);
	files.reserve(std::min<uint32_t>(n_files, static_cast<uint32_t>(end - p)));
	for (uint32_t i = 0; i < n_files; ++i)
	{
		const void* nul = memchr(p, '\0', static_cast<size_t>(end - p));
		JET_DIE_UNLESS(s, nul != nullptr, "invalid debug section");
		const char* file = reinterpret_cast<const char*>(p);
		p = static_cast<Code*>(const_cast<void*>(nul)) + 1;
		files.emplace_back(file);
	}
	uint32_t n_source_maps;
	require(sizeof(n_source_maps));
	memcpy(&n_source_maps, p, sizeof(n_source_maps));
	p += sizeof(n_source_maps);
	source_maps.reserve(std::min<uint32_t>(n_source_maps, static_cast<uint32_t>(end - p)));
	for (uint32_t i = 0; i < n_source_maps; ++i)
	{
		uint32_t n_entries;
		require(sizeof(n_entries));
		memcpy(&n_entries, p, sizeof(n_entries));
		p += sizeof(n_entries);
		std::vector<LambdaDebug::Line> lines;
		lines.reserve(std::min<uint32_t>(n_entries,
		                                 static_cast<uint32_t>((end - p) / sizeof(LambdaDebug::Line))));
		for (uint32_t j = 0; j < n_entries; ++j)
		{
			LambdaDebug::Line line;
			require(sizeof(line));
			memcpy(&line, p, sizeof(line));
			p += sizeof(line);
			lines.push_back(line);
		}
		LambdaDebug entry;
		entry.lines = std::move(lines);
		source_maps.push_back(std::move(entry));
	}
	return p;
}

LoadedProgram load_program(VmState& s, Code* bytecode, size_t n_bytes)
{
	auto&& link_opcode_handlers = [](Code* begin, Code* end)
	{
		// The opcode tag stays in place so trace and profile can recover it.
		Code* code = begin;
		while (code < end)
		{
			uint8_t op = code[VM_OP_SLOT_SIZE];
			size_t step = opcode_step(op, code + OPCODE_SIZE);
			VmOp handler = dispatch_table[op];
			std::memcpy(code, &handler, sizeof(handler));
			code += step;
		}
	};
	LoadedProgram prog;
	Code* p = bytecode;
	Code* const end = bytecode + n_bytes;
	std::vector<LambdaDebug> source_maps;
	p = parse_debug_section(&s, p, end, s.debug.files, source_maps);
	JET_DIE_UNLESS(&s, static_cast<size_t>(end - p) >= sizeof(prog.n_toplevel_slots),
	               "invalid bytecode: not enough bytes for n_toplevel_slots (size = %zu, consumed = %zu)",
	               n_bytes, static_cast<size_t>(p - bytecode));
	memcpy(&prog.n_toplevel_slots, p, sizeof(prog.n_toplevel_slots));
	p += sizeof(prog.n_toplevel_slots);
	uint32_t n_constants;
	JET_DIE_UNLESS(&s, static_cast<size_t>(end - p) >= sizeof(n_constants),
	               "invalid bytecode: not enough bytes for n_constants (size = %zu, consumed = %zu)",
	               n_bytes, static_cast<size_t>(p - bytecode));
	memcpy(&n_constants, p, sizeof(n_constants));
	p += sizeof(n_constants);
	size_t next_source_map = 0;
	auto&& decode_constant = [&](Code* code, Atom& out) -> Code*
	{
		ConstTag tag = static_cast<ConstTag>(*code++);
		switch (tag)
		{
			case ConstTag::Number:
			{
				double n;
				memcpy(&n, code, sizeof(n));
				out = box(Number::from_ieee(n));
				return code + sizeof(n);
			}
			case ConstTag::Boolean:
			{
				bool value;
				memcpy(&value, code, sizeof(value));
				out = box(value);
				return code + sizeof(value);
			}
			case ConstTag::Character:
			{
				Character character;
				memcpy(&character, code, sizeof(character));
				out = box(character);
				return code + sizeof(character);
			}
			case ConstTag::String:
			{
				uint32_t n_string_bytes;
				memcpy(&n_string_bytes, code, sizeof(n_string_bytes));
				code += sizeof(n_string_bytes);
				out = s.gc.alloc_tagged<String>(s, reinterpret_cast<char*>(code), n_string_bytes);
				return code + n_string_bytes;
			}
			case ConstTag::Symbol:
			{
				char* value = reinterpret_cast<char*>(code);
				out = box(s.symbols.intern(value));
				return code + strlen(value) + 1;
			}
			case ConstTag::EmptyList:
				out = box(EmptyList{});
				return code;
			case ConstTag::Unknown:
				out = Atom{};
				return code;
			case ConstTag::GlobalName:
			{
				char* name = reinterpret_cast<char*>(code);
				Atom* atom = s.env.lookup(name);
				JET_DIE_UNLESS(&s, atom, "unknown primitive in pool: <%s>", name);
				out = *atom;
				return code + strlen(name) + 1;
			}
			case ConstTag::Lambda:
			{
				bool is_n_ary;
				memcpy(&is_n_ary, code, sizeof(is_n_ary));
				code += sizeof(is_n_ary);
				Arity arity = n_ary();
				if (!is_n_ary)
				{
					size_t n;
					memcpy(&n, code, sizeof(n));
					code += sizeof(n);
					arity = exactly(n);
				}
				uint16_t n_locals;
				memcpy(&n_locals, code, sizeof(n_locals));
				code += sizeof(n_locals);
				size_t code_size;
				memcpy(&code_size, code, sizeof(code_size));
				code += sizeof(code_size);
				Code* lambda_code = code;
				code += code_size;
				link_opcode_handlers(lambda_code, lambda_code + code_size);
				const char* lambda_name = reinterpret_cast<const char*>(code);
				code += strlen(lambda_name) + 1;
				JET_DIE_UNLESS(&s, next_source_map < source_maps.size(), "invalid debug section");
				LambdaDebug entry = std::move(source_maps[next_source_map]);
				++next_source_map;
				entry.name = lambda_name;
				entry.code_size = code_size;
				s.debug.code.emplace(lambda_code, std::move(entry));
				out = Lambda::alloc(s, lambda_code, arity, n_locals, static_cast<uint16_t>(0));
				return code;
			}
		}
		JET_DIE(&s, "unknown constant-pool tag <%d>", static_cast<int>(tag));
	};
	prog.constants.reserve(n_constants);
	for (uint32_t i = 0; i < n_constants; ++i)
	{
		Atom a;
		p = decode_constant(p, a);
		prog.constants.push_back(a);
	}
	Code* toplevel_code = p;
	JET_DIE_UNLESS(&s, next_source_map + 1 == source_maps.size(), "invalid debug section");
	LambdaDebug toplevel = std::move(source_maps[next_source_map]);
	toplevel.code_size = static_cast<size_t>(bytecode + n_bytes - toplevel_code);
	s.toplevel_code = toplevel_code;
	s.toplevel_code_size = toplevel.code_size;
	s.debug.code.emplace(toplevel_code, std::move(toplevel));
	link_opcode_handlers(p, bytecode + n_bytes);
	prog.code = p;
	JET_PROFILE_PREPARE(s, bytecode, n_bytes);
	return prog;
}

enum class CopyVariadic
{
	No,
	Yes,
};

template <size_t max_unrolled, CopyVariadic variadic>
JET_ALWAYS_INLINE static void copy_atoms(Atom* dst, const Atom* src, size_t count)
{
	[&]<size_t... counts>(std::index_sequence<counts...>)
	{
		if (((count == counts && (std::memmove(dst, src, counts * sizeof(Atom)), true)) || ...))
		{
			return;
		}
		if constexpr (variadic == CopyVariadic::Yes)
		{
			std::memmove(dst, src, count * sizeof(Atom));
		}
	}(std::make_index_sequence<max_unrolled + 1>{});
}

enum class CallTail
{
	No,
	Yes,
};

// Return addresses installed at coroutine stack bottoms (`retc`), at frames
// suspended by a tail-position yield (`retu`), and under a host call
// (`return_to_host`); none exists in compiled code.
static Code g_retc_code[OPCODE_SIZE];
static Code g_retu_code[OPCODE_SIZE];
static Code g_return_to_host_code[OPCODE_SIZE];

JET_NOINLINE static void grow_stack(VmState& s, Atom* needed_top)
{
	size_t needed = static_cast<size_t>(needed_top - s.stack_base) + STACK_SLACK;
	size_t capacity = static_cast<size_t>(s.stack_end - s.stack_base);
	size_t new_capacity = capacity;
	while (new_capacity < needed)
	{
		new_capacity *= 2;
	}

	std::unique_ptr<Atom[]> bigger{new Atom[new_capacity]};
	std::memcpy(bigger.get(), s.stack_base, capacity * sizeof(Atom));

	Atom* old_base = s.stack_base;
	s.stack_top = bigger.get() + (s.stack_top - old_base);
	s.stack_watermark = bigger.get() + (s.stack_watermark - old_base);
	s.stack_end = bigger.get() + new_capacity;
	s.stack_base = bigger.get();
	s.stack = std::move(bigger);
}

template <CallTail tail>
JET_NOINLINE JET_PRESERVE_NONE static void op_enter_lambda_slow(VM_OP_PARAMS)
{
	auto&& pack_args_to_list = [&s](Atom* first, Atom* last) -> Atom
	{
		Atom result = box(EmptyList{});
		while (first != last)
		{
			result = cons(s, *--last, result);
		}
		return result;
	};
	auto&& install_args = [&](Lambda& lambda, size_t base, Atom* call_args, size_t nargs) -> size_t
	{
		bool nary = is_nary(lambda.arity);
		size_t n_copy = nary ? lambda.arity.expected : nargs;
		Atom* dst = stack_base + base;
		copy_atoms<4, CopyVariadic::Yes>(dst, call_args, n_copy);
		if (nary) [[unlikely]]
		{
			dst[n_copy] = pack_args_to_list(call_args + n_copy, call_args + nargs);
			return n_copy + 1;
		}
		return n_copy;
	};
	Lambda& la = *unbox<Lambda>(callee);
	size_t nargs = static_cast<size_t>(stack_top - args);
	size_t base = tail == CallTail::Yes ? frame->base : static_cast<size_t>(args - stack_base);
	if constexpr (tail == CallTail::Yes)
	{
		install_args(la, base, args, nargs);
		frame->code = la.code;
		frame->closure = &la;
		frame->top = base + la.n_locals;
	}
	else
	{
		// Non-tail args were evaluated in place at base: only nary rest-packing
		// remains.
		if (is_nary(la.arity)) [[unlikely]]
		{
			size_t n_copy = la.arity.expected;
			stack_base[base + n_copy] = pack_args_to_list(args + n_copy, args + nargs);
		}
		frame = &s.frames.push();
		frame->code = la.code;
		frame->closure = &la;
		frame->base = base;
		frame->top = base + la.n_locals;
	}
	frame_regs = stack_base + base;
	stack_top = stack_base + base + la.n_locals;
	if (stack_top > s.stack_watermark) [[unlikely]]
	{
		if (stack_top > s.stack_end - STACK_SLACK) [[unlikely]]
		{
			grow_stack(s, stack_top);
			stack_base = s.stack_base;
			frame_regs = stack_base + base;
			stack_top = stack_base + base + la.n_locals;
		}
		s.stack_watermark = stack_top;
	}
	pc = la.code;
	DISPATCH();
}

template <CallTail tail, class Ic = void>
JET_ALWAYS_INLINE JET_PRESERVE_NONE static void op_enter_lambda_fast(VM_OP_PARAMS)
{
	JET_PROFILE_LAMBDA;
	Lambda& la = *unbox<Lambda>(callee);
	Code* code;
	size_t n_locals;
	if constexpr (std::is_void_v<Ic>)
	{
		size_t nargs = static_cast<size_t>(stack_top - args);
		if (is_nary(la.arity) || (tail == CallTail::Yes && nargs > 16)) [[unlikely]]
		{
			JET_MUSTTAIL return op_enter_lambda_slow<tail>(VM_OP_ARGS);
		}
		code = la.code;
		n_locals = la.n_locals;
	}
	else
	{
		const Ic* op = reinterpret_cast<const Ic*>(pc - sizeof(Ic));
		uint64_t code_bits = op->ic_code;
		code = std::bit_cast<Code*>(code_bits);
		n_locals = op->ic_n_locals;
	}
	size_t base = tail == CallTail::Yes ? frame->base : static_cast<size_t>(args - stack_base);
	if (stack_base + base + n_locals > s.stack_watermark) [[unlikely]]
	{
		JET_MUSTTAIL return op_enter_lambda_slow<tail>(VM_OP_ARGS);
	}
	Atom* dst = stack_base + base;

	if constexpr (tail == CallTail::Yes)
	{
		copy_atoms<16, CopyVariadic::No>(dst, args, static_cast<size_t>(stack_top - args));
		frame->code = code;
		frame->closure = &la;
		frame->top = base + n_locals;
	}
	else
	{
		if (!s.frames.can_push()) [[unlikely]]
		{
			JET_MUSTTAIL return op_enter_lambda_slow<CallTail::No>(VM_OP_ARGS);
		}
		frame = &s.frames.push_unchecked();
		frame->code = code;
		frame->closure = &la;
		frame->base = base;
		frame->top = base + n_locals;
	}
	frame_regs = stack_base + base;

	stack_top = stack_base + base + n_locals;
	pc = code;
	DISPATCH();
}

inline Arity struct_arity(StructType* t)
{
	return t->arity();
}

JET_NOINLINE JET_PRESERVE_NONE static void op_enter_escape(VM_OP_PARAMS)
{
	Escape* escape = static_cast<Escape*>(unbox<Struct>(callee));
	JET_DIE_UNLESS(&s, escape->host_token == s.host_token, "escape crossed a host entry boundary");
	Atom value = args[0];

	Coro* owner = escape->owner;
	JET_DIE_UNLESS(&s, owner->running_index < s.running.size() && s.running[owner->running_index] == owner,
	               "escape used outside the extent of its let/ec");
	size_t after_owner = owner->running_index + 1;
	if (s.running.size() > after_owner)
	{
		// The let/ec frame is on an outer stack. The first coroutine the owner resumed
		// holds the owner's fields; switch to them. Each removed coroutine keeps the
		// storage it holds, fails the membership check forever, and dies on resume.
		Coro* holder = s.running[after_owner];
		s.stack_top = stack_top;
		s.switch_stack(holder->stack);
		s.running.truncate(after_owner);
		stack_base = s.stack_base;
		stack_top = s.stack_top;
	}

	JET_DIE_UNLESS(&s, escape->n_frames <= s.frames.size()
	               && s.frames.begin()[escape->n_frames - 1].code == escape->retk_code,
	               "escape used outside the extent of its let/ec");
	s.frames.truncate(escape->n_frames);
	frame = &s.frames.back();
	frame->code = escape->resume_pc;
	frame_regs = stack_base + frame->base;
	stack_top = stack_base + frame->top;
	frame_regs[escape->dst] = value;
	pc = escape->resume_pc;
	DISPATCH();
}

template <CallTail tail>
JET_NOINLINE JET_PRESERVE_NONE static void op_enter_yield(VM_OP_PARAMS)
{
	Yield* yield = static_cast<Yield*>(unbox<Struct>(callee));
	Coro* coro = static_cast<Coro*>(yield->target);
	JET_DIE_UNLESS(&s, s.running.back() == coro, "yield used outside its coroutine");
	Atom value = args[0];

	s.running.pop();
	coro->state = CoroState::Suspended;
	if constexpr (tail == CallTail::Yes)
	{
		// A tail-position yield has no code after the call; resume returns unknown
		// from the suspended frame instead.
		frame->code = g_retu_code;
	}
	else
	{
		// The yield expression's value: written now, read only after resume.
		args[0] = Atom{};
	}

	s.stack_top = stack_top;
	s.switch_stack(coro->stack);
	stack_base = s.stack_base;
	frame = &s.frames.back();
	frame_regs = stack_base + frame->base;
	stack_top = stack_base + frame->top;
	frame_regs[coro->dst] = value;
	pc = coro->consequent_pc;
	DISPATCH();
}

JET_NOINLINE JET_PRESERVE_NONE static void die_not_callable(VM_OP_PARAMS)
{
	std::string_view name = type_name(callee.type());
	JET_DIE(&s, "cannot call <%.*s>", static_cast<int>(name.size()), name.data());
}

template <CallTail tail>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_slow(VM_OP_PARAMS)
{
	if (is_type<jet::Type::Procedure>(callee))
	{
		Lambda* la = unbox<Lambda>(callee);
		check_arity(s, la->arity, stack_top - args);
		JET_MUSTTAIL return op_enter_lambda_fast<tail>(VM_OP_ARGS);
	}
	if (is_type<jet::Type::Primitive>(callee))
	{
		Prim* p = unbox<Prim>(callee);
		check_arity(s, p->arity, stack_top - args);
		JET_MUSTTAIL return p->stub(VM_OP_ARGS);
	}
	if (is_type<jet::Type::StructType>(callee))
	{
		StructType* t = unbox<StructType>(callee);
		Arity a = struct_arity(t);
		check_arity(s, a, static_cast<size_t>(stack_top - args));
		JET_MUSTTAIL return t->ops().constructor(VM_OP_ARGS);
	}
	if (!is_type<jet::Type::Struct>(callee)) [[unlikely]]
	{
		JET_MUSTTAIL return die_not_callable(VM_OP_ARGS);
	}
	StructKind kind = unbox<Struct>(callee)->type->kind();
	if (kind == StructKind::Escape)
	{
		check_arity(s, exactly(1), static_cast<size_t>(stack_top - args));
		JET_MUSTTAIL return op_enter_escape(VM_OP_ARGS);
	}
	if (kind == StructKind::Yield)
	{
		check_arity(s, exactly(1), static_cast<size_t>(stack_top - args));
		JET_MUSTTAIL return op_enter_yield<tail>(VM_OP_ARGS);
	}
	JET_MUSTTAIL return die_not_callable(VM_OP_ARGS);
}

JET_NOINLINE JET_PRESERVE_NONE static void op_gc_slow(VM_OP_PARAMS)
{
	s.stack_top = stack_top;
	collect(s);
	VmOp h = decode_op(pc - OPCODE_SIZE);
	JET_MUSTTAIL return h(VM_OP_ARGS);
}

ObjShape g_shape_by_tag[jet_tag::HEAP_END] = {};

JET_NOINLINE JET_PRESERVE_NONE static void die_iter_exhausted(VM_OP_PARAMS)
{
	JET_DIE(&s, "%%if/next!: cursor is exhausted");
}

JET_NOINLINE JET_PRESERVE_NONE static void die_iter_expected_cursor(VM_OP_PARAMS)
{
	JET_DIE(&s, "%%if/next!: expected a cursor");
}

template <int outputs>
JET_NOINLINE JET_PRESERVE_NONE static void die_iter_bad_outputs(VM_OP_PARAMS)
{
	JET_DIE(&s, "%%if/next!: cursor does not supply %d outputs", outputs);
}

enum class IterResult
{
	Yielded,
	Exhausted,
	Invalid,
};

template <typename Op, int outputs>
JET_PRESERVE_NONE static void op_iter_next_coro(VM_OP_PARAMS);

template <typename Op, int outputs>
JET_PRESERVE_NONE static void op_iter_impl(VM_OP_PARAMS)
{
	Op* op = reinterpret_cast<Op*>(pc);
	Atom value = frame_regs[op->cursor];
	if (!value.tag_is<jet_tag::struct_>()) [[unlikely]]
	{
		JET_MUSTTAIL return die_iter_expected_cursor(VM_OP_ARGS);
	}
	if (unbox<Struct>(value)->type->kind() == StructKind::Coro)
	{
		// The coroutine `StructType` is held by the permanent `%coroutine` `Env` binding.
		uint64_t type_key = std::bit_cast<uint64_t>(unbox<Struct>(value)->type);
		JET_PROFILE_MISS(op->ic.dispatch_key, type_key);
		op->ic.dispatch_key = type_key;
		VmOp coro_handler = op_iter_next_coro<Op, outputs>;
		std::memcpy(pc - OPCODE_SIZE, &coro_handler, sizeof(coro_handler));
		JET_MUSTTAIL return op_iter_next_coro<Op, outputs>(VM_OP_ARGS);
	}
	if (unbox<Struct>(value)->type->kind() != StructKind::Cursor) [[unlikely]]
	{
		JET_MUSTTAIL return die_iter_expected_cursor(VM_OP_ARGS);
	}
	Cursor* cursor = static_cast<Cursor*>(unbox<Struct>(value));
	JET_PROFILE_MISS(op->ic.dispatch_key, std::bit_cast<uint64_t>(cursor->type));
	VmOp handler = outputs == 1 ? cursor->ops->next1 : cursor->ops->next2;
	if (!handler) [[unlikely]]
	{
		JET_MUSTTAIL return die_iter_bad_outputs<outputs>(VM_OP_ARGS);
	}
	// Each cursor StructType is held by a permanent Env binding (%vector-cursor,
	// %hashset-cursor, %hashmap-cursor), so the type pointer outlives every cursor
	// and cannot be recycled behind this key.
	op->ic.dispatch_key = std::bit_cast<uint64_t>(cursor->type);
	std::memcpy(pc - OPCODE_SIZE, &handler, sizeof(handler));
	JET_MUSTTAIL return handler(VM_OP_ARGS);
}

// Detaching unlinks the cursor from the container's parallel cursor arrays, which is
// not call-free; it runs once per loop, so it stays out of the per-element handler.
template <typename Access>
JET_NOINLINE JET_PRESERVE_NONE static void op_iter_next_slow(VM_OP_PARAMS)
{
	typename Access::Op* op = reinterpret_cast<typename Access::Op*>(pc);
	static_cast<typename Access::CursorType*>(unbox<Struct>(frame_regs[op->cursor]))->detach();
	pc += sizeof(*op) + op->size;
	DISPATCH();
}

template <typename Access>
JET_PRESERVE_NONE static void op_iter_next_fast(VM_OP_PARAMS)
{
	using Op = typename Access::Op;
	Op* op = reinterpret_cast<Op*>(pc);
	Atom value = frame_regs[op->cursor];
	if (!value.tag_is<jet_tag::struct_>()
	    || op->ic.dispatch_key != std::bit_cast<uint64_t>(unbox<Struct>(value)->type)) [[unlikely]]
	{
		JET_MUSTTAIL return op_iter_impl<Op, Access::outputs>(VM_OP_ARGS);
	}
	typename Access::CursorType* cursor = static_cast<typename Access::CursorType*>(unbox<Struct>(value));
	IterResult result = Access::next_fast(op, frame_regs, cursor);
	if (result == IterResult::Yielded) [[likely]]
	{
		pc += sizeof(*op);
		DISPATCH();
	}
	if (result == IterResult::Exhausted)
	{
		JET_MUSTTAIL return op_iter_next_slow<Access>(VM_OP_ARGS);
	}
	JET_MUSTTAIL return Access::die(VM_OP_ARGS);
}

JET_NOINLINE JET_PRESERVE_NONE static void die_iter_vector_index(VM_OP_PARAMS)
{
	OP_iter_next1* op = reinterpret_cast<OP_iter_next1*>(pc);
	VectorCursor* cursor = static_cast<VectorCursor*>(unbox<Struct>(frame_regs[op->cursor]));
	if (!cursor->vector)
	{
		JET_DIE(&s, "%%if/next!: cursor is exhausted");
	}
	JET_DIE(&s, "%%if/next!: vector cursor index %zu exceeds size %zu",
	        cursor->vector->cursor_indices[cursor->slot], cursor->vector->size());
}

struct VectorCursorAccess
{
	using Op = OP_iter_next1;
	using CursorType = VectorCursor;
	static constexpr int outputs = 1;
	static constexpr VmOp die = die_iter_vector_index;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, VectorCursor* cursor)
	{
		if (!cursor->vector) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		Vec& vector = *cursor->vector;
		size_t& cursor_index = vector.cursor_indices[cursor->slot];
		size_t index = cursor_index;
		size_t size = vector.size();
		if (index >= size) [[unlikely]]
		{
			return index == size ? IterResult::Exhausted : IterResult::Invalid;
		}
		cursor_index = index + 1;
		frame_regs[op->dst] = vector.data()[index];
		return IterResult::Yielded;
	}
};

struct VectorCursorAccess2
{
	using Op = OP_iter_next2;
	using CursorType = VectorCursor;
	static constexpr int outputs = 2;
	static constexpr VmOp die = die_iter_vector_index;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, VectorCursor* cursor)
	{
		if (!cursor->vector) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		Vec& vector = *cursor->vector;
		size_t& cursor_index = vector.cursor_indices[cursor->slot];
		size_t index = cursor_index;
		size_t size = vector.size();
		if (index >= size) [[unlikely]]
		{
			return index == size ? IterResult::Exhausted : IterResult::Invalid;
		}
		cursor_index = index + 1;
		frame_regs[op->dst0] = Atom::from_double(static_cast<double>(index));
		frame_regs[op->dst1] = vector.data()[index];
		return IterResult::Yielded;
	}
};

static const CursorOps vector_cursor_ops = {
	op_iter_next_fast<VectorCursorAccess>,
	op_iter_next_fast<VectorCursorAccess2>,
};

static Cursor* make_vector_cursor(VmState& s, Atom target)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::StructType>(VectorCursor::type_atom),
	               "vector cursor type is not initialized");
	StructType* type = unbox<StructType>(VectorCursor::type_atom);
	void* mem = s.gc.alloc(s, sizeof(VectorCursor), jet_tag::struct_, type->destructor_id());
	return new (mem) VectorCursor{type, target, &vector_cursor_ops};
}

struct HashSetCursorAccess
{
	using Op = OP_iter_next1;
	using CursorType = HashSetCursor;
	static constexpr int outputs = 1;
	static constexpr VmOp die = die_iter_exhausted;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, HashSetCursor* cursor)
	{
		if (!cursor->table) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		HashSet& set = *cursor->table;
		size_t& cursor_position = set.cursor_positions[cursor->slot];
		size_t position = set.next_live(cursor_position);
		if (position >= set.last) [[unlikely]]
		{
			return IterResult::Exhausted;
		}
		cursor_position = position + 1;
		frame_regs[op->dst] = set.entries[position - set.first].atom;
		return IterResult::Yielded;
	}
};

struct HashSetCursorAccess2
{
	using Op = OP_iter_next2;
	using CursorType = HashSetCursor;
	static constexpr int outputs = 2;
	static constexpr VmOp die = die_iter_exhausted;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, HashSetCursor* cursor)
	{
		if (!cursor->table) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		HashSet& set = *cursor->table;
		size_t& cursor_position = set.cursor_positions[cursor->slot];
		size_t position = set.next_live(cursor_position);
		if (position >= set.last) [[unlikely]]
		{
			return IterResult::Exhausted;
		}
		cursor_position = position + 1;
		Atom element = set.entries[position - set.first].atom;
		frame_regs[op->dst0] = element;
		frame_regs[op->dst1] = element;
		return IterResult::Yielded;
	}
};

static const CursorOps hashset_cursor_ops = {
	op_iter_next_fast<HashSetCursorAccess>,
	op_iter_next_fast<HashSetCursorAccess2>,
};

Cursor* make_hashset_cursor(VmState& s, Atom target)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::StructType>(HashSetCursor::type_atom),
	               "hashset cursor type is not initialized");
	StructType* type = unbox<StructType>(HashSetCursor::type_atom);
	void* mem = s.gc.alloc(s, sizeof(HashSetCursor), jet_tag::struct_, type->destructor_id());
	return new (mem) HashSetCursor{type, target, &hashset_cursor_ops};
}

struct HashMapCursorAccess
{
	using Op = OP_iter_next2;
	using CursorType = HashMapCursor;
	static constexpr int outputs = 2;
	static constexpr VmOp die = die_iter_exhausted;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, HashMapCursor* cursor)
	{
		if (!cursor->table) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		HashMap& map = *cursor->table;
		size_t& cursor_position = map.cursor_positions[cursor->slot];
		size_t position = map.next_live(cursor_position);
		if (position >= map.last) [[unlikely]]
		{
			return IterResult::Exhausted;
		}
		const HashMapEntry& entry = map.entries[position - map.first];
		cursor_position = position + 1;
		frame_regs[op->dst0] = entry.key.atom;
		frame_regs[op->dst1] = entry.value;
		return IterResult::Yielded;
	}
};

struct HashMapCursorAccess1
{
	using Op = OP_iter_next1;
	using CursorType = HashMapCursor;
	static constexpr int outputs = 1;
	static constexpr VmOp die = die_iter_exhausted;

	JET_ALWAYS_INLINE static IterResult next_fast(Op* op, Atom* frame_regs, HashMapCursor* cursor)
	{
		if (!cursor->table) [[unlikely]]
		{
			return IterResult::Invalid;
		}
		HashMap& map = *cursor->table;
		size_t& cursor_position = map.cursor_positions[cursor->slot];
		size_t position = map.next_live(cursor_position);
		if (position >= map.last) [[unlikely]]
		{
			return IterResult::Exhausted;
		}
		const HashMapEntry& entry = map.entries[position - map.first];
		cursor_position = position + 1;
		frame_regs[op->dst] = entry.value;
		return IterResult::Yielded;
	}
};

static const CursorOps hashmap_cursor_ops = {
	op_iter_next_fast<HashMapCursorAccess1>,
	op_iter_next_fast<HashMapCursorAccess>,
};

Cursor* make_hashmap_cursor(VmState& s, Atom target)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::StructType>(HashMapCursor::type_atom),
	               "hashmap cursor type is not initialized");
	StructType* type = unbox<StructType>(HashMapCursor::type_atom);
	void* mem = s.gc.alloc(s, sizeof(HashMapCursor), jet_tag::struct_, type->destructor_id());
	return new (mem) HashMapCursor{type, target, &hashmap_cursor_ops};
}

namespace
{
	struct shape_table_init_t
	{
		shape_table_init_t()
		{
			g_shape_by_tag[jet_tag::vector] =
				make_field_shape<ContainerAccess<Vec>>(vector_ref, make_vector_cursor);
			g_shape_by_tag[jet_tag::string] = make_field_shape<StringAccess>(string_ref, nullptr);
			g_shape_by_tag[jet_tag::bytevector] =
				make_field_shape<ContainerAccess<ByteVector>>(bytevector_u8_ref, nullptr);
		}
	} shape_table_init;
} // namespace

static constexpr auto& op_ldf = op_field_impl<FieldAccess::Load, FieldKeySource::Register>;
static constexpr auto& op_stf = op_field_impl<FieldAccess::Store, FieldKeySource::Register>;
static constexpr auto& op_ldfk = op_field_impl<FieldAccess::Load, FieldKeySource::Constant>;
static constexpr auto& op_stfk = op_field_impl<FieldAccess::Store, FieldKeySource::Constant>;
static constexpr auto& op_ldfh = op_field_impl<FieldAccess::Load, FieldKeySource::Register, FieldMiss::Hole>;
static constexpr auto& op_ldfkh = op_field_impl<FieldAccess::Load, FieldKeySource::Constant, FieldMiss::Hole>;
static constexpr auto& op_ldfo = op_field_impl<FieldAccess::Load, FieldKeySource::Register,
                                               FieldMiss::Default>;
static constexpr auto& op_ldfko = op_field_impl<FieldAccess::Load, FieldKeySource::Constant,
                                                FieldMiss::Default>;

template <typename Op, int outputs>
JET_NOINLINE JET_PRESERVE_NONE static void op_iter_coro_slow(VM_OP_PARAMS)
{
	Op* op = reinterpret_cast<Op*>(pc);
	Coro* coro = static_cast<Coro*>(unbox<Struct>(frame_regs[op->cursor]));
	switch (coro->state)
	{
		case CoroState::Completed:
			coro->state = CoroState::Dead;
			pc += sizeof(*op) + op->size;
			DISPATCH();
		case CoroState::Running:
			if (coro->running_index < s.running.size() && s.running[coro->running_index] == coro)
			{
				JET_DIE(&s, "%%if/next!: coroutine is already running");
			}
			// Abandoned by a crossing escape; membership can never hold again.
			coro->state = CoroState::Dead;
			[[fallthrough]];
		case CoroState::Dead:
			JET_MUSTTAIL return die_iter_exhausted(VM_OP_ARGS);
		case CoroState::Suspended:
			// Reached when the fast path found `running` full; grow off the fast path.
			if (!s.running.can_push())
			{
				s.running.grow();
			}
			JET_MUSTTAIL return op_iter_next_coro<Op, outputs>(VM_OP_ARGS);
	}
	JET_DIE(&s, "unreachable coroutine state");
}

template <typename Op, int outputs>
JET_PRESERVE_NONE static void op_iter_next_coro(VM_OP_PARAMS)
{
	Op* op = reinterpret_cast<Op*>(pc);
	Atom value = frame_regs[op->cursor];
	if (!value.tag_is<jet_tag::struct_>()
	    || op->ic.dispatch_key != std::bit_cast<uint64_t>(unbox<Struct>(value)->type)) [[unlikely]]
	{
		JET_MUSTTAIL return op_iter_impl<Op, outputs>(VM_OP_ARGS);
	}
	Coro* coro = static_cast<Coro*>(unbox<Struct>(value));
	if (coro->state != CoroState::Suspended || !s.running.can_push()) [[unlikely]]
	{
		JET_MUSTTAIL return op_iter_coro_slow<Op, outputs>(VM_OP_ARGS);
	}

	coro->state = CoroState::Running;
	coro->running_index = static_cast<uint32_t>(s.running.size());
	s.running.push_unchecked() = coro;
	if constexpr (outputs == 2)
	{
		frame_regs[op->dst0] = Atom{};
		coro->dst = op->dst1;
	}
	else
	{
		coro->dst = op->dst;
	}
	Code* next = pc + sizeof(*op);
	coro->consequent_pc = next;
	frame->code = next;

	s.stack_top = stack_top;
	s.switch_stack(coro->stack);
	stack_base = s.stack_base;
	frame = &s.frames.back();
	frame_regs = stack_base + frame->base;
	stack_top = stack_base + frame->top;
	pc = frame->code;
	DISPATCH();
}

static constexpr auto& op_iter_next1 = op_iter_impl<OP_iter_next1, 1>;
static constexpr auto& op_iter_next2 = op_iter_impl<OP_iter_next2, 2>;

JET_ALWAYS_INLINE static Atom sub_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "-: expected numbers");
	return box(Number::from_sum(unbox<Number>(a) - unbox<Number>(b)));
}
JET_ALWAYS_INLINE static Atom add_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "+: expected numbers");
	return box(Number::from_sum(unbox<Number>(a) + unbox<Number>(b)));
}
JET_ALWAYS_INLINE static Atom mul_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "*: expected numbers");
	return box(Number::from_ieee(unbox<Number>(a) * unbox<Number>(b)));
}
JET_ALWAYS_INLINE static Atom div_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "/: expected numbers");
	return box(Number::from_ieee(unbox<Number>(a) / unbox<Number>(b)));
}
JET_ALWAYS_INLINE static Atom numeq_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "=: expected numbers");
	return box(a.bits == b.bits);
}
JET_ALWAYS_INLINE static Atom eq_atoms(VmState& s, Atom a, Atom b) { return box(is_eq(a, b)); }
JET_ALWAYS_INLINE static Atom lt_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), "<: expected numbers");
	return box(unbox<Number>(a) < unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom le_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b),
	               "<=: expected numbers");
	return box(unbox<Number>(a) <= unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom gt_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b), ">: expected numbers");
	return box(unbox<Number>(a) > unbox<Number>(b));
}
JET_ALWAYS_INLINE static Atom ge_atoms(VmState& s, Atom a, Atom b)
{
	JET_DIE_UNLESS(&s, is_type<jet::Type::Number>(a) && is_type<jet::Type::Number>(b),
	               ">=: expected numbers");
	return box(unbox<Number>(a) >= unbox<Number>(b));
}

JET_NOINLINE static VmOp resolve_callee(VmState& s, Atom callee, size_t nargs, CallTail tail)
{
	if (is_type<jet::Type::Procedure>(callee))
	{
		Lambda* la = unbox<Lambda>(callee);
		check_arity(s, la->arity, nargs);
		return tail == CallTail::Yes ? &op_enter_lambda_fast<CallTail::Yes>
		       : &op_enter_lambda_fast<CallTail::No>;
	}
	if (is_type<jet::Type::Primitive>(callee))
	{
		Prim* p = unbox<Prim>(callee);
		check_arity(s, p->arity, nargs);
		return p->stub;
	}
	if (is_type<jet::Type::StructType>(callee))
	{
		StructType* t = unbox<StructType>(callee);
		Arity a = struct_arity(t);
		check_arity(s, a, nargs);
		return t->ops().constructor;
	}
	if (!is_type<jet::Type::Struct>(callee)) [[unlikely]]
	{
		return &die_not_callable;
	}
	StructKind kind = unbox<Struct>(callee)->type->kind();
	if (kind == StructKind::Escape)
	{
		check_arity(s, exactly(1), nargs);
		return &op_enter_escape;
	}
	if (kind == StructKind::Yield)
	{
		check_arity(s, exactly(1), nargs);
		return tail == CallTail::Yes ? &op_enter_yield<CallTail::Yes> : &op_enter_yield<CallTail::No>;
	}
	return &die_not_callable;
}

JET_PRESERVE_NONE static void op_mov(VM_OP_PARAMS)
{
	OP_mov* op = reinterpret_cast<OP_mov*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = frame_regs[op->src];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_mov2(VM_OP_PARAMS)
{
	OP_mov2* op = reinterpret_cast<OP_mov2*>(pc);
	pc += sizeof(*op);
	frame_regs[op->first.dst] = frame_regs[op->first.src];
	frame_regs[op->second.dst] = frame_regs[op->second.src];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldk(VM_OP_PARAMS)
{
	OP_ldk* op = reinterpret_cast<OP_ldk*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = s.constants[op->idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldu(VM_OP_PARAMS)
{
	OP_ldu* op = reinterpret_cast<OP_ldu*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = frame->closure->captures[op->idx];
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldus(VM_OP_PARAMS)
{
	OP_ldus* op = reinterpret_cast<OP_ldus*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
	frame_regs[op->dst] = sl->value;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_stu(VM_OP_PARAMS)
{
	OP_stu* op = reinterpret_cast<OP_stu*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->idx]);
	sl->value = frame_regs[op->src];
	sl->version = next_slot_version();
	DISPATCH();
}

JET_PRESERVE_NONE static void op_ldd(VM_OP_PARAMS)
{
	OP_ldd* op = reinterpret_cast<OP_ldd*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame_regs[op->idx]);
	frame_regs[op->dst] = sl->value;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_std(VM_OP_PARAMS)
{
	OP_std* op = reinterpret_cast<OP_std*>(pc);
	pc += sizeof(*op);
	Slot* sl = unbox<Slot>(frame_regs[op->idx]);
	sl->value = frame_regs[op->src];
	sl->version = next_slot_version();
	DISPATCH();
}

JET_PRESERVE_NONE static void op_box(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_box* op = reinterpret_cast<OP_box*>(pc);
	pc += sizeof(*op);
	Atom prev = frame_regs[op->reg];
	frame_regs[op->reg] = s.gc.alloc_tagged<Slot>(s, prev);
	DISPATCH();
}

JET_PRESERVE_NONE static void op_clos(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_clos* op = reinterpret_cast<OP_clos*>(pc);
	pc += sizeof(*op);

	Lambda& tmpl = *unbox<Lambda>(s.constants[op->pool_idx]);
	Atom la_atom = Lambda::alloc(s, tmpl.code, tmpl.arity, tmpl.n_locals, op->n_captures);
	Lambda* la = unbox<Lambda>(la_atom);
	for (uint16_t i = 0; i < op->n_captures; ++i)
	{
		OP_make_closure_capture* cap = reinterpret_cast<OP_make_closure_capture*>(pc);
		pc += sizeof(*cap);
		CaptureSource src = static_cast<CaptureSource>(cap->src);
		la->captures[i] = src == CaptureSource::Local
		                  ? frame_regs[cap->idx]
		                  : frame->closure->captures[cap->idx];
	}
	frame_regs[op->dst] = la_atom;
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_binop_rr_impl(VM_OP_PARAMS)
{
	OP_binop_rr* op = reinterpret_cast<OP_binop_rr*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = Op(s, frame_regs[op->a], frame_regs[op->b]);
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_binop_rk_impl(VM_OP_PARAMS)
{
	OP_binop_rk* op = reinterpret_cast<OP_binop_rk*>(pc);
	pc += sizeof(*op);
	frame_regs[op->dst] = Op(s, frame_regs[op->a], s.constants[op->b]);
	DISPATCH();
}

static constexpr auto& op_add  = op_binop_rr_impl<add_atoms>;
static constexpr auto& op_sub  = op_binop_rr_impl<sub_atoms>;
static constexpr auto& op_mul  = op_binop_rr_impl<mul_atoms>;
static constexpr auto& op_div  = op_binop_rr_impl<div_atoms>;
static constexpr auto& op_numeq   = op_binop_rr_impl<numeq_atoms>;
static constexpr auto& op_eq      = op_binop_rr_impl<eq_atoms>;
static constexpr auto& op_lt   = op_binop_rr_impl<lt_atoms>;
static constexpr auto& op_le   = op_binop_rr_impl<le_atoms>;
static constexpr auto& op_gt   = op_binop_rr_impl<gt_atoms>;
static constexpr auto& op_ge   = op_binop_rr_impl<ge_atoms>;
static constexpr auto& op_addk = op_binop_rk_impl<add_atoms>;
static constexpr auto& op_subk = op_binop_rk_impl<sub_atoms>;
static constexpr auto& op_mulk = op_binop_rk_impl<mul_atoms>;
static constexpr auto& op_divk = op_binop_rk_impl<div_atoms>;
static constexpr auto& op_numeqk  = op_binop_rk_impl<numeq_atoms>;
static constexpr auto& op_eqk     = op_binop_rk_impl<eq_atoms>;
static constexpr auto& op_ltk  = op_binop_rk_impl<lt_atoms>;

JET_PRESERVE_NONE static void op_if_false(VM_OP_PARAMS)
{
	OP_if_false* op = reinterpret_cast<OP_if_false*>(pc);
	pc += sizeof(*op);
	if (!is_true(frame_regs[op->src]))
	{
		pc += op->size;
	}
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_if_cmp_rr_impl(VM_OP_PARAMS)
{
	OP_if_cmp* op = reinterpret_cast<OP_if_cmp*>(pc);
	pc += sizeof(*op);
	if (!is_true(Op(s, frame_regs[op->a], frame_regs[op->b])))
	{
		pc += op->size;
	}
	DISPATCH();
}

template<auto Op>
JET_PRESERVE_NONE static void op_if_cmp_rk_impl(VM_OP_PARAMS)
{
	OP_if_cmp* op = reinterpret_cast<OP_if_cmp*>(pc);
	pc += sizeof(*op);
	if (!is_true(Op(s, frame_regs[op->a], s.constants[op->b])))
	{
		pc += op->size;
	}
	DISPATCH();
}

static constexpr auto& op_if_numeq  = op_if_cmp_rr_impl<numeq_atoms>;
static constexpr auto& op_if_eq     = op_if_cmp_rr_impl<eq_atoms>;
static constexpr auto& op_if_lt  = op_if_cmp_rr_impl<lt_atoms>;
static constexpr auto& op_if_le  = op_if_cmp_rr_impl<le_atoms>;
static constexpr auto& op_if_gt  = op_if_cmp_rr_impl<gt_atoms>;
static constexpr auto& op_if_ge  = op_if_cmp_rr_impl<ge_atoms>;
static constexpr auto& op_if_numeqk = op_if_cmp_rk_impl<numeq_atoms>;
static constexpr auto& op_if_eqk    = op_if_cmp_rk_impl<eq_atoms>;
static constexpr auto& op_if_ltk = op_if_cmp_rk_impl<lt_atoms>;

JET_PRESERVE_NONE static void op_retv(VM_OP_PARAMS)
{
	OP_retv* op = reinterpret_cast<OP_retv*>(pc);
	Atom retval = frame_regs[op->src];
	Frame* prev = frame - 1;
	s.frames.pop();
	frame_regs[0] = retval;
	frame = prev;
	frame_regs = stack_base + prev->base;
	stack_top = stack_base + prev->top;
	pc = prev->code;
	DISPATCH();
}

void vm_exit(VmState& vm, int status)
{
#ifndef JET_PROFILE
	vm.~VmState();
#endif
	std::exit(status);
}

JET_PRESERVE_NONE static void op_halt(VM_OP_PARAMS)
{
	s.stack_top = stack_top;
	vm_exit(s, 0);
}

JET_PRESERVE_NONE static void op_skip(VM_OP_PARAMS)
{
	OP_skip* op = reinterpret_cast<OP_skip*>(pc);
	pc += sizeof(*op);
	pc += op->size;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_unknown(VM_OP_PARAMS)
{
	JET_DIE(&s, "unknown opcode 0x%02x. it could be anything", pc[-1]);
}

JET_PRESERVE_NONE static void op_label(VM_OP_PARAMS)
{
	JET_DIE(&s, "label pseudo-op reached the VM; LIR emit failed to strip it");
}

#define JET_CALL_WINDOW(w_, nargs_)                                                                          \
	do                                                                                                       \
	{                                                                                                        \
		args = frame_regs + (w_);                                                                            \
		stack_top = args + (nargs_);                                                                         \
		frame->code = pc;                                                                                    \
	} while (0)

template <CallTail tail>
JET_PRESERVE_NONE static void op_call_impl(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call* op = reinterpret_cast<OP_call*>(pc);
	pc += sizeof(*op);
	callee = frame_regs[op->callee];
	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return op_call_slow<tail>(VM_OP_ARGS);
}

static constexpr auto& op_call = op_call_impl<CallTail::No>;
static constexpr auto& op_tcall = op_call_impl<CallTail::Yes>;

JET_PRESERVE_NONE static void op_reset(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_reset* op = reinterpret_cast<OP_reset*>(pc);
	pc += sizeof(*op);
	JET_DIE_UNLESS(&s, is_type<jet::Type::StructType>(Escape::type_atom), "escape type is not initialized");
	StructType* type = unbox<StructType>(Escape::type_atom);
	void* mem = s.gc.alloc(s, sizeof(Escape), jet_tag::struct_, type->destructor_id());
	uint32_t n_frames = static_cast<uint32_t>(s.frames.size());
	uint16_t result_reg = static_cast<uint16_t>(op->w + 1);
	Escape* escape = new (mem) Escape{type, pc, s.running.back(), s.host_token, n_frames, result_reg};
	VmOp retk = dispatch_table[static_cast<int>(Opcode::retk)];
	Struct* operand = escape;
	std::memcpy(escape->retk_code, &retk, sizeof(retk));
	escape->retk_code[VM_OP_SLOT_SIZE] = static_cast<uint8_t>(Opcode::retk);
	std::memcpy(escape->retk_code + OPCODE_SIZE, &operand, sizeof(operand));

	Atom* window = frame_regs + op->w;
	callee = window[1];
	Atom escape_atom = Atom::make_tagged(jet_tag::struct_, escape);
	// window[0] is the escape's only root: the body's frame starts at window[1].
	window[0] = escape_atom;
	window[1] = escape_atom;
	args = window + 1;
	stack_top = args + 1;
	frame->code = escape->retk_code;
	JET_MUSTTAIL return op_call_slow<CallTail::No>(VM_OP_ARGS);
}

constexpr size_t CORO_STACK_CAPACITY = 2 * STACK_SLACK;

JET_PRESERVE_NONE static void op_coro(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_coro* op = reinterpret_cast<OP_coro*>(pc);
	pc += sizeof(*op);
	JET_DIE_UNLESS(&s, is_type<jet::Type::StructType>(Coro::type_atom), "coroutine type is not initialized");

	Atom body = frame_regs[op->w + 1];
	JET_DIE_UNLESS(&s, is_type<jet::Type::Procedure>(body), "let/coro body is not a lambda");
	Lambda* la = unbox<Lambda>(body);
	JET_DIE_WHEN(&s, is_nary(la->arity) || la->arity.expected != 1,
	             "let/coro body must take exactly the yield");

	StructType* coro_type = unbox<StructType>(Coro::type_atom);
	void* coro_mem = s.gc.alloc(s, sizeof(Coro), jet_tag::struct_, coro_type->destructor_id());
	Coro* coro = new (coro_mem) Coro{coro_type};
	Atom coro_atom = Atom::make_tagged(jet_tag::struct_, coro);
	// `window[0]` roots the coroutine across the yield allocation; `window[1]` is the result.
	frame_regs[op->w] = coro_atom;
	frame_regs[op->w + 1] = coro_atom;

	StructType* yield_type = unbox<StructType>(Yield::type_atom);
	void* yield_mem = s.gc.alloc(s, sizeof(Yield), jet_tag::struct_, yield_type->destructor_id());
	Yield* yield = new (yield_mem) Yield{yield_type, coro};

	size_t capacity = CORO_STACK_CAPACITY;
	while (capacity < la->n_locals + STACK_SLACK)
	{
		capacity *= 2;
	}
	SavedStack& stack = coro->stack;
	stack.storage.reset(new Atom[capacity]);
	stack.base = stack.storage.get();
	stack.end = stack.base + capacity;
	stack.base[0] = Atom::make_tagged(jet_tag::struct_, yield);
	stack.top = stack.base + la->n_locals;
	stack.watermark = stack.top;
	stack.frames.push({g_retc_code, nullptr, 0, 1});
	stack.frames.push({la->code, la, 0, la->n_locals});
	DISPATCH();
}

JET_PRESERVE_NONE static void op_retc(VM_OP_PARAMS)
{
	Coro* coro = s.running.back();
	Atom value = frame_regs[0];
	s.running.pop();
	coro->state = CoroState::Completed;

	s.take_stack(coro->stack);
	stack_base = s.stack_base;
	frame = &s.frames.back();
	frame_regs = stack_base + frame->base;
	stack_top = stack_base + frame->top;
	frame_regs[coro->dst] = value;
	pc = coro->consequent_pc;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_return_to_host(VM_OP_PARAMS)
{
	s.frames.pop();
	// Return through the musttail dispatch chain to jet_enter_vm.
}

JET_PRESERVE_NONE static void op_retu(VM_OP_PARAMS)
{
	Frame* prev = frame - 1;
	s.frames.pop();
	frame_regs[0] = Atom{};
	frame = prev;
	frame_regs = stack_base + prev->base;
	stack_top = stack_base + prev->top;
	pc = prev->code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_retk(VM_OP_PARAMS)
{
	Struct* operand = nullptr;
	std::memcpy(&operand, pc, sizeof(operand));
	Escape* escape = static_cast<Escape*>(operand);
	frame->code = escape->resume_pc;
	pc = escape->resume_pc;
	DISPATCH();
}

JET_NOINLINE JET_PRESERVE_NONE static void op_call_self_tail_slow(VM_OP_PARAMS)
{
	OP_call_self_tail* op = reinterpret_cast<OP_call_self_tail*>(pc);
	Lambda& la = *frame->closure;
	Atom* dst = frame_regs;
	Atom* src = frame_regs + op->w;
	size_t nargs = op->nargs;
	copy_atoms<4, CopyVariadic::Yes>(dst, src, nargs);
	frame->code = pc + sizeof(*op);
	pc = la.code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_call_self_tail(VM_OP_PARAMS)
{
	if (OP_call_self_tail* op = reinterpret_cast<OP_call_self_tail*>(pc); op->w != 0)
	{
		JET_MUSTTAIL return op_call_self_tail_slow(VM_OP_ARGS);
	}
	frame->code = pc + sizeof(OP_call_self_tail);
	pc = frame->closure->code;
	DISPATCH();
}

JET_PRESERVE_NONE static void op_apply(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_apply* op = reinterpret_cast<OP_apply*>(pc);
	pc += sizeof(*op);
	callee = frame_regs[op->w];
	Atom args_list = frame_regs[op->w + 1];
	args = frame_regs + op->w;
	// The bound must hold before any slot past the window is written; improper lists
	// die in `cdr` during the count.
	size_t nargs = 0;
	for (Atom x = args_list; !is_type<jet::Type::EmptyList>(x); x = cdr(s, x))
	{
		++nargs;
	}
	if (args + nargs > s.stack_watermark) [[unlikely]]
	{
		if (args + nargs > s.stack_end - STACK_SLACK) [[unlikely]]
		{
			size_t args_offset = static_cast<size_t>(args - stack_base);
			grow_stack(s, args + nargs);
			stack_base = s.stack_base;
			frame_regs = stack_base + frame->base;
			args = stack_base + args_offset;
		}
		s.stack_watermark = args + nargs;
	}
	stack_top = list_to_args(s, args_list, args);
	frame->code = pc;
	JET_MUSTTAIL return op_call_slow<CallTail::No>(VM_OP_ARGS);
}

// Lambda is installed only for callees whose entry cannot allocate, so the GC check
// belongs to Stub alone.
enum class CalleeKind
{
	Lambda,
	Stub
};

template <CallTail tail, class Ic>
static bool cache_lambda_entry(VmState& s, Atom callee, Ic* op)
{
	if (!is_type<jet::Type::Procedure>(callee))
	{
		return false;
	}
	Lambda* la = unbox<Lambda>(callee);
	if (is_nary(la->arity) || (tail == CallTail::Yes && op->nargs > 16))
	{
		return false;
	}
	op->ic_code = std::bit_cast<uint64_t>(la->code);
	op->ic_n_locals = la->n_locals;
	op->ic_epoch = s.gc.epoch;
	return true;
}

template <int N, CallTail tail, CalleeKind kind = CalleeKind::Stub>
JET_PRESERVE_NONE static void op_call_slot_impl(VM_OP_PARAMS);

template <int N, CallTail tail>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_slot_slow(VM_OP_PARAMS)
{
	OP_call_slot* op = reinterpret_cast<OP_call_slot*>(pc);
	Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	callee = sl->value;
	JET_PROFILE_CALL_MISS(op->ic_atom, callee);
	frame->code = pc + sizeof(*op);
	VmOp stub = resolve_callee(s, callee, op->nargs, tail);
	op->ic_slot = std::bit_cast<uint64_t>(sl);
	op->ic_atom = callee.bits;
	op->ic_version = sl->version;
	VmOp fast = op_call_slot_impl<N, tail, CalleeKind::Lambda>;
	if (!cache_lambda_entry<tail>(s, callee, op))
	{
		op->ic_stub = std::bit_cast<uint64_t>(stub);
		fast = op_call_slot_impl<N, tail, CalleeKind::Stub>;
	}
	std::memcpy(reinterpret_cast<Code*>(op) - OPCODE_SIZE, &fast, sizeof(fast));
	JET_MUSTTAIL return fast(VM_OP_ARGS);
}

template <int N, CallTail tail, CalleeKind kind>
JET_PRESERVE_NONE static void op_call_slot_impl(VM_OP_PARAMS)
{
	if constexpr (CalleeKind::Stub == kind)
	{
		JET_GC_CHECK();
	}
	OP_call_slot* op = reinterpret_cast<OP_call_slot*>(pc);
	if (Slot* sl = unbox<Slot>(frame->closure->captures[op->upvalue_idx]);
	    op->ic_slot != std::bit_cast<uint64_t>(sl) || op->ic_version != sl->version
	    || (CalleeKind::Lambda == kind && op->ic_epoch != s.gc.epoch)) [[unlikely]]
	{
		JET_MUSTTAIL return op_call_slot_slow<N, tail>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = Atom::from_bits(op->ic_atom);
	JET_CALL_WINDOW(op->w, op->nargs);
	if constexpr (CalleeKind::Lambda == kind)
	{
		JET_MUSTTAIL return op_enter_lambda_fast<tail, OP_call_slot>(VM_OP_ARGS);
	}
	else
	{
		uint64_t stub_bits = op->ic_stub;
		JET_MUSTTAIL return std::bit_cast<VmOp>(stub_bits)(VM_OP_ARGS);
	}
}

enum class CalleeSource
{
	Local,
	Upval
};

template <int N, CallTail tail, CalleeSource source, CalleeKind kind = CalleeKind::Stub>
JET_PRESERVE_NONE static void op_call_atom_impl(VM_OP_PARAMS);

template <int N, CallTail tail, CalleeSource source>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_atom_slow(VM_OP_PARAMS)
{
	OP_call_atom* op = reinterpret_cast<OP_call_atom*>(pc);
	if constexpr (source == CalleeSource::Local)
	{
		callee = frame_regs[op->idx];
	}
	else
	{
		callee = frame->closure->captures[op->idx];
	}
	JET_PROFILE_CALL_MISS(op->ic_atom, callee);
	frame->code = pc + sizeof(*op);
	VmOp stub = resolve_callee(s, callee, op->nargs, tail);
	op->ic_atom = callee.bits;
	VmOp fast = op_call_atom_impl<N, tail, source, CalleeKind::Lambda>;
	if (!cache_lambda_entry<tail>(s, callee, op))
	{
		op->ic_stub = std::bit_cast<uint64_t>(stub);
		fast = op_call_atom_impl<N, tail, source, CalleeKind::Stub>;
	}
	std::memcpy(reinterpret_cast<Code*>(op) - OPCODE_SIZE, &fast, sizeof(fast));
	JET_MUSTTAIL return fast(VM_OP_ARGS);
}

template <int N, CallTail tail, CalleeSource source, CalleeKind kind>
JET_PRESERVE_NONE static void op_call_atom_impl(VM_OP_PARAMS)
{
	if constexpr (CalleeKind::Stub == kind)
	{
		JET_GC_CHECK();
	}
	OP_call_atom* op = reinterpret_cast<OP_call_atom*>(pc);
	Atom current{};
	if constexpr (source == CalleeSource::Local)
	{
		current = frame_regs[op->idx];
	}
	else
	{
		current = frame->closure->captures[op->idx];
	}
	if (op->ic_atom != current.bits
	    || (CalleeKind::Lambda == kind && op->ic_epoch != s.gc.epoch)) [[unlikely]]
	{
		JET_MUSTTAIL return op_call_atom_slow<N, tail, source>(VM_OP_ARGS);
	}
	pc += sizeof(*op);
	callee = current;
	JET_CALL_WINDOW(op->w, op->nargs);
	if constexpr (CalleeKind::Lambda == kind)
	{
		JET_MUSTTAIL return op_enter_lambda_fast<tail, OP_call_atom>(VM_OP_ARGS);
	}
	else
	{
		uint64_t stub_bits = op->ic_stub;
		JET_MUSTTAIL return std::bit_cast<VmOp>(stub_bits)(VM_OP_ARGS);
	}
}

JET_PRESERVE_NONE static void op_call_self_fast(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	OP_call_self* op = reinterpret_cast<OP_call_self*>(pc);
	pc += sizeof(*op);
	callee = Atom::make_tagged(jet_tag::procedure, frame->closure);
	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return op_enter_lambda_fast<CallTail::No>(VM_OP_ARGS);
}

template <int N>
JET_NOINLINE JET_PRESERVE_NONE static void op_call_self_impl(VM_OP_PARAMS)
{
	JET_GC_CHECK();
	JET_PROFILE_MISS(0, 0);
	OP_call_self* op = reinterpret_cast<OP_call_self*>(pc);
	frame->code = pc + sizeof(*op);
	check_arity(s, frame->closure->arity, op->nargs);
	VmOp fast = op_call_self_fast;
	std::memcpy(pc - OPCODE_SIZE, &fast, sizeof(fast));
	pc += sizeof(*op);
	callee = Atom::make_tagged(jet_tag::procedure, frame->closure);
	JET_CALL_WINDOW(op->w, op->nargs);
	JET_MUSTTAIL return op_enter_lambda_fast<CallTail::No>(VM_OP_ARGS);
}

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_slot_impl<n, CallTail::No>;
JET_REPLICATE(X, call_upval_slot, "cus")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_slot_impl<n, CallTail::Yes>;
JET_REPLICATE(X, call_upval_slot_tail, "cust")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_atom_impl<n, CallTail::No, CalleeSource::Local>;
JET_REPLICATE(X, call_local, "cl")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_atom_impl<n, CallTail::Yes, CalleeSource::Local>;
JET_REPLICATE(X, call_local_tail, "clt")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_atom_impl<n, CallTail::No, CalleeSource::Upval>;
JET_REPLICATE(X, call_upval, "cu")
#undef X

#define X(name, disp, n)                                                                                     \
	static constexpr auto& op_##name = op_call_atom_impl<n, CallTail::Yes, CalleeSource::Upval>;
JET_REPLICATE(X, call_upval_tail, "cut")
#undef X

#define X(name, disp, n) static constexpr auto& op_##name = op_call_self_impl<n>;
JET_REPLICATE(X, call_self, "cself")
#undef X

[[noreturn]] void eval(VmState& vm, Frame& init_frame, Atom* constants, size_t n_constants,
                       size_t initial_stack_size)
{
	JET_DIE_WHEN(&vm, initial_stack_size > STACK_CAPACITY - STACK_SLACK,
	             "stack overflow: %zu toplevel slots", initial_stack_size);

	vm.stack_base = vm.stack.get();
	vm.stack_end = vm.stack.get() + STACK_CAPACITY;
	vm.stack_top = vm.stack.get() + initial_stack_size;
	vm.stack_watermark = vm.stack_top;
	vm.constants = constants;
	vm.n_constants = n_constants;

	VmOp halt_handler = dispatch_table[static_cast<int>(Opcode::halt)];
	std::memcpy(vm.halt_code, &halt_handler, sizeof(halt_handler));
	vm.halt_code[VM_OP_SLOT_SIZE] = static_cast<uint8_t>(Opcode::halt);
	vm.frames.push({vm.halt_code, nullptr, 0, initial_stack_size});
	vm.frames.push(init_frame);

	JET_DIE_UNLESS(&vm, is_type<jet::Type::StructType>(Coro::type_atom), "coroutine type is not initialized");
	StructType* coro_type = unbox<StructType>(Coro::type_atom);
	void* coro_mem = vm.gc.alloc(vm, sizeof(Coro), jet_tag::struct_, coro_type->destructor_id());
	Coro* main_coro = new (coro_mem) Coro{coro_type};
	main_coro->state = CoroState::Running;
	main_coro->running_index = 0;
	vm.running.push(main_coro);

	JET_PROFILE_BEGIN(vm);
	Frame* frame = &vm.frames.back();
	Code* pc = frame->code;
	Atom* stack_top = vm.stack_top;
	VmOp h = decode_op(pc);
	pc += OPCODE_SIZE;
	JET_PROFILE_OP(pc - OPCODE_SIZE);
	JET_TRACE_STEP(vm, frame, pc, stack_top);
	h(vm, frame, pc, stack_top, Atom{}, nullptr, vm.stack_base, vm.stack_base + frame->base);

	JET_DIE(&vm, "vm: halt returned to eval");
}

Atom jet_enter_vm(VmState& vm, Atom proc, Atom* args, size_t n_args)
{
	JET_PROFILE_HOST;
	Lambda* la = slow_unbox<Lambda>(vm, proc);
	check_arity(vm, la->arity, n_args);

	size_t base = static_cast<size_t>(vm.stack_top - vm.stack_base);
	JET_DIE_WHEN(&vm, vm.stack_top + n_args > vm.stack_end - STACK_SLACK, "jet_enter_vm: stack overflow");

	Atom* window = vm.stack_base + base;
	if (n_args != 0)
	{
		copy_atoms<4, CopyVariadic::Yes>(window, args, n_args);
	}
	vm.stack_top = window + n_args;

	uint64_t previous_host_token = vm.host_token;
	JET_DIE_WHEN(&vm, ++vm.next_host_token == 0, "jet_enter_vm: host token overflow");
	vm.host_token = vm.next_host_token;

	Frame& host_frame = vm.frames.push({g_return_to_host_code, nullptr, base, base + n_args});
	op_enter_lambda_fast<CallTail::No>(vm, &host_frame, g_return_to_host_code, window + n_args, proc, window,
	                                   vm.stack_base, window);

	Atom result = vm.stack_base[base];
	vm.stack_top = vm.stack_base + base;
	vm.host_token = previous_host_token;
	return result;
}

namespace
{
	struct dispatch_init_t
	{
		dispatch_init_t()
		{
			VmOp init[] = {
#define X(name, disp, ...) op_##name,
				JET_OPCODES(X)
#undef X
			};
			constexpr size_t n_init = sizeof(init) / sizeof(init[0]);
			for (size_t i = 0; i < n_init; ++i)
			{
				dispatch_table[i] = init[i];
			}
			for (size_t i = n_init; i < 256; ++i)
			{
				dispatch_table[i] = op_unknown;
			}
			auto&& build_return_code = [](Code* buffer, Opcode opcode)
			{
				VmOp handler = dispatch_table[static_cast<int>(opcode)];
				std::memcpy(buffer, &handler, sizeof(handler));
				buffer[VM_OP_SLOT_SIZE] = static_cast<uint8_t>(opcode);
			};
			build_return_code(g_retc_code, Opcode::retc);
			build_return_code(g_retu_code, Opcode::retu);
			build_return_code(g_return_to_host_code, Opcode::return_to_host);
		}
	} dispatch_init;
} // namespace

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef opcodes_h
#define opcodes_h

#include "error.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

struct Struct;

#define JET_REPLICATE_N 4
#define JET_REPLICATE(X, name, disp)                                                                        \
	X(name##_0, disp "_0", 0) X(name##_1, disp "_1", 1)                                                     \
	X(name##_2, disp "_2", 2) X(name##_3, disp "_3", 3)

// X(c++_name, "disasm_name") -- short, RISC-ish display strings keep traces
// scannable; C++ identifiers stay descriptive for source readability.
#define JET_OPCODES(X)                                                                                      \
	X(halt,                "halt")                                                                           \
	X(skip,                "b")                                                                              \
	X(label,               "label")                                                                          \
	X(mov,                 "mov")                                                                            \
	X(mov2,                "mov2")                                                                           \
	X(ldk,                 "ldk")                                                                            \
	X(ldu,                 "ldu")                                                                            \
	X(ldus,                "ldus")                                                                           \
	X(stu,                 "stu")                                                                            \
	X(ldd,                 "ldd")                                                                            \
	X(std,                 "std")                                                                            \
	X(box,                 "box")                                                                            \
	X(clos,                "clos")                                                                           \
	X(add,                 "add")                                                                            \
	X(sub,                 "sub")                                                                            \
	X(mul,                 "mul")                                                                            \
	X(div,                 "div")                                                                            \
	X(numeq,               "numeq")                                                                          \
	X(eq,                  "eq")                                                                             \
	X(lt,                  "lt")                                                                             \
	X(le,                  "le")                                                                             \
	X(gt,                  "gt")                                                                             \
	X(ge,                  "ge")                                                                             \
	X(addk,                "addk")                                                                           \
	X(subk,                "subk")                                                                           \
	X(mulk,                "mulk")                                                                           \
	X(divk,                "divk")                                                                           \
	X(numeqk,              "numeqk")                                                                         \
	X(eqk,                 "eqk")                                                                            \
	X(ltk,                 "ltk")                                                                            \
	X(if_false,            "if")                                                                             \
	X(if_numeq,            "ifnumeq")                                                                        \
	X(if_eq,               "ifeq")                                                                           \
	X(if_lt,               "iflt")                                                                           \
	X(if_le,               "ifle")                                                                           \
	X(if_gt,               "ifgt")                                                                           \
	X(if_ge,               "ifge")                                                                           \
	X(if_numeqk,           "ifnumeqk")                                                                       \
	X(if_eqk,              "ifeqk")                                                                          \
	X(if_ltk,              "ifltk")                                                                          \
	X(retv,                "ret")                                                                            \
	X(call,                "call")                                                                           \
	X(tcall,               "tcall")                                                                          \
	X(call_self_tail,      "cselft")                                                                         \
	X(apply,              "apply")                                                                          \
	X(iter_next1,          "iter1")                                                                          \
	X(iter_next2,          "iter2")                                                                          \
	JET_REPLICATE(X, call_local,           "cl")                                                             \
	JET_REPLICATE(X, call_local_tail,      "clt")                                                            \
	JET_REPLICATE(X, call_upval,           "cu")                                                             \
	JET_REPLICATE(X, call_upval_tail,      "cut")                                                            \
	JET_REPLICATE(X, call_upval_slot,      "cus")                                                            \
	JET_REPLICATE(X, call_upval_slot_tail, "cust")                                                           \
	JET_REPLICATE(X, call_self,            "cself")                                                          \
	X(ldf,                 "ldf")                                                                            \
	X(stf,                 "stf")                                                                            \
	X(ldfk,                "ldfk")                                                                           \
	X(stfk,                "stfk")                                                                           \
	X(ldfh,                "ldfh")                                                                           \
	X(ldfkh,               "ldfkh")                                                                          \
	X(ldfo,                "ldfo")                                                                           \
	X(ldfko,               "ldfko")                                                                          \
	X(reset,               "reset")                                                                          \
	X(retk,                "retk")                                                                           \
	X(coro,                "coro")                                                                           \
	X(retc,            "retc")                                                                          \
	X(retu,       "retu")                                                                          \
	X(return_to_host,      "rethost") \
	X(trunc,               "trunc") \
	X(min,                 "min") \
	X(max,                 "max") \
	X(mink,                "mink") \
	X(maxk,                "maxk")

enum class Opcode : uint8_t
{
#define X(name, disp, ...) name,
	JET_OPCODES(X)
#undef X
};

template <>
struct std::formatter<Opcode> : std::formatter<std::string_view>
{
	std::format_context::iterator format(Opcode opcode, std::format_context& context) const
	{
		switch (opcode)
		{
#define X(name, disp, ...) case Opcode::name: return std::formatter<std::string_view>::format(#name, context);
		JET_OPCODES(X)
#undef X
		}
		JET_DIE(nullptr, "invalid Opcode {}", static_cast<unsigned>(opcode));
	}
};

#define X(name, disp, ...) +1
constexpr int OPCODE_COUNT = 0 JET_OPCODES(X);
#undef X

#pragma pack(push, 1)

struct OP_skip
{
	size_t size;
};

struct OP_make_closure_capture
{
	uint8_t src;
	uint16_t idx;
};

struct FieldIc
{
	uint64_t dispatch_key;
	uint64_t cached_index;
	uint64_t cached_key;
};

struct IterIc
{
	uint64_t dispatch_key;
};

// Register ISA: every dst/src/a/b/w operand is a frame-relative slot index,
// stack_base[frame_base + r].

struct OP_mov
{
	uint16_t dst;
	uint16_t src;
};
using OP_trunc = OP_mov;
struct OP_mov2
{
	OP_mov first;
	OP_mov second;
};
struct OP_ldk
{
	uint16_t dst;
	uint16_t idx;
};
using OP_ldu = OP_ldk;
using OP_ldus = OP_ldk;
using OP_ldd = OP_ldk;
struct OP_stu
{
	uint16_t idx;
	uint16_t src;
};
using OP_std = OP_stu;
struct OP_box
{
	uint16_t reg;
};
struct OP_clos
{
	uint16_t dst;
	uint16_t pool_idx;
	uint16_t n_captures;
};
struct OP_binop_rr
{
	uint16_t dst;
	uint16_t a;
	// rk forms read b as a constant-pool index.
	uint16_t b;
};
using OP_binop_rk = OP_binop_rr;
struct OP_if_false
{
	uint16_t src;
	uint32_t size;
};
struct OP_if_cmp
{
	uint16_t a;
	// rk forms read b as a constant-pool index.
	uint16_t b;
	uint32_t size;
};
struct OP_retv
{
	uint16_t src;
};
struct OP_call
{
	uint16_t w;
	uint16_t callee;
	uint16_t nargs;
};
struct OP_call_self_tail
{
	uint16_t w;
	uint16_t nargs;
};
struct OP_apply
{
	uint16_t w;
};
struct OP_reset
{
	uint16_t w;
};
using OP_coro = OP_reset;

// Never emitted into a code stream: retk lives in an Escape's inline buffer.
struct OP_retk
{
	Struct* escape;
};

struct OP_iter_next1
{
	uint16_t cursor;
	uint16_t dst;
	uint32_t size;
	IterIc ic;
};
struct OP_iter_next2
{
	uint16_t cursor;
	uint16_t dst0;
	uint16_t dst1;
	uint32_t size;
	IterIc ic;
};
struct OP_call_slot
{
	uint16_t w;
	uint16_t upvalue_idx;
	uint16_t nargs;
	uint16_t ic_n_locals;
	uint32_t ic_epoch;
	uint64_t ic_slot;
	uint64_t ic_atom;
	union
	{
		uint64_t ic_stub;
		uint64_t ic_code;
	};
	uint64_t ic_version;
};
struct OP_call_atom
{
	uint16_t w;
	uint16_t idx;
	uint16_t nargs;
	uint16_t ic_n_locals;
	uint32_t ic_epoch;
	uint64_t ic_atom;
	union
	{
		uint64_t ic_stub;
		uint64_t ic_code;
	};
};
struct OP_call_self
{
	uint16_t w;
	uint16_t nargs;
};
struct OP_ldf
{
	uint16_t dst;
	uint16_t obj;
	uint16_t key;
	FieldIc ic;
};
struct OP_stf
{
	uint16_t obj;
	uint16_t key;
	uint16_t val;
	FieldIc ic;
};
struct OP_ldfk
{
	uint16_t dst;
	uint16_t obj;
	uint16_t key_idx;
	FieldIc ic;
};
struct OP_stfk
{
	uint16_t obj;
	uint16_t key_idx;
	uint16_t val;
	FieldIc ic;
};
using OP_ldfh = OP_ldf;
using OP_ldfkh = OP_ldfk;
struct OP_ldfo
{
	uint16_t dst;
	uint16_t obj;
	uint16_t key;
	uint16_t dfl;
	FieldIc ic;
};
struct OP_ldfko
{
	uint16_t dst;
	uint16_t obj;
	uint16_t key_idx;
	uint16_t dfl;
	FieldIc ic;
};

#pragma pack(pop)

// Instruction header: [handler ptr (VM_OP_SLOT_SIZE)][opcode tag (1B)].
// Loader writes the handler over the zero-filled slot; the tag survives so
// profile/trace can recover the opcode after direct threading.
constexpr size_t VM_OP_SLOT_SIZE = 8;
constexpr size_t OPCODE_SIZE = VM_OP_SLOT_SIZE + 1;

inline size_t opcode_step(uint8_t op, const uint8_t* operands)
{
	switch (static_cast<Opcode>(op))
	{
		case Opcode::halt:
		case Opcode::label:
			return OPCODE_SIZE;
		case Opcode::skip:
			return OPCODE_SIZE + sizeof(OP_skip);
		case Opcode::mov:
		case Opcode::trunc:
			return OPCODE_SIZE + sizeof(OP_mov);
		case Opcode::mov2:
			return OPCODE_SIZE + sizeof(OP_mov2);
		case Opcode::ldk:
		case Opcode::ldu:
		case Opcode::ldus:
		case Opcode::ldd:
			return OPCODE_SIZE + sizeof(OP_ldk);
		case Opcode::stu:
		case Opcode::std:
			return OPCODE_SIZE + sizeof(OP_stu);
		case Opcode::box:
			return OPCODE_SIZE + sizeof(OP_box);
		case Opcode::clos:
		{
			const OP_clos* c = reinterpret_cast<const OP_clos*>(operands);
			return OPCODE_SIZE + sizeof(OP_clos) + c->n_captures * sizeof(OP_make_closure_capture);
		}
		case Opcode::add:
		case Opcode::sub:
		case Opcode::mul:
		case Opcode::div:
		case Opcode::min:
		case Opcode::max:
		case Opcode::numeq:
		case Opcode::eq:
		case Opcode::lt:
		case Opcode::le:
		case Opcode::gt:
		case Opcode::ge:
			return OPCODE_SIZE + sizeof(OP_binop_rr);
		case Opcode::addk:
		case Opcode::subk:
		case Opcode::mulk:
		case Opcode::divk:
		case Opcode::mink:
		case Opcode::maxk:
		case Opcode::numeqk:
		case Opcode::eqk:
		case Opcode::ltk:
			return OPCODE_SIZE + sizeof(OP_binop_rr);
		case Opcode::if_false:
			return OPCODE_SIZE + sizeof(OP_if_false);
		case Opcode::if_numeq:
		case Opcode::if_eq:
		case Opcode::if_lt:
		case Opcode::if_le:
		case Opcode::if_gt:
		case Opcode::if_ge:
		case Opcode::if_numeqk:
		case Opcode::if_eqk:
		case Opcode::if_ltk:
			return OPCODE_SIZE + sizeof(OP_if_cmp);
		case Opcode::retv:
			return OPCODE_SIZE + sizeof(OP_retv);
		case Opcode::call:
		case Opcode::tcall:
			return OPCODE_SIZE + sizeof(OP_call);
		case Opcode::call_self_tail:
			return OPCODE_SIZE + sizeof(OP_call_self_tail);
		case Opcode::apply:
			return OPCODE_SIZE + sizeof(OP_apply);
		case Opcode::reset:
		case Opcode::coro:
			return OPCODE_SIZE + sizeof(OP_reset);
		case Opcode::retk:
			return OPCODE_SIZE + sizeof(OP_retk);
		// Never emitted into a code stream: they live in static buffers installed as
		// coroutine and host-call return addresses.
		case Opcode::retc:
		case Opcode::retu:
		case Opcode::return_to_host:
			return OPCODE_SIZE;
		case Opcode::iter_next1:
			return OPCODE_SIZE + sizeof(OP_iter_next1);
		case Opcode::iter_next2:
			return OPCODE_SIZE + sizeof(OP_iter_next2);
#define X(name, disp, n) case Opcode::name:
			JET_REPLICATE(X, call_upval_slot, "cus")
			JET_REPLICATE(X, call_upval_slot_tail, "cust")
#undef X
			return OPCODE_SIZE + sizeof(OP_call_slot);
#define X(name, disp, n) case Opcode::name:
			JET_REPLICATE(X, call_local, "cl")
			JET_REPLICATE(X, call_local_tail, "clt")
			JET_REPLICATE(X, call_upval, "cu")
			JET_REPLICATE(X, call_upval_tail, "cut")
#undef X
			return OPCODE_SIZE + sizeof(OP_call_atom);
#define X(name, disp, n) case Opcode::name:
			JET_REPLICATE(X, call_self, "cself")
#undef X
			return OPCODE_SIZE + sizeof(OP_call_self);
		case Opcode::ldf:
		case Opcode::ldfh:
			return OPCODE_SIZE + sizeof(OP_ldf);
		case Opcode::stf:
			return OPCODE_SIZE + sizeof(OP_stf);
		case Opcode::ldfk:
		case Opcode::ldfkh:
			return OPCODE_SIZE + sizeof(OP_ldfk);
		case Opcode::stfk:
			return OPCODE_SIZE + sizeof(OP_stfk);
		case Opcode::ldfo:
			return OPCODE_SIZE + sizeof(OP_ldfo);
		case Opcode::ldfko:
			return OPCODE_SIZE + sizeof(OP_ldfko);
	}
	return OPCODE_SIZE;
}

#endif

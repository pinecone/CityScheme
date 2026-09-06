// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "compiler.h"
#include "error.h"
#include "runtime.h"
#include "vm.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef __APPLE__
#include <xlocale.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <locale.h>
#include <stdlib.h>
#endif

struct Compiler;

struct SourceLoc
{
	uint32_t file_id = 0;
	int line = 0;
	int col = 0;

	static SourceLoc none() { return SourceLoc{}; }
};

#define JETC_DIE(db, loc, fmt, ...)                                                                      \
	do                                                                                                   \
	{                                                                                                    \
		const Compiler& compiler = db;                                                                   \
		const SourceLoc& source_loc = loc;                                                               \
		JET_DIE_UNLESS(nullptr, source_loc.file_id < compiler.file_table.size(),                         \
		               "compiler: invalid source location");                                             \
		if (source_loc.line != 0)                                                                        \
		{                                                                                                \
			JET_DIE(nullptr, "%s:%d:%d: " fmt, compiler.file_table[source_loc.file_id].c_str(),          \
			        source_loc.line, source_loc.col __VA_OPT__(, ) __VA_ARGS__);                         \
		}                                                                                                \
		else                                                                                             \
		{                                                                                                \
			JET_DIE(nullptr, fmt __VA_OPT__(, ) __VA_ARGS__);                                            \
		}                                                                                                \
	} while (0)

#define JETC_DIE_WHEN(db, loc, cond, ...)                                                                \
	do                                                                                                       \
	{                                                                                                        \
		if (cond) [[unlikely]]                                                                                \
		{                                                                                                      \
			JETC_DIE(db, loc, __VA_ARGS__);                                                                    \
		}                                                                                                      \
	} while (0)

#define JETC_DIE_UNLESS(db, loc, cond, ...) JETC_DIE_WHEN(db, loc, !(cond), __VA_ARGS__)

enum class TokenKind : uint8_t
{
	LParen,
	RParen,
	Quote,
	Quasiquote,
	Unquote,
	UnquoteSplicing,
	Hash,
	Number,
	String,
	Boolean,
	Character,
	Variable,
	Lambda,
	Define,
	If,
	Set,
	Setf,
	QuoteWord,
	Apply,
	Let,
	LetStar,
	Letrec,
	LetEc,
	LetCoro,
	Begin,
	When,
	Unless,
	Cond,
	And,
	Or,
	Include,
	Dot,
	Eof,
};

struct Token
{
	TokenKind kind;
	std::string_view text;
	SourceLoc loc;
};

template <typename T>
struct Slice
{
	T* data = nullptr;
	uint32_t count = 0;

	T* begin() { return data; }
	T* end() { return data + count; }
	uint32_t size() { return count; }
	bool empty() { return count == 0; }
	T& operator[](uint32_t i) { return data[i]; }
	T& back() { return data[count - 1]; }
};

struct Arena
{
	static constexpr size_t BLOCK_SIZE = 16384;

	struct Block
	{
		char* mem;
		size_t size;
	};

	std::vector<Block> blocks;
	char* ptr = nullptr;
	char* end = nullptr;

	Arena() = default;
	Arena(Arena&) = delete;
	Arena& operator=(Arena&) = delete;

	~Arena()
	{
		for (Block& b : blocks)
		{
			delete[] b.mem;
		}
	}

	void* alloc_raw(size_t size, size_t align)
	{
		uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
		p = (p + align - 1) & ~(align - 1);
		char* aligned = reinterpret_cast<char*>(p);

		if (ptr == nullptr || aligned + size > end)
		{
			size_t block_size = size + align > BLOCK_SIZE ? size + align : BLOCK_SIZE;
			char* mem = new char[block_size];
			blocks.push_back({mem, block_size});
			aligned = mem;
			end = mem + block_size;
		}

		ptr = aligned + size;
		return aligned;
	}

	template <typename T, typename... Args>
	T* alloc(Args&&... args)
	{
		T* p = static_cast<T*>(alloc_raw(sizeof(T), alignof(T)));
		return new (p) T{std::forward<Args>(args)...};
	}

	template <typename T>
	T* alloc_array(size_t count)
	{
		T* p = static_cast<T*>(alloc_raw(sizeof(T) * count, alignof(T)));
		for (size_t i = 0; i < count; ++i)
		{
			new (&p[i]) T{};
		}
		return p;
	}

	std::string_view copy_string(std::string_view s)
	{
		char* p = static_cast<char*>(alloc_raw(s.size() + 1, 1));
		memcpy(p, s.data(), s.size());
		p[s.size()] = '\0';
		return {p, s.size()};
	}

	template <typename T>
	Slice<T> copy_slice(const T* src, uint32_t count)
	{
		T* data = alloc_array<T>(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			data[i] = src[i];
		}
		return {data, count};
	}

	template <typename T>
	Slice<T> copy_slice(const std::vector<T>& src)
	{
		return copy_slice(src.data(), static_cast<uint32_t>(src.size()));
	}

	template <typename T>
	Slice<T> copy_slice(std::initializer_list<T> src)
	{
		return copy_slice(src.begin(), static_cast<uint32_t>(src.size()));
	}
};

struct Expr;

struct UpvalueRef
{
	Expr* owner;
	uint32_t breadth;
};

using ExprId = uint32_t;
constexpr ExprId NO_EXPR = ~uint32_t{0};

constexpr std::string_view RESET_PRIM = "%reset";
constexpr std::string_view CORO_PRIM = "%coro";
constexpr std::string_view REF_HOLE_PRIM = "%ref-hole";
constexpr std::string_view REF_DEFAULT_PRIM = "%ref-default";

enum class ExprKind : uint8_t
{
	NumberLit,
	StringLit,
	BooleanLit,
	CharacterLit,
	SymbolLit,
	UnknownLit,
	VarRef,
	Call,
	Apply,
	Lambda,
	Define,
	PrimRef,
	SetBang,
	SetRef,
	IterNext,
	If,
	Let,
	Letrec,
	Begin,
	When,
	Unless,
	Cond,
	And,
	Or,
};

struct Expr
{
	ExprKind kind;
	ExprId id;
	SourceLoc loc;

	union
	{
		struct
		{
			std::string_view text;
		} number_lit;
		struct
		{
			std::string_view value;
		} string_lit; // escape-processed
		struct
		{
			bool value;
		} boolean_lit;
		struct
		{
			char value;
		} character_lit;
		struct
		{
			std::string_view name;
		} symbol_lit;
		struct
		{
		} unknown_lit;
		struct
		{
			std::string_view name;
		} var_ref;
		struct
		{
			Expr* proc;
			Slice<Expr*> args;
		} call;
		struct
		{
			Expr* proc;
			Expr* args;
		} apply;
		struct
		{
			Slice<std::string_view> params;
			bool is_variadic;
			Slice<Expr*> body;
			Slice<std::string_view> names;
			Slice<bool> captured_locals;
			Slice<bool> captured_before_init_locals;
			Slice<bool> reassigned_after_init_locals;
			Slice<UpvalueRef> upvalues;
			std::string_view lambda_name;
		} lambda;
		struct
		{
			std::string_view name;
			Expr* value;
		} define;
		struct
		{
			std::string_view name;
		} prim_ref;
		struct
		{
			std::string_view name;
			Expr* value;
			bool is_init;
		} set_bang;
		struct
		{
			Expr* obj;
			Expr* key;
			Expr* value;
		} set_ref;
		struct
		{
			Expr* cursor;
			Slice<std::string_view> names;
			Expr* consequent;
			Expr* alternate;
			uint32_t slot_base;
			Expr* owner;
		} iter_next;
		struct
		{
			Expr* test;
			Expr* consequent;
			Expr* alternate;
		} if_;
		struct
		{
			Slice<std::string_view> names;
			Slice<Expr*> vals;
			Slice<Expr*> body;
			uint32_t slot_base;
			Expr* owner;
		} let;
		struct
		{
			Slice<Expr*> body;
		} begin;
		struct
		{
			Expr* test;
			Slice<Expr*> body;
		} when;
		struct
		{
			Expr* test;
			Slice<Expr*> body;
		} unless;
		// clauses are flat (test, expr) pairs.
		struct
		{
			Slice<Expr*> clauses;
		} cond;
		struct
		{
			Slice<Expr*> exprs;
		} and_;
		struct
		{
			Slice<Expr*> exprs;
		} or_;
	};

	Expr() : kind{ExprKind::UnknownLit}, id{NO_EXPR}, loc{}, unknown_lit{} {}
};

struct Program
{
	Slice<Expr*> forms;
};

inline bool is_delimiter(char c)
{
	return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' || c == '"' ||
	       c == ';' || c == '`' || c == ',';
}

// A struct form's constructor is also the name its type is bound to in the primitive Env.
struct HashForm
{
	std::string_view prefix;
	const char* constructor;
	bool is_struct_type;
	bool is_key_value;
};

inline constexpr HashForm HASH_FORMS[] = {
	{.prefix = "#", .constructor = "vector", .is_struct_type = false, .is_key_value = false},
	{.prefix = "#u8", .constructor = "bytevector", .is_struct_type = false, .is_key_value = false},
	{.prefix = "#tuple", .constructor = "tuple", .is_struct_type = true, .is_key_value = false},
	{.prefix = "#hashset", .constructor = "hashset", .is_struct_type = true, .is_key_value = false},
	{.prefix = "#hashmap", .constructor = "hashmap", .is_struct_type = true, .is_key_value = true},
};

inline const HashForm* find_hash_form(std::string_view prefix)
{
	for (const HashForm& form : HASH_FORMS)
	{
		if (form.prefix == prefix)
		{
			return &form;
		}
	}
	return nullptr;
}

inline bool is_struct_constructor(std::string_view name)
{
	for (const HashForm& form : HASH_FORMS)
	{
		if (form.is_struct_type && name == form.constructor)
		{
			return true;
		}
	}
	return false;
}

inline int hex_digit_value(char digit)
{
	if (digit >= '0' && digit <= '9')
	{
		return digit - '0';
	}
	if (digit >= 'a' && digit <= 'f')
	{
		return digit - 'a' + 10;
	}
	if (digit >= 'A' && digit <= 'F')
	{
		return digit - 'A' + 10;
	}
	return -1;
}

inline void append_utf8(std::string& out, uint32_t code)
{
	if (code < 0x80)
	{
		out += static_cast<char>(code);
	}
	else if (code < 0x800)
	{
		out += static_cast<char>(0xC0 | (code >> 6));
		out += static_cast<char>(0x80 | (code & 0x3F));
	}
	else if (code < 0x10000)
	{
		out += static_cast<char>(0xE0 | (code >> 12));
		out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (code & 0x3F));
	}
	else
	{
		out += static_cast<char>(0xF0 | (code >> 18));
		out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (code & 0x3F));
	}
}

inline bool is_ident_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '!' || c == '$' || c == '%' || c == '&' ||
	       c == '*' || c == '/' || c == ':' || c == '<' || c == '=' || c == '>' || c == '?' || c == '^' ||
	       c == '_' || c == '~' || c == '-' || c == '.';
}

inline bool is_ident_cont(char c)
{
	return is_ident_start(c) || (c >= '0' && c <= '9') || c == '+' || c == '@';
}

static const locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));

static double number_lit_value(std::string_view text)
{
	// NumberLit text is arena-interned by the lexer/parser (copy_string), so
	// text.data() is NUL-terminated.
	return strtod_l(text.data(), nullptr, c_locale);
}

struct ResolvedBinding
{
	Expr* lambda;
	size_t breadth;
};

template <typename T>
typename std::vector<T>::reference get(std::vector<T>& v, size_t i)
{
	if (v.size() <= i)
	{
		v.resize(i + 1);
	}
	return v[i];
}

static uint64_t binding_key(ResolvedBinding b)
{
	return (static_cast<uint64_t>(b.lambda->id) << 32) | static_cast<uint32_t>(b.breadth);
}

struct OrderedNameSet
{
	std::vector<std::string_view> ordered;
	std::unordered_set<std::string_view> seen;

	bool insert(std::string_view name)
	{
		if (!seen.insert(name).second)
		{
			return false;
		}
		ordered.push_back(name);
		return true;
	}
};

struct Compiler
{
	std::string source;
	std::string filename;
	std::string_view prelude;
	std::vector<std::string> file_table;
	Arena arena;
	CompileFlags flags_;

	std::optional<std::vector<Token>> tokens_;
	std::optional<Program> ast_;
	// Counter used by both parse() and expand-time desugaring so synthesized
	// Expr nodes get unique IDs the binding/tail caches can index by.
	uint32_t next_expr_id_ = 0;
	std::vector<Token>& tokens();
	Program& ast();

	Expr* make_expr(SourceLoc loc, ExprKind kind);
	Expr* make_boolean_lit(SourceLoc loc, bool value);
	Expr* expand(Expr* expr);
	Expr* expand_let(Expr* expr);
	Expr* expand_letrec(Expr* expr);
	Expr* expand_begin(Expr* expr);
	Slice<Expr*> hoist_defines_in_body(SourceLoc loc, Slice<Expr*> body);
	Expr* rewrite_define_in(Expr* expr, OrderedNameSet& names);
	OrderedNameSet toplevel_names_;

	using AnfBindings = std::vector<std::pair<std::string_view, Expr*>>;
	Expr* compute_anf(Expr* expr);
	Expr* anf_atomize(Expr* expr, AnfBindings& bindings);
	Expr* anf_wrap(AnfBindings& bindings, Expr* body);
	std::string_view gensym();
	void verify_anf(Expr* expr);
	uint32_t gensym_counter_ = 0;

	std::vector<ResolvedBinding> bindings_;
	std::vector<bool> tail_cache_;
	std::unordered_map<uint64_t, uint32_t> binding_use_counts_;

	// Compile-time chain of enclosing lambdas during binding resolution.
	// Index 0 is a synthesized toplevel lambda whose names are the program's
	// top-level defines, so binding lookup and codegen can treat the program
	// uniformly as one big nested lambda.
	std::vector<Expr*> lambdas_;
	std::vector<std::unordered_map<std::string_view, size_t>> lambda_name_index_;
	std::vector<std::vector<std::string_view>> frame_names_;
	Expr* toplevel_lambda_ = nullptr;

	std::vector<Expr*> all_lambdas_;
	std::vector<bool> intrinsic_callee_;

	struct LambdaBindings
	{
		std::vector<UpvalueRef> upvalues;
		std::unordered_set<uint64_t> upvalue_keys;
		std::vector<bool> captured;
		std::vector<bool> captured_before_init;
		std::vector<bool> is_initialized;
		std::vector<bool> reassigned_after_init;
		std::vector<Expr*> bound_init;
	};
	std::unordered_map<Expr*, LambdaBindings> lambda_bindings_;

	ResolvedBinding binding(Expr* expr);
	bool is_tail(Expr* expr);
	bool is_self_tail_call(Expr* call, Expr* current);
	bool is_intrinsic_callee(Expr* call, Expr* current);
	void collect_intrinsic_callees(Expr* expr, Expr* current);
	std::optional<ResolvedBinding> lookup_name(std::string_view name);
	void push_lambda_scope(Expr* lambda);
	void pop_lambda_scope();
	bool prim_binding_lowerable(ResolvedBinding b, std::string_view prim);
	struct PrimLowering
	{
		enum class Kind { None, Unary, Arith, Ref };
		Kind kind{};
		Opcode op{};
		Opcode op_k{};
	};
	PrimLowering prim_call_lowering(Expr* call);
	void record_ref(ResolvedBinding b);
	void record_set(ResolvedBinding b, bool is_init, Expr* value);
	void collect_binding_uses(Program& program);
	void collect_binding_uses_in(Expr* expr);
	uint32_t binding_use_count(Expr* owner, uint32_t breadth);
	bool binding_used(Expr* owner, uint32_t breadth);
	void resolve_bindings(Program& program);
	void run_optimization_passes(Program& program);
	void compute_binding_addresses(Program& program);
	void compute_binding_addresses_in(Expr* expr);
	void recompute_lambda_bindings(Program& program);
	void recompute_lambda_bindings_in(Expr* expr);
	void freeze_lambda(Expr* lambda);
	void collect_tail_calls(Program& program);
	void collect_tail_calls(Expr* expr, bool in_tail);

	struct OpSelection
	{
		Opcode op;
		union
		{
			struct { uint16_t addr; } var;     // register / upvalue idx of ref/set
			struct { uint16_t upvalue_idx; } call_ic_slot;
			struct { uint16_t idx; } call_ic_atom;
		} u;
	};
	std::vector<std::optional<OpSelection>> selected_ops_;
	std::unordered_map<uint32_t, Expr*> branch_fusions_;
	void run_op_selection(Program& program);
	void select_ops_in(Expr* expr, Expr* current);
	void select_call_op(Expr* expr, Expr* current);
	void collect_branch_fusion_facts(Program& program);
	void select_branch_fusions();
	void select_field_op(Expr* expr, Expr* current, Expr* receiver, Expr* key, Opcode reg_op,
	                     Opcode const_op);
	void select_var_op(Expr* expr, Expr* current, bool is_set);

	void run_anf_inline(Program& program);
	void run_binarize(Program& program);
	void run_lambda_lift(Program& program);

	Bytecode compile();
};

inline char decode_char_literal(Compiler& db, SourceLoc loc, std::string_view body)
{
	if (body.size() == 1)
	{
		return body[0];
	}
	struct Named
	{
		std::string_view name;
		char value;
	};
	static constexpr Named names[] = {
		{"alarm",     0x07},
		{"backspace", 0x08},
		{"delete",    0x7F},
		{"escape",    0x1B},
		{"newline",   0x0A},
		{"null",      0x00},
		{"return",    0x0D},
		{"space",     0x20},
		{"tab",       0x09},
	};
	for (Named n : names)
	{
		if (n.name == body)
		{
			return n.value;
		}
	}
	JETC_DIE(db, loc, "unknown character name '#\\%.*s'", static_cast<int>(body.size()), body.data());
}

static std::vector<Token> lex(Compiler& db, IPort* port, uint32_t file_id);

namespace
{

	struct LexState
	{
		IPort* port;
		Compiler& db;
		uint32_t file_id = 0;
		int line = 1;
		int col = 1;
		bool read_mode = false;
		std::vector<Token> tokens{};
		Token pending_{};

		LexState(IPort* p, Compiler& compiler, uint32_t fid) : port{p}, db{compiler}, file_id{fid}
		{
		}

		TokenKind classify_token_text(std::string_view text)
		{
			return read_mode ? TokenKind::Variable : classify_ident(text);
		}

		bool at_end() { return port->eof(); }
		char peek() { return at_end() ? '\0' : port->peek_byte(); }

		char advance()
		{
			char c = port->read_byte();
			if (c == '\n')
			{
				++line;
				col = 1;
			}
			else
			{
				++col;
			}
			return c;
		}

		SourceLoc loc() { return {file_id, line, col}; }

		void skip_whitespace_and_comments()
		{
			while (!at_end())
			{
				char c = peek();
				if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
				{
					advance();
				}
				else if (c == ';')
				{
					while (!at_end() && peek() != '\n')
					{
						advance();
					}
				}
				else
				{
					break;
				}
			}
		}

		void emit(SourceLoc l, TokenKind kind, std::string_view text) { pending_ = {kind, text, l}; }

		std::string_view intern(const std::string& buf) { return db.arena.copy_string(buf); }

		void lex_string()
		{
			SourceLoc start = loc();
			std::string buf;
			buf += advance();
			while (!at_end() && peek() != '"')
			{
				if (peek() == '\\')
				{
					buf += advance();
				}
				if (!at_end())
				{
					buf += advance();
				}
			}
			if (!at_end())
			{
				buf += advance();
			}
			emit(start, TokenKind::String, intern(buf));
		}

		void lex_hash()
		{
			SourceLoc start = loc();
			std::string buf;
			buf += advance();

			if (at_end())
			{
				emit(start, TokenKind::Hash, intern(buf));
				return;
			}

			switch (peek())
			{
				case '\\':
					buf += advance();
					if (!at_end())
					{
						char first = advance();
						buf += first;
						if (std::isalpha(static_cast<unsigned char>(first)))
						{
							while (std::isalpha(static_cast<unsigned char>(peek())))
							{
								buf += advance();
							}
						}
					}
					emit(start, TokenKind::Character, intern(buf));
					break;

				case 'x':
					advance();
					buf.clear();
					if (!at_end() && (peek() == '+' || peek() == '-'))
					{
						buf += advance();
					}
					buf += "0x";
					JETC_DIE_UNLESS(
						db,
						loc(),
						!at_end() && ((peek() >= '0' && peek() <= '9') ||
						              (peek() >= 'a' && peek() <= 'f') ||
						              (peek() >= 'A' && peek() <= 'F')),
						"invalid hexadecimal literal"
						);
					while (!at_end() && ((peek() >= '0' && peek() <= '9') ||
					                     (peek() >= 'a' && peek() <= 'f') ||
					                     (peek() >= 'A' && peek() <= 'F')))
					{
						buf += advance();
					}
					emit(start, TokenKind::Number, intern(buf));
					break;

				case '(':
					// Leave the '(' in the stream; it lexes as LParen next.
					emit(start, TokenKind::Hash, intern(buf));
					break;

				default:
					finish_hash(start, buf);
			}
		}

		void finish_hash(SourceLoc start, std::string& buf)
		{
			while (!at_end() && std::isalnum(static_cast<unsigned char>(peek())))
			{
				buf += advance();
			}
			if ((buf == "#t" || buf == "#f") && is_delimiter(peek()))
			{
				emit(start, TokenKind::Boolean, intern(buf));
				return;
			}
			JETC_DIE_UNLESS(db, loc(), find_hash_form(buf) && peek() == '(', "invalid # syntax");
			emit(start, TokenKind::Hash, intern(buf));
		}

		void finish_ident(SourceLoc start, std::string& buf)
		{
			while (!at_end() && is_ident_cont(peek()))
			{
				buf += advance();
			}
			std::string_view text = intern(buf);
			emit(start, classify_token_text(text), text);
		}

		void finish_number(SourceLoc start, std::string& buf)
		{
			// peek() is on the first digit; buf may already hold a sign.
			char first = advance();
			buf += first;

			if (first == '0' && !at_end() && peek() == 'x')
			{
				buf += advance();
				while (!at_end() && ((peek() >= '0' && peek() <= '9') ||
				                     (peek() >= 'a' && peek() <= 'f') ||
				                     (peek() >= 'A' && peek() <= 'F')))
				{
					buf += advance();
				}
				emit(start, TokenKind::Number, intern(buf));
				return;
			}

			while (!at_end() && peek() >= '0' && peek() <= '9')
			{
				buf += advance();
			}
			if (!at_end() && peek() == '.')
			{
				buf += advance();
				while (!at_end() && peek() >= '0' && peek() <= '9')
				{
					buf += advance();
				}
			}
			if (!at_end() && (peek() == 'e' || peek() == 'E'))
			{
				buf += advance();
				if (!at_end() && (peek() == '+' || peek() == '-'))
				{
					buf += advance();
				}
				while (!at_end() && peek() >= '0' && peek() <= '9')
				{
					buf += advance();
				}
			}
			emit(start, TokenKind::Number, intern(buf));
		}

		void lex_number_or_ident()
		{
			SourceLoc start = loc();
			std::string buf;
			char c = peek();

			if (c == '+' || c == '-')
			{
				buf += advance();
				if (at_end() || is_delimiter(peek()))
				{
					std::string_view text = intern(buf);
					emit(start, classify_token_text(text), text);
					return;
				}
				if (peek() >= '0' && peek() <= '9')
				{
					finish_number(start, buf);
					return;
				}
				finish_ident(start, buf);
				return;
			}

			if (c >= '0' && c <= '9')
			{
				finish_number(start, buf);
				return;
			}

			if (is_ident_start(c))
			{
				finish_ident(start, buf);
				return;
			}

			JETC_DIE(db, loc(), "unexpected character '%c'", c);
		}

		TokenKind classify_ident(std::string_view text)
		{
			struct Keyword
			{
				std::string_view name;
				TokenKind kind;
			};
			static constexpr Keyword keywords[] = {
				{"lambda",  TokenKind::Lambda},
				{"define",  TokenKind::Define},
				{"if",      TokenKind::If},
				{"set!",    TokenKind::Set},
				{"setf!",   TokenKind::Setf},
				{"quote",   TokenKind::QuoteWord},
				{"apply",   TokenKind::Apply},
				{"let",     TokenKind::Let},
				{"let*",    TokenKind::LetStar},
				{"letrec",  TokenKind::Letrec},
				{"letrec*", TokenKind::Letrec},
				{"let/ec",  TokenKind::LetEc},
				{"let/coro", TokenKind::LetCoro},
				{"begin",   TokenKind::Begin},
				{"when",    TokenKind::When},
				{"unless",  TokenKind::Unless},
				{"cond",    TokenKind::Cond},
				{"and",     TokenKind::And},
				{"or",      TokenKind::Or},
				{"include", TokenKind::Include},
			};
			for (Keyword k : keywords)
			{
				if (k.name == text)
				{
					return k.kind;
				}
			}
			return TokenKind::Variable;
		}

		Token next_token()
		{
			skip_whitespace_and_comments();
			if (at_end())
			{
				return {TokenKind::Eof, {}, loc()};
			}

			char c = peek();
			SourceLoc l = loc();

			switch (c)
			{
				case '(':
					advance();
					emit(l, TokenKind::LParen, intern(std::string{c}));
					break;

				case ')':
					advance();
					emit(l, TokenKind::RParen, intern(std::string{c}));
					break;

				case '\'':
					advance();
					emit(l, TokenKind::Quote, intern(std::string{c}));
					break;

				case '`':
					advance();
					emit(l, TokenKind::Quasiquote, intern(std::string{c}));
					break;

				case ',':
					advance();
					if (!at_end() && peek() == '@')
					{
						advance();
						emit(l, TokenKind::UnquoteSplicing, intern(std::string{",@"}));
					}
					else
					{
						emit(l, TokenKind::Unquote, intern(std::string{c}));
					}
					break;

				case '"':
					lex_string();
					break;

				case '#':
					lex_hash();
					break;

				case '.':
					advance();
					if (at_end() || is_delimiter(peek()))
					{
						emit(l, TokenKind::Dot, intern(std::string{c}));
					}
					else
					{
						std::string buf{c};
						finish_ident(l, buf);
					}
					break;

				default:
					lex_number_or_ident();
					break;
			}

			return pending_;
		}

		void lex_all()
		{
			while (true)
			{
				Token t = next_token();
				tokens.push_back(t);
				if (t.kind == TokenKind::Eof)
				{
					break;
				}
			}
		}
	};

	struct ParseState
	{
		std::span<Token> tokens;
		size_t pos = 0;
		LexState* stream_lex = nullptr;
		Token la_{};
		bool la_valid_ = false;
		Token current_{};

		Compiler& db;
		std::vector<Expr*> scratch{};

		Token& peek()
		{
			if (stream_lex)
			{
				if (!la_valid_)
				{
					la_ = stream_lex->next_token();
					la_valid_ = true;
				}
				return la_;
			}
			return tokens[pos];
		}

		bool at_end() { return peek().kind == TokenKind::Eof; }

		Token& advance()
		{
			if (stream_lex)
			{
				peek();
				current_ = la_;
				la_valid_ = false;
				return current_;
			}
			Token& t = tokens[pos];
			if (t.kind != TokenKind::Eof)
			{
				++pos;
			}
			return t;
		}

		void expect(TokenKind kind)
		{
			if (peek().kind != kind)
			{
				JETC_DIE(db, peek().loc, "expected token %d, got %d", static_cast<int>(kind),
				         static_cast<int>(peek().kind));
			}
			advance();
		}

		std::string_view expect_identifier(const char* what)
		{
			JETC_DIE_UNLESS(db, peek().loc, peek().kind == TokenKind::Variable, "expected identifier for %s",
			                what);
			return advance().text;
		}

		Expr* make_expr(SourceLoc loc, ExprKind kind)
		{
			Expr* e = db.arena.alloc<Expr>();
			e->kind = kind;
			e->id = db.next_expr_id_++;
			e->loc = loc;
			return e;
		}

		Slice<Expr*> make_slice(std::vector<Expr*>& vec) { return db.arena.copy_slice(vec); }

		Slice<std::string_view> make_string_slice(std::vector<std::string_view>& vec)
		{
			return db.arena.copy_slice(vec);
		}

		std::string_view process_string_escapes(SourceLoc loc, std::string_view raw)
		{
			// `raw` is the lexer slice including surrounding quotes.
			std::string_view inner = raw.substr(1, raw.size() - 2);

			if (inner.find('\\') == std::string_view::npos)
			{
				return inner;
			}

			std::string result;
			result.reserve(inner.size());

			for (size_t i = 0; i < inner.size(); ++i)
			{
				if (inner[i] != '\\')
				{
					result += inner[i];
					continue;
				}

				JETC_DIE_WHEN(db, loc, i + 1 == inner.size(), "trailing '\\' in string");
				++i;

				switch (inner[i])
				{
					case '\\':
						result += '\\';
						break;
					case '"':
						result += '"';
						break;
					case '|':
						result += '|';
						break;
					case 'a':
						result += '\a';
						break;
					case 'b':
						result += '\b';
						break;
					case 'n':
						result += '\n';
						break;
					case 'r':
						result += '\r';
						break;
					case 't':
						result += '\t';
						break;
					case 'x':
						append_utf8(result, parse_hex_escape(loc, inner, i));
						break;
					default:
						JETC_DIE_UNLESS(db, loc, skip_line_continuation(inner, i),
						                "unknown string escape '\\%c'", inner[i]);
						break;
				}
			}

			return db.arena.copy_string(result);
		}

		uint32_t parse_hex_escape(SourceLoc loc, std::string_view inner, size_t& i)
		{
			size_t first = i + 1;
			size_t end = inner.find(';', first);
			JETC_DIE_WHEN(
				db,
				loc,
				end == std::string_view::npos || end == first,
				"'\\x' escape needs hex digits and a ';'"
				);

			uint32_t code = 0;
			for (size_t at = first; at < end; ++at)
			{
				int value = hex_digit_value(inner[at]);
				JETC_DIE_WHEN(db, loc, value < 0, "'\\x' escape has a non-hex digit '%c'", inner[at]);
				code = code * 16 + static_cast<uint32_t>(value);
				JETC_DIE_WHEN(db, loc, code > 0x10FFFF, "'\\x' escape is above U+10FFFF");
			}

			JETC_DIE_WHEN(db, loc, code >= 0xD800 && code <= 0xDFFF, "'\\x' escape is a UTF-16 surrogate");

			i = end;
			return code;
		}

		bool skip_line_continuation(std::string_view inner, size_t& i)
		{
			size_t at = i;
			while (at < inner.size() && (inner[at] == ' ' || inner[at] == '\t'))
			{
				++at;
			}
			if (at == inner.size() || inner[at] != '\n')
			{
				return false;
			}
			++at;
			while (at < inner.size() && (inner[at] == ' ' || inner[at] == '\t'))
			{
				++at;
			}

			i = at - 1;
			return true;
		}

		Expr* parse_expr()
		{
			Token& tok = peek();

			switch (tok.kind)
			{
				case TokenKind::Number:
				{
					Expr* e = make_expr(tok.loc, ExprKind::NumberLit);
					e->number_lit.text = advance().text;
					return e;
				}
				case TokenKind::String:
				{
					Expr* e = make_expr(tok.loc, ExprKind::StringLit);
					e->string_lit.value = process_string_escapes(tok.loc, advance().text);
					return e;
				}
				case TokenKind::Boolean:
				{
					Expr* e = make_expr(tok.loc, ExprKind::BooleanLit);
					e->boolean_lit.value = (advance().text.back() == 't');
					return e;
				}
				case TokenKind::Character:
				{
					Expr* e = make_expr(tok.loc, ExprKind::CharacterLit);
					std::string text = std::string{advance().text};
					std::string_view body{text.data() + 2, text.size() - 2};
					e->character_lit.value = decode_char_literal(db, tok.loc, body);
					return e;
				}
				case TokenKind::Variable:
				{
					Expr* e = make_expr(tok.loc, ExprKind::VarRef);
					e->var_ref.name = advance().text;
					return e;
				}
				case TokenKind::Quote:
				{
					advance();
					return parse_datum();
				}
				case TokenKind::Quasiquote:
				{
					advance();
					return parse_quasiquote(1);
				}
				case TokenKind::Hash:
					return parse_hash_form(0);
				case TokenKind::LParen:
					return parse_paren_form();
				default:
					JETC_DIE(db, tok.loc, "unexpected token");
			}
		}

		Expr* parse_paren_form()
		{
			SourceLoc loc = peek().loc;
			expect(TokenKind::LParen);
			Token& head = peek();

			if (head.kind == TokenKind::Variable)
			{
				if (head.text == "%prim")
				{
					return parse_prim(loc);
				}
				if (head.text == "$file")
				{
					return parse_dollar_file(loc);
				}
				if (head.text == "$line")
				{
					return parse_dollar_loc_field(loc, loc.line);
				}
				if (head.text == "$col")
				{
					return parse_dollar_loc_field(loc, loc.col);
				}
				if (head.text == "$check")
				{
					return parse_dollar_check(loc);
				}
				if (head.text == "%if/next!")
				{
					return parse_if_next(loc);
				}
				if (head.text == "quasiquote")
				{
					advance();
					Expr* e = parse_quasiquote(1);
					expect(TokenKind::RParen);
					return e;
				}
			}

			switch (head.kind)
			{
				case TokenKind::Lambda:
					return parse_lambda(loc);
				case TokenKind::Define:
					return parse_define(loc);
				case TokenKind::If:
					return parse_if(loc);
				case TokenKind::Set:
					return parse_set_bang(loc);
				case TokenKind::Setf:
					return parse_setf_bang(loc);
				case TokenKind::Apply:
					return parse_apply(loc);
				case TokenKind::QuoteWord:
					return parse_quote_form(loc);
				case TokenKind::Let:
					return parse_let_form(loc, ExprKind::Let);
				case TokenKind::LetStar:
					return parse_let_star(loc);
				case TokenKind::Letrec:
					return parse_let_form(loc, ExprKind::Letrec);
				case TokenKind::LetEc:
					return parse_let_ec(loc);
				case TokenKind::LetCoro:
					return parse_let_coro(loc);
				case TokenKind::Begin:
					return parse_begin(loc);
				case TokenKind::When:
					return parse_when(loc);
				case TokenKind::Unless:
					return parse_unless(loc);
				case TokenKind::Cond:
					return parse_cond(loc);
				case TokenKind::And:
					return parse_and(loc);
				case TokenKind::Or:
					return parse_or(loc);
				case TokenKind::Include:
					return parse_include(loc);
				default:
					return parse_call(loc);
			}
		}

		Expr* parse_lambda(SourceLoc loc)
		{
			advance();

			std::vector<std::string_view> params;
			bool is_variadic = false;

			if (peek().kind == TokenKind::LParen)
			{
				advance();
				while (peek().kind == TokenKind::Variable)
				{
					params.push_back(advance().text);
				}
				expect(TokenKind::RParen);
			}
			else if (peek().kind == TokenKind::Variable)
			{
				params.push_back(advance().text);
				is_variadic = true;
			}
			else
			{
				JETC_DIE(db, loc, "expected formals");
			}

			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::Lambda);
			e->lambda.params = make_string_slice(params);
			e->lambda.is_variadic = is_variadic;
			e->lambda.body = make_slice(body);
			e->lambda.lambda_name = {};
			return e;
		}

		Expr* parse_define(SourceLoc loc)
		{
			advance();

			if (peek().kind == TokenKind::LParen)
			{
				// (define (f arg ...) body ...)
				//   ==> (define f (lambda (arg ...) body ...))
				advance();
				std::string_view name = expect_identifier("define");

				std::vector<std::string_view> params;
				while (peek().kind == TokenKind::Variable)
				{
					params.push_back(advance().text);
				}
				expect(TokenKind::RParen);

				std::vector<Expr*> body;
				while (peek().kind != TokenKind::RParen)
				{
					body.push_back(parse_expr());
				}
				expect(TokenKind::RParen);

				Expr* lam = make_expr(loc, ExprKind::Lambda);
				lam->lambda.params = make_string_slice(params);
				lam->lambda.is_variadic = false;
				lam->lambda.body = make_slice(body);
				lam->lambda.lambda_name = name;

				Expr* e = make_expr(loc, ExprKind::Define);
				e->define.name = name;
				e->define.value = lam;
				return e;
			}

			std::string_view name = expect_identifier("define");
			Expr* value = parse_expr();
			expect(TokenKind::RParen);

			if (value->kind == ExprKind::Lambda && value->lambda.lambda_name.empty())
			{
				value->lambda.lambda_name = name;
			}

			Expr* e = make_expr(loc, ExprKind::Define);
			e->define.name = name;
			e->define.value = value;
			return e;
		}

		Expr* parse_prim(SourceLoc loc)
		{
			advance();
			if (peek().kind != TokenKind::String)
			{
				JETC_DIE(db, loc, "%%prim expects a string literal");
			}
			std::string_view name = process_string_escapes(loc, advance().text);
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::PrimRef);
			e->prim_ref.name = name;
			return e;
		}

		Expr* parse_if(SourceLoc loc)
		{
			advance();
			Expr* test = parse_expr();
			Expr* consequent = parse_expr();
			Expr* alternate = nullptr;
			if (peek().kind != TokenKind::RParen)
			{
				alternate = parse_expr();
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::If);
			e->if_.test = test;
			e->if_.consequent = consequent;
			e->if_.alternate = alternate;
			return e;
		}

		Expr* parse_set_bang(SourceLoc loc)
		{
			advance();

			std::string_view name = expect_identifier("set!");
			Expr* value = parse_expr();
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::SetBang);
			e->set_bang.name = name;
			e->set_bang.value = value;
			e->set_bang.is_init = false;
			return e;
		}

		Expr* parse_setf_bang(SourceLoc loc)
		{
			advance();

			Expr* obj = parse_expr();
			Expr* key = parse_expr();
			Expr* value = parse_expr();
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::SetRef);
			e->set_ref.obj = obj;
			e->set_ref.key = key;
			e->set_ref.value = value;
			return e;
		}

		Expr* parse_if_next(SourceLoc loc)
		{
			advance();
			expect(TokenKind::LParen);
			expect(TokenKind::LParen);
			std::vector<std::string_view> names;
			while (peek().kind == TokenKind::Variable)
			{
				names.push_back(advance().text);
			}
			expect(TokenKind::RParen);
			Expr* cursor = parse_expr();
			expect(TokenKind::RParen);
			JETC_DIE_UNLESS(
				db,
				loc,
				names.size() == 1 || names.size() == 2,
				"if/next! expects one or two output names"
				);
			Expr* consequent = parse_expr();
			Expr* alternate = nullptr;
			if (peek().kind != TokenKind::RParen)
			{
				alternate = parse_expr();
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::IterNext);
			e->iter_next.cursor = cursor;
			e->iter_next.names = make_string_slice(names);
			e->iter_next.consequent = consequent;
			e->iter_next.alternate = alternate;
			return e;
		}

		Expr* parse_apply(SourceLoc loc)
		{
			advance();
			Expr* proc = parse_expr();
			Expr* args = parse_expr();
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::Apply);
			e->apply.proc = proc;
			e->apply.args = args;
			return e;
		}

		Expr* parse_quote_form(SourceLoc loc)
		{
			advance();
			Expr* datum = parse_datum();
			expect(TokenKind::RParen);
			return datum;
		}

		Expr* make_prim_call(SourceLoc loc, std::string_view name, std::initializer_list<Expr*> call_args)
		{
			Expr* prim = make_expr(loc, ExprKind::PrimRef);
			prim->prim_ref.name = name;
			std::vector<Expr*> args{call_args};
			Expr* call = make_expr(loc, ExprKind::Call);
			call->call.proc = prim;
			call->call.args = make_slice(args);
			return call;
		}

		Expr* parse_call(SourceLoc loc)
		{
			Expr* proc = parse_expr();
			std::vector<Expr*> args;
			while (peek().kind != TokenKind::RParen)
			{
				if (peek().kind == TokenKind::Variable && peek().text == ":default")
				{
					advance();
					if (proc->kind != ExprKind::VarRef || proc->var_ref.name != "ref")
					{
						JETC_DIE(db, loc, "':default' is only valid in a ref call");
					}
					JETC_DIE_WHEN(db, loc, args.size() < 2, "ref with ':default' needs a receiver and a key");
					Expr* fallback = parse_expr();
					expect(TokenKind::RParen);

					// Each inner step propagates the hole and the last step
					// replaces it with the fallback, so a miss at any step of the key
					// path yields the default.
					Expr* acc = args[0];
					for (size_t i = 1; i + 1 < args.size(); ++i)
					{
						acc = make_prim_call(loc, REF_HOLE_PRIM, {acc, args[i]});
					}
					return make_prim_call(loc, REF_DEFAULT_PRIM, {acc, args.back(), fallback});
				}
				args.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::Call);
			e->call.proc = proc;
			e->call.args = make_slice(args);
			return e;
		}

		// (let/ec k ((a v) ...) body ...)
		//   ==> (%reset (lambda (k) (let ((a v) ...) body ...)))
		// (let/coro yield ((a v) ...) body ...)
		//   ==> (%coro (lambda (yield) (let ((a v) ...) body ...)))
		// The body must be a real lambda: `reset` and `coro` give it a frame of its own,
		// so tail calls inside it reuse that frame instead of the one holding the form.
		Expr* parse_let_control(SourceLoc loc, std::string_view prim, const char* form_name,
		                        const char* binding_name)
		{
			advance();

			std::string_view bound_name = expect_identifier(form_name);
			expect(TokenKind::LParen);

			std::vector<std::string_view> names;
			std::vector<Expr*> vals;
			while (peek().kind != TokenKind::RParen)
			{
				expect(TokenKind::LParen);
				names.push_back(expect_identifier(binding_name));
				vals.push_back(parse_expr());
				expect(TokenKind::RParen);
			}
			expect(TokenKind::RParen);

			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* inner = make_expr(loc, ExprKind::Let);
			inner->let.names = make_string_slice(names);
			inner->let.vals = make_slice(vals);
			inner->let.body = make_slice(body);

			std::vector<std::string_view> params{bound_name};
			std::vector<Expr*> lambda_body{inner};
			Expr* lam = make_expr(loc, ExprKind::Lambda);
			lam->lambda.params = make_string_slice(params);
			lam->lambda.is_variadic = false;
			lam->lambda.lambda_name = form_name;
			lam->lambda.body = make_slice(lambda_body);

			Expr* proc = make_expr(loc, ExprKind::PrimRef);
			proc->prim_ref.name = prim;

			std::vector<Expr*> args{lam};
			Expr* e = make_expr(loc, ExprKind::Call);
			e->call.proc = proc;
			e->call.args = make_slice(args);
			return e;
		}

		Expr* parse_let_ec(SourceLoc loc)
		{
			return parse_let_control(loc, RESET_PRIM, "let/ec", "let/ec binding");
		}

		Expr* parse_let_coro(SourceLoc loc)
		{
			return parse_let_control(loc, CORO_PRIM, "let/coro", "let/coro binding");
		}

		Expr* parse_let_form(SourceLoc loc, ExprKind kind)
		{
			advance();

			std::string_view loop_name;
			if (kind == ExprKind::Let && peek().kind == TokenKind::Variable)
			{
				loop_name = advance().text;
			}

			expect(TokenKind::LParen);

			std::vector<std::string_view> names;
			std::vector<Expr*> vals;
			while (peek().kind != TokenKind::RParen)
			{
				expect(TokenKind::LParen);
				names.push_back(expect_identifier("let binding"));
				vals.push_back(parse_expr());
				expect(TokenKind::RParen);
			}
			expect(TokenKind::RParen);

			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			if (loop_name.empty())
			{
				Expr* e = make_expr(loc, kind);
				e->let.names = make_string_slice(names);
				e->let.vals = make_slice(vals);
				e->let.body = make_slice(body);
				return e;
			}

			// Named let: (let f ((x v) ...) body ...)
			//   ==> (letrec ((f (lambda (x ...) body ...))) (f v ...))
			Expr* lam = make_expr(loc, ExprKind::Lambda);
			lam->lambda.params = make_string_slice(names);
			lam->lambda.is_variadic = false;
			lam->lambda.body = make_slice(body);
			lam->lambda.lambda_name = loop_name;

			Expr* var = make_expr(loc, ExprKind::VarRef);
			var->var_ref.name = loop_name;

			Expr* call = make_expr(loc, ExprKind::Call);
			call->call.proc = var;
			call->call.args = make_slice(vals);

			std::vector<std::string_view> rec_names{loop_name};
			std::vector<Expr*> rec_vals{lam};
			std::vector<Expr*> rec_body{call};

			Expr* e = make_expr(loc, ExprKind::Letrec);
			e->let.names = make_string_slice(rec_names);
			e->let.vals = make_slice(rec_vals);
			e->let.body = make_slice(rec_body);
			return e;
		}

		// (let* ((x v) binding ...) body ...)
		//   ==> (let ((x v)) (let* (binding ...) body ...))
		// (let* () body ...) ==> (let () body ...)
		Expr* parse_let_star(SourceLoc loc)
		{
			advance();
			expect(TokenKind::LParen);

			std::vector<std::string_view> names;
			std::vector<Expr*> vals;
			while (peek().kind != TokenKind::RParen)
			{
				expect(TokenKind::LParen);
				names.push_back(expect_identifier("let* binding"));
				vals.push_back(parse_expr());
				expect(TokenKind::RParen);
			}
			expect(TokenKind::RParen);

			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			if (names.empty())
			{
				Expr* e = make_expr(loc, ExprKind::Let);
				e->let.names = make_string_slice(names);
				e->let.vals = make_slice(vals);
				e->let.body = make_slice(body);
				return e;
			}

			Expr* inner = nullptr;
			for (size_t i = names.size(); i-- > 0;)
			{
				Expr* let_expr = make_expr(loc, ExprKind::Let);
				std::vector<std::string_view> n{names[i]};
				std::vector<Expr*> v{vals[i]};
				let_expr->let.names = make_string_slice(n);
				let_expr->let.vals = make_slice(v);
				if (inner == nullptr)
				{
					let_expr->let.body = make_slice(body);
				}
				else
				{
					std::vector<Expr*> b{inner};
					let_expr->let.body = make_slice(b);
				}
				inner = let_expr;
			}
			return inner;
		}

		Expr* parse_begin(SourceLoc loc)
		{
			advance();
			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::Begin);
			e->begin.body = make_slice(body);
			return e;
		}

		Expr* parse_when(SourceLoc loc)
		{
			advance();
			Expr* test = parse_expr();
			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::When);
			e->when.test = test;
			e->when.body = make_slice(body);
			return e;
		}

		Expr* parse_unless(SourceLoc loc)
		{
			advance();
			Expr* test = parse_expr();
			std::vector<Expr*> body;
			while (peek().kind != TokenKind::RParen)
			{
				body.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::Unless);
			e->unless.test = test;
			e->unless.body = make_slice(body);
			return e;
		}

		Expr* parse_cond(SourceLoc loc)
		{
			advance();
			std::vector<Expr*> clauses;
			while (peek().kind != TokenKind::RParen)
			{
				SourceLoc clause_loc = peek().loc;
				expect(TokenKind::LParen);
				clauses.push_back(parse_expr());
				std::vector<Expr*> body;
				while (peek().kind != TokenKind::RParen)
				{
					body.push_back(parse_expr());
				}
				expect(TokenKind::RParen);
				if (body.size() == 1)
				{
					clauses.push_back(body[0]);
				}
				else
				{
					Expr* begin_e = make_expr(clause_loc, ExprKind::Begin);
					begin_e->begin.body = make_slice(body);
					clauses.push_back(begin_e);
				}
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::Cond);
			e->cond.clauses = make_slice(clauses);
			return e;
		}

		Expr* parse_and(SourceLoc loc)
		{
			advance();
			std::vector<Expr*> exprs;
			while (peek().kind != TokenKind::RParen)
			{
				exprs.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::And);
			e->and_.exprs = make_slice(exprs);
			return e;
		}

		Expr* parse_or(SourceLoc loc)
		{
			advance();
			std::vector<Expr*> exprs;
			while (peek().kind != TokenKind::RParen)
			{
				exprs.push_back(parse_expr());
			}
			expect(TokenKind::RParen);

			Expr* e = make_expr(loc, ExprKind::Or);
			e->or_.exprs = make_slice(exprs);
			return e;
		}

		// (include "path")
		//   ==> (begin form ...) over the forms parsed out of path
		// The included text gets its own file_id, so error positions name that file.
		Expr* parse_include(SourceLoc loc)
		{
			advance();
			if (peek().kind != TokenKind::String)
			{
				JETC_DIE(db, loc, "include expects a string path");
			}
			std::string_view raw_path = advance().text;
			expect(TokenKind::RParen);

			std::string_view raw_inc_path = process_string_escapes(loc, raw_path);

			std::string path{raw_inc_path};
			if (path[0] != '/')
			{
				std::string& parent = db.file_table[loc.file_id];
				if (size_t slash = parent.rfind('/'); slash != std::string::npos)
				{
					path = parent.substr(0, slash + 1) + path;
				}
			}

			FILE* f = fopen(path.c_str(), "rb");
			if (!f)
			{
				JETC_DIE(db, loc, "cannot open '%.*s'", static_cast<int>(path.size()), path.data());
			}
			std::string source;
			char read_buf[4096];
			size_t n;
			while ((n = fread(read_buf, 1, sizeof(read_buf), f)) > 0)
			{
				source.append(read_buf, n);
			}
			fclose(f);

			uint32_t file_id = static_cast<uint32_t>(db.file_table.size());
			db.file_table.push_back(std::string{path});
			char* source_copy = static_cast<char*>(db.arena.alloc_raw(source.size() + 1, 1));
			memcpy(source_copy, source.data(), source.size());
			source_copy[source.size()] = '\0';
			std::string_view source_view{source_copy, source.size()};

			IPortMem inc_port{source_view};
			std::vector<Token> inc_tokens = lex(db, &inc_port, file_id);

			ParseState inc_state{
				.tokens = inc_tokens,
				.db = db,
			};
			std::vector<Expr*> forms;
			while (!inc_state.at_end())
			{
				forms.push_back(inc_state.parse_expr());
			}
			Expr* e = make_expr(loc, ExprKind::Begin);
			e->begin.body = make_slice(forms);
			return e;
		}

		// ($file) ==> "the path of the file being parsed"
		Expr* parse_dollar_file(SourceLoc loc)
		{
			advance();
			expect(TokenKind::RParen);
			Expr* e = make_expr(loc, ExprKind::StringLit);
			e->string_lit.value = db.arena.copy_string(db.file_table[loc.file_id]);
			return e;
		}

		Expr* make_number_lit(SourceLoc loc, int value)
		{
			Expr* e = make_expr(loc, ExprKind::NumberLit);
			char buf[16];
			snprintf(buf, sizeof(buf), "%d", value);
			e->number_lit.text = db.arena.copy_string(buf);
			return e;
		}

		// ($line) ==> the line of the form, ($col) ==> its column
		Expr* parse_dollar_loc_field(SourceLoc loc, int value)
		{
			advance();
			expect(TokenKind::RParen);
			return make_number_lit(loc, value);
		}

		// ($check expr) ==> (%check expr "file" line col)
		Expr* parse_dollar_check(SourceLoc loc)
		{
			advance();
			Expr* test = parse_expr();
			expect(TokenKind::RParen);

			Expr* check_ref = make_expr(loc, ExprKind::VarRef);
			check_ref->var_ref.name = db.arena.copy_string("%check");

			Expr* file_lit = make_expr(loc, ExprKind::StringLit);
			file_lit->string_lit.value = db.arena.copy_string(db.file_table[loc.file_id]);

			std::vector<Expr*> args{
				test,
				file_lit,
				make_number_lit(loc, loc.line),
				make_number_lit(loc, loc.col),
			};

			Expr* e = make_expr(loc, ExprKind::Call);
			e->call.proc = check_ref;
			e->call.args = make_slice(args);
			return e;
		}

		static bool is_symbol_token(TokenKind k)
		{
			switch (k)
			{
				case TokenKind::Variable:
				case TokenKind::Lambda:
				case TokenKind::Define:
				case TokenKind::If:
				case TokenKind::Set:
				case TokenKind::Setf:
				case TokenKind::QuoteWord:
				case TokenKind::Apply:
				case TokenKind::Let:
				case TokenKind::LetStar:
				case TokenKind::Letrec:
				case TokenKind::LetEc:
				case TokenKind::LetCoro:
				case TokenKind::Begin:
				case TokenKind::When:
				case TokenKind::Unless:
				case TokenKind::Cond:
				case TokenKind::And:
				case TokenKind::Or:
				case TokenKind::Include:
					return true;
				default:
					return false;
			}
		}

		Expr* make_symbol(SourceLoc loc, std::string_view name)
		{
			Expr* e = make_expr(loc, ExprKind::SymbolLit);
			e->symbol_lit.name = name;
			return e;
		}

		Expr* make_call(SourceLoc loc, std::string_view proc, std::vector<Expr*> args)
		{
			Expr* proc_ref = make_expr(loc, ExprKind::VarRef);
			proc_ref->var_ref.name = db.arena.copy_string(proc);

			Expr* e = make_expr(loc, ExprKind::Call);
			e->call.proc = proc_ref;
			e->call.args = make_slice(args);
			return e;
		}

		// (list 'tag inner): the reader form of a nested quasiquote level, rebuilt as data.
		Expr* make_tagged_datum(SourceLoc loc, const char* tag, Expr* inner)
		{
			return make_call(loc, "list", {make_symbol(loc, db.arena.copy_string(tag)), inner});
		}

		static const char* reader_tag_name(TokenKind k)
		{
			switch (k)
			{
				case TokenKind::Quote:
					return "quote";
				case TokenKind::Quasiquote:
					return "quasiquote";
				case TokenKind::Unquote:
					return "unquote";
				default:
					return "unquote-splicing";
			}
		}

		Expr* parse_datum()
		{
			Token& tok = peek();

			switch (tok.kind)
			{
				case TokenKind::Number:
				case TokenKind::String:
				case TokenKind::Boolean:
				case TokenKind::Character:
					return parse_expr();

				case TokenKind::Quote:
				case TokenKind::Quasiquote:
				case TokenKind::Unquote:
				case TokenKind::UnquoteSplicing:
				{
					SourceLoc qloc = advance().loc;
					return make_tagged_datum(qloc, reader_tag_name(tok.kind), parse_datum());
				}

				case TokenKind::LParen:
					return parse_quoted_list();

				case TokenKind::Hash:
					return parse_hash_form(0);

				default:
					JETC_DIE_UNLESS(db, tok.loc, is_symbol_token(tok.kind), "unexpected token in datum");
					return make_symbol(tok.loc, advance().text);
			}
		}

		struct QqItem
		{
			Expr* value;
			bool splice;
		};

		static bool has_splice(std::vector<QqItem>& items)
		{
			for (QqItem& item : items)
			{
				if (item.splice)
				{
					return true;
				}
			}
			return false;
		}

		std::vector<Expr*> item_values(std::vector<QqItem>& items)
		{
			std::vector<Expr*> values;
			for (QqItem& item : items)
			{
				values.push_back(item.value);
			}
			return values;
		}

		// Builds the template right to left so a dotted tail and splices compose:
		// spliced segments become (append seg rest), plain ones (cons val rest).
		Expr* build_qq_list(SourceLoc loc, std::vector<QqItem>& items, Expr* tail)
		{
			if (!tail && !has_splice(items))
			{
				return make_call(loc, "list", item_values(items));
			}

			Expr* result = tail ? tail : make_call(loc, "list", {});
			for (size_t i = items.size(); i-- > 0;)
			{
				result = make_call(loc, items[i].splice ? "append" : "cons", {items[i].value, result});
			}
			return result;
		}

		// A nested unquote is rebuilt as data. Its operand may itself splice, and then
		// the values land in the rebuilt form's tail: ,,@(list 1 2) is (unquote 1 2).
		Expr* parse_qq_reconstruct(SourceLoc loc, const char* tag, int depth)
		{
			if (depth == 1 && peek().kind == TokenKind::UnquoteSplicing)
			{
				advance();
				return make_call(loc, "cons", {make_symbol(loc, db.arena.copy_string(tag)), parse_expr()});
			}
			return make_tagged_datum(loc, tag, parse_quasiquote(depth));
		}

		// (unquote e), (unquote-splicing e) and (quasiquote t) mean exactly what the
		// reader marks mean; the caller has already consumed the opening paren.
		Expr* parse_qq_word_form(SourceLoc loc, int depth, bool* splice)
		{
			if (peek().kind != TokenKind::Variable)
			{
				return nullptr;
			}
			std::string_view word = peek().text;
			bool splicing = word == "unquote-splicing";
			bool unquoting = word == "unquote";
			bool nesting = word == "quasiquote";
			if (!splicing && !unquoting && !nesting)
			{
				return nullptr;
			}
			advance();

			Expr* result = nullptr;
			if (nesting)
			{
				result = make_tagged_datum(loc, "quasiquote", parse_quasiquote(depth + 1));
			}
			else if (depth == 1)
			{
				JETC_DIE_UNLESS(db, loc, !splicing || splice, "unquote-splicing outside of a list template");
				if (splicing)
				{
					*splice = true;
				}
				result = parse_expr();
			}
			else
			{
				result = parse_qq_reconstruct(loc, splicing ? "unquote-splicing" : "unquote", depth - 1);
			}
			expect(TokenKind::RParen);
			return result;
		}

		Expr* parse_qq_after_lparen(SourceLoc loc, int depth, bool* splice)
		{
			if (Expr* word = parse_qq_word_form(loc, depth, splice))
			{
				return word;
			}
			std::vector<QqItem> items;
			Expr* tail = nullptr;
			parse_items(depth, items, tail);
			expect(TokenKind::RParen);
			return build_qq_list(loc, items, tail);
		}

		// depth 0 is plain quoted data: no level to count, so nothing evaluates.
		Expr* parse_element(int depth)
		{
			return depth == 0 ? parse_datum() : parse_quasiquote(depth);
		}

		void parse_items(int depth, std::vector<QqItem>& items, Expr*& tail)
		{
			while (peek().kind != TokenKind::RParen)
			{
				if (peek().kind == TokenKind::Dot)
				{
					advance();
					JETC_DIE_UNLESS(db, peek().loc, !items.empty(), "dotted pair needs a head");
					tail = parse_element(depth);
					JETC_DIE_UNLESS(
						db,
						peek().loc,
						peek().kind == TokenKind::RParen,
						"extra tokens after dot in dotted pair"
						);
					return;
				}
				if (depth == 1 && peek().kind == TokenKind::UnquoteSplicing)
				{
					advance();
					items.push_back({parse_expr(), true});
					continue;
				}
				if (depth > 0 && peek().kind == TokenKind::LParen)
				{
					SourceLoc loc = advance().loc;
					bool splice = false;
					Expr* value = parse_qq_after_lparen(loc, depth, &splice);
					items.push_back({value, splice});
					continue;
				}
				items.push_back({parse_element(depth), false});
			}
		}

		// #(v ...) ==> (vector v ...), one call per HASH_FORMS entry
		// A splice among the elements makes it (apply vector <elements built as a list>).
		Expr* parse_hash_form(int depth)
		{
			const HashForm* form = find_hash_form(peek().text);
			SourceLoc loc = advance().loc;
			JETC_DIE_UNLESS(db, loc, form, "invalid # syntax");
			expect(TokenKind::LParen);

			std::vector<QqItem> items;
			Expr* tail = nullptr;
			parse_items(depth, items, tail);
			expect(TokenKind::RParen);
			JETC_DIE_UNLESS(db, loc, !tail, "dotted tail in a %s literal", form->constructor);

			if (has_splice(items))
			{
				Expr* e = make_expr(loc, ExprKind::Apply);
				e->apply.proc = make_expr(loc, ExprKind::VarRef);
				e->apply.proc->var_ref.name = db.arena.copy_string(form->constructor);
				e->apply.args = build_qq_list(loc, items, nullptr);
				return e;
			}
			if (form->is_key_value)
			{
				JETC_DIE_UNLESS(
					db,
					loc,
					items.size() % 2 == 0,
					"%s literal needs an even number of elements",
					form->constructor
					);
			}
			return make_call(loc, form->constructor, item_values(items));
		}

		// `t ==> the code that rebuilds t, where ,e evaluates e and ,@e splices it in.
		// Only depth 1 evaluates: a nested quasiquote raises the depth, and its own
		// unquotes are rebuilt as data.
		Expr* parse_quasiquote(int depth)
		{
			Token& tok = peek();
			SourceLoc loc = tok.loc;

			switch (tok.kind)
			{
				case TokenKind::Unquote:
					advance();
					if (depth == 1)
					{
						return parse_expr();
					}
					return parse_qq_reconstruct(loc, "unquote", depth - 1);

				case TokenKind::UnquoteSplicing:
					JETC_DIE_UNLESS(db, loc, depth > 1, "unquote-splicing outside of a list template");
					advance();
					return parse_qq_reconstruct(loc, "unquote-splicing", depth - 1);

				case TokenKind::Quasiquote:
					advance();
					return make_tagged_datum(loc, "quasiquote", parse_quasiquote(depth + 1));

				case TokenKind::Quote:
					advance();
					return make_tagged_datum(loc, "quote", parse_quasiquote(depth));

				case TokenKind::LParen:
					advance();
					return parse_qq_after_lparen(loc, depth, nullptr);

				case TokenKind::Hash:
					return parse_hash_form(depth);

				default:
					return parse_datum();
			}
		}

		// '(d ...) ==> (list d ...)
		// '(d . t) ==> (cons d 't)
		Expr* parse_quoted_list()
		{
			SourceLoc loc = peek().loc;
			expect(TokenKind::LParen);

			std::vector<QqItem> items;
			Expr* tail = nullptr;
			parse_items(0, items, tail);
			expect(TokenKind::RParen);
			return build_qq_list(loc, items, tail);
		}

	};

} // namespace

static std::vector<Token> lex(Compiler& db, IPort* port, uint32_t file_id)
{
	LexState state{port, db, file_id};
	state.lex_all();
	return std::move(state.tokens);
}

std::vector<Token>& Compiler::tokens()
{
	if (file_table.empty())
	{
		file_table.push_back(filename);
	}
	if (!tokens_)
	{
		IPortMem port{source};
		tokens_ = lex(*this, &port, 0);
	}
	return *tokens_;
}

Program& Compiler::ast()
{
	if (!ast_)
	{
		if (file_table.empty())
		{
			file_table.push_back(filename);
		}
		std::vector<Expr*> forms;
		auto&& parse = [&](std::vector<Token>& toks)
		{
			ParseState state{
				.tokens = toks,
				.db = *this,
			};
			while (!state.at_end())
			{
				forms.push_back(state.parse_expr());
			}
		};
		if (!prelude.empty())
		{
			uint32_t file_id = static_cast<uint32_t>(file_table.size());
			file_table.push_back("<prelude>");
			IPortMem port{prelude};
			std::vector<Token> prelude_tokens = lex(*this, &port, file_id);
			parse(prelude_tokens);
		}
		parse(tokens());
		ast_ = Program{arena.copy_slice(forms)};
		for (uint32_t i = 0; i < ast_->forms.size(); ++i)
		{
			ast_->forms[i] = expand(ast_->forms[i]);
		}
		// The loader pre-allocates one slot per collected toplevel name from the
		// bytecode header before any code runs.
		for (uint32_t i = 0; i < ast_->forms.size(); ++i)
		{
			ast_->forms[i] = rewrite_define_in(ast_->forms[i], toplevel_names_);
		}
		for (uint32_t i = 0; i < ast_->forms.size(); ++i)
		{
			ast_->forms[i] = compute_anf(ast_->forms[i]);
			verify_anf(ast_->forms[i]);
		}
		toplevel_lambda_ = make_expr({}, ExprKind::Lambda);
		toplevel_lambda_->lambda.params = {};
		toplevel_lambda_->lambda.is_variadic = false;
		toplevel_lambda_->lambda.body = {};
		toplevel_lambda_->lambda.lambda_name = {};
		resolve_bindings(*ast_);
		run_optimization_passes(*ast_);
		collect_tail_calls(*ast_);
		run_op_selection(*ast_);
	}
	return *ast_;
}

ResolvedBinding Compiler::binding(Expr* expr)
{
	ast();

	if (bindings_[expr->id].lambda != nullptr)
	{
		return bindings_[expr->id];
	}

	JETC_DIE(*this, expr->loc, "unresolved binding");
}

bool Compiler::is_tail(Expr* expr)
{
	ast();
	return tail_cache_[expr->id];
}

Expr* Compiler::make_expr(SourceLoc loc, ExprKind kind)
{
	Expr* e = arena.alloc<Expr>();
	e->kind = kind;
	e->id = next_expr_id_++;
	e->loc = loc;
	return e;
}

Expr* Compiler::make_boolean_lit(SourceLoc loc, bool value)
{
	Expr* e = make_expr(loc, ExprKind::BooleanLit);
	e->boolean_lit.value = value;
	return e;
}

template <typename F>
static void walk_children(Compiler& db, Expr* expr, F&& f)
{
	switch (expr->kind)
	{
		case ExprKind::NumberLit:
		case ExprKind::StringLit:
		case ExprKind::BooleanLit:
		case ExprKind::CharacterLit:
		case ExprKind::SymbolLit:
		case ExprKind::UnknownLit:
		case ExprKind::VarRef:
		case ExprKind::PrimRef:
			break;

		case ExprKind::Call:
			f(expr->call.proc);
			for (Expr*& arg : expr->call.args)
			{
				f(arg);
			}
			break;

		case ExprKind::Apply:
			f(expr->apply.proc);
			f(expr->apply.args);
			break;

		case ExprKind::Lambda:
			for (Expr*& form : expr->lambda.body)
			{
				f(form);
			}
			break;

		case ExprKind::Let:
			for (Expr*& val : expr->let.vals)
			{
				f(val);
			}
			for (Expr*& form : expr->let.body)
			{
				f(form);
			}
			break;

		case ExprKind::SetBang:
			f(expr->set_bang.value);
			break;

		case ExprKind::SetRef:
			f(expr->set_ref.obj);
			f(expr->set_ref.key);
			f(expr->set_ref.value);
			break;

		case ExprKind::IterNext:
			f(expr->iter_next.cursor);
			f(expr->iter_next.consequent);
			if (expr->iter_next.alternate)
			{
				f(expr->iter_next.alternate);
			}
			break;

		case ExprKind::If:
			f(expr->if_.test);
			f(expr->if_.consequent);
			if (expr->if_.alternate)
			{
				f(expr->if_.alternate);
			}
			break;

		default:
			JETC_DIE(db, expr->loc, "walk_children: unhandled ExprKind %d (not ANF?)",
			         static_cast<int>(expr->kind));
	}
}

Expr* Compiler::expand(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::Let:
			return expand_let(expr);

		case ExprKind::Letrec:
			return expand_letrec(expr);

		case ExprKind::Begin:
			return expand_begin(expr);

		case ExprKind::When:
		case ExprKind::Unless:
		{
			// (when test body ...)   ==> (if test (begin body ...))
			// (unless test body ...) ==> (if test <void> (begin body ...))
			bool is_when = expr->kind == ExprKind::When;
			Expr* test = expand(is_when ? expr->when.test : expr->unless.test);
			Expr* begin_e = make_expr(expr->loc, ExprKind::Begin);
			begin_e->begin.body = is_when ? expr->when.body : expr->unless.body;
			begin_e = expand_begin(begin_e);

			Expr* if_e = make_expr(expr->loc, ExprKind::If);
			if_e->if_.test = test;
			if_e->if_.consequent = is_when ? begin_e : make_expr(expr->loc, ExprKind::UnknownLit);
			if_e->if_.alternate = is_when ? nullptr : begin_e;
			return if_e;
		}

		case ExprKind::And:
		{
			// (and e1 e ...)
			//   ==> (if e1 (and e ...) #f)
			// (and) is #t and (and e) is e.
			Slice<Expr*>& exprs = expr->and_.exprs;
			if (exprs.empty())
			{
				return make_boolean_lit(expr->loc, true);
			}
			if (exprs.size() == 1)
			{
				return expand(exprs[0]);
			}

			Expr* result = expand(exprs[exprs.size() - 1]);
			for (int i = exprs.size() - 2; i >= 0; --i)
			{
				Expr* if_e = make_expr(exprs[i]->loc, ExprKind::If);
				if_e->if_.test = expand(exprs[i]);
				if_e->if_.consequent = result;
				if_e->if_.alternate = make_boolean_lit(expr->loc, false);
				result = if_e;
			}
			return result;
		}

		case ExprKind::Or:
		{
			// (or e1 e ...)
			//   ==> (let ((t e1)) (if t t (or e ...)))
			// (or) is #f and (or e) is e. The temp returns a true test value without
			// evaluating e1 twice.
			Slice<Expr*>& exprs = expr->or_.exprs;
			if (exprs.empty())
			{
				return make_boolean_lit(expr->loc, false);
			}
			if (exprs.size() == 1)
			{
				return expand(exprs[0]);
			}

			Expr* result = expand(exprs[exprs.size() - 1]);
			for (int i = exprs.size() - 2; i >= 0; --i)
			{
				std::string_view tmp_name = gensym();
				Expr* ref1 = make_expr(expr->loc, ExprKind::VarRef);
				ref1->var_ref.name = tmp_name;
				Expr* ref2 = make_expr(expr->loc, ExprKind::VarRef);
				ref2->var_ref.name = tmp_name;

				Expr* if_e = make_expr(exprs[i]->loc, ExprKind::If);
				if_e->if_.test = ref1;
				if_e->if_.consequent = ref2;
				if_e->if_.alternate = result;

				Expr* let_e = make_expr(expr->loc, ExprKind::Let);
				let_e->let.names = arena.copy_slice({tmp_name});
				let_e->let.vals = arena.copy_slice({expand(exprs[i])});
				let_e->let.body = arena.copy_slice({if_e});
				result = let_e;
			}
			return result;
		}

		case ExprKind::Cond:
		{
			// (cond (test body) clause ...)
			//   ==> (if test body (cond clause ...))
			// (cond (else body)) ==> body. (cond) is <void>, and so is falling off the
			// last clause with no else.
			Slice<Expr*>& clauses = expr->cond.clauses;
			if (clauses.empty())
			{
				return make_expr(expr->loc, ExprKind::UnknownLit);
			}
			Expr* result = nullptr;
			for (int i = clauses.size() - 2; i >= 0; i -= 2)
			{
				Expr* test = expand(clauses[i]);
				Expr* body = expand(clauses[i + 1]);
				if (test->kind == ExprKind::VarRef && test->var_ref.name == "else")
				{
					result = body;
					continue;
				}
				Expr* if_e = make_expr(test->loc, ExprKind::If);
				if_e->if_.test = test;
				if_e->if_.consequent = body;
				if_e->if_.alternate = result;
				result = if_e;
			}
			return result ? result : make_expr(expr->loc, ExprKind::UnknownLit);
		}

		case ExprKind::Call:
		case ExprKind::Apply:
		case ExprKind::SetBang:
		case ExprKind::SetRef:
		case ExprKind::IterNext:
		case ExprKind::If:
			walk_children(*this, expr, [&](Expr*& c) { c = expand(c); });
			return expr;

		case ExprKind::Lambda:
			walk_children(*this, expr, [&](Expr*& c) { c = expand(c); });
			expr->lambda.body = hoist_defines_in_body(expr->loc, expr->lambda.body);
			return expr;

		case ExprKind::Define:
			expr->define.value = expand(expr->define.value);
			return expr;
		default:
			return expr;
	}
}

Expr* Compiler::expand_let(Expr* expr)
{
	walk_children(*this, expr, [&](Expr*& c) { c = expand(c); });
	expr->let.body = hoist_defines_in_body(expr->loc, expr->let.body);
	return expr;
}

Expr* Compiler::expand_begin(Expr* expr)
{
	for (uint32_t i = 0; i < expr->begin.body.size(); ++i)
	{
		expr->begin.body[i] = expand(expr->begin.body[i]);
	}
	return expr;
}

// (define x e) ==> (set! x e), with x collected into names for the caller to bind
Expr* Compiler::rewrite_define_in(Expr* expr, OrderedNameSet& names)
{
	if (expr->kind == ExprKind::Define)
	{
		bool inserted = names.insert(expr->define.name);
		Expr* set_e = make_expr(expr->loc, ExprKind::SetBang);
		set_e->set_bang.name = expr->define.name;
		set_e->set_bang.value = expr->define.value;
		set_e->set_bang.is_init = inserted;
		return set_e;
	}
	// Begin is the only transparent sequence sugar sharing the enclosing scope,
	// so every other kind is treated as opaque.
	if (expr->kind == ExprKind::Begin)
	{
		for (uint32_t i = 0; i < expr->begin.body.size(); ++i)
		{
			expr->begin.body[i] = rewrite_define_in(expr->begin.body[i], names);
		}
	}
	return expr;
}

// form ... (define x e) form ...
//   ==> (let ((x #f)) form ... (set! x e) form ...)
// One let binds every name defined in the body, and running each set! in place
// of its define gives the body letrec* semantics.
Slice<Expr*> Compiler::hoist_defines_in_body(SourceLoc loc, Slice<Expr*> body)
{
	OrderedNameSet names;
	for (uint32_t i = 0; i < body.size(); ++i)
	{
		body[i] = rewrite_define_in(body[i], names);
	}
	if (names.ordered.empty())
	{
		return body;
	}

	uint32_t n = static_cast<uint32_t>(names.ordered.size());

	Expr** vals = arena.alloc_array<Expr*>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		vals[i] = make_boolean_lit(loc, false);
	}

	Expr* let_e = make_expr(loc, ExprKind::Let);
	let_e->let.names = arena.copy_slice(names.ordered);
	let_e->let.vals = {vals, n};
	let_e->let.body = body;

	return arena.copy_slice({let_e});
}

Expr* Compiler::expand_letrec(Expr* expr)
{
	// (letrec ((x e) ...) body ...)
	//   ==>
	// (let ((x #f) ...)
	//   (set! x e) ...
	//   body ...)
	// Sequential set!s give letrec* semantics, which is what almost all uses
	// of letrec actually want and matches what we accept for both keywords.
	uint32_t n = expr->let.names.size();

	Expr** sentinels = arena.alloc_array<Expr*>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		sentinels[i] = make_boolean_lit(expr->loc, false);
	}

	uint32_t body_n = n + expr->let.body.size();
	Expr** new_body = arena.alloc_array<Expr*>(body_n);
	for (uint32_t i = 0; i < n; ++i)
	{
		Expr* set_e = make_expr(expr->let.vals[i]->loc, ExprKind::SetBang);
		set_e->set_bang.name = expr->let.names[i];
		set_e->set_bang.value = expr->let.vals[i];
		set_e->set_bang.is_init = true;
		new_body[i] = set_e;
	}
	for (uint32_t i = 0; i < expr->let.body.size(); ++i)
	{
		new_body[n + i] = expr->let.body[i];
	}

	Expr* let_e = make_expr(expr->loc, ExprKind::Let);
	let_e->let.names = expr->let.names;
	let_e->let.vals = {sentinels, n};
	let_e->let.body = {new_body, body_n};

	return expand_let(let_e);
}

std::string_view Compiler::gensym()
{
	constexpr size_t max =
		sizeof("%t ") + std::numeric_limits<decltype(gensym_counter_)>::digits10 + 1;
	char* buf = static_cast<char*>(arena.alloc_raw(max, 1));
	// The embedded space cannot appear inside a lexed identifier, so a user
	// binding can never collide with a temp.
	int n = snprintf(buf, max, "%%t %u", gensym_counter_++);
	return {buf, static_cast<size_t>(n)};
}

// ANF hoist temps are single-use by construction.
static bool is_anf_temp(std::string_view name)
{
	return name.starts_with("%t ");
}

Expr* Compiler::anf_wrap(AnfBindings& bindings, Expr* body)
{
	// bindings[0] becomes the outermost Let: vals evaluate in the order they
	// were collected.
	for (size_t i = bindings.size(); i-- > 0;)
	{
		Expr* let_e = make_expr(bindings[i].second->loc, ExprKind::Let);
		let_e->let.names = arena.copy_slice({bindings[i].first});
		let_e->let.vals = arena.copy_slice({bindings[i].second});
		let_e->let.body = arena.copy_slice({body});
		body = let_e;
	}
	return body;
}

Expr* Compiler::anf_atomize(Expr* expr, AnfBindings& bindings)
{
	switch (expr->kind)
	{
		case ExprKind::NumberLit:
		case ExprKind::StringLit:
		case ExprKind::BooleanLit:
		case ExprKind::CharacterLit:
		case ExprKind::SymbolLit:
		case ExprKind::UnknownLit:
		case ExprKind::VarRef:
		case ExprKind::PrimRef:
			return expr;

		case ExprKind::Lambda:
			return compute_anf(expr);

		default:
		{
			std::string_view tmp = gensym();
			bindings.push_back({tmp, compute_anf(expr)});
			Expr* ref = make_expr(expr->loc, ExprKind::VarRef);
			ref->var_ref.name = tmp;
			return ref;
		}
	}
}

Expr* Compiler::compute_anf(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::NumberLit:
		case ExprKind::StringLit:
		case ExprKind::BooleanLit:
		case ExprKind::CharacterLit:
		case ExprKind::SymbolLit:
		case ExprKind::UnknownLit:
		case ExprKind::VarRef:
		case ExprKind::PrimRef:
			return expr;

		case ExprKind::Lambda:
		case ExprKind::Let:
			walk_children(*this, expr, [&](Expr*& c) { c = compute_anf(c); });
			return expr;

		case ExprKind::If:
		{
			AnfBindings bindings;
			expr->if_.test = anf_atomize(expr->if_.test, bindings);
			expr->if_.consequent = compute_anf(expr->if_.consequent);
			if (expr->if_.alternate)
			{
				expr->if_.alternate = compute_anf(expr->if_.alternate);
			}
			return anf_wrap(bindings, expr);
		}

		case ExprKind::Begin:
		{
			// (begin e1 e2 e3) -> (let ((t1 e1)) (let ((t2 e2)) e3))
			Slice<Expr*>& body = expr->begin.body;
			if (body.empty())
			{
				return make_expr(expr->loc, ExprKind::UnknownLit);
			}
			AnfBindings bindings;
			for (uint32_t i = 0; i + 1 < body.size(); ++i)
			{
				bindings.push_back({gensym(), compute_anf(body[i])});
			}
			return anf_wrap(bindings, compute_anf(body.back()));
		}

		case ExprKind::Call:
		{
			Expr* proc = expr->call.proc;
			if (proc->kind == ExprKind::Lambda
			    && !proc->lambda.is_variadic
			    && proc->lambda.params.size() == expr->call.args.size())
			{
				// ((lambda (x ...) body ...) e ...) -> (let ((x e) ...) body ...)
				Expr* let_e = make_expr(expr->loc, ExprKind::Let);
				let_e->let.names = proc->lambda.params;
				let_e->let.vals = expr->call.args;
				let_e->let.body = proc->lambda.body;
				return compute_anf(let_e);
			}
			AnfBindings bindings;
			expr->call.proc = anf_atomize(proc, bindings);
			for (uint32_t i = 0; i < expr->call.args.size(); ++i)
			{
				expr->call.args[i] = anf_atomize(expr->call.args[i], bindings);
			}
			return anf_wrap(bindings, expr);
		}

		case ExprKind::Apply:
		{
			AnfBindings bindings;
			expr->apply.proc = anf_atomize(expr->apply.proc, bindings);
			expr->apply.args = anf_atomize(expr->apply.args, bindings);
			return anf_wrap(bindings, expr);
		}

		case ExprKind::SetBang:
		{
			AnfBindings bindings;
			expr->set_bang.value = anf_atomize(expr->set_bang.value, bindings);
			return anf_wrap(bindings, expr);
		}

		case ExprKind::SetRef:
		{
			AnfBindings bindings;
			expr->set_ref.obj = anf_atomize(expr->set_ref.obj, bindings);
			expr->set_ref.key = anf_atomize(expr->set_ref.key, bindings);
			expr->set_ref.value = anf_atomize(expr->set_ref.value, bindings);
			return anf_wrap(bindings, expr);
		}

		case ExprKind::IterNext:
		{
			AnfBindings bindings;
			expr->iter_next.cursor = anf_atomize(expr->iter_next.cursor, bindings);
			expr->iter_next.consequent = compute_anf(expr->iter_next.consequent);
			if (expr->iter_next.alternate)
			{
				expr->iter_next.alternate = compute_anf(expr->iter_next.alternate);
			}
			return anf_wrap(bindings, expr);
		}

		default:
			JETC_DIE(*this, expr->loc, "anf: unhandled ExprKind %d (surface form not expanded?)",
			         static_cast<int>(expr->kind));
	}
}

void Compiler::verify_anf(Expr* expr)
{
	auto&& is_anf_atom = [](Expr* e) -> bool
	{
		switch (e->kind)
		{
			case ExprKind::NumberLit:
			case ExprKind::StringLit:
			case ExprKind::BooleanLit:
			case ExprKind::CharacterLit:
			case ExprKind::SymbolLit:
			case ExprKind::UnknownLit:
			case ExprKind::VarRef:
			case ExprKind::PrimRef:
			case ExprKind::Lambda:
				return true;
			default:
				return false;
		}
	};
	auto check_atom = [&](Expr* e)
	{
		JETC_DIE_UNLESS(
			*this,
			e->loc,
			is_anf_atom(e),
			"anf: non-atomic operand (kind %d)",
			static_cast<int>(e->kind)
			);
		verify_anf(e);
	};

	switch (expr->kind)
	{
		case ExprKind::Call:
		case ExprKind::Apply:
		case ExprKind::SetBang:
		case ExprKind::SetRef:
			walk_children(*this, expr, check_atom);
			break;

		case ExprKind::If:
			check_atom(expr->if_.test);
			verify_anf(expr->if_.consequent);
			if (expr->if_.alternate)
			{
				verify_anf(expr->if_.alternate);
			}
			break;

		case ExprKind::IterNext:
			check_atom(expr->iter_next.cursor);
			verify_anf(expr->iter_next.consequent);
			if (expr->iter_next.alternate)
			{
				verify_anf(expr->iter_next.alternate);
			}
			break;

		default:
			walk_children(*this, expr, [&](Expr* e) { verify_anf(e); });
			break;
	}
}

bool Compiler::prim_binding_lowerable(ResolvedBinding b, std::string_view prim)
{
	if (b.lambda != toplevel_lambda_)
	{
		return false;
	}
	LambdaBindings& tl = lambda_bindings_[toplevel_lambda_];
	if (get(tl.reassigned_after_init, b.breadth))
	{
		return false;
	}
	Expr* init = get(tl.bound_init, b.breadth);
	return init && init->kind == ExprKind::PrimRef && init->prim_ref.name == prim;
}

void Compiler::record_ref(ResolvedBinding b)
{
	// Codegen wires each transit lambda's clos to forward the Slot, so
	// every lambda between owner and the current scope needs an upvalue entry.
	if (lambdas_.back() == b.lambda)
	{
		return;
	}

	LambdaBindings& ob = lambda_bindings_[b.lambda];
	get(ob.captured, b.breadth) = true;
	if (!get(ob.is_initialized, b.breadth))
	{
		get(ob.captured_before_init, b.breadth) = true;
	}

	uint32_t bw = static_cast<uint32_t>(b.breadth);
	uint64_t key = binding_key(b);
	for (size_t i = lambdas_.size(); i-- > 0;)
	{
		Expr* lam = lambdas_[i];
		if (lam == b.lambda)
		{
			return;
		}
		if (LambdaBindings& lb = lambda_bindings_[lam]; lb.upvalue_keys.insert(key).second)
		{
			lb.upvalues.push_back({b.lambda, bw});
		}
	}
	JETC_DIE(*this, SourceLoc::none(), "record_ref: owner not in lambdas_");
}

void Compiler::record_set(ResolvedBinding b, bool is_init, Expr* value)
{
	LambdaBindings& lb = lambda_bindings_[b.lambda];
	if (!is_init || get(lb.is_initialized, b.breadth))
	{
		get(lb.reassigned_after_init, b.breadth) = true;
		return;
	}
	get(lb.is_initialized, b.breadth) = true;
	if (value)
	{
		get(lb.bound_init, b.breadth) = value;
	}
}

std::optional<ResolvedBinding> Compiler::lookup_name(std::string_view name)
{
	for (size_t i = lambdas_.size(); i-- > 0;)
	{
		std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_[i];
		if (auto it = idx.find(name); it != idx.end())
		{
			return ResolvedBinding{.lambda = lambdas_[i], .breadth = it->second};
		}
	}
	return std::nullopt;
}

void Compiler::push_lambda_scope(Expr* lambda)
{
	lambdas_.push_back(lambda);
	std::unordered_map<std::string_view, size_t> idx;
	Slice<std::string_view>& names = lambda->lambda.names;
	idx.reserve(names.size());
	for (size_t b = 0; b < names.size(); ++b)
	{
		idx[names[b]] = b;
	}
	lambda_name_index_.push_back(std::move(idx));
}

void Compiler::pop_lambda_scope()
{
	lambdas_.pop_back();
	lambda_name_index_.pop_back();
}

void Compiler::freeze_lambda(Expr* lambda)
{
	LambdaBindings& lb = lambda_bindings_[lambda];

	lambda->lambda.upvalues = arena.copy_slice(lb.upvalues);

	uint32_t n = lambda->lambda.names.size();
	bool* captured_data = arena.alloc_array<bool>(n);
	bool* captured_before_init_data = arena.alloc_array<bool>(n);
	bool* reassigned_data = arena.alloc_array<bool>(n);
	for (uint32_t i = 0; i < n; ++i)
	{
		captured_data[i] = get(lb.captured, i);
		// The is_initialized gate keeps never-initialized bindings (parameters) out:
		// their captures always copy the final value.
		captured_before_init_data[i] = get(lb.captured_before_init, i) && get(lb.is_initialized, i);
		reassigned_data[i] = get(lb.reassigned_after_init, i);
	}
	lambda->lambda.captured_locals = {captured_data, n};
	lambda->lambda.captured_before_init_locals = {captured_before_init_data, n};
	lambda->lambda.reassigned_after_init_locals = {reassigned_data, n};
}

void Compiler::compute_binding_addresses(Program& program)
{
	bindings_.assign(next_expr_id_ + 1, ResolvedBinding{});

	// `let` re-pushes during the walk; the seed stays defines-only.
	toplevel_lambda_->lambda.names = arena.copy_slice(toplevel_names_.ordered);
	push_lambda_scope(toplevel_lambda_);
	frame_names_.push_back({toplevel_lambda_->lambda.names.begin(), toplevel_lambda_->lambda.names.end()});
	for (Expr* form : program.forms)
	{
		compute_binding_addresses_in(form);
	}
	toplevel_lambda_->lambda.names = arena.copy_slice(frame_names_.back());
	pop_lambda_scope();
}

void Compiler::recompute_lambda_bindings(Program& program)
{
	lambda_bindings_.clear();
	all_lambdas_.clear();

	push_lambda_scope(toplevel_lambda_);
	for (Expr* form : program.forms)
	{
		recompute_lambda_bindings_in(form);
	}
	pop_lambda_scope();
	all_lambdas_.push_back(toplevel_lambda_);
}

void Compiler::resolve_bindings(Program& program)
{
	compute_binding_addresses(program);
	recompute_lambda_bindings(program);
}

void Compiler::collect_binding_uses_in(Expr* expr)
{
	if (expr->kind == ExprKind::VarRef || expr->kind == ExprKind::SetBang)
	{
		if (ResolvedBinding b = bindings_[expr->id]; b.lambda)
		{
			++binding_use_counts_[binding_key(b)];
		}
	}
	walk_children(*this, expr, [&](Expr* e) { collect_binding_uses_in(e); });
}

void Compiler::collect_binding_uses(Program& program)
{
	binding_use_counts_.clear();
	for (Expr* form : program.forms)
	{
		collect_binding_uses_in(form);
	}
}

uint32_t Compiler::binding_use_count(Expr* owner, uint32_t breadth)
{
	auto found = binding_use_counts_.find(binding_key({.lambda = owner, .breadth = breadth}));
	return found == binding_use_counts_.end() ? 0 : found->second;
}

bool Compiler::binding_used(Expr* owner, uint32_t breadth)
{
	return binding_use_count(owner, breadth) != 0;
}

void Compiler::run_optimization_passes(Program& program)
{
	if (flags_.inlining)
	{
		run_anf_inline(program);
		recompute_lambda_bindings(program);
	}

	if (flags_.specialize_ops)
	{
		run_binarize(program);
		recompute_lambda_bindings(program);
	}

	if (flags_.lift_lambdas)
	{
		run_lambda_lift(program);
		resolve_bindings(program);
	}
}

void Compiler::compute_binding_addresses_in(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::VarRef:
		{
			std::optional<ResolvedBinding> found = lookup_name(expr->var_ref.name);
			if (!found)
			{
				JETC_DIE(*this, expr->loc, "unresolved variable '%.*s'",
				         static_cast<int>(expr->var_ref.name.size()), expr->var_ref.name.data());
			}
			bindings_[expr->id] = *found;
			break;
		}

		case ExprKind::Lambda:
		{
			std::vector<std::string_view> names;
			for (std::string_view param : expr->lambda.params)
			{
				names.push_back(param);
			}
			expr->lambda.names = arena.copy_slice(names);

			push_lambda_scope(expr);
			frame_names_.push_back(std::move(names));
			walk_children(*this, expr, [&](Expr* e) { compute_binding_addresses_in(e); });
			expr->lambda.names = arena.copy_slice(frame_names_.back());
			frame_names_.pop_back();
			pop_lambda_scope();
			break;
		}

		case ExprKind::Let:
		{
			for (Expr* val : expr->let.vals)
			{
				compute_binding_addresses_in(val);
			}

			std::vector<std::pair<std::string_view, std::optional<size_t>>> shadowed;
			{
				std::vector<std::string_view>& frame = frame_names_.back();
				std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_.back();
				expr->let.slot_base = static_cast<uint32_t>(frame.size());
				expr->let.owner = lambdas_.back();
				for (std::string_view name : expr->let.names)
				{
					size_t breadth = frame.size();
					frame.push_back(name);
					auto it = idx.find(name);
					shadowed.push_back({name, it == idx.end()
					                    ? std::nullopt
					                    : std::optional<size_t>(it->second)});
					idx[name] = breadth;
				}
			}

			for (Expr* form : expr->let.body)
			{
				compute_binding_addresses_in(form);
			}

			// A lambda in the body pushes lambda_name_index_, which can
			// reallocate it: no reference to it may survive the body walk.
			std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_.back();
			for (auto it = shadowed.rbegin(); it != shadowed.rend(); ++it)
			{
				if (it->second)
				{
					idx[it->first] = *it->second;
				}
				else
				{
					idx.erase(it->first);
				}
			}
			break;
		}

		case ExprKind::IterNext:
		{
			compute_binding_addresses_in(expr->iter_next.cursor);
			std::vector<std::pair<std::string_view, std::optional<size_t>>> shadowed;
			{
				std::vector<std::string_view>& frame = frame_names_.back();
				std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_.back();
				expr->iter_next.slot_base = static_cast<uint32_t>(frame.size());
				expr->iter_next.owner = lambdas_.back();
				for (std::string_view name : expr->iter_next.names)
				{
					size_t breadth = frame.size();
					frame.push_back(name);
					auto it = idx.find(name);
					shadowed.push_back({name, it == idx.end()
					                    ? std::nullopt
					                    : std::optional<size_t>(it->second)});
					idx[name] = breadth;
				}
			}
			compute_binding_addresses_in(expr->iter_next.consequent);
			std::unordered_map<std::string_view, size_t>& idx = lambda_name_index_.back();
			for (auto it = shadowed.rbegin(); it != shadowed.rend(); ++it)
			{
				if (it->second)
				{
					idx[it->first] = *it->second;
				}
				else
				{
					idx.erase(it->first);
				}
			}
			if (expr->iter_next.alternate)
			{
				compute_binding_addresses_in(expr->iter_next.alternate);
			}
			break;
		}

		case ExprKind::SetBang:
		{
			compute_binding_addresses_in(expr->set_bang.value);
			std::optional<ResolvedBinding> found = lookup_name(expr->set_bang.name);
			if (!found)
			{
				JETC_DIE(*this, expr->loc, "unresolved variable '%.*s' in set!",
				         static_cast<int>(expr->set_bang.name.size()), expr->set_bang.name.data());
			}
			bindings_[expr->id] = *found;
			break;
		}

		default:
			walk_children(*this, expr, [&](Expr* e) { compute_binding_addresses_in(e); });
			break;
	}
}

void Compiler::recompute_lambda_bindings_in(Expr* expr)
{
	switch (expr->kind)
	{
		case ExprKind::VarRef:
			if (!get(intrinsic_callee_, expr->id))
			{
				record_ref(bindings_[expr->id]);
			}
			break;

		case ExprKind::Lambda:
			push_lambda_scope(expr);
			walk_children(*this, expr, [&](Expr* e) { recompute_lambda_bindings_in(e); });
			pop_lambda_scope();
			all_lambdas_.push_back(expr);
			break;

		case ExprKind::SetBang:
		{
			recompute_lambda_bindings_in(expr->set_bang.value);
			ResolvedBinding b = bindings_[expr->id];
			record_ref(b);
			record_set(b, expr->set_bang.is_init, expr->set_bang.value);
			break;
		}

		default:
			walk_children(*this, expr, [&](Expr* e) { recompute_lambda_bindings_in(e); });
			break;
	}
}

void Compiler::collect_tail_calls(Program& program)
{
	tail_cache_.assign(next_expr_id_ + 1, false);
	for (Expr* form : program.forms)
	{
		collect_tail_calls(form, false);
	}
}

void Compiler::collect_tail_calls(Expr* expr, bool in_tail)
{
	switch (expr->kind)
	{
		case ExprKind::Call:
			if (in_tail)
			{
				tail_cache_[expr->id] = true;
			}
			walk_children(*this, expr, [&](Expr* e) { collect_tail_calls(e, false); });
			break;

		case ExprKind::Lambda:
			for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
			{
				bool is_last = (i == expr->lambda.body.size() - 1);
				collect_tail_calls(expr->lambda.body[i], is_last);
			}
			break;

		case ExprKind::If:
			collect_tail_calls(expr->if_.test, false);
			collect_tail_calls(expr->if_.consequent, in_tail);
			if (expr->if_.alternate)
			{
				collect_tail_calls(expr->if_.alternate, in_tail);
			}
			break;

		case ExprKind::Let:
			for (Expr* val : expr->let.vals)
			{
				collect_tail_calls(val, false);
			}
			for (uint32_t i = 0; i < expr->let.body.size(); ++i)
			{
				bool is_last = (i == expr->let.body.size() - 1);
				collect_tail_calls(expr->let.body[i], is_last && in_tail);
			}
			break;

		case ExprKind::IterNext:
			collect_tail_calls(expr->iter_next.cursor, false);
			collect_tail_calls(expr->iter_next.consequent, in_tail);
			if (expr->iter_next.alternate)
			{
				collect_tail_calls(expr->iter_next.alternate, in_tail);
			}
			break;

		default:
			walk_children(*this, expr, [&](Expr* e) { collect_tail_calls(e, false); });
			break;
	}
}

namespace
{

	bool needs_slot(Expr* owner, uint32_t breadth)
	{
		return owner->lambda.captured_locals[breadth]
		       && (owner->lambda.reassigned_after_init_locals[breadth]
		           || owner->lambda.captured_before_init_locals[breadth]);
	}

	std::optional<uint16_t> find_upvalue(Expr* current, Expr* owner, uint32_t breadth)
	{
		Slice<UpvalueRef>& ups = current->lambda.upvalues;
		for (uint32_t i = 0; i < ups.size(); ++i)
		{
			if (ups[i].owner == owner && ups[i].breadth == breadth)
			{
				return static_cast<uint16_t>(i);
			}
		}
		return std::nullopt;
	}

	bool is_literal_key(Expr* e)
	{
		switch (e->kind)
		{
			case ExprKind::NumberLit:
			case ExprKind::SymbolLit:
			case ExprKind::CharacterLit:
			case ExprKind::BooleanLit:
			case ExprKind::StringLit:
				return true;
			default:
				return false;
		}
	}

	template <typename T>
	T narrow_or_die(Compiler& db, SourceLoc loc, size_t v)
	{
		JETC_DIE_WHEN(
			db,
			loc,
			v > std::numeric_limits<T>::max(),
			"codegen: value %zu overflows a narrower field",
			v
			);
		return static_cast<T>(v);
	}

	std::optional<Opcode> binary_arith_opcode(std::string_view name)
	{
		if (name == "-")
		{
			return Opcode::sub;
		}
		if (name == "+")
		{
			return Opcode::add;
		}
		if (name == "*")
		{
			return Opcode::mul;
		}
		if (name == "/")
		{
			return Opcode::div;
		}
		if (name == "=")
		{
			return Opcode::numeq;
		}
		if (name == "eq?")
		{
			return Opcode::eq;
		}
		if (name == "<")
		{
			return Opcode::lt;
		}
		if (name == "<=")
		{
			return Opcode::le;
		}
		if (name == ">")
		{
			return Opcode::gt;
		}
		if (name == ">=")
		{
			return Opcode::ge;
		}
		return std::nullopt;
	}

} // namespace

void Compiler::run_op_selection(Program& program)
{
	// Remove references to callees that will be lowered to intrinsics to avoid classifying them
	// as upvalues during instruction selection.
	intrinsic_callee_.assign(next_expr_id_ + 1, false);
	for (Expr* form : program.forms)
	{
		collect_intrinsic_callees(form, toplevel_lambda_);
	}
	recompute_lambda_bindings(program);
	collect_binding_uses(program);

	for (Expr* L : all_lambdas_)
	{
		freeze_lambda(L);
	}

	collect_branch_fusion_facts(program);
	selected_ops_.assign(next_expr_id_ + 1, std::nullopt);
	for (Expr* form : program.forms)
	{
		select_ops_in(form, toplevel_lambda_);
	}
	select_branch_fusions();
}

void Compiler::select_ops_in(Expr* expr, Expr* current)
{
	switch (expr->kind)
	{
		case ExprKind::VarRef:
			select_var_op(expr, current, false);
			break;

		case ExprKind::Call:
			if (get(intrinsic_callee_, expr->call.proc->id))
			{
				for (Expr* arg : expr->call.args)
				{
					select_ops_in(arg, current);
				}
			}
			else
			{
				walk_children(*this, expr, [&](Expr* e) { select_ops_in(e, current); });
			}
			select_call_op(expr, current);
			break;

		case ExprKind::Lambda:
			walk_children(*this, expr, [&](Expr* e) { select_ops_in(e, expr); });
			break;

		case ExprKind::SetBang:
			select_ops_in(expr->set_bang.value, current);
			select_var_op(expr, current, true);
			break;

		case ExprKind::SetRef:
			walk_children(*this, expr, [&](Expr* e) { select_ops_in(e, current); });
			select_field_op(expr, current, expr->set_ref.obj, expr->set_ref.key, Opcode::stf, Opcode::stfk);
			break;

		case ExprKind::IterNext:
		{
			walk_children(*this, expr, [&](Expr* e) { select_ops_in(e, current); });
			OpSelection& sel = selected_ops_[expr->id].emplace();
			sel.op = expr->iter_next.names.size() == 1 ? Opcode::iter_next1 : Opcode::iter_next2;
			break;
		}

		default:
			walk_children(*this, expr, [&](Expr* e) { select_ops_in(e, current); });
			break;
	}
}

bool Compiler::is_self_tail_call(Expr* expr, Expr* current)
{
	if (expr->kind != ExprKind::Call)
	{
		return false;
	}
	Expr* proc = expr->call.proc;
	if (proc->kind != ExprKind::VarRef)
	{
		return false;
	}
	if (!is_tail(expr))
	{
		return false;
	}
	if (current->lambda.is_variadic)
	{
		return false;
	}
	if (current->lambda.params.size() != expr->call.args.size())
	{
		return false;
	}
	ResolvedBinding proc_binding = binding(proc);
	LambdaBindings& lb = lambda_bindings_[proc_binding.lambda];
	return !get(lb.reassigned_after_init, proc_binding.breadth)
	       && get(lb.bound_init, proc_binding.breadth) == current;
}

Compiler::PrimLowering Compiler::prim_call_lowering(Expr* call)
{
	if (!flags_.specialize_ops)
	{
		return {};
	}
	Expr* proc = call->call.proc;
	if (proc->kind == ExprKind::PrimRef)
	{
		if (proc->prim_ref.name == REF_HOLE_PRIM && call->call.args.size() == 2)
		{
			return {PrimLowering::Kind::Ref, Opcode::ldfh, Opcode::ldfkh};
		}
		if (proc->prim_ref.name == REF_DEFAULT_PRIM && call->call.args.size() == 3)
		{
			return {PrimLowering::Kind::Ref, Opcode::ldfo, Opcode::ldfko};
		}
		return {};
	}
	if (proc->kind != ExprKind::VarRef)
	{
		return {};
	}
	std::string_view name = proc->var_ref.name;
	if (name == "truncate" && call->call.args.size() == 1 && prim_binding_lowerable(binding(proc), name))
	{
		return {PrimLowering::Kind::Unary, Opcode::trunc};
	}
	if (call->call.args.size() != 2)
	{
		return {};
	}
	std::optional<Opcode> arith = binary_arith_opcode(name);
	if (!arith && name != "ref")
	{
		return {};
	}
	if (!prim_binding_lowerable(binding(proc), name))
	{
		return {};
	}
	if (arith)
	{
		return {PrimLowering::Kind::Arith, *arith};
	}
	return {PrimLowering::Kind::Ref, Opcode::ldf, Opcode::ldfk};
}

bool Compiler::is_intrinsic_callee(Expr* expr, Expr* current)
{
	if (!flags_.specialize_ops)
	{
		return false;
	}
	if (is_self_tail_call(expr, current))
	{
		return true;
	}
	return prim_call_lowering(expr).kind != PrimLowering::Kind::None;
}

void Compiler::collect_intrinsic_callees(Expr* expr, Expr* current)
{
	if (expr->kind == ExprKind::Call && is_intrinsic_callee(expr, current))
	{
		get(intrinsic_callee_, expr->call.proc->id) = true;
	}
	if (expr->kind == ExprKind::Lambda)
	{
		current = expr;
	}
	walk_children(*this, expr, [&](Expr* e) { collect_intrinsic_callees(e, current); });
}

void Compiler::select_call_op(Expr* expr, Expr* current)
{
	Expr* proc = expr->call.proc;
	OpSelection& sel = selected_ops_[expr->id].emplace();
	sel.op = Opcode::call;

	if (proc->kind == ExprKind::PrimRef && proc->prim_ref.name == RESET_PRIM)
	{
		JETC_DIE_UNLESS(
			*this,
			expr->loc,
			expr->call.args.size() == 1,
			"%.*s expects exactly one argument",
			static_cast<int>(RESET_PRIM.size()),
			RESET_PRIM.data()
			);
		sel.op = Opcode::reset;
		return;
	}

	if (proc->kind == ExprKind::PrimRef && proc->prim_ref.name == CORO_PRIM)
	{
		JETC_DIE_UNLESS(
			*this,
			expr->loc,
			expr->call.args.size() == 1,
			"%.*s expects exactly one argument",
			static_cast<int>(CORO_PRIM.size()),
			CORO_PRIM.data()
			);
		sel.op = Opcode::coro;
		return;
	}

	if (!flags_.specialize_ops)
	{
		return;
	}

	if (is_self_tail_call(expr, current))
	{
		sel.op = Opcode::call_self_tail;
		return;
	}

	if (proc->kind != ExprKind::VarRef)
	{
		return;
	}

	ResolvedBinding proc_binding = binding(proc);

	PrimLowering pl = prim_call_lowering(expr);
	if (pl.kind == PrimLowering::Kind::Unary)
	{
		sel.op = pl.op;
		return;
	}

	// Two-arg arithmetic: rr, or rk when the rhs is a number literal.
	if (pl.kind == PrimLowering::Kind::Arith)
	{
		Opcode op = pl.op;
		if (expr->call.args[1]->kind == ExprKind::NumberLit
		    || (op == Opcode::eq && is_literal_key(expr->call.args[1])))
		{
			switch (op)
			{
				case Opcode::sub: op = Opcode::subk; break;
				case Opcode::add: op = Opcode::addk; break;
				case Opcode::mul: op = Opcode::mulk; break;
				case Opcode::div: op = Opcode::divk; break;
				case Opcode::numeq:  op = Opcode::numeqk;  break;
				case Opcode::eq:     op = Opcode::eqk;     break;
				case Opcode::lt:  op = Opcode::ltk;  break;
				default:          break;   // no rk form for le/gt/ge
			}
		}
		sel.op = op;
		return;
	}

	if (pl.kind == PrimLowering::Kind::Ref)
	{
		select_field_op(expr, current, expr->call.args[0], expr->call.args[1], pl.op, pl.op_k);
		return;
	}

	// Callee whose init value is this very closure and is never reassigned: the
	// binding always holds the closure we're running, so call frame->closure
	// directly
	{
		if (LambdaBindings& lb = lambda_bindings_[proc_binding.lambda];
		    !get(lb.reassigned_after_init, proc_binding.breadth)
		    && get(lb.bound_init, proc_binding.breadth) == current)
		{
			sel.op = Opcode::call_self_0;
			return;
		}
	}

	bool slot = needs_slot(proc_binding.lambda, static_cast<uint32_t>(proc_binding.breadth));

	// Callee in a boxed binding: slot IC.
	if (proc_binding.lambda != current && slot)
	{
		std::optional<uint16_t> found = find_upvalue(current, proc_binding.lambda,
		                                             static_cast<uint32_t>(proc_binding.breadth));
		JETC_DIE_UNLESS(*this, expr->loc, found, "codegen: cacheable call missing upvalue entry");
		sel.op = Opcode::call_upval_slot_0;
		sel.u.call_ic_slot.upvalue_idx = *found;
		return;
	}

	// Callee in an unboxed binding: direct IC.
	if (!slot)
	{
		if (proc_binding.lambda == current)
		{
			sel.op = Opcode::call_local_0;
			sel.u.call_ic_atom.idx = static_cast<uint16_t>(proc_binding.breadth);
		}
		else
		{
			std::optional<uint16_t> found = find_upvalue(current, proc_binding.lambda,
			                                             static_cast<uint32_t>(proc_binding.breadth));
			JETC_DIE_UNLESS(*this, expr->loc, found, "codegen: cacheable call missing upvalue entry");
			sel.op = Opcode::call_upval_0;
			sel.u.call_ic_atom.idx = *found;
		}
	}
}

void Compiler::collect_branch_fusion_facts(Program& program)
{
	branch_fusions_.clear();
	struct Candidate
	{
		Expr* producer;
		Expr* branch = nullptr;
		uint32_t uses;
	};
	std::unordered_map<uint64_t, Candidate> candidates;
	auto&& collect_candidates = [&](Expr* expr, auto&& self) -> void
	{
		if (expr->kind == ExprKind::Let)
		{
			for (uint32_t i = 0; i < expr->let.names.size(); ++i)
			{
				if (!is_anf_temp(expr->let.names[i]))
				{
					continue;
				}
				Expr* value = expr->let.vals[i];
				while (value->kind == ExprKind::Let && value->let.body.size() == 1)
				{
					value = value->let.body[0];
				}
				if (value->kind == ExprKind::Call)
				{
					ResolvedBinding binding{.lambda = expr->let.owner, .breadth = expr->let.slot_base + i};
					uint32_t uses = binding_use_count(binding.lambda, binding.breadth);
					candidates.emplace(binding_key(binding), Candidate{value, nullptr, uses});
				}
			}
		}
		walk_children(*this, expr, [&](Expr* child) { self(child, self); });
	};
	for (Expr* form : program.forms)
	{
		collect_candidates(form, collect_candidates);
	}
	auto&& candidate_for = [&](Expr* expr) -> Candidate*
	{
		ResolvedBinding binding = bindings_[expr->id];
		if (!binding.lambda)
		{
			return nullptr;
		}
		auto found = candidates.find(binding_key(binding));
		return found == candidates.end() ? nullptr : &found->second;
	};
	auto&& collect_branches = [&](Expr* expr, auto&& self) -> void
	{
		if (expr->kind == ExprKind::If && expr->if_.test->kind == ExprKind::VarRef)
		{
			if (Candidate* candidate = candidate_for(expr->if_.test); candidate)
			{
				candidate->branch = expr;
			}
		}
		walk_children(*this, expr, [&](Expr* child) { self(child, self); });
	};
	for (Expr* form : program.forms)
	{
		collect_branches(form, collect_branches);
	}
	for (const std::pair<const uint64_t, Candidate>& entry : candidates)
	{
		if (const Candidate& candidate = entry.second; candidate.uses == 1 && candidate.branch)
		{
			branch_fusions_[candidate.branch->id] = candidate.producer;
		}
	}
}

void Compiler::select_branch_fusions()
{
	for (auto it = branch_fusions_.begin(); it != branch_fusions_.end();)
	{
		Expr* comparison = it->second;
		Opcode fused;
		switch (selected_ops_[comparison->id]->op)
		{
			case Opcode::numeq:  fused = Opcode::if_numeq;  break;
			case Opcode::eq:     fused = Opcode::if_eq;      break;
			case Opcode::lt:     fused = Opcode::if_lt;      break;
			case Opcode::le:     fused = Opcode::if_le;      break;
			case Opcode::gt:     fused = Opcode::if_gt;      break;
			case Opcode::ge:     fused = Opcode::if_ge;      break;
			case Opcode::numeqk: fused = Opcode::if_numeqk; break;
			case Opcode::eqk:    fused = Opcode::if_eqk;     break;
			case Opcode::ltk:    fused = Opcode::if_ltk;     break;
			default:
				it = branch_fusions_.erase(it);
				continue;
		}
		selected_ops_[comparison->id]->op = fused;
		++it;
	}
}

void Compiler::select_field_op(Expr* expr, Expr* current, Expr* receiver, Expr* key, Opcode reg_op,
                               Opcode const_op)
{
	OpSelection& sel = selected_ops_[expr->id].emplace();

	if (!flags_.specialize_ops)
	{
		sel.op = reg_op;
		return;
	}

	sel.op = is_literal_key(key) ? const_op : reg_op;
}

void Compiler::select_var_op(Expr* expr, Expr* current, bool is_set)
{
	ResolvedBinding b = binding(expr);
	bool slot = needs_slot(b.lambda, static_cast<uint32_t>(b.breadth));
	OpSelection& sel = selected_ops_[expr->id].emplace();
	if (b.lambda == current)
	{
		// mov marks a plain register access: refs read the register directly
		// (no code), sets write it.
		sel.op = slot ? (is_set ? Opcode::std : Opcode::ldd) : Opcode::mov;
		sel.u.var.addr = narrow_or_die<uint16_t>(*this, expr->loc, b.breadth);
		return;
	}
	std::optional<uint16_t> found = find_upvalue(current, b.lambda, static_cast<uint32_t>(b.breadth));
	std::string_view name = expr->kind == ExprKind::SetBang ? expr->set_bang.name : expr->var_ref.name;
	JETC_DIE_UNLESS(
		*this,
		expr->loc,
		found,
		"select-pass: ref to non-local without upvalue entry: '%.*s'",
		static_cast<int>(name.size()),
		name.data()
		);
	sel.op = is_set ? Opcode::stu : (slot ? Opcode::ldus : Opcode::ldu);
	sel.u.var.addr = *found;
}

namespace
{

	// Counts post-ANF nodes: every non-atomic operand costs an extra
	// Let + VarRef pair on top of the expression itself.
	constexpr uint32_t INLINE_BUDGET = 32;

	uint32_t count_exprs(Expr* e);

	uint32_t count_exprs_slice(Slice<Expr*> body)
	{
		uint32_t n = 0;
		for (uint32_t i = 0; i < body.size(); ++i)
		{
			n += count_exprs(body[i]);
		}
		return n;
	}

	uint32_t count_exprs(Expr* e)
	{
		if (!e)
		{
			return 0;
		}
		uint32_t n = 1;
		switch (e->kind)
		{
			case ExprKind::Call:
				n += count_exprs(e->call.proc);
				n += count_exprs_slice(e->call.args);
				break;
			case ExprKind::Apply:
				n += count_exprs(e->apply.proc);
				n += count_exprs(e->apply.args);
				break;
			case ExprKind::If:
				n += count_exprs(e->if_.test);
				n += count_exprs(e->if_.consequent);
				n += count_exprs(e->if_.alternate);
				break;
			case ExprKind::Lambda:
				n += count_exprs_slice(e->lambda.body);
				break;
			case ExprKind::SetBang:
				n += count_exprs(e->set_bang.value);
				break;
			case ExprKind::SetRef:
				n += count_exprs(e->set_ref.obj);
				n += count_exprs(e->set_ref.key);
				n += count_exprs(e->set_ref.value);
				break;
			case ExprKind::IterNext:
				n += count_exprs(e->iter_next.cursor);
				n += count_exprs(e->iter_next.consequent);
				n += count_exprs(e->iter_next.alternate);
				break;
			case ExprKind::Let:
				n += count_exprs_slice(e->let.vals);
				n += count_exprs_slice(e->let.body);
				break;
			default:
				break;
		}
		return n;
	}

	struct AnfInline
	{
		Compiler& db;
		// A hit means the binding provably holds this init wherever it is
		// referenced, so a call may splice it and a reference may become it.
		std::unordered_map<uint64_t, Expr*> lambda_cands{};
		std::unordered_map<uint64_t, Expr*> const_cands{};
		std::unordered_set<Expr*> candidate_lambdas{};
		// Candidates being spliced or walked at their own definition: calls
		// to them stay calls, so splicing terminates and a recursive body is
		// never unrolled into itself.
		std::unordered_set<Expr*> active{};
		std::vector<Expr*> hosts{};
		// Ids at or above this are clones this pass made. A cloned Let has no
		// row in the pre-pass analysis, so its locals' write flags are
		// unknowable and it never registers candidates.
		uint32_t first_clone_id;

		void consider(Expr* owner, size_t breadth, Expr* init)
		{
			if (init->kind == ExprKind::Lambda)
			{
				if (init->lambda.is_variadic || count_exprs_slice(init->lambda.body) > INLINE_BUDGET)
				{
					return;
				}
				lambda_cands[binding_key({owner, breadth})] = init;
				candidate_lambdas.insert(init);
			}
			else if (init->kind == ExprKind::NumberLit || init->kind == ExprKind::BooleanLit ||
			         init->kind == ExprKind::CharacterLit)
			{
				const_cands[binding_key({owner, breadth})] = init;
			}
		}

		struct CloneCtx
		{
			Expr* callee;
			Expr* host;
			uint32_t base;
			std::vector<std::pair<Expr*, Expr*>> lambda_stack;
		};

		ResolvedBinding translate(ResolvedBinding rb, CloneCtx& ctx)
		{
			if (rb.lambda == ctx.callee)
			{
				return {ctx.host, ctx.base + rb.breadth};
			}
			for (auto it = ctx.lambda_stack.rbegin(); it != ctx.lambda_stack.rend(); ++it)
			{
				if (it->first == rb.lambda)
				{
					rb.lambda = it->second;
					return rb;
				}
			}
			// A free variable of the whole spliced body: its scope encloses
			// the callee's definition and therefore every call site that can
			// reference the callee, so the binding is valid unchanged.
			return rb;
		}

		Slice<Expr*> clone_slice(Slice<Expr*> src, CloneCtx& ctx)
		{
			Expr** data = db.arena.alloc_array<Expr*>(src.size());
			for (uint32_t i = 0; i < src.size(); ++i)
			{
				data[i] = clone(src[i], ctx);
			}
			return {data, src.size()};
		}

		Expr* clone(Expr* orig, CloneCtx& ctx)
		{
			Expr* e = db.make_expr(orig->loc, orig->kind);
			switch (orig->kind)
			{
				case ExprKind::NumberLit:
					e->number_lit = orig->number_lit;
					break;
				case ExprKind::StringLit:
					e->string_lit = orig->string_lit;
					break;
				case ExprKind::BooleanLit:
					e->boolean_lit = orig->boolean_lit;
					break;
				case ExprKind::CharacterLit:
					e->character_lit = orig->character_lit;
					break;
				case ExprKind::SymbolLit:
					e->symbol_lit = orig->symbol_lit;
					break;
				case ExprKind::UnknownLit:
					e->unknown_lit = orig->unknown_lit;
					break;
				case ExprKind::PrimRef:
					e->prim_ref = orig->prim_ref;
					break;
				case ExprKind::VarRef:
					e->var_ref = orig->var_ref;
					get(db.bindings_, e->id) = translate(get(db.bindings_, orig->id), ctx);
					break;
				case ExprKind::Call:
					e->call.proc = clone(orig->call.proc, ctx);
					e->call.args = clone_slice(orig->call.args, ctx);
					break;
				case ExprKind::Apply:
					e->apply.proc = clone(orig->apply.proc, ctx);
					e->apply.args = clone(orig->apply.args, ctx);
					break;
				case ExprKind::Lambda:
					e->lambda.params = orig->lambda.params;
					e->lambda.is_variadic = orig->lambda.is_variadic;
					e->lambda.lambda_name = orig->lambda.lambda_name;
					e->lambda.names = orig->lambda.names;
					e->lambda.captured_locals = {};
					e->lambda.captured_before_init_locals = {};
					e->lambda.reassigned_after_init_locals = {};
					e->lambda.upvalues = {};
					ctx.lambda_stack.push_back({orig, e});
					e->lambda.body = clone_slice(orig->lambda.body, ctx);
					ctx.lambda_stack.pop_back();
					break;
				case ExprKind::SetBang:
					e->set_bang.name = orig->set_bang.name;
					e->set_bang.is_init = orig->set_bang.is_init;
					e->set_bang.value = clone(orig->set_bang.value, ctx);
					get(db.bindings_, e->id) = translate(get(db.bindings_, orig->id), ctx);
					break;
				case ExprKind::SetRef:
					e->set_ref.obj = clone(orig->set_ref.obj, ctx);
					e->set_ref.key = clone(orig->set_ref.key, ctx);
					e->set_ref.value = clone(orig->set_ref.value, ctx);
					break;
				case ExprKind::IterNext:
					e->iter_next.cursor = clone(orig->iter_next.cursor, ctx);
					e->iter_next.names = orig->iter_next.names;
					e->iter_next.consequent = clone(orig->iter_next.consequent, ctx);
					e->iter_next.alternate = orig->iter_next.alternate
					                         ? clone(orig->iter_next.alternate, ctx)
					                         : nullptr;
					if (orig->iter_next.owner == ctx.callee)
					{
						e->iter_next.owner = ctx.host;
						e->iter_next.slot_base = ctx.base + orig->iter_next.slot_base;
					}
					else
					{
						Expr* owner = nullptr;
						for (auto it = ctx.lambda_stack.rbegin(); it != ctx.lambda_stack.rend(); ++it)
						{
							if (it->first == orig->iter_next.owner)
							{
								owner = it->second;
								break;
							}
						}
						JETC_DIE_UNLESS(db, orig->loc, owner,
						                "anf-inline: cloned iteration owned outside the clone");
						e->iter_next.owner = owner;
						e->iter_next.slot_base = orig->iter_next.slot_base;
					}
					break;
				case ExprKind::If:
					e->if_.test = clone(orig->if_.test, ctx);
					e->if_.consequent = clone(orig->if_.consequent, ctx);
					e->if_.alternate = orig->if_.alternate ? clone(orig->if_.alternate, ctx) : nullptr;
					break;
				case ExprKind::Let:
				{
					e->let.names = orig->let.names;
					if (orig->let.owner == ctx.callee)
					{
						e->let.owner = ctx.host;
						e->let.slot_base = ctx.base + orig->let.slot_base;
					}
					else
					{
						Expr* owner = nullptr;
						for (auto it = ctx.lambda_stack.rbegin(); it != ctx.lambda_stack.rend(); ++it)
						{
							if (it->first == orig->let.owner)
							{
								owner = it->second;
								break;
							}
						}
						JETC_DIE_UNLESS(db, orig->loc, owner,
						                "anf-inline: cloned let owned by a lambda outside the clone");
						e->let.owner = owner;
						e->let.slot_base = orig->let.slot_base;
					}
					e->let.vals = clone_slice(orig->let.vals, ctx);
					e->let.body = clone_slice(orig->let.body, ctx);
					break;
				}
				default:
					JETC_DIE(db, orig->loc, "anf-inline: unhandled ExprKind %d in clone",
					         static_cast<int>(orig->kind));
			}
			return e;
		}

		Expr* splice(Expr* call, Expr* callee)
		{
			Expr* host = hosts.back();
			uint32_t base = host->lambda.names.size();

			// The callee's whole frame (params + let slots) is appended to
			// the host frame, so {callee, i} translates to {host, base + i}.
			uint32_t n_callee = callee->lambda.names.size();
			std::string_view* names = db.arena.alloc_array<std::string_view>(base + n_callee);
			for (uint32_t i = 0; i < base; ++i)
			{
				names[i] = host->lambda.names[i];
			}
			for (uint32_t i = 0; i < n_callee; ++i)
			{
				names[base + i] = callee->lambda.names[i];
			}
			host->lambda.names = {names, base + n_callee};

			CloneCtx ctx{callee, host, base, {}};
			Expr* e = db.make_expr(call->loc, ExprKind::Let);
			e->let.names = callee->lambda.params;
			e->let.vals = call->call.args;
			e->let.slot_base = base;
			e->let.owner = host;
			e->let.body = clone_slice(callee->lambda.body, ctx);
			return e;
		}

		Expr* walk(Expr* expr)
		{
			switch (expr->kind)
			{
				case ExprKind::NumberLit:
				case ExprKind::StringLit:
				case ExprKind::BooleanLit:
				case ExprKind::CharacterLit:
				case ExprKind::SymbolLit:
				case ExprKind::UnknownLit:
				case ExprKind::PrimRef:
					return expr;

				case ExprKind::VarRef:
				{
					auto it = const_cands.find(binding_key(get(db.bindings_, expr->id)));
					if (it == const_cands.end())
					{
						return expr;
					}
					Expr* lit = it->second;
					Expr* e = db.make_expr(expr->loc, lit->kind);
					switch (lit->kind)
					{
						case ExprKind::NumberLit:
							e->number_lit = lit->number_lit;
							break;
						case ExprKind::BooleanLit:
							e->boolean_lit = lit->boolean_lit;
							break;
						case ExprKind::CharacterLit:
							e->character_lit = lit->character_lit;
							break;
						default:
							JETC_DIE(db, expr->loc, "anf-inline: non-literal const candidate");
					}
					return e;
				}

				case ExprKind::Call:
				{
					Expr* proc = expr->call.proc;
					if (proc->kind == ExprKind::VarRef)
					{
						if (auto it = lambda_cands.find(binding_key(get(db.bindings_, proc->id)));
						    it != lambda_cands.end())
						{
							if (Expr* callee = it->second;
							    callee->lambda.params.size() == expr->call.args.size() && !active.count(
									callee))
							{
								Expr* let = splice(expr, callee);
								active.insert(callee);
								let = walk(let);
								active.erase(callee);
								return let;
							}
						}
					}
					walk_children(db, expr, [&](Expr*& c) { c = walk(c); });
					return expr;
				}

				case ExprKind::Apply:
				case ExprKind::SetBang:
				case ExprKind::SetRef:
				case ExprKind::IterNext:
				case ExprKind::If:
					walk_children(db, expr, [&](Expr*& c) { c = walk(c); });
					return expr;

				case ExprKind::Lambda:
				{
					bool guard = candidate_lambdas.count(expr) && active.insert(expr).second;
					hosts.push_back(expr);
					walk_children(db, expr, [&](Expr*& c) { c = walk(c); });
					hosts.pop_back();
					if (guard)
					{
						active.erase(expr);
					}
					return expr;
				}

				case ExprKind::Let:
				{
					if (expr->id < first_clone_id)
					{
						Compiler::LambdaBindings& ob = db.lambda_bindings_[expr->let.owner];
						for (uint32_t i = 0; i < expr->let.names.size(); ++i)
						{
							// A never-written slot's val is its value for the
							// whole scope; a written one (incl. letrec-style
							// is_init set!s over #f sentinels) is covered by
							// bound_init candidacy instead.
							if (!get(ob.is_initialized, expr->let.slot_base + i)
							    && !get(ob.reassigned_after_init, expr->let.slot_base + i))
							{
								consider(expr->let.owner, expr->let.slot_base + i, expr->let.vals[i]);
							}
						}
					}
					walk_children(db, expr, [&](Expr*& c) { c = walk(c); });
					return expr;
				}

				default:
					JETC_DIE(db, expr->loc, "anf-inline: unhandled ExprKind %d",
					         static_cast<int>(expr->kind));
			}
		}
	};

} // namespace

void Compiler::run_anf_inline(Program& program)
{
	// Needs current recompute_lambda_bindings results, and the caller must rerun
	// that pass afterwards: splices change captures and upvalues.
	AnfInline pass{.db = *this, .first_clone_id = next_expr_id_};
	pass.hosts.push_back(toplevel_lambda_);

	for (Expr* L : all_lambdas_)
	{
		LambdaBindings& lb = lambda_bindings_[L];
		for (size_t i = 0; i < lb.bound_init.size(); ++i)
		{
			if (Expr* init = lb.bound_init[i]; init && !get(lb.reassigned_after_init, i))
			{
				pass.consider(L, i, init);
			}
		}
	}

	for (uint32_t i = 0; i < program.forms.size(); ++i)
	{
		program.forms[i] = pass.walk(program.forms[i]);
		verify_anf(program.forms[i]);
	}
}

namespace
{

	bool is_binarizable(std::string_view name)
	{
		// + - * / must stay a subset of select_call_op's fused names: a binarized
		// chain the selection cannot fuse is strictly worse than one n-ary call.
		// ref folds a key path into single-step field loads.
		return name == "+" || name == "-" || name == "*" || name == "/" || name == "ref";
	}

	struct Binarize
	{
		Compiler& db;

		bool lowerable(Expr* e)
		{
			if (e->call.args.size() < 3 || e->call.proc->kind != ExprKind::VarRef)
			{
				return false;
			}
			std::string_view name = e->call.proc->var_ref.name;
			return is_binarizable(name) && db.prim_binding_lowerable(db.binding(e->call.proc), name);
		}

		Expr* make_binary(SourceLoc loc, Expr* proc, Expr* lhs, Expr* rhs)
		{
			Expr* e = db.make_expr(loc, ExprKind::Call);
			e->call.proc = proc;
			e->call.args = db.arena.copy_slice({lhs, rhs});
			return e;
		}

		Expr* clone_proc(Expr* proc)
		{
			// TODO: this `get` call has to happen first, otherwise another
			//       `get` could resize `bindings_` and cause a UAF. would
			//       be better if the compiler just didn't have this hazard
			//       to begin with.
			ResolvedBinding source{get(db.bindings_, proc->id)};

			Expr* e = db.make_expr(proc->loc, ExprKind::VarRef);
			e->var_ref.name = proc->var_ref.name;
			get(db.bindings_, e->id) = source;
			return e;
		}

		void walk(Expr* e)
		{
			walk_children(db, e, [&](Expr* c) { walk(c); });
			if (e->kind != ExprKind::Call || !lowerable(e))
			{
				return;
			}
			// (op a b c) -> (op (op a b) c), left-nested like the prim's fold:
			// bit-identical for arithmetic doubles, a left-to-right path for ref.
			Slice<Expr*> args = e->call.args;
			Expr* acc = args[0];
			for (uint32_t i = 1; i + 1 < args.size(); ++i)
			{
				acc = make_binary(e->loc, clone_proc(e->call.proc), acc, args[i]);
			}
			e->call.args = db.arena.copy_slice({acc, args.back()});
		}
	};

} // namespace

void Compiler::run_binarize(Program& program)
{
	Binarize pass{.db = *this};
	for (Expr* form : program.forms)
	{
		pass.walk(form);
	}
}

namespace
{

	struct LambdaLift
	{
		Compiler& db;

		static bool is_false_lit(Expr* e)
		{
			return e->kind == ExprKind::BooleanLit && !e->boolean_lit.value;
		}

		bool name_used_as_value(Expr* expr, std::string_view name)
		{
			switch (expr->kind)
			{
				case ExprKind::VarRef:
					return expr->var_ref.name == name;
				case ExprKind::Call:
				{
					if (bool proc_is_name = expr->call.proc->kind == ExprKind::VarRef
					                        && expr->call.proc->var_ref.name == name;
					    !proc_is_name && name_used_as_value(expr->call.proc, name))
					{
						return true;
					}
					for (Expr* arg : expr->call.args)
					{
						if (name_used_as_value(arg, name))
						{
							return true;
						}
					}
					return false;
				}
				case ExprKind::SetBang:
					return expr->set_bang.name == name
					       || name_used_as_value(expr->set_bang.value, name);
				default:
				{
					bool found = false;
					walk_children(db, expr, [&](Expr*& c) { found = found || name_used_as_value(c, name); });
					return found;
				}
			}
		}

		bool self_calls_all_tail(Expr* expr, std::string_view name, bool in_tail)
		{
			switch (expr->kind)
			{
				case ExprKind::Call:
				{
					if (bool is_self = expr->call.proc->kind == ExprKind::VarRef
					                   && expr->call.proc->var_ref.name == name;
					    is_self && !in_tail)
					{
						return false;
					}
					if (!self_calls_all_tail(expr->call.proc, name, false))
					{
						return false;
					}
					for (Expr* arg : expr->call.args)
					{
						if (!self_calls_all_tail(arg, name, false))
						{
							return false;
						}
					}
					return true;
				}
				case ExprKind::If:
					return self_calls_all_tail(expr->if_.test, name, false)
					       && self_calls_all_tail(expr->if_.consequent, name, in_tail)
					       && (!expr->if_.alternate
					           || self_calls_all_tail(expr->if_.alternate, name, in_tail));
				case ExprKind::IterNext:
					return self_calls_all_tail(expr->iter_next.cursor, name, false)
					       && self_calls_all_tail(expr->iter_next.consequent, name, in_tail)
					       && (!expr->iter_next.alternate
					           || self_calls_all_tail(expr->iter_next.alternate, name, in_tail));
				case ExprKind::Let:
				{
					for (Expr* val : expr->let.vals)
					{
						if (!self_calls_all_tail(val, name, false))
						{
							return false;
						}
					}
					for (uint32_t i = 0; i < expr->let.body.size(); ++i)
					{
						if (bool last = (i == expr->let.body.size() - 1);
						    !self_calls_all_tail(expr->let.body[i], name, last && in_tail))
						{
							return false;
						}
					}
					return true;
				}
				case ExprKind::Lambda:
					for (uint32_t i = 0; i < expr->lambda.body.size(); ++i)
					{
						if (bool last = (i == expr->lambda.body.size() - 1);
						    !self_calls_all_tail(expr->lambda.body[i], name, last))
						{
							return false;
						}
					}
					return true;
				default:
				{
					bool ok = true;
					walk_children(db, expr, [&](Expr*& c)
					{
						ok = ok && self_calls_all_tail(c, name, false);
					});
					return ok;
				}
			}
		}

		struct Capture
		{
			std::string_view name;
			ResolvedBinding binding;
		};

		void collect_captures_in(Expr* expr, Expr* lambda, std::string_view self_name,
		                         std::vector<Capture>& out, std::unordered_set<uint64_t>& seen)
		{
			switch (expr->kind)
			{
				case ExprKind::VarRef:
				{
					if (expr->var_ref.name == self_name)
					{
						return;
					}
					ResolvedBinding b = db.binding(expr);
					if (b.lambda == lambda)
					{
						return;
					}
					if (uint64_t key = (static_cast<uint64_t>(b.lambda->id) << 32) | b.breadth;
					    seen.insert(key).second)
					{
						out.push_back({expr->var_ref.name, b});
					}
					return;
				}
				case ExprKind::Call:
				{
					// A proc ref that op selection will lower to an intrinsic never
					// becomes an upvalue; parameterizing it would block the lowering.
					if (db.prim_call_lowering(expr).kind == Compiler::PrimLowering::Kind::None)
					{
						collect_captures_in(expr->call.proc, lambda, self_name, out, seen);
					}
					for (Expr* arg : expr->call.args)
					{
						collect_captures_in(arg, lambda, self_name, out, seen);
					}
					return;
				}
				case ExprKind::Lambda:
					// Do not descend into nested lambdas: their captures reach the
					// binding through the closure chain and stay captures.
					return;
				default:
				{
					auto&& collect_child = [&](Expr* c)
					{
						collect_captures_in(c, lambda, self_name, out, seen);
					};
					walk_children(db, expr, collect_child);
					return;
				}
			}
		}

		std::vector<Capture> collect_captures(Expr* lambda, std::string_view self_name)
		{
			std::vector<Capture> captures;
			std::unordered_set<uint64_t> seen;
			for (Expr* form : lambda->lambda.body)
			{
				collect_captures_in(form, lambda, self_name, captures, seen);
			}
			return captures;
		}

		Expr* make_resolved_ref(SourceLoc loc, const Capture& cap)
		{
			// Record the binding immediately so lifts of enclosing lets can resolve
			// this ref before resolve_bindings reruns.
			Expr* e = db.make_expr(loc, ExprKind::VarRef);
			e->var_ref.name = cap.name;
			get(db.bindings_, e->id) = cap.binding;
			return e;
		}

		void prepend_capture_args(Expr* expr, std::string_view name, const std::vector<Capture>& captures)
		{
			if (expr->kind == ExprKind::Call
			    && expr->call.proc->kind == ExprKind::VarRef
			    && expr->call.proc->var_ref.name == name)
			{
				uint32_t n = static_cast<uint32_t>(expr->call.args.size());
				Expr** new_args = db.arena.alloc_array<Expr*>(n + captures.size());
				for (uint32_t i = 0; i < captures.size(); ++i)
				{
					new_args[i] = make_resolved_ref(expr->loc, captures[i]);
				}
				for (uint32_t i = 0; i < n; ++i)
				{
					new_args[captures.size() + i] = expr->call.args[i];
				}
				expr->call.args = {new_args, static_cast<uint32_t>(n + captures.size())};
				for (Expr* arg : expr->call.args)
				{
					prepend_capture_args(arg, name, captures);
				}
				return;
			}
			walk_children(db, expr, [&](Expr*& c) { prepend_capture_args(c, name, captures); });
		}

		Expr* try_lift(Expr* let_expr)
		{
			if (let_expr->kind != ExprKind::Let || let_expr->let.names.size() != 1)
			{
				return let_expr;
			}

			std::string_view name = let_expr->let.names[0];
			if (Expr* init_val = let_expr->let.vals[0]; !is_false_lit(init_val))
			{
				return let_expr;
			}

			Expr* lambda = nullptr;
			size_t setbang_idx = 0;
			for (size_t i = 0; i < let_expr->let.body.size(); ++i)
			{
				if (Expr* form = let_expr->let.body[i]; form->kind == ExprKind::SetBang
				    && form->set_bang.name == name
				    && form->set_bang.is_init
				    && form->set_bang.value->kind == ExprKind::Lambda)
				{
					if (lambda)
					{
						return let_expr;
					}
					lambda = form->set_bang.value;
					setbang_idx = i;
				}
			}
			if (!lambda)
			{
				return let_expr;
			}

			for (Expr* form : let_expr->let.body)
			{
				if (form->kind == ExprKind::SetBang
				    && form->set_bang.name == name
				    && !form->set_bang.is_init)
				{
					return let_expr;
				}
			}

			if (lambda->lambda.is_variadic)
			{
				return let_expr;
			}

			for (size_t i = setbang_idx + 1; i < let_expr->let.body.size(); ++i)
			{
				if (name_used_as_value(let_expr->let.body[i], name))
				{
					return let_expr;
				}
			}

			for (Expr* form : lambda->lambda.body)
			{
				if (name_used_as_value(form, name))
				{
					return let_expr;
				}
			}

			std::vector<Capture> captures = collect_captures(lambda, name);
			if (captures.empty())
			{
				return let_expr;
			}

			// A parameter is a copy: a capture whose binding is written after init must
			// stay a capture or writes through one copy are lost to the others.
			for (Capture& cap : captures)
			{
				if (Compiler::LambdaBindings& owner = db.lambda_bindings_[cap.binding.lambda];
				    get(owner.reassigned_after_init, cap.binding.breadth))
				{
					return let_expr;
				}
			}

			if (!self_calls_all_tail(lambda, name, false))
			{
				return let_expr;
			}

			uint32_t n_captures = static_cast<uint32_t>(captures.size());
			uint32_t n_params = static_cast<uint32_t>(lambda->lambda.params.size());
			std::string_view* new_params = db.arena.alloc_array<std::string_view>(n_captures + n_params);
			for (uint32_t i = 0; i < n_captures; ++i)
			{
				new_params[i] = captures[i].name;
			}
			for (uint32_t i = 0; i < n_params; ++i)
			{
				new_params[n_captures + i] = lambda->lambda.params[i];
			}
			lambda->lambda.params = {new_params, n_captures + n_params};

			for (size_t i = setbang_idx + 1; i < let_expr->let.body.size(); ++i)
			{
				prepend_capture_args(let_expr->let.body[i], name, captures);
			}
			for (Expr* form : lambda->lambda.body)
			{
				prepend_capture_args(form, name, captures);
			}

			return let_expr;
		}

		Expr* walk(Expr* expr)
		{
			walk_children(db, expr, [&](Expr*& c) { c = walk(c); });
			if (expr->kind == ExprKind::Let)
			{
				return try_lift(expr);
			}
			return expr;
		}
	};

} // namespace

void Compiler::run_lambda_lift(Program& program)
{
	LambdaLift pass{.db = *this};
	for (Expr* form : program.forms)
	{
		pass.walk(form);
	}
}

namespace
{

	struct LirInst
	{
		Opcode op;   // base opcode only: no _1.._7 replicas; label is IR-only
		union
		{
			struct { uint16_t dst; uint16_t src; } mov;              // mov
			OP_trunc unary;
			struct { uint16_t dst0; uint16_t src0; uint16_t dst1; uint16_t src1; } mov2;
			struct { uint16_t dst; uint16_t idx; } load;             // ldk ldu ldus ldd
			struct { uint16_t idx; uint16_t src; } store;            // stu std
			struct { uint16_t reg; } box;                            // box
			struct { uint16_t src; } ret;                            // retv
			struct { uint32_t id; uint16_t src; } label;             // label; if_false/skip target
			struct { uint32_t id; uint16_t a; uint16_t b; } if_cmp;  // if_eq..if_ltk; rk holds the pool idx in b
			// One payload for every call op: call/tcall read callee, call_upval_slot
			// reads upvalue_idx, call_local/call_upval read idx, the rest only w+nargs.
			struct { uint16_t w; uint16_t nargs; uint16_t callee; uint16_t upvalue_idx; uint16_t idx; } call;
			struct { uint16_t dst; uint16_t pool_idx; uint16_t first_capture; uint16_t n_captures; } closure;
			struct { uint16_t dst; uint16_t a; uint16_t b; } arith;  // rr; rk holds the pool idx in b
			struct { uint16_t dst; uint16_t obj; uint16_t key; uint16_t val; } field;  // ldf stf; *k holds the pool idx in key
			struct { uint32_t id; uint16_t cursor; uint16_t dst0; uint16_t dst1; } iter;
		} u;
		// Stamped from the SourceLoc passed to LirEmitter::emit; line 0 means
		// no position.
		SourceLoc loc;
	};

	struct LirLambda
	{
		std::vector<LirInst> code;
		// make_closure payload, never code: closure insts hold (first, n)
		// into this sidecar.
		std::vector<OP_make_closure_capture> captures;
		bool is_variadic = false;
		uint32_t n_params = 0;
		// Registers [0, reg_floor) are params (identity, call convention).
		// Above the floor, allocation is first-fit over reg_used; reg_used[i]
		// is register reg_floor + i; the frame high water is reg_floor + reg_used.size().
		size_t reg_floor = 0;
		std::vector<bool> reg_used;
		SourceLoc loc;

		size_t frame_regs() const
		{
			return reg_floor + reg_used.size();
		}
		uint16_t pool_slot = 0;   // unused for the toplevel (index 0)
		std::string_view lambda_name;
		// Physical register per binding breadth; absent means the identity
		// register (call results adopted as homes).
		std::unordered_map<uint16_t, uint16_t> phys_home;
		std::unordered_set<uint16_t> temp_regs;
	};

	struct LirProgram
	{
		std::vector<LirLambda> lambdas;    // [0] = toplevel
		std::vector<std::string> pool;     // lambda entries stay empty until emit
		std::vector<int32_t> pool_to_lambda;  // pool slot -> lambdas index, -1 = constant
		uint32_t next_label = 0;
	};

	struct LirEmitter
	{
		Compiler& db;
		LirProgram& prog;
		std::vector<Expr*> outer_lambdas{db.toplevel_lambda_};
		std::vector<uint32_t> lambda_stack{0};

		// Lambda entries bypass the dedup: one pool slot per Lambda expr.
		std::unordered_map<std::string, uint16_t> pool_idx{};

		// Windows allocated ahead of their call site by argument sinking, keyed by
		// the Call expr id.
		std::unordered_map<uint32_t, uint16_t> call_windows{};
		struct SelfTailSave
		{
			uint16_t target;
			uint16_t temp;
		};
		std::unordered_map<uint32_t, SelfTailSave> self_tail_saves{};

		LirLambda& current_lambda()
		{
			// prog.lambdas reallocates as nested lambdas append; never hold the
			// returned reference across a recursive walk.
			return prog.lambdas[lambda_stack.back()];
		}

		static LirInst inst(Opcode op)
		{
			LirInst i{};
			i.op = op;
			return i;
		}

		void emit(SourceLoc loc, LirInst i)
		{
			i.loc = loc;
			current_lambda().code.push_back(i);
		}

		uint16_t alloc_reg(SourceLoc loc)
		{
			LirLambda& L = current_lambda();
			std::vector<bool>& used = L.reg_used;
			for (size_t i = 0; i < used.size(); ++i)
			{
				if (!used[i])
				{
					used[i] = true;
					return narrow_or_die<uint16_t>(db, loc, L.reg_floor + i);
				}
			}
			used.push_back(true);
			constexpr size_t reg_limit = std::numeric_limits<uint16_t>::max();
			JETC_DIE_WHEN(db, loc, L.frame_regs() > reg_limit, "codegen: frame exceeds %zu registers",
			              reg_limit);
			return narrow_or_die<uint16_t>(db, loc, L.reg_floor + used.size() - 1);
		}

		uint16_t alloc_window(SourceLoc loc, size_t n)
		{
			// A call window sits above every live register: the VM overlays the
			// callee's frame on the window (op_enter_lambda_fast: base = args -
			// stack_base), so the call clobbers everything above the window's
			// first slot. Registers above a pending window may still be handed
			// out: whatever lands there is consumed before the pending call is
			// emitted (ANF hoists each call's operand temps immediately before
			// its own binding; verify_anf guards it).
			if (n == 0)
			{
				n = 1;
			}
			LirLambda& L = current_lambda();
			std::vector<bool>& used = L.reg_used;
			size_t top = 0;
			for (size_t i = used.size(); i-- > 0;)
			{
				if (used[i])
				{
					top = i + 1;
					break;
				}
			}
			if (used.size() < top + n)
			{
				used.resize(top + n);
			}
			for (size_t i = top; i < top + n; ++i)
			{
				used[i] = true;
			}
			constexpr size_t reg_limit = std::numeric_limits<uint16_t>::max();
			JETC_DIE_WHEN(db, loc, L.frame_regs() > reg_limit, "codegen: frame exceeds %zu registers",
			              reg_limit);
			return narrow_or_die<uint16_t>(db, loc, L.reg_floor + top);
		}

		void release_reg(uint16_t reg)
		{
			LirLambda& L = current_lambda();
			size_t slot = reg - L.reg_floor;
			JETC_DIE_WHEN(db, L.loc, slot >= L.reg_used.size(), "codegen: release of unallocated register %d",
			              reg);
			JETC_DIE_WHEN(db, L.loc, !L.reg_used[slot], "codegen: double release of register %d", reg);
			L.reg_used[slot] = false;
		}

		void mark_temp(uint16_t reg) { current_lambda().temp_regs.insert(reg); }

		void release_if_temp(uint16_t reg)
		{
			if (current_lambda().temp_regs.erase(reg) != 0)
			{
				release_reg(reg);
			}
		}

		uint16_t physical(uint16_t breadth)
		{
			std::unordered_map<uint16_t, uint16_t>& m = current_lambda().phys_home;
			auto it = m.find(breadth);
			return it == m.end() ? breadth : it->second;
		}

		// Find-or-allocate the window for a call site; argument sinking claims it
		// ahead of emit_call reaching the site.
		uint16_t claim_call_window(Expr* call_expr, size_t nargs)
		{
			auto it = call_windows.find(call_expr->id);
			if (it == call_windows.end())
			{
				it = call_windows.emplace(call_expr->id, alloc_window(call_expr->loc, nargs)).first;
			}
			return it->second;
		}

		void emit_mov(SourceLoc loc, uint16_t dst, uint16_t src)
		{
			if (dst == src)
			{
				return;
			}
			LirInst i = inst(Opcode::mov);
			i.u.mov.dst = dst;
			i.u.mov.src = src;
			emit(loc, i);
		}

		void emit_load(SourceLoc loc, Opcode op, uint16_t dst, uint16_t idx)
		{
			LirInst i = inst(op);
			i.u.load.dst = dst;
			i.u.load.idx = idx;
			emit(loc, i);
		}

		void emit_store(SourceLoc loc, Opcode op, uint16_t idx, uint16_t src)
		{
			LirInst i = inst(op);
			i.u.store.idx = idx;
			i.u.store.src = src;
			emit(loc, i);
		}

		void emit_box(SourceLoc loc, uint16_t reg)
		{
			LirInst i = inst(Opcode::box);
			i.u.box.reg = reg;
			emit(loc, i);
		}

		void emit_label(SourceLoc loc, Opcode op, uint32_t id, uint16_t src = 0)
		{
			LirInst i = inst(op);
			i.u.label.id = id;
			i.u.label.src = src;
			emit(loc, i);
		}

		void emit_ldk(SourceLoc loc, uint16_t dst, uint16_t idx)
		{
			emit_load(loc, Opcode::ldk, dst, idx);
		}

		uint16_t intern_constant(std::string& serialized)
		{
			if (auto it = pool_idx.find(serialized); it != pool_idx.end())
			{
				return it->second;
			}
			uint16_t idx = static_cast<uint16_t>(prog.pool.size());
			prog.pool.push_back(serialized);
			prog.pool_to_lambda.push_back(-1);
			pool_idx[serialized] = idx;
			return idx;
		}

		template <typename T>
		uint16_t intern_typed(ConstTag t, T& payload)
		{
			std::string s;
			s.push_back(static_cast<char>(t));
			s.append(reinterpret_cast<char*>(&payload), sizeof(T));
			return intern_constant(s);
		}

		uint16_t intern_name(ConstTag t, std::string_view payload)
		{
			std::string s;
			s.push_back(static_cast<char>(t));
			s.append(payload.data(), payload.size());
			s.push_back(0);
			return intern_constant(s);
		}

		uint16_t intern_text(SourceLoc loc, std::string_view payload)
		{
			JETC_DIE_WHEN(db, loc, payload.size() > UINT32_MAX, "string literal is too long");
			uint32_t n_bytes = static_cast<uint32_t>(payload.size());

			std::string s;
			s.push_back(static_cast<char>(ConstTag::String));
			s.append(reinterpret_cast<const char*>(&n_bytes), sizeof(n_bytes));
			s.append(payload.data(), payload.size());
			return intern_constant(s);
		}

		uint16_t intern_empty(ConstTag t)
		{
			std::string s;
			s.push_back(static_cast<char>(t));
			return intern_constant(s);
		}

		uint16_t intern_global_name(std::string_view name)
		{
			return intern_name(ConstTag::GlobalName, name);
		}

		uint16_t intern_literal_key(Expr* e)
		{
			switch (e->kind)
			{
				case ExprKind::NumberLit:
				{
					double val = number_lit_value(e->number_lit.text);
					Number n = Number::from_ieee(val);
					return intern_typed(ConstTag::Number, n);
				}
				case ExprKind::SymbolLit:
					return intern_name(ConstTag::Symbol, e->symbol_lit.name);
				case ExprKind::CharacterLit:
				{
					Character c = static_cast<Character>(e->character_lit.value);
					return intern_typed(ConstTag::Character, c);
				}
				case ExprKind::BooleanLit:
				{
					bool v = e->boolean_lit.value;
					return intern_typed(ConstTag::Boolean, v);
				}
				case ExprKind::StringLit:
					return intern_text(e->loc, e->string_lit.value);
				default:
					JETC_DIE(db, e->loc, "intern_literal_key: not a literal Expr (kind %d)",
					         static_cast<int>(e->kind));
			}
		}

		void emit_ret(SourceLoc loc, uint16_t src)
		{
			LirInst i = inst(Opcode::retv);
			i.u.ret.src = src;
			emit(loc, i);
		}

		uint16_t emit_sequence_value(SourceLoc empty_loc, Slice<Expr*>& forms)
		{
			uint32_t n = forms.size();
			if (n == 0)
			{
				uint16_t t = alloc_reg(empty_loc);
				emit_ldk(empty_loc, t, intern_empty(ConstTag::Unknown));
				return t;
			}
			for (uint32_t i = 0; i + 1 < n; ++i)
			{
				emit_ignoring_result(forms[i]);
			}
			return emit_to_any_reg(forms[n - 1]);
		}

		void emit_sequence_to(SourceLoc loc, Slice<Expr*>& forms, uint16_t dst)
		{
			uint32_t n = forms.size();
			if (n == 0)
			{
				emit_ldk(loc, dst, intern_empty(ConstTag::Unknown));
				return;
			}
			for (uint32_t i = 0; i + 1 < n; ++i)
			{
				emit_ignoring_result(forms[i]);
			}
			emit_to_reg(forms[n - 1], dst);
		}

		void emit_ignoring_result(Expr* expr)
		{
			switch (expr->kind)
			{
				case ExprKind::NumberLit:
				case ExprKind::StringLit:
				case ExprKind::BooleanLit:
				case ExprKind::CharacterLit:
				case ExprKind::SymbolLit:
				case ExprKind::UnknownLit:
				case ExprKind::VarRef:
				case ExprKind::PrimRef:
					return;
				case ExprKind::Let:
					emit_let_ignoring_result(expr);
					return;
				default:
				{
					uint16_t t = emit_to_any_reg(expr);
					release_if_temp(t);
					return;
				}
			}
		}

		void emit_prologue(Expr* lambda)
		{
			for (uint32_t i = 0; i < lambda->lambda.params.size(); ++i)
			{
				if (needs_slot(lambda, i))
				{
					emit_box(lambda->loc, narrow_or_die<uint16_t>(db, lambda->loc, i));
				}
			}
		}

		uint16_t emit_lifted_lambda(Expr* expr)
		{
			uint32_t idx = static_cast<uint32_t>(prog.lambdas.size());
			prog.lambdas.emplace_back();
			prog.lambdas[idx].loc = expr->loc;
			prog.lambdas[idx].is_variadic = expr->lambda.is_variadic;
			prog.lambdas[idx].n_params = expr->lambda.params.size();
			prog.lambdas[idx].lambda_name = expr->lambda.lambda_name;
			size_t n_named = expr->lambda.params.size() + (expr->lambda.is_variadic ? 1 : 0);
			prog.lambdas[idx].reg_floor = n_named;

			outer_lambdas.push_back(expr);
			lambda_stack.push_back(idx);
			emit_prologue(expr);
			uint16_t r = emit_sequence_value(expr->loc, expr->lambda.body);
			emit_ret(expr->loc, r);
			lambda_stack.pop_back();
			outer_lambdas.pop_back();

			// The slot must be reserved after the body completes: the body's
			// constants and nested lambda entries take lower pool indices.
			uint16_t slot = static_cast<uint16_t>(prog.pool.size());
			prog.pool.emplace_back();
			prog.pool_to_lambda.push_back(static_cast<int32_t>(idx));
			prog.lambdas[idx].pool_slot = slot;
			return slot;
		}

		void emit_capture_recipe(Expr* inner)
		{
			Expr* current = outer_lambdas.back();
			Slice<UpvalueRef>& ups = inner->lambda.upvalues;
			for (uint32_t i = 0; i < ups.size(); ++i)
			{
				UpvalueRef u = ups[i];
				ResolvedBinding rb{.lambda = u.owner, .breadth = u.breadth};
				OP_make_closure_capture cap{};
				if (rb.lambda == current)
				{
					cap.src = static_cast<uint8_t>(CaptureSource::Local);
					cap.idx = physical(narrow_or_die<uint16_t>(db, inner->loc, rb.breadth));
				}
				else
				{
					std::optional<uint16_t> found =
						find_upvalue(current, rb.lambda, static_cast<uint32_t>(rb.breadth));
					JETC_DIE_UNLESS(db, inner->loc, found, "codegen: upvalue not in parent's upvalue list");
					cap.src = static_cast<uint8_t>(CaptureSource::Upvalue);
					cap.idx = *found;
				}
				current_lambda().captures.push_back(cap);
			}
		}

		void emit_lambda_value(Expr* expr, uint16_t dst)
		{
			uint16_t pool_index = emit_lifted_lambda(expr);
			if (expr->lambda.upvalues.empty())
			{
				emit_ldk(expr->loc, dst, pool_index);
				return;
			}
			LirInst i = inst(Opcode::clos);
			i.u.closure.dst = dst;
			i.u.closure.pool_idx = pool_index;
			i.u.closure.first_capture = static_cast<uint16_t>(current_lambda().captures.size());
			i.u.closure.n_captures = static_cast<uint16_t>(expr->lambda.upvalues.size());
			emit_capture_recipe(expr);
			emit(expr->loc, i);
		}

		Compiler::OpSelection selection(Expr* expr, const char* what)
		{
			JETC_DIE_WHEN(db, expr->loc, !db.selected_ops_[expr->id], "codegen: %s without selection", what);
			Compiler::OpSelection sel = *db.selected_ops_[expr->id];
			// Single read point for selections: these payloads name an unboxed
			// local register (for ldu/stu/ldus the same field holds an upvalue
			// index).
			switch (sel.op)
			{
				case Opcode::mov:
				case Opcode::ldd:
				case Opcode::std:
					sel.u.var.addr = physical(sel.u.var.addr);
					break;
				case Opcode::call_local_0:
					sel.u.call_ic_atom.idx = physical(sel.u.call_ic_atom.idx);
					break;
				default:
					break;
			}
			return sel;
		}

		uint16_t emit_set_bang(Expr* expr)
		{
			Compiler::OpSelection sel = selection(expr, "set!");
			switch (sel.op)
			{
				case Opcode::mov:
				{
					emit_to_reg(expr->set_bang.value, sel.u.var.addr);
					return sel.u.var.addr;
				}
				case Opcode::std:
				case Opcode::stu:
				{
					uint16_t v = emit_to_any_reg(expr->set_bang.value);
					emit_store(expr->loc, sel.op, sel.u.var.addr, v);
					// Like emit_set_ref, the value register stays alive for the
					// caller: emit_to_reg moves it into dst before releasing, and
					// emit_to_any_reg returns it still marked as a temp.
					return v;
				}
				default:
					JETC_DIE(db, expr->loc, "codegen: unexpected set! selection %d",
					         static_cast<int>(sel.op));
			}
		}

		uint16_t emit_set_ref(Expr* expr)
		{
			Compiler::OpSelection sel = selection(expr, "SetRef");
			LirInst i = inst(sel.op);
			i.u.field.obj = emit_to_any_reg(expr->set_ref.obj);
			bool literal_key = takes_literal_key(sel.op);
			i.u.field.key = literal_key ? intern_literal_key(expr->set_ref.key)
			                : emit_to_any_reg(expr->set_ref.key);
			uint16_t v = emit_to_any_reg(expr->set_ref.value);
			i.u.field.val = v;
			emit(expr->loc, i);
			release_if_temp(i.u.field.obj);
			if (!literal_key)
			{
				release_if_temp(i.u.field.key);
			}
			return v;
		}

		void emit_field_get(Expr* expr, Compiler::OpSelection sel, Expr* receiver, Expr* key,
		                    uint16_t dst, Expr* fallback = nullptr)
		{
			LirInst i = inst(sel.op);
			i.u.field.dst = dst;
			i.u.field.obj = emit_to_any_reg(receiver);
			bool literal_key = takes_literal_key(sel.op);
			i.u.field.key = literal_key ? intern_literal_key(key) : emit_to_any_reg(key);
			if (fallback)
			{
				i.u.field.val = emit_to_any_reg(fallback);
			}
			emit(expr->loc, i);
			release_if_temp(i.u.field.obj);
			if (!literal_key)
			{
				release_if_temp(i.u.field.key);
			}
			if (fallback)
			{
				release_if_temp(i.u.field.val);
			}
		}

		void emit_window_args(Slice<Expr*>& args, uint16_t w)
		{
			for (uint32_t i = 0; i < args.size(); ++i)
			{
				emit_to_reg(args[i], static_cast<uint16_t>(w + i));
			}
		}

		bool is_binding_ref(Expr* e, Expr* owner, size_t breadth)
		{
			if (e->kind != ExprKind::VarRef)
			{
				return false;
			}
			Compiler::OpSelection sel = selection(e, "var access");
			ResolvedBinding rb = db.binding(e);
			return sel.op == Opcode::mov && rb.lambda == owner && rb.breadth == breadth;
		}

		std::optional<uint16_t> window_slot(Expr* e, Expr* owner, size_t breadth,
		                                    bool allow_self_tail = false)
		{
			if (e->kind != ExprKind::Call)
			{
				return std::nullopt;
			}
			Compiler::OpSelection sel = selection(e, "Call");
			if (!is_call_shaped(sel.op) || (sel.op == Opcode::call_self_tail && !allow_self_tail))
			{
				return std::nullopt;
			}
			for (size_t i = 0; i < e->call.args.size(); ++i)
			{
				if (is_binding_ref(e->call.args[i], owner, breadth))
				{
					if (sel.op == Opcode::call_self_tail)
					{
						for (size_t j = 0; j < e->call.args.size(); ++j)
						{
							if (j != i && is_binding_ref(e->call.args[j], owner, i))
							{
								uint16_t temp = alloc_reg(e->loc);
								emit_mov(e->loc, temp, narrow_or_die<uint16_t>(db, e->loc, i));
								self_tail_saves.emplace(
									e->id, SelfTailSave{narrow_or_die<uint16_t>(db, e->loc, i), temp});
								break;
							}
						}
						return narrow_or_die<uint16_t>(db, e->loc, i);
					}
					return narrow_or_die<uint16_t>(db, e->loc, claim_call_window(e, e->call.args.size()) + i);
				}
			}
			return std::nullopt;
		}

		void emit_let_bindings(Expr* expr, std::vector<uint16_t>& owned)
		{
			uint32_t sb = expr->let.slot_base;
			uint32_t n = expr->let.names.size();
			for (uint32_t i = 0; i < n; ++i)
			{
				uint16_t breadth = narrow_or_die<uint16_t>(db, expr->loc, sb + i);
				Expr* val = expr->let.vals[i];
				if (needs_slot(expr->let.owner, breadth))
				{
					uint16_t home = alloc_reg(expr->loc);
					current_lambda().phys_home[breadth] = home;
					owned.push_back(breadth);
					emit_to_reg(val, home);
					continue;
				}
				// ANF nests hoists, so a value is often a let-chain around the
				// expression that produces it: emit the inner bindings here and
				// coalesce home with the terminal itself. Chain homes release at
				// the outer `let` exit: the terminal consumes them after the inner
				// emit_let_bindings returns.
				while (val->kind == ExprKind::Let && val->let.body.size() == 1)
				{
					emit_let_bindings(val, owned);
					val = val->let.body[0];
				}
				if (val->kind == ExprKind::Call)
				{
					if (Compiler::OpSelection sel = selection(val, "Call"); is_if_cmp(sel.op))
					{
						continue;
					}
					else if (is_call_shaped(sel.op) && sel.op != Opcode::call_self_tail)
					{
						current_lambda().phys_home[breadth] = *emit_call(val, sel);
						owned.push_back(breadth);
						continue;
					}
				}
				if (val->kind == ExprKind::VarRef)
				{
					if (Compiler::OpSelection sel = selection(val, "var access"); sel.op == Opcode::mov)
					{
						current_lambda().phys_home[breadth] = sel.u.var.addr;
						continue;
					}
				}
				bool anf_temp = is_anf_temp(expr->let.names[i]);
				if (anf_temp && !db.binding_used(expr->let.owner, breadth))
				{
					emit_ignoring_result(val);
					continue;
				}
				if (anf_temp)
				{
					if (std::optional<uint16_t> target = sink_target(expr, expr->let.owner, breadth); target)
					{
						uint32_t uses = db.binding_use_count(expr->let.owner, breadth);
						JETC_DIE_WHEN(db, expr->loc, uses != 1,
						              "codegen: sunk temporary has %u uses, expected exactly 1", uses);
						current_lambda().phys_home[breadth] = *target;
						emit_to_reg(val, *target);
						continue;
					}
				}
				owned.push_back(breadth);
				uint16_t home = alloc_reg(expr->loc);
				current_lambda().phys_home[breadth] = home;
				emit_to_reg(val, home);
			}
			for (uint32_t i = 0; i < n; ++i)
			{
				if (needs_slot(expr->let.owner, sb + i))
				{
					emit_box(expr->loc, physical(narrow_or_die<uint16_t>(db, expr->loc, sb + i)));
				}
			}
		}

		void release_let_homes(const std::vector<uint16_t>& owned)
		{
			for (uint16_t breadth : owned)
			{
				uint16_t home = physical(breadth);
				current_lambda().phys_home.erase(breadth);
				release_reg(home);
			}
		}

		void emit_let(Expr* expr, uint16_t dst)
		{
			std::vector<uint16_t> owned;
			emit_let_bindings(expr, owned);
			emit_sequence_to(expr->loc, expr->let.body, dst);
			release_let_homes(owned);
		}

		void emit_let_ignoring_result(Expr* expr)
		{
			std::vector<uint16_t> owned;
			emit_let_bindings(expr, owned);
			for (Expr* form : expr->let.body)
			{
				emit_ignoring_result(form);
			}
			release_let_homes(owned);
		}

		std::optional<uint16_t> sink_target(Expr* let_expr, Expr* owner, size_t breadth)
		{
			Expr* cur = let_expr;
			bool direct = true;
			while (cur->let.body.size() == 1)
			{
				cur = cur->let.body[0];
				if (cur->kind != ExprKind::Let)
				{
					return window_slot(cur, owner, breadth, direct);
				}
				direct = false;
				for (uint32_t i = 0; i < cur->let.vals.size(); ++i)
				{
					if (std::optional<uint16_t> w = window_slot(cur->let.vals[i], owner, breadth); w)
					{
						return w;
					}
				}
			}
			return std::nullopt;
		}

		std::optional<uint16_t> emit_call(Expr* expr, Compiler::OpSelection sel)
		{
			bool tail = db.is_tail(expr);
			uint16_t nargs = static_cast<uint16_t>(expr->call.args.size());

			LirInst i = inst(sel.op);
			std::optional<uint16_t> callee_temp;
			switch (sel.op)
			{
				case Opcode::reset:
				case Opcode::coro:
				{
					// Two slots for one argument: slot 0 roots the escape or coroutine, slot 1
					// carries the body closure in and the result out.
					uint16_t w = alloc_window(expr->loc, 2);
					uint16_t result = narrow_or_die<uint16_t>(db, expr->loc, w + 1);
					emit_to_reg(expr->call.args[0], result);
					i.u.call.w = w;
					i.u.call.nargs = 1;
					emit(expr->loc, i);
					// Slot 0 roots the escape or coroutine only while the op runs
					// (vm.cc op_reset / op_coro); the result arrives in slot 1.
					release_reg(w);
					return result;
				}
				case Opcode::call_self_tail:
				{
					std::vector<std::optional<uint16_t>> src_reg(nargs, std::nullopt);
					for (uint16_t k = 0; k < nargs; ++k)
					{
						if (Expr* arg = expr->call.args[k]; arg->kind == ExprKind::VarRef)
						{
							if (Compiler::OpSelection arg_sel = selection(arg, "self tail call arg");
							    arg_sel.op == Opcode::mov)
							{
								src_reg[k] = arg_sel.u.var.addr;
							}
						}
					}
					std::unordered_map<uint16_t, uint16_t> saved;
					if (auto early_save = self_tail_saves.find(expr->id);
					    early_save != self_tail_saves.end())
					{
						saved[early_save->second.target] = early_save->second.temp;
					}
					for (uint16_t k = 0; k < nargs; ++k)
					{
						if (std::optional<uint16_t> src = src_reg[k];
						    src && *src < nargs && *src != k && src_reg[*src] != *src)
						{
							if (saved.find(*src) == saved.end())
							{
								uint16_t t = alloc_reg(expr->loc);
								emit_mov(expr->loc, t, *src);
								saved[*src] = t;
							}
						}
					}
					for (uint16_t k = 0; k < nargs; ++k)
					{
						std::optional<uint16_t> src = src_reg[k];
						auto saved_src = src ? saved.find(*src) : saved.end();
						if (src && *src == k)
						{
							continue;
						}
						if (saved_src != saved.end())
						{
							emit_mov(expr->loc, k, saved_src->second);
						}
						else
						{
							emit_to_reg(expr->call.args[k], k);
						}
					}
					i.u.call.w = 0;
					i.u.call.nargs = nargs;
					emit(expr->loc, i);
					for (const std::pair<const uint16_t, uint16_t>& save : saved)
					{
						release_reg(save.second);
					}
					self_tail_saves.erase(expr->id);
					return std::nullopt;
				}
				case Opcode::call_upval_slot_0:
					i.op = tail ? Opcode::call_upval_slot_tail_0 : Opcode::call_upval_slot_0;
					i.u.call.upvalue_idx = sel.u.call_ic_slot.upvalue_idx;
					break;
				case Opcode::call_local_0:
					i.op = tail ? Opcode::call_local_tail_0 : Opcode::call_local_0;
					i.u.call.idx = sel.u.call_ic_atom.idx;
					break;
				case Opcode::call_upval_0:
					i.op = tail ? Opcode::call_upval_tail_0 : Opcode::call_upval_0;
					i.u.call.idx = sel.u.call_ic_atom.idx;
					break;
				case Opcode::call_self_0:
					JETC_DIE_WHEN(db, expr->loc, tail,
					              "codegen: self direct call in tail position escaped recur");
					break;
				case Opcode::call:
					i.op = tail ? Opcode::tcall : Opcode::call;
					callee_temp = emit_to_any_reg(expr->call.proc);
					i.u.call.callee = *callee_temp;
					break;
				default:
					JETC_DIE(db, expr->loc, "codegen: unexpected Call selection %d",
					         static_cast<int>(sel.op));
			}

			uint16_t w = claim_call_window(expr, nargs);
			emit_window_args(expr->call.args, w);
			i.u.call.w = w;
			i.u.call.nargs = nargs;
			emit(expr->loc, i);
			if (callee_temp)
			{
				release_if_temp(*callee_temp);
			}
			for (uint16_t slot = 1; slot < nargs; ++slot)
			{
				release_reg(narrow_or_die<uint16_t>(db, expr->loc, w + slot));
			}
			return w;
		}

		uint16_t emit_apply(Expr* expr)
		{
			uint16_t w = alloc_window(expr->loc, 2);
			emit_to_reg(expr->apply.proc, w);
			emit_to_reg(expr->apply.args, static_cast<uint16_t>(w + 1));
			LirInst i = inst(Opcode::apply);
			i.u.call.w = w;
			emit(expr->loc, i);
			release_reg(narrow_or_die<uint16_t>(db, expr->loc, w + 1));
			return w;
		}

		void emit_iter_next(Expr* expr, uint16_t dst)
		{
			Compiler::OpSelection sel = selection(expr, "IterNext");
			uint32_t l_alt = prog.next_label++;
			uint32_t l_end = prog.next_label++;
			std::vector<uint16_t> iter_homes;
			for (size_t n = 0; n < expr->iter_next.names.size(); ++n)
			{
				uint16_t breadth = narrow_or_die<uint16_t>(db, expr->loc, expr->iter_next.slot_base + n);
				iter_homes.push_back(alloc_reg(expr->loc));
				current_lambda().phys_home[breadth] = iter_homes.back();
			}
			LirInst i = inst(sel.op);
			i.u.iter.id = l_alt;
			uint16_t cursor_reg = emit_to_any_reg(expr->iter_next.cursor);
			i.u.iter.cursor = cursor_reg;
			i.u.iter.dst0 = iter_homes[0];
			if (iter_homes.size() == 2)
			{
				i.u.iter.dst1 = iter_homes[1];
			}
			emit(expr->loc, i);
			release_if_temp(cursor_reg);
			for (size_t n = 0; n < expr->iter_next.names.size(); ++n)
			{
				if (needs_slot(expr->iter_next.owner, expr->iter_next.slot_base + n))
				{
					emit_box(expr->loc, iter_homes[n]);
				}
			}
			emit_to_reg(expr->iter_next.consequent, dst);
			emit_label(expr->loc, Opcode::skip, l_end);
			emit_label(expr->loc, Opcode::label, l_alt);
			if (expr->iter_next.alternate)
			{
				emit_to_reg(expr->iter_next.alternate, dst);
			}
			else
			{
				emit_ldk(expr->loc, dst, intern_empty(ConstTag::Unknown));
			}
			emit_label(expr->loc, Opcode::label, l_end);
			for (size_t n = 0; n < expr->iter_next.names.size(); ++n)
			{
				current_lambda().phys_home.erase(
					narrow_or_die<uint16_t>(db, expr->loc, expr->iter_next.slot_base + n));
				release_reg(iter_homes[n]);
			}
		}

		void emit_program(Program& program)
		{
			prog.lambdas.emplace_back();
			if (!program.forms.empty())
			{
				prog.lambdas[0].loc = program.forms[0]->loc;
			}
			// Toplevel defines are bare `set!` forms whose breadths are the frame
			// prefix [0, toplevel_names_.ordered.size()): allocate their homes
			// once and box needs_slot ones at entry, like params.
			size_t n_defines = db.toplevel_names_.ordered.size();
			SourceLoc define_loc = prog.lambdas[0].loc;
			for (size_t i = 0; i < n_defines; ++i)
			{
				uint16_t home = alloc_reg(define_loc);
				prog.lambdas[0].phys_home[narrow_or_die<uint16_t>(db, define_loc, i)] = home;
				if (needs_slot(db.toplevel_lambda_, i))
				{
					emit_box(define_loc, home);
				}
			}
			uint16_t r = emit_sequence_value(prog.lambdas[0].loc, program.forms);
			emit_ret(prog.lambdas[0].loc, r);
		}

		bool is_call_shaped(Opcode op)
		{
			switch (op)
			{
				case Opcode::call:
				case Opcode::call_upval_slot_0:
				case Opcode::call_local_0:
				case Opcode::call_upval_0:
				case Opcode::call_self_0:
				case Opcode::call_self_tail:
				case Opcode::reset:
				case Opcode::coro:
					return true;
				default:
					return false;
			}
		}

		bool takes_literal_key(Opcode op)
		{
			switch (op)
			{
				case Opcode::addk:
				case Opcode::subk:
				case Opcode::mulk:
				case Opcode::divk:
				case Opcode::numeqk:
				case Opcode::eqk:
				case Opcode::ltk:
				case Opcode::if_numeqk:
				case Opcode::if_eqk:
				case Opcode::if_ltk:
				case Opcode::ldfk:
				case Opcode::ldfkh:
				case Opcode::ldfko:
				case Opcode::stfk:
					return true;
				default:
					return false;
			}
		}

		bool is_if_cmp(Opcode op)
		{
			switch (op)
			{
				case Opcode::if_numeq:
				case Opcode::if_eq:
				case Opcode::if_lt:
				case Opcode::if_le:
				case Opcode::if_gt:
				case Opcode::if_ge:
				case Opcode::if_numeqk:
				case Opcode::if_eqk:
				case Opcode::if_ltk:
					return true;
				default:
					return false;
			}
		}

		uint16_t emit_to_any_reg(Expr* expr)
		{
			switch (expr->kind)
			{
				case ExprKind::VarRef:
				{
					if (Compiler::OpSelection sel = selection(expr, "var access"); sel.op == Opcode::mov)
					{
						return sel.u.var.addr;
					}
					break;
				}

				case ExprKind::Call:
				{
					if (Compiler::OpSelection sel = selection(expr, "Call"); is_call_shaped(sel.op))
					{
						std::optional<uint16_t> result = emit_call(expr, sel);
						JETC_DIE_WHEN(db, expr->loc, !result, "codegen: self-tail call in operand position");
						mark_temp(*result);
						return *result;
					}
					break;
				}

				case ExprKind::Apply:
				{
					uint16_t result = emit_apply(expr);
					mark_temp(result);
					return result;
				}

				case ExprKind::SetBang:
					return emit_set_bang(expr);

				case ExprKind::SetRef:
					return emit_set_ref(expr);

				default:
					break;
			}
			uint16_t t = alloc_reg(expr->loc);
			emit_to_reg(expr, t);
			mark_temp(t);
			return t;
		}

		void emit_to_reg(Expr* expr, uint16_t dst)
		{
			switch (expr->kind)
			{
				case ExprKind::NumberLit:
				{
					double val = number_lit_value(expr->number_lit.text);
					Number n = Number::from_ieee(val);
					emit_ldk(expr->loc, dst, intern_typed(ConstTag::Number, n));
					break;
				}

				case ExprKind::BooleanLit:
				{
					bool v = expr->boolean_lit.value;
					emit_ldk(expr->loc, dst, intern_typed(ConstTag::Boolean, v));
					break;
				}

				case ExprKind::CharacterLit:
				{
					Character c = static_cast<Character>(expr->character_lit.value);
					emit_ldk(expr->loc, dst, intern_typed(ConstTag::Character, c));
					break;
				}

				case ExprKind::StringLit:
					emit_ldk(expr->loc, dst, intern_text(expr->loc, expr->string_lit.value));
					break;

				case ExprKind::SymbolLit:
					emit_ldk(expr->loc, dst, intern_name(ConstTag::Symbol, expr->symbol_lit.name));
					break;

				case ExprKind::UnknownLit:
					emit_ldk(expr->loc, dst, intern_empty(ConstTag::Unknown));
					break;

				case ExprKind::PrimRef:
					emit_ldk(expr->loc, dst, intern_global_name(expr->prim_ref.name));
					break;

				case ExprKind::VarRef:
				{
					Compiler::OpSelection sel = selection(expr, "var access");
					switch (sel.op)
					{
						case Opcode::mov:
							emit_mov(expr->loc, dst, sel.u.var.addr);
							break;
						case Opcode::ldd:
							[[fallthrough]];
						case Opcode::ldu:
						case Opcode::ldus:
							emit_load(expr->loc, sel.op, dst, sel.u.var.addr);
							break;
						default:
							JETC_DIE(db, expr->loc, "codegen: unexpected var selection %d",
							         static_cast<int>(sel.op));
					}
					break;
				}

				case ExprKind::Call:
				{
					Compiler::OpSelection sel = selection(expr, "Call");
					switch (sel.op)
					{
						case Opcode::trunc:
						{
							LirInst instruction{inst(sel.op)};
							instruction.u.unary = {dst, emit_to_any_reg(expr->call.args[0])};
							emit(expr->loc, instruction);
							release_if_temp(instruction.u.unary.src);
							break;
						}

						case Opcode::add:
						case Opcode::sub:
						case Opcode::mul:
						case Opcode::div:
						case Opcode::numeq:
						case Opcode::eq:
						case Opcode::lt:
						case Opcode::le:
						case Opcode::gt:
						case Opcode::ge:
						case Opcode::addk:
						case Opcode::subk:
						case Opcode::mulk:
						case Opcode::divk:
						case Opcode::numeqk:
						case Opcode::eqk:
						case Opcode::ltk:
						{
							bool k = takes_literal_key(sel.op);
							LirInst i = inst(sel.op);
							i.u.arith.dst = dst;
							i.u.arith.a = emit_to_any_reg(expr->call.args[0]);
							i.u.arith.b = k
							              ? intern_literal_key(expr->call.args[1])
							              : emit_to_any_reg(expr->call.args[1]);
							emit(expr->loc, i);
							release_if_temp(i.u.arith.a);
							if (!k)
							{
								release_if_temp(i.u.arith.b);
							}
							break;
						}

						case Opcode::ldf:
						case Opcode::ldfk:
						case Opcode::ldfh:
						case Opcode::ldfkh:
							emit_field_get(expr, sel, expr->call.args[0], expr->call.args[1], dst);
							break;
						case Opcode::ldfo:
						case Opcode::ldfko:
							emit_field_get(expr, sel, expr->call.args[0], expr->call.args[1], dst,
							               expr->call.args[2]);
							break;

						default:
						{
							// A self-tail call jumps back to the lambda entry; it is
							// selected only in tail position, where dst is dead.
							if (std::optional<uint16_t> result = emit_call(expr, sel))
							{
								emit_mov(expr->loc, dst, *result);
								release_reg(*result);
							}
							break;
						}
					}
					break;
				}

				case ExprKind::Apply:
				{
					uint16_t result = emit_apply(expr);
					emit_mov(expr->loc, dst, result);
					release_reg(result);
					break;
				}

				case ExprKind::Lambda:
					emit_lambda_value(expr, dst);
					break;

				case ExprKind::SetBang:
				{
					uint16_t result = emit_set_bang(expr);
					emit_mov(expr->loc, dst, result);
					release_if_temp(result);
					break;
				}

				case ExprKind::SetRef:
				{
					uint16_t result = emit_set_ref(expr);
					emit_mov(expr->loc, dst, result);
					release_if_temp(result);
					break;
				}

				case ExprKind::IterNext:
					emit_iter_next(expr, dst);
					break;

				case ExprKind::Let:
					emit_let(expr, dst);
					break;

				case ExprKind::If:
				{
					uint32_t l_alt = prog.next_label++;
					uint32_t l_end = prog.next_label++;
					if (auto fused = db.branch_fusions_.find(expr->id); fused != db.branch_fusions_.end())
					{
						Expr* cmp = fused->second;
						Compiler::OpSelection sel = selection(cmp, "fused test");
						LirInst i = inst(sel.op);
						i.u.if_cmp.id = l_alt;
						i.u.if_cmp.a = emit_to_any_reg(cmp->call.args[0]);
						bool cmp_k = takes_literal_key(sel.op);
						i.u.if_cmp.b = cmp_k ? intern_literal_key(cmp->call.args[1])
						               : emit_to_any_reg(cmp->call.args[1]);
						emit(expr->loc, i);
						release_if_temp(i.u.if_cmp.a);
						if (!cmp_k)
						{
							release_if_temp(i.u.if_cmp.b);
						}
					}
					else
					{
						uint16_t test = emit_to_any_reg(expr->if_.test);
						emit_label(expr->loc, Opcode::if_false, l_alt, test);
						release_if_temp(test);
					}
					emit_to_reg(expr->if_.consequent, dst);
					emit_label(expr->loc, Opcode::skip, l_end);
					emit_label(expr->loc, Opcode::label, l_alt);
					if (expr->if_.alternate)
					{
						emit_to_reg(expr->if_.alternate, dst);
					}
					else
					{
						emit_ldk(expr->loc, dst, intern_empty(ConstTag::Unknown));
					}
					emit_label(expr->loc, Opcode::label, l_end);
					break;
				}

				default:
					JETC_DIE(db, expr->loc, "codegen: unhandled ExprKind %d (not ANF?)",
					         static_cast<int>(expr->kind));
			}
		}
	};

	struct BytecodeEmitter
	{
		using SourceMaps = std::vector<std::vector<LambdaDebug::Line>>;

		Compiler& db;
		LirProgram& prog;

		// Rotate among JET_REPLICATE_N variants so distinct call sites
		// land on distinct asm dispatch tails (Ertl & Gregg 2003).
		size_t v_cus = 0;
		size_t v_cust = 0;
		size_t v_cl = 0;
		size_t v_clt = 0;
		size_t v_cu = 0;
		size_t v_cut = 0;
		size_t v_cself = 0;

		static size_t encoded_size(const LirInst& i)
		{
			if (i.op == Opcode::label)
			{
				return 0;
			}
			if (i.op == Opcode::clos)
			{
				return OPCODE_SIZE + sizeof(OP_clos) +
				       i.u.closure.n_captures * sizeof(OP_make_closure_capture);
			}
			return opcode_step(static_cast<uint8_t>(i.op), nullptr);
		}

		void emit_raw(Bytecode& bc, const void* data, size_t size)
		{
			const uint8_t* p = static_cast<const uint8_t*>(data);
			bc.insert(bc.end(), p, p + size);
		}

		template <typename T>
		void emit_operand(Bytecode& bc, T& val)
		{
			emit_raw(bc, &val, sizeof(T));
		}

		void debug_section(SourceMaps& source_maps, Bytecode& out)
		{
			std::vector<uint32_t> file_ids;
			auto&& file_index = [&](uint32_t file) -> uint32_t
			{
				for (uint32_t i = 0; i < file_ids.size(); ++i)
				{
					if (file_ids[i] == file)
					{
						return i;
					}
				}
				JETC_DIE_UNLESS(
					db,
					SourceLoc::none(),
					file < db.file_table.size(),
					"codegen: unknown source file id %u",
					file
					);
				file_ids.push_back(file);
				return static_cast<uint32_t>(file_ids.size() - 1);
			};
			// Single pass: resolve each record's file id in place (first occurrence order,
			// die on invalid id).
			for (std::vector<LambdaDebug::Line>& lines : source_maps)
			{
				for (LambdaDebug::Line& e : lines)
				{
					e.file = file_index(e.file);
				}
			}

			// [u32 n_files][file names \0-terminated][u32 n_source_maps][per source map: u32 n_entries]
			// [{u32 off, u32 line, u32 file}...]
			uint32_t n_files = static_cast<uint32_t>(file_ids.size());
			emit_raw(out, &n_files, sizeof(n_files));
			for (uint32_t id : file_ids)
			{
				out.insert(out.end(), db.file_table[id].begin(), db.file_table[id].end());
				out.push_back('\0');
			}
			uint32_t n_source_maps = static_cast<uint32_t>(source_maps.size());
			emit_raw(out, &n_source_maps, sizeof(n_source_maps));
			for (const std::vector<LambdaDebug::Line>& lines : source_maps)
			{
				uint32_t n_entries = static_cast<uint32_t>(lines.size());
				emit_raw(out, &n_entries, sizeof(n_entries));
				for (const LambdaDebug::Line& record : lines)
				{
					emit_raw(out, &record, sizeof(record));
				}
			}
		}

		void emit_opcode(Bytecode& bc, Opcode op)
		{
			size_t at = bc.size();
			bc.resize(at + OPCODE_SIZE);
			bc[at + VM_OP_SLOT_SIZE] = static_cast<uint8_t>(op);
		}

		void emit_replicated(Bytecode& bc, Opcode base, size_t& counter)
		{
			int offset = static_cast<int>(counter++ % JET_REPLICATE_N);
			emit_opcode(bc, static_cast<Opcode>(static_cast<int>(base) + offset));
		}

		size_t label_target(SourceLoc loc, std::unordered_map<uint32_t, size_t>& label_pos, uint32_t id)
		{
			auto it = label_pos.find(id);
			JETC_DIE_WHEN(db, loc, it == label_pos.end(), "lir emit: unresolved label %u", id);
			return it->second;
		}

		std::vector<LambdaDebug::Line> fill_lambda_entry(uint16_t slot)
		{
			// Pool entry: [tag=Lambda][is_n_ary][n_params if !is_n_ary][n_regs][code_size][bytes...][name\0]
			LirLambda& L = prog.lambdas[static_cast<uint32_t>(prog.pool_to_lambda[slot])];
			std::vector<LambdaDebug::Line> lines;
			Bytecode body = emit_code(L, lines);

			std::string entry;
			entry.push_back(static_cast<char>(ConstTag::Lambda));
			bool is_n_ary = L.is_variadic;
			entry.append(reinterpret_cast<char*>(&is_n_ary), sizeof(is_n_ary));
			if (!is_n_ary)
			{
				size_t n = static_cast<size_t>(L.n_params);
				entry.append(reinterpret_cast<char*>(&n), sizeof(n));
			}
			uint16_t n_regs = narrow_or_die<uint16_t>(db, L.loc, L.frame_regs());
			entry.append(reinterpret_cast<char*>(&n_regs), sizeof(n_regs));
			size_t code_size = body.size();
			entry.append(reinterpret_cast<char*>(&code_size), sizeof(code_size));
			entry.append(reinterpret_cast<char*>(body.data()), code_size);
			if (!L.lambda_name.empty())
			{
				entry.append(L.lambda_name.data(), L.lambda_name.size());
			}
			entry.push_back('\0');
			prog.pool[slot] = std::move(entry);
			return lines;
		}

		Bytecode emit_code(LirLambda& L, std::vector<LambdaDebug::Line>& lines)
		{
			size_t write = 0;
			for (size_t read = 0; read < L.code.size(); ++read)
			{
				if (read + 1 < L.code.size() && L.code[read].op == Opcode::mov
				    && L.code[read + 1].op == Opcode::mov)
				{
					LirInst fused{};
					fused.loc = L.code[read].loc;
					fused.op = Opcode::mov2;
					fused.u.mov2.dst0 = L.code[read].u.mov.dst;
					fused.u.mov2.src0 = L.code[read].u.mov.src;
					fused.u.mov2.dst1 = L.code[read + 1].u.mov.dst;
					fused.u.mov2.src1 = L.code[read + 1].u.mov.src;
					L.code[write++] = fused;
					++read;
				}
				else
				{
					L.code[write++] = L.code[read];
				}
			}
			L.code.resize(write);

			std::unordered_map<uint32_t, size_t> label_pos;
			size_t off = 0;
			int last_line = 0;
			uint32_t last_file = 0;
			for (LirInst& i : L.code)
			{
				if (i.op == Opcode::label)
				{
					label_pos[i.u.label.id] = off;
				}
				else
				{
					if (i.loc.line != 0 && (i.loc.line != last_line || i.loc.file_id != last_file))
					{
						last_line = i.loc.line;
						last_file = i.loc.file_id;
						lines.push_back({static_cast<uint32_t>(off), static_cast<uint32_t>(i.loc.line),
						                 last_file});
					}
					off += encoded_size(i);
				}
			}

			Bytecode bc;
			for (LirInst& i : L.code)
			{
				emit_inst(bc, L, i, label_pos);
			}
			return bc;
		}

		void emit_inst(Bytecode& bc, LirLambda& L, LirInst& i,
		               std::unordered_map<uint32_t, size_t>& label_pos)
		{
			switch (i.op)
			{
				case Opcode::label:
					break;

				case Opcode::trunc:
					emit_opcode(bc, i.op);
					emit_operand(bc, i.u.unary);
					break;

				case Opcode::mov:
				{
					emit_opcode(bc, Opcode::mov);
					OP_mov op{};
					op.dst = i.u.mov.dst;
					op.src = i.u.mov.src;
					emit_operand(bc, op);
					break;
				}

				case Opcode::mov2:
				{
					emit_opcode(bc, Opcode::mov2);
					OP_mov2 op{};
					op.first.dst = i.u.mov2.dst0;
					op.first.src = i.u.mov2.src0;
					op.second.dst = i.u.mov2.dst1;
					op.second.src = i.u.mov2.src1;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldk:
				case Opcode::ldu:
				case Opcode::ldus:
				case Opcode::ldd:
				{
					emit_opcode(bc, i.op);
					OP_ldk op{};
					op.dst = i.u.load.dst;
					op.idx = i.u.load.idx;
					emit_operand(bc, op);
					break;
				}

				case Opcode::stu:
				case Opcode::std:
				{
					emit_opcode(bc, i.op);
					OP_stu op{};
					op.idx = i.u.store.idx;
					op.src = i.u.store.src;
					emit_operand(bc, op);
					break;
				}

				case Opcode::box:
				{
					emit_opcode(bc, Opcode::box);
					OP_box op{};
					op.reg = i.u.box.reg;
					emit_operand(bc, op);
					break;
				}

				case Opcode::clos:
				{
					emit_opcode(bc, Opcode::clos);
					OP_clos op{};
					op.dst = i.u.closure.dst;
					op.pool_idx = i.u.closure.pool_idx;
					op.n_captures = i.u.closure.n_captures;
					emit_operand(bc, op);
					for (uint16_t c = 0; c < i.u.closure.n_captures; ++c)
					{
						emit_operand(bc, L.captures[static_cast<size_t>(i.u.closure.first_capture) + c]);
					}
					break;
				}

				case Opcode::add:
				case Opcode::sub:
				case Opcode::mul:
				case Opcode::div:
				case Opcode::numeq:
				case Opcode::eq:
				case Opcode::lt:
				case Opcode::le:
				case Opcode::gt:
				case Opcode::ge:
				case Opcode::addk:
				case Opcode::subk:
				case Opcode::mulk:
				case Opcode::divk:
				case Opcode::numeqk:
				case Opcode::eqk:
				case Opcode::ltk:
				{
					emit_opcode(bc, i.op);
					OP_binop_rr op{};
					op.dst = i.u.arith.dst;
					op.a = i.u.arith.a;
					op.b = i.u.arith.b;
					emit_operand(bc, op);
					break;
				}

				case Opcode::if_false:
				{
					emit_opcode(bc, Opcode::if_false);
					OP_if_false op{};
					op.src = i.u.label.src;
					op.size = static_cast<uint32_t>(
						label_target(i.loc, label_pos, i.u.label.id) - (bc.size() + sizeof(OP_if_false)));
					emit_operand(bc, op);
					break;
				}

				case Opcode::if_numeq:
				case Opcode::if_eq:
				case Opcode::if_lt:
				case Opcode::if_le:
				case Opcode::if_gt:
				case Opcode::if_ge:
				case Opcode::if_numeqk:
				case Opcode::if_eqk:
				case Opcode::if_ltk:
				{
					emit_opcode(bc, i.op);
					OP_if_cmp op{};
					op.a = i.u.if_cmp.a;
					op.b = i.u.if_cmp.b;
					op.size = static_cast<uint32_t>(
						label_target(i.loc, label_pos, i.u.if_cmp.id) - (bc.size() + sizeof(OP_if_cmp)));
					emit_operand(bc, op);
					break;
				}

				case Opcode::skip:
				{
					emit_opcode(bc, Opcode::skip);
					OP_skip op{};
					op.size = label_target(i.loc, label_pos, i.u.label.id) - (bc.size() + sizeof(OP_skip));
					emit_operand(bc, op);
					break;
				}

				case Opcode::retv:
				{
					emit_opcode(bc, Opcode::retv);
					OP_retv op{};
					op.src = i.u.ret.src;
					emit_operand(bc, op);
					break;
				}

				case Opcode::call:
				case Opcode::tcall:
				{
					emit_opcode(bc, i.op);
					OP_call op{};
					op.w = i.u.call.w;
					op.callee = i.u.call.callee;
					op.nargs = i.u.call.nargs;
					emit_operand(bc, op);
					break;
				}

				case Opcode::call_self_tail:
				{
					emit_opcode(bc, Opcode::call_self_tail);
					OP_call_self_tail op{};
					op.w = i.u.call.w;
					op.nargs = i.u.call.nargs;
					emit_operand(bc, op);
					break;
				}

				case Opcode::apply:
				{
					emit_opcode(bc, Opcode::apply);
					OP_apply op{};
					op.w = i.u.call.w;
					emit_operand(bc, op);
					break;
				}

				case Opcode::reset:
				case Opcode::coro:
				{
					emit_opcode(bc, i.op);
					OP_reset op{};
					op.w = i.u.call.w;
					emit_operand(bc, op);
					break;
				}

				case Opcode::iter_next1:
				{
					emit_opcode(bc, i.op);
					OP_iter_next1 op{};
					op.cursor = i.u.iter.cursor;
					op.dst = i.u.iter.dst0;
					op.size = static_cast<uint32_t>(
						label_target(i.loc, label_pos, i.u.iter.id) - (bc.size() + sizeof(OP_iter_next1)));
					emit_operand(bc, op);
					break;
				}

				case Opcode::iter_next2:
				{
					emit_opcode(bc, i.op);
					OP_iter_next2 op{};
					op.cursor = i.u.iter.cursor;
					op.dst0 = i.u.iter.dst0;
					op.dst1 = i.u.iter.dst1;
					op.size = static_cast<uint32_t>(
						label_target(i.loc, label_pos, i.u.iter.id) - (bc.size() + sizeof(OP_iter_next2)));
					emit_operand(bc, op);
					break;
				}

				case Opcode::call_upval_slot_0:
				case Opcode::call_upval_slot_tail_0:
				{
					emit_replicated(bc, i.op, i.op == Opcode::call_upval_slot_tail_0 ? v_cust : v_cus);
					OP_call_slot op{};
					op.w = i.u.call.w;
					op.upvalue_idx = i.u.call.upvalue_idx;
					op.nargs = i.u.call.nargs;
					emit_operand(bc, op);
					break;
				}

				case Opcode::call_local_0:
				case Opcode::call_local_tail_0:
				case Opcode::call_upval_0:
				case Opcode::call_upval_tail_0:
				{
					size_t& counter = i.op == Opcode::call_local_0
					                  ? v_cl
					                  : i.op == Opcode::call_local_tail_0
					                  ? v_clt
					                  : i.op == Opcode::call_upval_0
					                  ? v_cu
					                  : v_cut;
					emit_replicated(bc, i.op, counter);
					OP_call_atom op{};
					op.w = i.u.call.w;
					op.idx = i.u.call.idx;
					op.nargs = i.u.call.nargs;
					emit_operand(bc, op);
					break;
				}

				case Opcode::call_self_0:
				{
					emit_replicated(bc, i.op, v_cself);
					OP_call_self op{};
					op.w = i.u.call.w;
					op.nargs = i.u.call.nargs;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldf:
				{
					emit_opcode(bc, Opcode::ldf);
					OP_ldf op{};
					op.dst = i.u.field.dst;
					op.obj = i.u.field.obj;
					op.key = i.u.field.key;
					emit_operand(bc, op);
					break;
				}

				case Opcode::stf:
				{
					emit_opcode(bc, Opcode::stf);
					OP_stf op{};
					op.obj = i.u.field.obj;
					op.key = i.u.field.key;
					op.val = i.u.field.val;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldfk:
				{
					emit_opcode(bc, Opcode::ldfk);
					OP_ldfk op{};
					op.dst = i.u.field.dst;
					op.obj = i.u.field.obj;
					op.key_idx = i.u.field.key;
					emit_operand(bc, op);
					break;
				}

				case Opcode::stfk:
				{
					emit_opcode(bc, Opcode::stfk);
					OP_stfk op{};
					op.obj = i.u.field.obj;
					op.key_idx = i.u.field.key;
					op.val = i.u.field.val;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldfh:
				{
					emit_opcode(bc, Opcode::ldfh);
					OP_ldfh op{};
					op.dst = i.u.field.dst;
					op.obj = i.u.field.obj;
					op.key = i.u.field.key;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldfkh:
				{
					emit_opcode(bc, Opcode::ldfkh);
					OP_ldfkh op{};
					op.dst = i.u.field.dst;
					op.obj = i.u.field.obj;
					op.key_idx = i.u.field.key;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldfo:
				{
					emit_opcode(bc, Opcode::ldfo);
					OP_ldfo op{};
					op.dst = i.u.field.dst;
					op.obj = i.u.field.obj;
					op.key = i.u.field.key;
					op.dfl = i.u.field.val;
					emit_operand(bc, op);
					break;
				}

				case Opcode::ldfko:
				{
					emit_opcode(bc, Opcode::ldfko);
					OP_ldfko op{};
					op.dst = i.u.field.dst;
					op.obj = i.u.field.obj;
					op.key_idx = i.u.field.key;
					op.dfl = i.u.field.val;
					emit_operand(bc, op);
					break;
				}

				default:
					JETC_DIE(db, i.loc, "lir emit: unexpected opcode %d", static_cast<int>(i.op));
			}
		}

		Bytecode emit()
		{
			std::vector<LambdaDebug::Line> toplevel_lines;
			Bytecode code = emit_code(prog.lambdas[0], toplevel_lines);
			SourceMaps source_maps;
			for (size_t slot = 0; slot < prog.pool.size(); ++slot)
			{
				if (prog.pool_to_lambda[slot] >= 0)
				{
					source_maps.push_back(fill_lambda_entry(static_cast<uint16_t>(slot)));
				}
			}
			source_maps.push_back(std::move(toplevel_lines));

			// [debug section][u32 n_toplevel_slots][u32 n_pool_entries][pool entries][toplevel code]
			Bytecode out;
			debug_section(source_maps, out);
			uint32_t n_slots = narrow_or_die<uint32_t>(db, prog.lambdas[0].loc, prog.lambdas[0].frame_regs());
			uint8_t* sp = reinterpret_cast<uint8_t*>(&n_slots);
			out.insert(out.end(), sp, sp + sizeof(n_slots));
			uint32_t n = static_cast<uint32_t>(prog.pool.size());
			uint8_t* np = reinterpret_cast<uint8_t*>(&n);
			out.insert(out.end(), np, np + sizeof(n));
			for (std::string& entry : prog.pool)
			{
				out.insert(out.end(), entry.begin(), entry.end());
			}
			out.insert(out.end(), code.begin(), code.end());
			return out;
		}
	};

} // namespace

Bytecode Compiler::compile()
{
	Program& program = ast();

	LirProgram lir;
	LirEmitter emitter{*this, lir};
	emitter.emit_program(program);

	return BytecodeEmitter{*this, lir}.emit();
}

Bytecode compile(std::string source, std::string filename, CompileFlags flags, std::string_view prelude)
{
	Compiler compiler;
	compiler.source = std::move(source);
	compiler.filename = std::move(filename);
	compiler.prelude = prelude;
	compiler.flags_ = flags;
	return compiler.compile();
}

namespace
{

	Atom datum_to_atom(VmState& s, Expr* e)
	{
		switch (e->kind)
		{
			case ExprKind::NumberLit:
			{
				double v = number_lit_value(e->number_lit.text);
				return box(Number::from_ieee(v));
			}
			case ExprKind::StringLit:
				return s.gc.alloc_tagged<String>(s, e->string_lit.value);
			case ExprKind::BooleanLit:
				return box(e->boolean_lit.value);
			case ExprKind::CharacterLit:
				return box(static_cast<Character>(e->character_lit.value));
			case ExprKind::SymbolLit:
				return box(s.symbols.intern(e->symbol_lit.name));
			case ExprKind::Call:
			{
				Expr* proc = e->call.proc;
				JET_DIE_UNLESS(&s, proc->kind == ExprKind::VarRef, "datum_to_atom: bad call proc");
				std::string_view name = proc->var_ref.name;
				if (name == "list")
				{
					Atom result = box(EmptyList{});
					for (size_t i = e->call.args.size(); i-- > 0;)
					{
						result = cons(s, datum_to_atom(s, e->call.args[i]), result);
					}
					return result;
				}
				if (name == "cons")
				{
					JET_DIE_UNLESS(&s, e->call.args.size() == 2, "datum_to_atom: cons arity");
					return cons(s, datum_to_atom(s, e->call.args[0]),
					            datum_to_atom(s, e->call.args[1]));
				}
				if (name == "vector")
				{
					Vec v;
					for (uint32_t i = 0; i < e->call.args.size(); ++i)
					{
						v.push_back(datum_to_atom(s, e->call.args[i]));
					}
					return s.gc.alloc_tagged<Vec>(s, std::move(v));
				}
				if (name == "bytevector")
				{
					ByteVector bv;
					bv.reserve(e->call.args.size());
					for (uint32_t i = 0; i < e->call.args.size(); ++i)
					{
						Atom byte_val = datum_to_atom(s, e->call.args[i]);
						bv.push_back(static_cast<uint8_t>(unbox<Number>(byte_val)));
					}
					return s.gc.alloc_tagged<ByteVector>(s, std::move(bv));
				}
				if (is_struct_constructor(name))
				{
					Atom* bound_type = s.env.lookup(name);
					JET_DIE_UNLESS(
						&s,
						bound_type,
						"read: '%.*s' is unbound",
						static_cast<int>(name.size()),
						name.data()
						);
					std::vector<Atom> args;
					args.reserve(e->call.args.size());
					for (uint32_t i = 0; i < e->call.args.size(); ++i)
					{
						args.push_back(datum_to_atom(s, e->call.args[i]));
					}
					return construct_struct(s, unbox<StructType>(*bound_type), args.data(),
					                        args.data() + args.size());
				}
				JET_DIE(&s, "datum_to_atom: unexpected call proc");
			}
			default:
				JET_DIE(&s, "datum_to_atom: unexpected ExprKind %d", static_cast<int>(e->kind));
		}
	}

	Atom read_port(VmState& vm, Atom p)
	{
		Compiler db;
		db.file_table.push_back("<read>");
		Port* base = slow_unbox<Port>(vm, p);
		JET_DIE_UNLESS(&vm, base->is_input(), "read: not an input port");
		IPort* port = static_cast<IPort*>(base);
		LexState lex{port, db, 0};
		lex.read_mode = true;
		ParseState parser{
			.tokens = {},
			.stream_lex = &lex,
			.db = db,
		};
		if (parser.at_end())
		{
			return make_eof();
		}
		Expr* datum = parser.parse_datum();
		return datum_to_atom(vm, datum);
	}

} // namespace

void init_reader(VmState& s)
{
	Env& e = s.env;
	e.bind("read", make_prim<read_port>(s));
}

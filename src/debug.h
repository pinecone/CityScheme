// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef debug_h
#define debug_h

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "opcodes.h"
#include "platform.h"

#ifdef JET_DEBUG
#  include <cstdio>
#  define JET_LOG(fmt, ...) \
	do { \
		print(stderr, "{:<40} " fmt "\n", \
		      "[jet " __FILE__ ":" JET_LOG_STRINGIFY(__LINE__) "]" __VA_OPT__(,) __VA_ARGS__); \
	} while (0)
#  define JET_LOG_STRINGIFY(x) JET_LOG_STRINGIFY_(x)
#  define JET_LOG_STRINGIFY_(x) #x
#else
#  define JET_LOG(fmt, ...) do { (void)sizeof(fmt); } while (0)
#endif

struct VmState;
struct Frame;
using Code = uint8_t;
class Atom;

#include <cstdio>

void disassemble(FILE* out, Code* bc, size_t bc_size);

#ifdef JET_TRACE

extern bool g_trace_enabled;

void trace_step(VmState& s, Frame* frame, Code* pc, Atom* stack_top);

#define JET_TRACE_STEP(s, frame, pc, stack_top)                                                             \
	do                                                                                                       \
	{                                                                                                        \
		if (g_trace_enabled)                                                                                 \
		{                                                                                                    \
			trace_step((s), (frame), (pc), (stack_top));                                                     \
		}                                                                                                    \
	} while (0)

#else

#define JET_TRACE_STEP(s, frame, pc, stack_top) ((void)0)

#endif

#ifdef JET_PROFILE

#include <atomic>
#include <deque>
#include <thread>

enum class FieldReceiver : uint8_t
{
	Vector,
	String,
	Bytevector,
	SchemeStruct,
	Tuple,
	HashSet,
	HashMap,
	Cursor,
	Other,
	Count
};

enum class FieldOutcome : uint8_t
{
	HitHit,
	HitMiss,
	MissHit,
	MissMiss,
	Count
};

struct IcMisses
{
	uint64_t first{};
	uint64_t same_code{};
	uint64_t changed{};
	uint64_t invalidated{};

	uint64_t total() const { return first + same_code + changed + invalidated; }
};

struct FieldProfile
{
	uint64_t count{};
	uint64_t outcome_counts[static_cast<size_t>(FieldOutcome::Count)]{};
};

struct WorkProfile
{
	uint64_t count{};
	uint64_t samples{};
};

struct PrimitiveProfile
{
	std::string name;
	WorkProfile work{};
};

struct SiteProfile
{
	const Code* owner{};
	size_t offset{};
	uint8_t op{};
	WorkProfile work{};
	IcMisses misses{};
	const Code* callee_code{};
};

struct ProfileContext
{
	uint8_t op{};
	SiteProfile* site{};
	FieldProfile* field{};
	size_t outcome{};
	bool linked{};
};

struct ProfileDurations
{
	uint64_t count{};
	uint64_t total{};
	uint64_t minimum{UINT64_MAX};
	uint64_t maximum{};
	uint64_t buckets[1024]{};

	void add(uint64_t ns);
	uint64_t quantile(double fraction) const;
};

struct Profile
{
	uint64_t op_counts[256]{};
	uint64_t op_samples[256]{};
	uint64_t ic_misses[256]{};
	uintptr_t code_begin{};
	size_t code_size{};
	std::vector<uint32_t> site_index;
	std::vector<SiteProfile> sites;
	std::deque<PrimitiveProfile> primitives;
	FieldProfile fields[256][static_cast<size_t>(FieldReceiver::Count)]{};
	uint64_t pair_after[256][256]{};
	uint64_t lambda_calls{};
	uint64_t prim_calls{};
	uint64_t gc_collections{};
	uint64_t gc_samples{};
	uint64_t host_samples{};
	uint64_t entry_samples{};
	uint64_t start_ns{};
	ProfileContext context;
	ProfileDurations gc_pauses;
	ProfileDurations host_calls;
	std::atomic<uint64_t*> active{};
	std::jthread sampler;

	struct GcTimer
	{
		Profile& profile;
		uint64_t start;
		uint64_t* samples;

		explicit GcTimer(Profile& profile);
		~GcTimer();
	};

	struct HostTimer
	{
		Profile& profile;
		uint64_t start;
		ProfileContext context;
		uint64_t* samples;

		explicit HostTimer(Profile& profile);
		~HostTimer();
	};

	~Profile();
	void sample();
	void begin();
	void stop();
	void bind(std::string_view name, Atom atom);
	void prepare(const VmState& state, const Code* code, size_t size);

	void select(uint64_t* samples)
	{
		active.store(samples, std::memory_order_release);
	}

	JET_ALWAYS_INLINE void op(const Code* instruction)
	{
		uint8_t op{instruction[OPCODE_SIZE - 1]};
		if (context.linked)
		{
			++pair_after[context.op][op];
		}
		++op_counts[op];
		context = {.op = op, .linked = true};

		uint64_t* samples{&op_samples[op]};
		uintptr_t offset{reinterpret_cast<uintptr_t>(instruction) - code_begin};
		if (offset < code_size)
		{
			SiteProfile& site{sites[site_index[offset / OPCODE_SIZE]]};
			++site.work.count;
			context.site = &site;
			samples = &site.work.samples;
		}
		select(samples);
	}

	void primitive(PrimitiveProfile* primitive)
	{
		++prim_calls;
		++primitive->work.count;
		select(&primitive->work.samples);
	}

	void field(Opcode op, FieldReceiver receiver, bool hit)
	{
		FieldProfile& field{fields[static_cast<size_t>(op)][static_cast<size_t>(receiver)]};
		context.field = &field;
		context.outcome = hit ? 0 : 2;
		++field.count;
		++field.outcome_counts[context.outcome];
	}

	void key_miss()
	{
		FieldProfile& field{*context.field};
		--field.outcome_counts[context.outcome];
		context.outcome |= 1;
		++field.outcome_counts[context.outcome];
	}

	void miss(uint64_t cached, uint64_t current, const Code* code = nullptr)
	{
		++ic_misses[context.op];
		SiteProfile* site{context.site};
		if (site == nullptr)
		{
			return;
		}
		if (cached == 0)
		{
			++site->misses.first;
		}
		else if (code != site->callee_code)
		{
			++site->misses.changed;
		}
		else if (cached == current)
		{
			++site->misses.invalidated;
		}
		else if (code != nullptr)
		{
			++site->misses.same_code;
		}
		else
		{
			++site->misses.changed;
		}
		site->callee_code = code;
	}
};

extern Profile g_profile;

inline uint64_t profile_wall_ns()
{
	timespec stamp;
	clock_gettime(CLOCK_MONOTONIC, &stamp);
	return static_cast<uint64_t>(stamp.tv_sec) * 1000000000ULL + static_cast<uint64_t>(stamp.tv_nsec);
}

#define JET_PROFILE_OP(instruction) g_profile.op(instruction)
#define JET_PROFILE_LAMBDA (++g_profile.lambda_calls)
#define JET_PROFILE_PRIM g_profile.primitive(unbox<Prim>(callee)->profile)
#define JET_PROFILE_GC (++g_profile.gc_collections)
#define JET_PROFILE_FIELD_DISPATCH(op, receiver, hit) g_profile.field(op, receiver, hit)
#define JET_PROFILE_FIELD_KEY_MISS() g_profile.key_miss()
#define JET_PROFILE_GC_TIMER Profile::GcTimer profile_gc_timer{g_profile}
#define JET_PROFILE_BEGIN(vm) profile_begin(vm)
#define JET_PROFILE_MISS(cached, current) g_profile.miss(cached, current)
#define JET_PROFILE_CALL_MISS(cached, current) \
	g_profile.miss(cached, current.bits, \
	               is_type<jet::Type::Procedure>(current) ? unbox<Lambda>(current)->code : nullptr)
#define JET_PROFILE_HOST Profile::HostTimer profile_host_timer{g_profile}
#define JET_PROFILE_BIND(name, atom) g_profile.bind(name, atom)
#define JET_PROFILE_PREPARE(state, code, size) g_profile.prepare(state, code, size)

FieldReceiver profile_field_receiver(Atom object);
void profile_begin(VmState& vm);

#else

#define JET_PROFILE_OP(op) ((void)0)
#define JET_PROFILE_LAMBDA ((void)0)
#define JET_PROFILE_PRIM ((void)0)
#define JET_PROFILE_GC ((void)0)
#define JET_PROFILE_FIELD_DISPATCH(op, kind, hit) ((void)0)
#define JET_PROFILE_FIELD_KEY_MISS() ((void)0)
#define JET_PROFILE_GC_TIMER ((void)0)
#define JET_PROFILE_BEGIN(vm) ((void)0)
#define JET_PROFILE_MISS(cached, current) ((void)0)
#define JET_PROFILE_CALL_MISS(cached, current) ((void)0)
#define JET_PROFILE_HOST ((void)0)
#define JET_PROFILE_BIND(name, atom) ((void)0)
#define JET_PROFILE_PREPARE(state, code, size) ((void)0)

#endif

#endif

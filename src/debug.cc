// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "debug.h"

#include "opcodes.h"
#include "runtime.h"
#include "vm.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

const char* opcode_name(uint8_t op);

#ifdef JET_PROFILE

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>

Profile g_profile;

static VmState* profile_vm;

static void profile_print(const VmState& state);

void profile_begin(VmState& vm)
{
	JET_DIE_WHEN(&vm, profile_vm != nullptr, "profiler VM already registered");
	profile_vm = &vm;
	int result{std::atexit([]
		{
			profile_print(*profile_vm);
			profile_vm->~VmState();
		})};
	JET_DIE_WHEN(&vm, result != 0, "cannot register profiler exit handler");
	g_profile.begin();
}

static_assert(std::atomic<uint64_t*>::is_always_lock_free);
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<uint64_t>::required_alignment <= alignof(uint64_t));

Profile::~Profile()
{
	stop();
}

void Profile::sample()
{
	uint64_t* samples{active.load(std::memory_order_acquire)};
	if (samples != nullptr)
	{
		std::atomic_ref<uint64_t>{*samples}.fetch_add(1, std::memory_order_relaxed);
	}
}

void Profile::begin()
{
	JET_DIE_WHEN(nullptr, sampler.joinable(), "profiler already running");
	start_ns = profile_wall_ns();
	select(&host_samples);
	sampler = std::jthread{[this](std::stop_token stop)
		{
			while (!stop.stop_requested())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
				if (!stop.stop_requested())
				{
					sample();
				}
			}
		}};
}

void Profile::stop()
{
	sampler.request_stop();
	if (sampler.joinable())
	{
		sampler.join();
	}
	select(nullptr);
}

Profile::GcTimer::GcTimer(Profile& profile)
	: profile{profile}, start{profile_wall_ns()},
	samples{profile.active.load(std::memory_order_relaxed)}
{
	profile.select(&profile.gc_samples);
}

Profile::GcTimer::~GcTimer()
{
	profile.gc_pauses.add(profile_wall_ns() - start);
	profile.select(samples);
}

Profile::HostTimer::HostTimer(Profile& profile)
	: profile{profile}, start{profile_wall_ns()}, context{profile.context},
	samples{profile.active.load(std::memory_order_relaxed)}
{
	profile.context = {};
	profile.select(&profile.entry_samples);
}

Profile::HostTimer::~HostTimer()
{
	profile.host_calls.add(profile_wall_ns() - start);
	profile.context = context;
	profile.select(samples);
}

void ProfileDurations::add(uint64_t ns)
{
	unsigned width{static_cast<unsigned>(std::bit_width(ns))};
	unsigned shift{width > 5 ? width - 5 : 0};
	++buckets[shift * 16 + (ns >> shift)];
	++count;
	total += ns;
	minimum = std::min(minimum, ns);
	maximum = std::max(maximum, ns);
}

uint64_t ProfileDurations::quantile(double fraction) const
{
	uint64_t rank{std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(fraction * count)))};
	uint64_t seen{0};
	for (size_t index = 0; index < std::size(buckets); ++index)
	{
		seen += buckets[index];
		if (seen >= rank)
		{
			unsigned shift{index < 32 ? 0 : static_cast<unsigned>(index / 16 - 1)};
			uint64_t lower{index < 32 ? index : uint64_t{16 + index % 16} << shift};
			return std::min(maximum, lower | ((uint64_t{1} << shift) - 1));
		}
	}
	return maximum;
}

FieldReceiver profile_field_receiver(Atom object)
{
	switch (object.type())
	{
		case jet::Type::Vector:
			return FieldReceiver::Vector;
		case jet::Type::String:
			return FieldReceiver::String;
		case jet::Type::ByteVector:
			return FieldReceiver::Bytevector;
		case jet::Type::Struct:
			switch (unbox<Struct>(object)->type->kind())
			{
				case StructKind::Scheme:
					return FieldReceiver::SchemeStruct;
				case StructKind::Tuple:
					return FieldReceiver::Tuple;
				case StructKind::HashSet:
					return FieldReceiver::HashSet;
				case StructKind::HashMap:
					return FieldReceiver::HashMap;
				case StructKind::Cursor:
					return FieldReceiver::Cursor;
				case StructKind::Escape:
				case StructKind::Coro:
				case StructKind::Yield:
					return FieldReceiver::Other;
			}
		default:
			return FieldReceiver::Other;
	}
}

void Profile::bind(std::string_view name, Atom atom)
{
	if (is_type<jet::Type::Primitive>(atom))
	{
		PrimitiveProfile& primitive{*unbox<Prim>(atom)->profile};
		if (primitive.name == "<primitive>")
		{
			primitive.name = name;
		}
	}
}

void Profile::prepare(const VmState& state, const Code* code, size_t size)
{
	stop();
	code_begin = reinterpret_cast<uintptr_t>(code);
	code_size = size;
	site_index.resize(size / OPCODE_SIZE + 1);
	sites.clear();
	for (auto&& [owner, debug] : state.debug.code)
	{
		for (size_t offset = 0; offset < debug.code_size;)
		{
			const Code* instruction{owner + offset};
			uint8_t op{instruction[OPCODE_SIZE - 1]};
			// Instruction starts are at least OPCODE_SIZE bytes apart.
			size_t index{static_cast<size_t>(instruction - code) / OPCODE_SIZE};
			JET_DIE_WHEN(nullptr, sites.size() >= UINT32_MAX, "too many profile sites");
			site_index[index] = static_cast<uint32_t>(sites.size());
			sites.push_back({owner, offset, op});
			offset += opcode_step(op, instruction + OPCODE_SIZE);
		}
	}
}

static std::string profile_location(const VmState& state, const Code* owner, size_t offset)
{
	const LambdaDebug& debug{state.debug.code.at(owner)};
	const LambdaDebug::Line* line{debug.find_line(offset)};
	std::string location{"<no source>"};
	if (line != nullptr && line->file < state.debug.files.size())
	{
		location = state.debug.files[line->file] + ":" + std::to_string(line->line);
	}
	std::string name{debug.name};
	if (name.empty())
	{
		name = owner == state.toplevel_code ? "<toplevel>" : "<lambda>";
	}
	size_t address{reinterpret_cast<uintptr_t>(owner) - g_profile.code_begin};
	return location + " " + name + "@" + std::to_string(address) + "+" + std::to_string(offset);
}

static void print_sites(const VmState& state)
{
	std::vector<const SiteProfile*> sites;
	for (const SiteProfile& site : g_profile.sites)
	{
		if (site.misses.total() != 0)
		{
			sites.push_back(&site);
		}
	}
	std::sort(sites.begin(), sites.end(), [](const SiteProfile* first, const SiteProfile* second)
	{
		return first->misses.total() > second->misses.total();
	});
	print(stderr, "\nIC misses by instruction (top 25 by misses):\n");
	print(stderr, " same-code: different closure; changed: different code or non-lambda callee\n");
	print(stderr, " {:<8} {:>12} {:>12} {:>7} {:>12} {:>12} {:>12} {:>12} {}\n", "opcode", "executions",
	      "misses", "miss%", "first-fill", "same-code", "changed", "invalidated", "site");
	size_t shown{std::min<size_t>(sites.size(), 25)};
	for (size_t index = 0; index < shown; ++index)
	{
		const SiteProfile& site{*sites[index]};
		const IcMisses& misses{site.misses};
		double percent{site.work.count ? 100.0 * misses.total() / site.work.count : 0.0};
		print(stderr, " {:<8} {:12} {:12} {:6.2f}% {:12} {:12} {:12} {:12} {}\n", opcode_name(site.op),
		      site.work.count, misses.total(), percent, misses.first, misses.same_code,
		      misses.changed, misses.invalidated, profile_location(state, site.owner, site.offset));
	}
}

static void print_work(const VmState& state, uint64_t total_samples)
{
	auto&& percent = [total_samples](uint64_t samples)
	{
		return total_samples ? 100.0 * samples / total_samples : 0.0;
	};
	struct Function
	{
		const Code* owner;
		WorkProfile work;
	};
	std::unordered_map<const Code*, Function> functions;
	std::vector<const SiteProfile*> sites;
	for (const SiteProfile& site : g_profile.sites)
	{
		if (site.work.count == 0)
		{
			continue;
		}
		sites.push_back(&site);
		Function& function{functions[site.owner]};
		function.owner = site.owner;
		function.work.count += site.work.count;
		function.work.samples += site.work.samples;
	}
	std::vector<Function> ordered;
	for (auto&& [owner, function] : functions)
	{
		ordered.push_back(function);
	}
	std::sort(ordered.begin(), ordered.end(), [](const Function& first, const Function& second)
	{
		return first.work.samples > second.work.samples;
	});
	print(stderr, "\nfunction samples (top 25; primitives and gc excluded):\n");
	print(stderr, " {:>12} {:>12} {:>7} {}\n", "dispatches", "samples", "wall%", "site");
	for (size_t index = 0; index < std::min<size_t>(ordered.size(), 25); ++index)
	{
		const Function& function{ordered[index]};
		print(stderr, " {:12} {:12} {:6.2f}% {}\n", function.work.count, function.work.samples,
		      percent(function.work.samples), profile_location(state, function.owner, 0));
	}
	std::sort(sites.begin(), sites.end(), [](const SiteProfile* first, const SiteProfile* second)
	{
		return first->work.samples > second->work.samples;
	});
	print(stderr, "\ninstruction samples (top 25; primitives and gc excluded):\n");
	print(stderr, " {:<8} {:>12} {:>12} {:>7} {}\n", "opcode", "dispatches", "samples", "wall%", "site");
	for (size_t index = 0; index < std::min<size_t>(sites.size(), 25); ++index)
	{
		const SiteProfile& site{*sites[index]};
		print(stderr, " {:<8} {:12} {:12} {:6.2f}% {}\n", opcode_name(site.op), site.work.count,
		      site.work.samples, percent(site.work.samples),
		      profile_location(state, site.owner, site.offset));
	}

	std::vector<const PrimitiveProfile*> primitives;
	for (const PrimitiveProfile& primitive : g_profile.primitives)
	{
		if (primitive.work.count != 0)
		{
			primitives.push_back(&primitive);
		}
	}
	std::sort(primitives.begin(), primitives.end(), [](const PrimitiveProfile* first,
	                                                   const PrimitiveProfile* second)
	{
		return first->work.samples > second->work.samples;
	});
	print(stderr, "\nprimitive samples (nested VM calls and gc excluded):\n");
	print(stderr, " host primitives include time between callbacks\n");
	print(stderr, " {:>12} {:>12} {:>7} {}\n", "calls", "samples", "wall%", "primitive");
	for (const PrimitiveProfile* primitive : primitives)
	{
		print(stderr, " {:12} {:12} {:6.2f}% {}\n", primitive->work.count,
		      primitive->work.samples, percent(primitive->work.samples), primitive->name);
	}
}

static void print_durations(const char* name, const ProfileDurations& durations)
{
	if (durations.count == 0)
	{
		return;
	}
	auto&& upper = [&durations](double fraction)
	{
		return std::ceil(durations.quantile(fraction) / 1e3) / 1e3;
	};
	print(stderr, "\n{}: {} calls, {:.3f} ms total\n", name, durations.count, durations.total / 1e6);
	print(stderr, " min {:.3f}  p50<={:.3f}  p90<={:.3f}  p99<={:.3f}  max {:.3f}  (ms)\n",
	      durations.minimum / 1e6, upper(0.50), upper(0.90), upper(0.99), durations.maximum / 1e6);
	print(stderr, " quantile upper bounds have at most 6.25% bucket error\n");
}

static void print_fields()
{
	constexpr Opcode field_ops[] = {Opcode::ldf, Opcode::stf, Opcode::ldfk, Opcode::stfk,
		                            Opcode::ldfh, Opcode::ldfkh, Opcode::ldfo, Opcode::ldfko};
	constexpr const char* field_receivers[] = {
		"vector", "string", "bytevector", "scheme", "tuple", "hashset", "hashmap", "cursor", "other",
	};
	print(stderr, "\nfield IC outcomes:\n");
	print(stderr, " {:<8} {:<9} {:>12} {:>12} {:>12} {:>12} {:>12}\n", "opcode", "receiver", "total",
	      "hit/hit", "hit/key-miss", "recv-miss/hit", "both-miss");
	for (Opcode field_op : field_ops)
	{
		int op{static_cast<int>(field_op)};
		for (size_t receiver = 0; receiver < static_cast<size_t>(FieldReceiver::Count); ++receiver)
		{
			const FieldProfile& field = g_profile.fields[op][receiver];
			if (field.count == 0)
			{
				continue;
			}
			print(stderr, " {:<8} {:<9} {:12}", opcode_name(op), field_receivers[receiver], field.count);
			for (uint64_t count : field.outcome_counts)
			{
				print(stderr, " {:12}", count);
			}
			std::fputc('\n', stderr);
		}
	}

}

static void print_pairs(uint64_t total_ops)
{
	struct Pair
	{
		int prev;
		int curr;
		uint64_t count;
	};
	std::vector<Pair> pairs;
	pairs.reserve(256);
	for (int previous = 0; previous < 256; ++previous)
	{
		for (int current = 0; current < 256; ++current)
		{
			if (uint64_t count = g_profile.pair_after[previous][current]; count > 0)
			{
				pairs.push_back({previous, current, count});
			}
		}
	}
	size_t shown{pairs.size() < 30 ? pairs.size() : 30};
	std::sort(pairs.begin(), pairs.end(), [](const Pair& first, const Pair& second)
	{
		return first.count > second.count;
	});
	print(stderr, "\ntop dispatched pairs by count (prev -> curr):\n");
	for (size_t index = 0; index < shown; ++index)
	{
		double percent{total_ops ? 100.0 * pairs[index].count / total_ops : 0.0};
		print(stderr, " {:<14} -> {:<14} {:12} {:5.1f}%\n", opcode_name(pairs[index].prev),
		      opcode_name(pairs[index].curr), pairs[index].count, percent);
	}
}

static void profile_print(const VmState& state)
{
	uint64_t end_ns{profile_wall_ns()};
	g_profile.stop();
	if (g_profile.start_ns == 0)
	{
		return;
	}
	uint64_t elapsed_ns{end_ns - g_profile.start_ns};
	g_profile.start_ns = 0;

	uint64_t total_ops{0};
	for (int index = 0; index < 256; ++index)
	{
		total_ops += g_profile.op_counts[index];
	}

	uint64_t samples[256];
	std::copy(std::begin(g_profile.op_samples), std::end(g_profile.op_samples), samples);
	for (const SiteProfile& site : g_profile.sites)
	{
		samples[site.op] += site.work.samples;
	}
	uint64_t primitive_samples{0};
	for (const PrimitiveProfile& primitive : g_profile.primitives)
	{
		primitive_samples += primitive.work.samples;
	}
	uint64_t total_samples{primitive_samples + g_profile.gc_samples + g_profile.host_samples
		                   + g_profile.entry_samples};
	for (uint64_t count : samples)
	{
		total_samples += count;
	}

	print(stderr, "\n--- JET_PROFILE ---\n");
	print(stderr, "opcodes dispatched: {}\n", total_ops);
	print(stderr, " lambda calls: {}\n", g_profile.lambda_calls);
	print(stderr, " primitive calls: {}\n", g_profile.prim_calls);
	print(stderr, " gc collections: {}\n", g_profile.gc_collections);
	print(stderr, " wall time: {:.3f} ms\n", elapsed_ns / 1e6);
	print(stderr, " wall samples: {} (1 ms target interval; scheduling can delay samples)\n", total_samples);
	print(stderr, " counts are exact; sample shares are estimates and include profiler overhead\n");
	print(stderr, " unsampled work has unknown time; periodic work can bias sampling\n");
	print(stderr, " sites use function@bytecode-offset+instruction-offset\n");
	print(stderr, "\nopcode histogram (sorted by count):\n");

	int order[256];
	for (int index = 0; index < 256; ++index)
	{
		order[index] = index;
	}
	std::sort(order, order + 256, [](int first, int second)
	{
		return g_profile.op_counts[first] > g_profile.op_counts[second];
	});

	for (int index = 0; index < 256; ++index)
	{
		uint64_t count{g_profile.op_counts[order[index]]};
		if (count == 0)
		{
			break;
		}
		double percent{total_ops ? 100.0 * count / total_ops : 0.0};
		print(stderr, " {:<14} {:12} {:5.1f}%\n", opcode_name(order[index]), count, percent);
	}

	std::sort(order, order + 256, [&samples](int first, int second)
	{
		return samples[first] > samples[second];
	});
	print(stderr, "\nopcode samples (primitives and gc excluded):\n");
	print(stderr, " {:<14} {:>12} {:>7}\n", "opcode", "samples", "wall%");
	auto&& print_samples = [total_samples](const char* name, uint64_t count)
	{
		double percent{total_samples ? 100.0 * count / total_samples : 0.0};
		print(stderr, " {:<14} {:12} {:6.2f}%\n", name, count, percent);
	};
	for (int op : order)
	{
		if (samples[op] != 0)
		{
			print_samples(opcode_name(op), samples[op]);
		}
	}
	print_samples("(primitives)", primitive_samples);
	print_samples("(gc)", g_profile.gc_samples);
	print_samples("(host)", g_profile.host_samples);
	print_samples("(VM entry)", g_profile.entry_samples);
	print_durations("gc pauses", g_profile.gc_pauses);

	uint64_t total_ic_misses{0};
	for (int index = 0; index < 256; ++index)
	{
		total_ic_misses += g_profile.ic_misses[index];
	}
	if (total_ic_misses > 0)
	{
		std::sort(order, order + 256, [](int first, int second)
		{
			return g_profile.ic_misses[first] > g_profile.ic_misses[second];
		});

		print(stderr, "\nIC misses (sorted by miss count):\n");
		print(stderr, " {:<14} {:>12} {:>12} {:>7}\n", "opcode", "total", "misses", "miss%");
		for (int index = 0; index < 256; ++index)
		{
			int op{order[index]};
			uint64_t misses{g_profile.ic_misses[op]};
			if (misses == 0)
			{
				break;
			}
			uint64_t total{g_profile.op_counts[op]};
			double miss_pct{total ? 100.0 * misses / total : 0.0};
			print(stderr, " {:<14} {:12} {:12} {:6.2f}%\n", opcode_name(op), total, misses, miss_pct);
		}

		print_sites(state);
	}

	print_work(state, total_samples);
	print_durations("host-to-VM calls", g_profile.host_calls);
	if (g_profile.host_calls.count != 0)
	{
		print(stderr, " callback durations include gc; nested calls overlap\n");
	}
	print_fields();
	print_pairs(total_ops);
}
#endif

const char* opcode_name(uint8_t op)
{
	switch (op)
	{
#define X(name, disp, ...)                                                                                   \
	case static_cast<uint8_t>(Opcode::name):                                                                 \
		return disp;
	JET_OPCODES(X)
#undef X
		default:
			return "?unknown";
	}
}

bool is_call_slot_op(uint8_t op)
{
#define X(name, disp, n)                                                                                     \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, call_upval_slot, "cus")
	JET_REPLICATE(X, call_upval_slot_tail, "cust")
#undef X
	return false;
}

bool is_call_atom_op(uint8_t op)
{
#define X(name, disp, n)                                                                                     \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, call_local, "cl")
	JET_REPLICATE(X, call_local_tail, "clt")
	JET_REPLICATE(X, call_upval, "cu")
	JET_REPLICATE(X, call_upval_tail, "cut")
#undef X
	return false;
}

bool is_call_self_op(uint8_t op)
{
#define X(name, disp, n)                                                                                     \
	if (op == static_cast<uint8_t>(Opcode::name))                                                            \
	{                                                                                                        \
		return true;                                                                                         \
	}
	JET_REPLICATE(X, call_self, "cself")
#undef X
	return false;
}

void decode_args(FILE* out, uint8_t op, Code* p)
{
	if (is_call_slot_op(op))
	{
		OP_call_slot* o = reinterpret_cast<OP_call_slot*>(p);
		print(out, " w={} upvalue={} nargs={}", o->w, o->upvalue_idx, o->nargs);
		return;
	}
	if (is_call_atom_op(op))
	{
		OP_call_atom* o = reinterpret_cast<OP_call_atom*>(p);
		print(out, " w={} idx={} nargs={}", o->w, o->idx, o->nargs);
		return;
	}
	if (is_call_self_op(op))
	{
		OP_call_self* o = reinterpret_cast<OP_call_self*>(p);
		print(out, " w={} nargs={}", o->w, o->nargs);
		return;
	}
	switch (static_cast<Opcode>(op))
	{
		case Opcode::skip:
			print(out, " size={}", reinterpret_cast<OP_skip*>(p)->size);
			break;
		case Opcode::mov:
		case Opcode::trunc:
		{
			OP_mov* o = reinterpret_cast<OP_mov*>(p);
			print(out, " dst={} src={}", o->dst, o->src);
			break;
		}
		case Opcode::mov2:
		{
			OP_mov2* o = reinterpret_cast<OP_mov2*>(p);
			print(out, " dst0={} src0={} dst1={} src1={}", o->first.dst, o->first.src,
			      o->second.dst, o->second.src);
			break;
		}
		case Opcode::ldk:
		{
			OP_ldk* o = reinterpret_cast<OP_ldk*>(p);
			print(out, " dst={} k={}", o->dst, o->idx);
			break;
		}
		case Opcode::ldu:
		case Opcode::ldus:
		case Opcode::ldd:
		{
			OP_ldu* o = reinterpret_cast<OP_ldu*>(p);
			print(out, " dst={} idx={}", o->dst, o->idx);
			break;
		}
		case Opcode::stu:
		case Opcode::std:
		{
			OP_stu* o = reinterpret_cast<OP_stu*>(p);
			print(out, " idx={} src={}", o->idx, o->src);
			break;
		}
		case Opcode::box:
			print(out, " reg={}", reinterpret_cast<OP_box*>(p)->reg);
			break;
		case Opcode::clos:
		{
			OP_clos* o = reinterpret_cast<OP_clos*>(p);
			print(out, " dst={} idx={} n_captures={}", o->dst, o->pool_idx, o->n_captures);
			break;
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
		{
			OP_binop_rr* o = reinterpret_cast<OP_binop_rr*>(p);
			print(out, " dst={} a={} b={}", o->dst, o->a, o->b);
			break;
		}
		case Opcode::addk:
		case Opcode::subk:
		case Opcode::mulk:
		case Opcode::divk:
		case Opcode::mink:
		case Opcode::maxk:
		case Opcode::numeqk:
		case Opcode::eqk:
		case Opcode::ltk:
		{
			OP_binop_rk* o = reinterpret_cast<OP_binop_rk*>(p);
			print(out, " dst={} a={} k={}", o->dst, o->a, o->b);
			break;
		}
		case Opcode::if_false:
		{
			OP_if_false* o = reinterpret_cast<OP_if_false*>(p);
			print(out, " src={} size={}", o->src, o->size);
			break;
		}
		case Opcode::if_numeq:
		case Opcode::if_eq:
		case Opcode::if_lt:
		case Opcode::if_le:
		case Opcode::if_gt:
		case Opcode::if_ge:
		{
			OP_if_cmp* o = reinterpret_cast<OP_if_cmp*>(p);
			print(out, " a={} b={} size={}", o->a, o->b, o->size);
			break;
		}
		case Opcode::if_numeqk:
		case Opcode::if_eqk:
		case Opcode::if_ltk:
		{
			OP_if_cmp* o = reinterpret_cast<OP_if_cmp*>(p);
			print(out, " a={} k={} size={}", o->a, o->b, o->size);
			break;
		}
		case Opcode::retv:
			print(out, " src={}", reinterpret_cast<OP_retv*>(p)->src);
			break;
		case Opcode::call:
		case Opcode::tcall:
		{
			OP_call* o = reinterpret_cast<OP_call*>(p);
			print(out, " w={} callee={} nargs={}", o->w, o->callee, o->nargs);
			break;
		}
		case Opcode::call_self_tail:
		{
			OP_call_self_tail* o = reinterpret_cast<OP_call_self_tail*>(p);
			print(out, " w={} nargs={}", o->w, o->nargs);
			break;
		}
		case Opcode::apply:
			print(out, " w={}", reinterpret_cast<OP_apply*>(p)->w);
			break;
		case Opcode::reset:
		case Opcode::coro:
			print(out, " w={}", reinterpret_cast<OP_reset*>(p)->w);
			break;
		case Opcode::iter_next1:
		{
			OP_iter_next1* o = reinterpret_cast<OP_iter_next1*>(p);
			print(out, " cursor={} dst={} size={}", o->cursor, o->dst, o->size);
			break;
		}
		case Opcode::iter_next2:
		{
			OP_iter_next2* o = reinterpret_cast<OP_iter_next2*>(p);
			print(out, " cursor={} dst0={} dst1={} size={}", o->cursor, o->dst0, o->dst1, o->size);
			break;
		}
		case Opcode::ldf:
		{
			OP_ldf* o = reinterpret_cast<OP_ldf*>(p);
			print(out, " dst={} obj={} key={}", o->dst, o->obj, o->key);
			break;
		}
		case Opcode::stf:
		{
			OP_stf* o = reinterpret_cast<OP_stf*>(p);
			print(out, " obj={} key={} val={}", o->obj, o->key, o->val);
			break;
		}
		case Opcode::ldfk:
		{
			OP_ldfk* o = reinterpret_cast<OP_ldfk*>(p);
			print(out, " dst={} obj={} k={}", o->dst, o->obj, o->key_idx);
			break;
		}
		case Opcode::stfk:
		{
			OP_stfk* o = reinterpret_cast<OP_stfk*>(p);
			print(out, " obj={} k={} val={}", o->obj, o->key_idx, o->val);
			break;
		}
		case Opcode::ldfh:
		{
			OP_ldfh* o = reinterpret_cast<OP_ldfh*>(p);
			print(out, " dst={} obj={} key={}", o->dst, o->obj, o->key);
			break;
		}
		case Opcode::ldfkh:
		{
			OP_ldfkh* o = reinterpret_cast<OP_ldfkh*>(p);
			print(out, " dst={} obj={} k={}", o->dst, o->obj, o->key_idx);
			break;
		}
		case Opcode::ldfo:
		{
			OP_ldfo* o = reinterpret_cast<OP_ldfo*>(p);
			print(out, " dst={} obj={} key={} dfl={}", o->dst, o->obj, o->key, o->dfl);
			break;
		}
		case Opcode::ldfko:
		{
			OP_ldfko* o = reinterpret_cast<OP_ldfko*>(p);
			print(out, " dst={} obj={} k={} dfl={}", o->dst, o->obj, o->key_idx, o->dfl);
			break;
		}
		default:
			break;
	}
}

#ifdef JET_TRACE

bool g_trace_enabled = false;

void trace_step(VmState& s, Frame*, Code* pc, Atom* stack_top)
{
	auto&& brief = [&s](Atom a) -> std::string
	{
		std::string result;
		write_to(s, a, result);
		for (size_t i = 0; i < result.size(); ++i)
		{
			if (result[i] == '\n' || result[i] == '\r' || result[i] == '\t')
			{
				result[i] = ' ';
			}
		}
		constexpr size_t MAX_LEN = 24;
		if (result.size() > MAX_LEN)
		{
			result.resize(MAX_LEN);
			result += "...";
		}
		return result;
	};
	uint8_t op = pc[-1];
	print(stderr, "[d={} sp={}] {}", s.frames.size(), stack_top - s.stack_base, opcode_name(op));
	decode_args(stderr, op, pc);

	print(stderr, "  | top:");
	long depth = stack_top - s.stack_base;
	long show = depth < 6 ? depth : 6;
	for (long i = show; i > 0; --i)
	{
		print(stderr, " {}", brief(stack_top[-i]));
	}
	std::fputc('\n', stderr);
}

#endif

// The static disassembler doesn't require link_opcode_handlers to have run:
// it reads only the 1-byte opcode tag at +VM_OP_SLOT_SIZE and the operand
// bytes; handler slots are ignored.

namespace
{

	struct LambdaBlock
	{
		uint32_t pool_idx;
		Code* code;
		size_t size;
		size_t arity;
		bool is_n_ary;
		uint16_t n_locals;
		const char* name;
	};

	void print_source_loc(FILE* out, const LambdaDebug& dbg,
	                      const std::vector<std::string>& files, size_t off, size_t code_size)
	{
		const LambdaDebug::Line* line = dbg.find_line(off, code_size);
		if (line == nullptr || line->line == 0)
		{
			return;
		}
		std::string_view file = "?";
		if (line->file < files.size())
		{
			file = files[line->file];
		}
		print(out, "  ; {}:{}", file, line->line);
	}

	void disasm_code_block(FILE* out, Code* start, size_t size,
	                       const LambdaDebug& dbg,
	                       const std::vector<std::string>& files)
	{
		Code* p = start;
		Code* end = start + size;
		while (p < end)
		{
			size_t off = static_cast<size_t>(p - start);
			uint8_t tag = p[VM_OP_SLOT_SIZE];
			Code* operand = p + OPCODE_SIZE;
			print(out, "  {:04}  {}", off, opcode_name(tag));
			decode_args(out, tag, operand);
			print_source_loc(out, dbg, files, off, size);
			std::fputc('\n', out);
			p += opcode_step(tag, operand);
		}
	}

	const char* const_tag_name(ConstTag t)
	{
		switch (t)
		{
			case ConstTag::Number:     return "Number";
			case ConstTag::Boolean:    return "Boolean";
			case ConstTag::Character:  return "Character";
			case ConstTag::String:     return "String";
			case ConstTag::Symbol:     return "Symbol";
			case ConstTag::EmptyList:  return "EmptyList";
			case ConstTag::Unknown:    return "Unknown";
			case ConstTag::GlobalName: return "GlobalName";
			case ConstTag::Lambda:     return "Lambda";
		}
		return "?";
	}

	Code* disasm_pool_entry(FILE* out, Code* p, uint32_t idx, std::vector<LambdaBlock>& lambdas)
	{
		ConstTag tag = static_cast<ConstTag>(*p++);
		print(out, "  [{:4}] {:<10} ", idx, const_tag_name(tag));
		switch (tag)
		{
			case ConstTag::Number:
			{
				double n;
				std::memcpy(&n, p, sizeof(n));
				print(out, "{}\n", n);
				return p + sizeof(n);
			}
			case ConstTag::Boolean:
			{
				bool b;
				std::memcpy(&b, p, sizeof(b));
				print(out, "{}\n", b ? "#t" : "#f");
				return p + sizeof(b);
			}
			case ConstTag::Character:
			{
				Character c;
				std::memcpy(&c, p, sizeof(c));
				print(out, "U+{:04x}\n", c);
				return p + sizeof(c);
			}
			case ConstTag::String:
			{
				uint32_t n_string_bytes;
				std::memcpy(&n_string_bytes, p, sizeof(n_string_bytes));
				p += sizeof(n_string_bytes);
				std::string_view text{reinterpret_cast<const char*>(p), n_string_bytes};
				print(out, "\"{}\"\n", text);
				return p + n_string_bytes;
			}
			case ConstTag::Symbol:
			case ConstTag::GlobalName:
			{
				const char* s = reinterpret_cast<const char*>(p);
				print(out, "\"{}\"\n", s);
				return p + std::strlen(s) + 1;
			}
			case ConstTag::EmptyList:
			case ConstTag::Unknown:
				std::fputc('\n', out);
				return p;
			case ConstTag::Lambda:
			{
				bool is_n_ary;
				std::memcpy(&is_n_ary, p, sizeof(is_n_ary));
				p += sizeof(is_n_ary);
				size_t arity = 0;
				if (!is_n_ary)
				{
					std::memcpy(&arity, p, sizeof(arity));
					p += sizeof(arity);
				}
				uint16_t n_locals;
				std::memcpy(&n_locals, p, sizeof(n_locals));
				p += sizeof(n_locals);
				size_t code_size;
				std::memcpy(&code_size, p, sizeof(code_size));
				p += sizeof(code_size);
				Code* code = p;
				const char* name = reinterpret_cast<const char*>(code + code_size);
				print(out, "arity={}{} n_locals={} code_size={}", is_n_ary ? "n-ary≥" : "", arity,
				      n_locals, code_size);
				if (*name)
				{
					print(out, " name=\"{}\"", name);
				}
				std::fputc('\n', out);
				lambdas.push_back({idx, code, code_size, arity, is_n_ary, n_locals, name});
				return code + code_size + std::strlen(name) + 1;
			}
		}
		return p;
	}

} // anon

void disassemble(FILE* out, Code* bc, size_t bc_size)
{
	Code* p = bc;
	Code* end = bc + bc_size;
	std::vector<std::string> files;
	std::vector<LambdaDebug> source_maps;
	p = parse_debug_section(nullptr, p, end, files, source_maps);

	auto require_bytes = [&](size_t n, const char* what)
	{
		JET_DIE_WHEN(nullptr, static_cast<size_t>(end - p) < n,
		             "invalid bytecode: not enough bytes for {}", what);
	};

	uint32_t n_toplevel_slots, n_constants;
	require_bytes(sizeof(n_toplevel_slots), "n_toplevel_slots");
	std::memcpy(&n_toplevel_slots, p, sizeof(n_toplevel_slots));
	p += sizeof(n_toplevel_slots);
	require_bytes(sizeof(n_constants), "n_constants");
	std::memcpy(&n_constants, p, sizeof(n_constants));
	p += sizeof(n_constants);

	print(out, "=== header ===\n");
	print(out, "  n_toplevel_slots = {}\n", n_toplevel_slots);
	print(out, "  n_constants      = {}\n\n", n_constants);

	print(out, "=== pool ===\n");
	std::vector<LambdaBlock> lambdas;
	for (uint32_t i = 0; i < n_constants; ++i)
	{
		p = disasm_pool_entry(out, p, i, lambdas);
	}
	std::fputc('\n', out);

	JET_DIE_WHEN(nullptr, source_maps.size() != lambdas.size() + 1,
	             "invalid debug section: table count {} does not match {} lambdas plus toplevel",
	             source_maps.size(), lambdas.size());
	size_t code_size = static_cast<size_t>(end - p);
	print(out, "=== toplevel code ({} bytes) ===\n", code_size);
	disasm_code_block(out, p, code_size, source_maps[lambdas.size()], files);

	for (size_t li = 0; li < lambdas.size(); ++li)
	{
		const LambdaBlock& lb = lambdas[li];
		std::fputc('\n', out);
		print(out, "=== lambda [{}]", lb.pool_idx);
		if (*lb.name)
		{
			print(out, " {}", lb.name);
		}
		print(out, " ({} bytes, arity={}{}, n_locals={}) ===\n", lb.size,
		      lb.is_n_ary ? "n-ary≥" : "", lb.arity, lb.n_locals);
		disasm_code_block(out, lb.code, lb.size, source_maps[li], files);
	}
}

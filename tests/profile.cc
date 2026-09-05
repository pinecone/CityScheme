// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "compiler.h"
#include "runtime.h"
#include <cstdlib>

static Atom enter(VmState& state, Atom callback)
{
	uint64_t* samples{g_profile.active.load()};
	SiteProfile* site{g_profile.context.site};
	Atom result{jet_enter_vm(state, callback, nullptr, 0)};
	JET_DIE_UNLESS(&state, g_profile.active.load() == samples && g_profile.context.site == site,
	               "profile: restored caller attribution");
	return result;
}

static bool wait_sample(uint64_t& samples)
{
	uint64_t before{std::atomic_ref<uint64_t>{samples}.load()};
	uint64_t deadline{profile_wall_ns() + 1000000000};
	while (std::atomic_ref<uint64_t>{samples}.load() == before && profile_wall_ns() < deadline)
	{
	}
	return std::atomic_ref<uint64_t>{samples}.load() > before;
}

static Atom sample(VmState& state)
{
	JET_DIE_UNLESS(&state, wait_sample(*g_profile.active.load()), "profile: sampled primitive work");
	return box(true);
}

static Atom process_exit(VmState&)
{
	std::exit(7);
}

static Atom process_fail(VmState&)
{
	JET_DIE(nullptr, "profile: host failure");
}

static Atom verify(VmState& state)
{
	JET_DIE_UNLESS(&state, g_profile.host_calls.count == 2, "profile: host call count");
	PrimitiveProfile* primitive{unbox<Prim>(*state.env.lookup("enter"))->profile};
	JET_DIE_UNLESS(&state, primitive->work.count == 2, "profile: primitive call count");
	JET_DIE_UNLESS(&state, primitive->name == "enter", "profile: primitive alias");

	uint64_t same_code{0};
	uint64_t changed{0};
	uint64_t invalidated{0};
	for (const SiteProfile& site : g_profile.sites)
	{
		JET_DIE_UNLESS(&state, site.misses.total() <= site.work.count, "profile: miss denominator");
		same_code += site.misses.same_code;
		changed += site.misses.changed;
		invalidated += site.misses.invalidated;
	}
	JET_DIE_UNLESS(&state, same_code != 0, "profile: same-code closures");
	JET_DIE_UNLESS(&state, changed != 0, "profile: changed callee");
	JET_DIE_UNLESS(&state, invalidated != 0, "profile: invalidated cache");
	return box(true);
}

static void test_samples()
{
	Profile profile;
	Code instruction[OPCODE_SIZE]{};
	instruction[OPCODE_SIZE - 1] = static_cast<uint8_t>(Opcode::ldk);
	profile.code_begin = reinterpret_cast<uintptr_t>(instruction);
	profile.code_size = sizeof(instruction);
	profile.site_index = {0};
	profile.sites.push_back({instruction, 0, instruction[OPCODE_SIZE - 1]});
	SiteProfile& site{profile.sites.front()};
	PrimitiveProfile primitive{"test"};

	profile.op(instruction);
	profile.sample();
	JET_DIE_UNLESS(nullptr, site.work.count == 1 && site.work.samples == 1, "profile: site sample");
	profile.primitive(&primitive);
	profile.sample();
	JET_DIE_UNLESS(nullptr, primitive.work.count == 1 && primitive.work.samples == 1
	               && site.work.samples == 1, "profile: exclusive primitive sample");
	{
		Profile::GcTimer timer{profile};
		profile.sample();
		JET_DIE_UNLESS(nullptr, profile.gc_samples == 1 && primitive.work.samples == 1,
		               "profile: exclusive gc sample");
	}
	profile.sample();
	JET_DIE_UNLESS(nullptr, primitive.work.samples == 2, "profile: gc restored attribution");
	{
		Profile::HostTimer timer{profile};
		profile.sample();
		JET_DIE_UNLESS(nullptr, profile.entry_samples == 1, "profile: entry sample");
		profile.op(instruction);
		profile.sample();
	}
	profile.sample();
	JET_DIE_UNLESS(nullptr, site.work.samples == 2 && primitive.work.samples == 3
	               && profile.host_calls.count == 1, "profile: host attribution");
	uint8_t op{instruction[OPCODE_SIZE - 1]};
	JET_DIE_UNLESS(nullptr, profile.pair_after[op][op] == 0, "profile: entry has no incoming opcode pair");

	Code first[OPCODE_SIZE]{};
	Code second[OPCODE_SIZE]{};
	profile.miss(0, 1, first);
	profile.miss(1, 2, first);
	profile.miss(2, 2, first);
	profile.miss(2, 2, second);
	JET_DIE_UNLESS(nullptr, site.misses.first == 1 && site.misses.same_code == 1
	               && site.misses.invalidated == 1 && site.misses.changed == 1,
	               "profile: cache miss classification");
	profile.stop();
	profile.sample();
	JET_DIE_UNLESS(nullptr, primitive.work.samples == 3, "profile: stopped sampler");
	for (unsigned run = 0; run < 2; ++run)
	{
		profile.begin();
		JET_DIE_UNLESS(nullptr, wait_sample(profile.host_samples), "profile: sampler uses its owner");
		profile.select(&primitive.work.samples);
		bool sampled{wait_sample(primitive.work.samples)};
		profile.stop();
		JET_DIE_UNLESS(nullptr, sampled, "profile: restarted sampler");
	}
}

static void test_durations()
{
	ProfileDurations durations;
	for (uint64_t value = 0; value < 10000; ++value)
	{
		durations.add(value);
	}
	JET_DIE_UNLESS(nullptr, durations.count == 10000 && durations.total == 49995000
	               && durations.minimum == 0 && durations.maximum == 9999, "profile: duration totals");
	for (double fraction : {0.50, 0.90, 0.99})
	{
		uint64_t exact{static_cast<uint64_t>(fraction * durations.count) - 1};
		uint64_t bound{durations.quantile(fraction)};
		JET_DIE_UNLESS(nullptr, bound >= exact && bound - exact <= exact / 16,
		               "profile: duration quantile bound");
	}
	for (unsigned exponent = 0; exponent < 64; ++exponent)
	{
		uint64_t value{uint64_t{1} << exponent};
		ProfileDurations boundary;
		boundary.add(value - 1);
		boundary.add(value);
		boundary.add(value + 1);
		JET_DIE_UNLESS(nullptr, boundary.quantile(0.50) >= value
		               && boundary.quantile(0.50) <= value + 1, "profile: duration bucket boundary");
	}
	ProfileDurations maximum;
	maximum.add(UINT64_MAX);
	JET_DIE_UNLESS(nullptr, maximum.quantile(1) == UINT64_MAX, "profile: maximum duration");
}

int main(int argc, char* argv[])
{
	test_samples();
	test_durations();

	Env env;
	VmState state{.env = env};
	init_runtime(state);
	env.bind("enter", make_prim<enter>(state));
	env.bind("enter-alias", *env.lookup("enter"));
	env.bind("sample", make_prim<sample>(state));
	env.bind("verify", make_prim<verify>(state));
	env.bind("process-exit", make_prim<process_exit>(state));
	env.bind("process-fail", make_prim<process_fail>(state));
	const char* source{
		R"(
(define + (%prim "+"))
(define - (%prim "-"))
(define = (%prim "="))
(define cons (%prim "cons"))
(define enter (%prim "enter"))
(define (invoke callback) (callback))
(define (first) 1)
(define (second) 2)
(define (closures count)
  (if (= count 0) 0
      (begin (invoke (lambda () count)) (closures (- count 1)))))
(define (switch count)
  (if (= count 0) 0
      (begin (invoke (if (= count 1) first second)) (switch (- count 1)))))
(define (allocate count)
  (if (= count 0) 0
      (begin (cons count count) (first) (allocate (- count 1)))))
(closures 100)
(switch 2)
(allocate 3000)
(enter (lambda () (first)))
(enter (lambda () (second)))
((%prim "sample"))
((%prim "verify"))
)"};
	if (argc > 1)
	{
		source = argv[1];
	}
	Bytecode code{compile(source, "profile-test", {.inlining = false, .lift_lambdas = false})};
	LoadedProgram program{load_program(state, code.data(), code.size())};
	Frame frame{program.code, nullptr, 0, program.n_toplevel_slots};
	eval(state, frame, program.constants.data(), program.constants.size(), program.n_toplevel_slots);
}

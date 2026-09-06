// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef error_h
#define error_h

#include "platform.h"
#include <cstdlib>

struct VmState;

void print_stack_trace(VmState* vm);

#define JET_DIE(vm_, fmt, ...)                                                                              \
	do                                                                                                       \
	{                                                                                                        \
		print(stderr, fmt "\n" __VA_OPT__(, ) __VA_ARGS__);                                           \
		print_stack_trace(vm_);                                                                              \
		std::exit(1);                                                                                        \
	} while (0)

#define JET_DIE_WHEN(vm_, cond, fmt, ...)                                                                   \
	do                                                                                                       \
	{                                                                                                        \
		if (cond) [[unlikely]]                                                                                \
		{                                                                                                     \
			JET_DIE(vm_, fmt __VA_OPT__(, ) __VA_ARGS__);                                                    \
		}                                                                                                     \
	} while (0)

#define JET_DIE_UNLESS(vm_, cond, fmt, ...) JET_DIE_WHEN(vm_, !(cond), fmt __VA_OPT__(, ) __VA_ARGS__)

#endif

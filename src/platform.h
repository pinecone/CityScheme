// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef platform_h
#define platform_h

#if __has_cpp_attribute(clang::preserve_none)
#define JET_PRESERVE_NONE [[clang::preserve_none]]
#else
#define JET_PRESERVE_NONE
#endif

#if __has_cpp_attribute(gnu::always_inline)
#define JET_ALWAYS_INLINE [[gnu::always_inline]]
#else
#define JET_ALWAYS_INLINE
#endif

#if __has_cpp_attribute(gnu::noinline)
#define JET_NOINLINE [[gnu::noinline]]
#else
#define JET_NOINLINE
#endif

#if __has_cpp_attribute(gnu::cold)
#define JET_COLD [[gnu::cold]]
#else
#define JET_COLD
#endif

#if __has_cpp_attribute(clang::musttail)
#define JET_MUSTTAIL [[clang::musttail]]
#elif __has_cpp_attribute(gnu::musttail)
#define JET_MUSTTAIL [[gnu::musttail]]
#else
#define JET_MUSTTAIL
#endif

#endif

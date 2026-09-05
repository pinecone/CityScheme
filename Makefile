# Builds one binary per variant: build/jet[suffix]
#
#		make										release build (default)
#		make debug							debug build:	 build/jet-debug
#		make profile						profile build: build/jet-profile
#		make all-variants				build release + debug + profile in one go
#
#		make test								run the release, profile, and sanitize targets
#		make test-release				run tests against release binary
#		make test-profile				run tests against profile binary
#		make sanitize					run debug-binary tests and benchmarks under UBSan
#
#		make clean							wipe build/
#
# Object files live under build/<variant>/ so all three variants coexist.

CXX				 := clang++
UNCRUSTIFY := uncrustify
ASAN_RTDIR := $(shell $(CXX) -print-runtime-dir 2>/dev/null)

# Parallel build by default; override with `make JOBS=1` or `make -j1`.
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)
MAKEFLAGS += -j$(JOBS)

V ?= 0

ifeq ($(V),1)
	Q :=
else
	Q := @
endif

VARIANT ?= release

SRC			 := src
BUILD		 := build
PRELUDE := lib/prelude.ss
PRELUDE_H := $(BUILD)/prelude.h

# --- Variant selection ---------------------------------------------------

ifeq ($(VARIANT),debug)
	OPT		 := -g3 -DJET_DEBUG -DJET_TRACE -fsanitize=address,undefined -fno-sanitize=vptr,function \
						-fno-omit-frame-pointer -O1
	LDOPT	 := -fsanitize=address,undefined -Wl,-rpath,$(ASAN_RTDIR)
	SUFFIX := -debug
else ifeq ($(VARIANT),profile)
	OPT		 := -O2 -g3
	SUFFIX := -profile
	PROFILE_DEF := -DJET_PROFILE -pthread
	LDOPT	 := -pthread
else ifeq ($(VARIANT),release)
	OPT		 := -O2 -g3
	SUFFIX :=
	LDOPT	 :=
else
	$(error unknown VARIANT '$(VARIANT)'; use release, debug, or profile)
endif

OBJDIR := $(BUILD)/$(VARIANT)

JET_BIN := $(BUILD)/jet$(SUFFIX)

# --- Flags ---------------------------------------------------------------

CXXFLAGS := -std=c++20 -fno-exceptions -fno-rtti \
						-Wall -Werror -pipe -Wold-style-cast -Wextra -Wno-unused-parameter \
						$(OPT) $(PROFILE_DEF) -I$(SRC) -I$(BUILD) -Ivendor

# --- Modules -------------------------------------------------------------

MODULES ?= $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard modules/*/module.mk)))))
MODULE_DIRS := $(addprefix modules/,$(MODULES))
MODULE_MKS := $(addsuffix /module.mk,$(MODULE_DIRS))
MODULE_CC :=
MODULE_CPP :=
MODULE_SOKOL_CC :=
MODULE_VENDOR_CC :=
MODULE_INIT :=
MODULE_PRELUDE :=
MODULE_TESTS :=

include $(MODULE_MKS)

UNAME_S := $(shell uname -s)

ifneq ($(strip $(MODULE_SOKOL_CC)),)
ifeq ($(UNAME_S),Darwin)
	SOKOL_LANG := -x objective-c++
	MODULE_LDFLAGS := -framework AppKit -framework QuartzCore -framework OpenGL -framework AudioToolbox
else
	SOKOL_LANG :=
	MODULE_LDFLAGS := -pthread -lX11 -lXi -lXcursor -lGL -lasound -ldl -lm
endif
endif

LDFLAGS	 := $(LDOPT) $(MODULE_LDFLAGS)

# Third-party code, so warnings-as-errors and the house warning set do not apply.
VENDOR_CXXFLAGS := $(filter-out -Wall -Wextra -Werror -Wold-style-cast,$(CXXFLAGS)) -w
SOKOL_CXXFLAGS := $(VENDOR_CXXFLAGS) $(SOKOL_LANG)

# --- Sources -------------------------------------------------------------

CORE_CC := $(wildcard $(SRC)/*.cc)
ALL_CC := $(CORE_CC) $(MODULE_CC)
ALL_CPP := $(ALL_CC) $(wildcard $(SRC)/*.h) $(MODULE_CPP) tests/profile.cc
SOKOL_OBJ := $(patsubst %.cc,$(OBJDIR)/%.o,$(MODULE_SOKOL_CC))
VENDOR_OBJ := $(patsubst %.cpp,$(OBJDIR)/%.o,$(MODULE_VENDOR_CC))
ALL_OBJ := $(patsubst %.cc,$(OBJDIR)/%.o,$(ALL_CC)) $(SOKOL_OBJ) $(VENDOR_OBJ)
PRELUDE_SOURCES := $(PRELUDE) $(MODULE_PRELUDE)
MODULES_H := $(BUILD)/modules.h
BENCHMARK_SS := $(sort $(wildcard bench/bench-*.ss))
SANITIZE_OPTIONS ?= halt_on_error=1:print_stacktrace=1
SANITIZE_MARKER := Sanitizer
SANITIZE_ENV := ASAN_OPTIONS='$(SANITIZE_OPTIONS)' UBSAN_OPTIONS='$(SANITIZE_OPTIONS)' \
								JET_MODULE_TESTS='$(MODULE_TESTS)' JET_TEST_DIAGNOSTICS=1

DEPS := $(ALL_OBJ:.o=.d) $(OBJDIR)/tests/profile.d

# --- Targets -------------------------------------------------------------

.PHONY: all release debug profile all-variants \
				test test-release test-profile sanitize show-sanitizers \
				ab-cross-bench format format-check clean tags FORCE
.DEFAULT_GOAL := all

all: $(JET_BIN)

release:
	$(Q)$(MAKE) VARIANT=release

debug:
	$(Q)$(MAKE) VARIANT=debug

profile:
	$(Q)$(MAKE) VARIANT=profile

all-variants:
	$(Q)$(MAKE) VARIANT=release
	$(Q)$(MAKE) VARIANT=debug
	$(Q)$(MAKE) VARIANT=profile

# --- Run targets (variant-aware via JET env var) --------------------

# Recipe lines run in order, so a test target never builds variants in parallel.
test:
	$(Q)$(MAKE) test-release
	$(Q)$(MAKE) test-profile
	$(Q)$(MAKE) sanitize

test-release:
	$(Q)$(MAKE) VARIANT=release
	@printf '  TEST release\n'
	$(Q)cd tests && JET=../build/jet JET_MODULE_TESTS='$(MODULE_TESTS)' ./run-tests

test-profile:
	$(Q)$(MAKE) VARIANT=profile
	@printf '  TEST profile\n'
	$(Q)cd tests && JET=../build/jet-profile JET_MODULE_TESTS='$(MODULE_TESTS)' ./run-tests
	$(Q)$(MAKE) VARIANT=profile $(BUILD)/profile/profile-test
	$(Q)cd tests && JET=../build/jet-profile ./run-profile-tests

show-sanitizers:
	@list=$$($(CXX) $(CXXFLAGS) -x c++ /dev/null -c -o /dev/null -### 2>&1 | tr ' ' '\n' | \
		sed -n 's/^"-fsanitize=\(.*\)"$$/\1/p'); \
		printf 'sanitizers (%s): %s\n' '$(VARIANT)' "$${list:-none}"

define sanitize_run
	$(Q)$(MAKE) VARIANT=$(1) --no-print-directory show-sanitizers
	@printf '  TEST $(1)\n'
	$(Q)cd tests && $(SANITIZE_ENV) JET=../$(BUILD)/jet$(2) ./run-tests
	@set -e; \
		echo "runnin' benchmarks:"; \
		for benchmark in $(BENCHMARK_SS); do \
			name=$${benchmark#bench/bench-}; \
			name=$${name%.ss}; \
			printf "   %s... " "$$name"; \
			if diagnostics=$$($(SANITIZE_ENV) $(BUILD)/jet$(2) run "$$benchmark" 2>&1 >/dev/null) && \
				! printf '%s' "$$diagnostics" | grep -q '$(SANITIZE_MARKER)'; then \
				echo "success"; \
			else \
				echo "fail"; \
				printf '%s\n' "$$diagnostics" >&2; \
				exit 1; \
			fi; \
		done
endef

sanitize:
	$(Q)$(MAKE) VARIANT=debug
	$(call sanitize_run,debug,-debug)

# Builds its own worktree of REF (default HEAD), so no build dependency.
ab-cross-bench:
	@printf '  BENCH cross\n'
	$(Q)cd bench && ./ab-cross $(REF)

format:
	@printf '  FMT   source\n'
	$(Q)$(UNCRUSTIFY) -q -c uncrustify.cfg --replace --no-backup $(ALL_CPP)

format-check:
	@printf '  FMT   source\n'
	$(Q)$(UNCRUSTIFY) -q -c uncrustify.cfg --check $(ALL_CPP)

clean:
	@printf '  CLEAN build\n'
	$(Q)rm -rf $(BUILD)

# Sorted tag file so readtags can binary-search (O(log n)) instead of
# scanning linearly. --sort=yes is ctags' default, but we pass it
# explicitly since correctness here depends on it.
tags: | $(BUILD)
	@printf '  TAGS  %s\n' '$(BUILD)/TAGS'
	$(Q)ctags --sort=yes -f $(BUILD)/TAGS -R $(SRC) $(MODULE_DIRS)

$(JET_BIN): $(ALL_OBJ) | $(BUILD)
	@printf '  LINK  %s\n' '$@'
	$(Q)$(CXX) $(LDFLAGS) -o $@ $^

$(OBJDIR)/profile-test: $(OBJDIR)/tests/profile.o \
                      $(patsubst %.cc,$(OBJDIR)/%.o,$(filter-out $(SRC)/main.cc,$(CORE_CC)))
	@printf '  LINK  %s\n' '$@'
	$(Q)$(CXX) $(LDOPT) -o $@ $^

$(OBJDIR)/src/main.o: $(PRELUDE_H) $(MODULES_H)

$(OBJDIR)/vendor/sokol/%.o: vendor/sokol/%.cc Makefile | $(OBJDIR)
	@printf '  CXX   %s\n' '$<'
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(SOKOL_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/vendor/%.o: vendor/%.cpp Makefile | $(OBJDIR)
	@printf '  CXX   %s\n' '$<'
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(VENDOR_CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/%.o: %.cc Makefile | $(OBJDIR)
	@printf '  CXX   %s\n' '$<'
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

FORCE:

$(MODULES_H): FORCE Makefile $(MODULE_MKS) | $(BUILD)
	@{ \
		printf '%s\n' '#pragma once' 'struct VmState;'; \
		for initializer in $(MODULE_INIT); do \
			printf 'void %s(VmState& state);\n' "$$initializer"; \
		done; \
		printf '%s\n' 'inline void init_modules(VmState& state)' '{'; \
		for initializer in $(MODULE_INIT); do \
			printf '\t%s(state);\n' "$$initializer"; \
		done; \
		printf '%s\n' '}'; \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

$(PRELUDE_H): FORCE Makefile $(MODULE_MKS) $(PRELUDE_SOURCES) | $(BUILD)
	@{ \
		printf '%s\n' '#pragma once' '#include <cstddef>' \
			'inline constexpr unsigned char prelude_source[] = {'; \
		LC_ALL=C cat $(PRELUDE_SOURCES) | od -An -v -tx1 | \
			awk '{ for (i = 1; i <= NF; ++i) printf "0x%s,", $$i; print "" }'; \
		printf '%s\n' '};' \
			'inline constexpr std::size_t prelude_source_size = sizeof(prelude_source);'; \
	} > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

$(BUILD):
	@mkdir -p $@

$(OBJDIR): | $(BUILD)
	@mkdir -p $@

-include $(DEPS)

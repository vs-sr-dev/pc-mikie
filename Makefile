# pc-mikie - build the recompiled game.
#
# Needs GCC or Clang: the dispatch table in the generated code uses
# label-as-value, which MSVC does not support.
#
# src/gen/mikie_gen.c is NOT in this repository. Generate it from your own ROM:
#     python tools/transpile.py

CC      ?= gcc
CFLAGS  ?= -O2 -Isrc -Wall
GEN      = src/gen/mikie_gen.c
SRC      = src/mikie_main.c src/mikie_video.c

# One giant function with ~12,800 labels is hard on the optimiser: expect
# several minutes. Use OPT=-O1 (or -O0) while iterating.
all: build/mikie

build/mikie: $(GEN) $(SRC) src/m6809_rt.h src/mikie_video.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(GEN) $(SRC)

# Trace build, for diffing against MAME instruction by instruction.
build/mikie_trace: $(GEN) $(SRC) src/m6809_rt.h src/mikie_video.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DMIKIE_TRACE -o $@ $(GEN) $(SRC)

$(GEN):
	@echo "src/gen/mikie_gen.c is missing - run: python tools/transpile.py"
	@false

clean:
	rm -rf build

.PHONY: all clean

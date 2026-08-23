# pc-mikie - build the recompiled game.
#
# Needs GCC or Clang: the dispatch table in the generated code uses
# label-as-value, which MSVC does not support.
#
# src/gen/mikie_gen.c is NOT in this repository. Generate it from your own ROM:
#     python tools/transpile.py

CC      ?= gcc
# -O1 deliberately. cpu_run() is one function of ~82,000 lines with ~12,800
# computed-goto labels; GCC 15 at -O2 was still going after 26 minutes of CPU
# and 17 GB resident, while -O1 finishes in about 6 minutes. If you want -O2,
# split the generated code into several functions sharing the dispatch table
# first.
CFLAGS  ?= -O1 -Isrc -Wall
GEN      = src/gen/mikie_gen.c
SRC      = src/mikie_main.c src/mikie_video.c

# Expect several minutes either way. Use CFLAGS=-O0 while iterating on the
# runtime; the generated file dominates the build time regardless.
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

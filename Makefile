# pc-mikie - build the recompiled game.
#
# Needs GCC or Clang: the dispatch table in the generated code uses
# label-as-value, which MSVC does not support, and the sound board's clock
# conversion needs 128-bit integers.
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
GENOBJ   = build/mikie_gen.o
SRC      = src/mikie_main.c src/mikie_video.c \
           src/z80.c src/sn76489.c src/mikie_sound.c
HDR      = src/m6809_rt.h src/mikie_video.h src/z80.h src/sn76489.h src/mikie_sound.h

all: build/mikie

# The generated file is the whole build time, so it is compiled once on its own
# and the small runtime is relinked against it. Expect several minutes the first
# time and a second or two afterwards.
$(GENOBJ): $(GEN)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $(GEN) -o $@

build/mikie: $(GENOBJ) $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(GENOBJ) $(SRC)

# The playable build. sdl2-config comes with SDL2; on Windows you also need
# SDL2.dll beside the executable or on PATH.
SDL_CFLAGS ?= $(shell sdl2-config --cflags | sed 's/-Dmain=SDL_main//')
SDL_LIBS   ?= -lSDL2

build/mikie_sdl: $(GENOBJ) $(SRC) src/mikie_host_sdl.c $(HDR) src/mikie_host.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -DMIKIE_SDL -o $@ \
	    $(GENOBJ) $(SRC) src/mikie_host_sdl.c $(SDL_LIBS)

# Trace builds, for diffing against MAME instruction by instruction.
# MIKIE_TRACE reaches into the generated code, so that one cannot reuse the
# object file; MIKIE_Z80_TRACE only touches the runtime, so it can.
build/mikie_trace: $(GEN) $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -DMIKIE_TRACE -o $@ $(GEN) $(SRC)

build/mikie_z80trace: $(GENOBJ) $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -DMIKIE_Z80_TRACE -o $@ $(GENOBJ) $(SRC)

# The sound board on its own, with no main CPU: enough to run the sound program
# from reset and trace it, which is how the Z80 core was first checked.
build/z80_trace: src/z80.c src/sn76489.c src/mikie_sound.c src/z80_trace.c $(HDR)
	@mkdir -p build
	$(CC) -O2 -Isrc -Wall -DMIKIE_Z80_TRACE -o $@ \
	    src/z80.c src/sn76489.c src/mikie_sound.c src/z80_trace.c

$(GEN):
	@echo "src/gen/mikie_gen.c is missing - run: python tools/transpile.py"
	@false

clean:
	rm -rf build

.PHONY: all clean

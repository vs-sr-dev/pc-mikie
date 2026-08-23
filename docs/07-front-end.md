# The front end

`src/mikie_host.h` is seven functions wide on purpose. The emulated machine hands over a
finished frame and a block of samples and gets back the state of eight input lines;
scaling, the audio queue and the event loop stay on the host side, and swapping SDL for
something else means writing one file.

`src/mikie_host_sdl.c` is that file for SDL2.

---

## Pacing: follow the sound device

The obvious design is a wall-clock timer per frame. It is the wrong one. The sound card's
clock is the only one in the machine that cannot be argued with — it is what the player
actually hears — and it is never exactly the nominal 48 000 Hz. Pace on a timer and the
audio queue slowly grows or drains until it either adds latency or runs dry.

So: after queueing the frame's samples, block until the queue drops below about two
frames' worth.

```c
while (host_audio_queued() > SOUND_RATE * 2 / 60 && guard++ < 500)
    host_sleep(1);
```

The emulation now runs at exactly the rate the sound device consumes, and video follows
audio for free because both are driven from the same frame hook. Measured: 20.003 emulated
seconds in 20.45 s of wall clock, window open, of which about 0.45 s is SDL start-up.

The `guard` matters. If the device stalls, the emulator should run fast rather than hang.

When there is no sound device at all, fall back to the clock — but compute the deadline
from the frame *index*, not by adding a per-frame constant:

```c
deadline = epoch + frames_shown * VBL_NUM * 1000 / (VBL_DEN * 1536000);
```

A frame is 16.5 ms. Adding a rounded 16 ms each time runs 3 % fast, which is audible as
pitch and visible as a clock that gains a minute an hour.

## Do not vsync

The board's screen runs at **60.59 Hz**. That is nobody's refresh rate. Synchronising
presentation to the monitor means dropping or repeating a frame every few seconds — the
one artefact a cycle-accurate port exists to avoid. Present without vsync and let the
pacing above decide when the next frame happens.

## Keyboard

Follow the reference emulator's defaults rather than inventing a layout: anyone who has
played the game in MAME already has the muscle memory.

```
5 / 6   coin 1 / coin 2        1 / 2   start 1 / start 2       9  service
arrows  player 1 joystick      LCtrl / LAlt   buttons 1 and 2
F11     fullscreen             Esc     quit
```

In Mikie the head butt is on **button 2**, so the key that attacks is Alt, not Ctrl. The
bits are MAME's, so that is true there too - worth writing down, because "the fire button
does nothing" is the first thing anyone concludes instead.


The ports are active low, exactly as the board reads them, so the front end hands back
`0xFF` with a bit cleared per pressed key and the memory map needs no translation layer.

## One platform detail

Including `SDL.h` on Windows redefines `main`. With `main()` in another translation unit,
and wanting a console subsystem so `stderr` still works, the fix is to opt out:

```c
#define SDL_MAIN_HANDLED
#include <SDL.h>
...
SDL_SetMainReady();     /* first thing in host_init() */
```

## Checking the front end did not change anything

The runtime can write the audio it produces to a WAV file (`MIKIE_WAV`) and stop after a
given number of frames (`MIKIE_RUN_FRAMES`), in any build. Run both the headless and the
SDL binary over the same forty seconds and compare:

```sh
MIKIE_WAV=trace/a.wav MIKIE_RUN_FRAMES=2424 ./build/mikie rom/maincpu.bin
MIKIE_WAV=trace/b.wav MIKIE_RUN_FRAMES=2424 ./build/mikie_sdl rom/maincpu.bin
cmp trace/a.wav trace/b.wav
```

Byte-identical is the expected answer, and it says the front end is doing nothing except
displaying and pacing. Anything else means real-time work has leaked into the emulation.

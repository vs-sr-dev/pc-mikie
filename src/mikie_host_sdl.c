/* SDL2 implementation of the host front end. See mikie_host.h. */
#include "mikie_host.h"
/* We keep our own main(), so SDL must not rename it; SDL_SetMainReady()
   below is what that costs. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>

static SDL_Window       *win;
static SDL_Renderer     *ren;
static SDL_Texture      *tex;
static SDL_AudioDeviceID adev;
static int               scr_w, scr_h;
static int               quit;
static int               fullscreen;

int host_init(int width, int height, int rate)
{
    SDL_AudioSpec want, got;

    SDL_SetMainReady();
    scr_w = width;
    scr_h = height;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("Mikie", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           width * 2, height * 2, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    /* No vsync: the frame rate is 60.59 Hz and comes from the emulated board,
       so letting the display pace us would either stretch or drop frames. The
       sound device paces the emulation instead. */
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(ren, width, height);   /* keeps the 7:8 aspect */
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);

    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                            SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return 1; }

    SDL_memset(&want, 0, sizeof want);
    want.freq     = rate;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 1024;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!adev)
        fprintf(stderr, "no sound device (%s) - running without audio\n", SDL_GetError());
    else
        SDL_PauseAudioDevice(adev, 0);

    return 0;
}

void host_shutdown(void)
{
    if (adev) SDL_CloseAudioDevice(adev);
    if (tex)  SDL_DestroyTexture(tex);
    if (ren)  SDL_DestroyRenderer(ren);
    if (win)  SDL_DestroyWindow(win);
    SDL_Quit();
}

void host_frame(const uint8_t *rgb)
{
    SDL_UpdateTexture(tex, NULL, rgb, scr_w * 3);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

void host_audio(const int16_t *buf, int nsamples)
{
    if (adev) SDL_QueueAudio(adev, buf, (Uint32)nsamples * 2);
}

int host_audio_queued(void)
{
    return adev ? (int)(SDL_GetQueuedAudioSize(adev) / 2) : 0;
}

int host_audio_running(void) { return adev != 0; }

void host_sleep(int ms) { SDL_Delay((Uint32)ms); }

unsigned host_ticks(void) { return (unsigned)SDL_GetTicks(); }

int host_should_quit(void) { return quit; }

/* Keyboard layout follows MAME's defaults, so muscle memory carries over:
   5/6 coin, 1/2 start, arrows and Ctrl/Alt for player one. */
void host_inputs(uint8_t *system, uint8_t *p1, uint8_t *p2)
{
    const Uint8 *k;
    SDL_Event ev;
    uint8_t sys = 0xFF, a = 0xFF, b = 0xFF;

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) quit = 1;
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            SDL_Keycode c = ev.key.keysym.sym;
            if (c == SDLK_ESCAPE) quit = 1;
            if (c == SDLK_F11 || (c == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
        }
    }
    k = SDL_GetKeyboardState(NULL);

#define LOW(port, bit, key) do { if (k[key]) (port) &= (uint8_t)~(bit); } while (0)
    LOW(sys, 0x01, SDL_SCANCODE_5);          /* coin 1   */
    LOW(sys, 0x02, SDL_SCANCODE_6);          /* coin 2   */
    LOW(sys, 0x04, SDL_SCANCODE_9);          /* service  */
    LOW(sys, 0x08, SDL_SCANCODE_1);          /* start 1  */
    LOW(sys, 0x10, SDL_SCANCODE_2);          /* start 2  */

    LOW(a, 0x01, SDL_SCANCODE_LEFT);
    LOW(a, 0x02, SDL_SCANCODE_RIGHT);
    LOW(a, 0x04, SDL_SCANCODE_UP);
    LOW(a, 0x08, SDL_SCANCODE_DOWN);
    LOW(a, 0x10, SDL_SCANCODE_LCTRL);        /* button 1 */
    LOW(a, 0x20, SDL_SCANCODE_LALT);         /* button 2 */

    /* Player two shares the same stick on an upright cabinet, which is what
       DSW3 is set to; the port is read anyway, so keep it idle. */
    LOW(b, 0x01, SDL_SCANCODE_D);
    LOW(b, 0x02, SDL_SCANCODE_G);
    LOW(b, 0x04, SDL_SCANCODE_R);
    LOW(b, 0x08, SDL_SCANCODE_F);
    LOW(b, 0x10, SDL_SCANCODE_A);
    LOW(b, 0x20, SDL_SCANCODE_S);
#undef LOW

    *system = sys;
    *p1     = a;
    *p2     = b;
}

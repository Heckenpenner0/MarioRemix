/* frontend.h - platform-agnostic fixed-step game loop for the crossover.
 * The loop can drive the shared hub (both worlds) or a single live machine
 * (a loaded Game Boy or SNES ROM). It hands a frame buffer to whichever
 * backend is compiled in (SDL2 on Windows, or a headless stub for testing).
 */
#ifndef FRONTEND_H
#define FRONTEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../core/hub.h"
#include "../core/gb.h"
#include "../core/snes.h"

#define FB_W SNES_W   /* base frame buffer: large enough for the SNES frame */
#define FB_H SNES_H

/* Which machine owns the screen. */
typedef enum { FE_VIEW_HUB = 0, FE_VIEW_GB, FE_VIEW_SNES, FE_VIEW_BOTH } fe_view;

/* Backend callbacks: the concrete windowing layer supplies these. */
typedef struct {
    bool (*init)(void *user);                 /* create window/renderer */
    void (*present)(void *user, const uint32_t *fb, int w, int h); /* show a frame */
    bool (*poll)(void *user);                 /* pump events; false = quit */
    void (*shutdown)(void *user);             /* teardown */
    void *user;
} fe_backend;

/* Buttons abstract over GB and SNES pads. */
enum {
    FE_BTN_A = 1<<0, FE_BTN_B = 1<<1, FE_BTN_SELECT = 1<<2, FE_BTN_START = 1<<3,
    FE_BTN_UP = 1<<4, FE_BTN_DOWN = 1<<5, FE_BTN_LEFT = 1<<6, FE_BTN_RIGHT = 1<<7,
    FE_BTN_X = 1<<8, FE_BTN_Y = 1<<9, FE_BTN_L = 1<<10, FE_BTN_R = 1<<11
};

typedef struct {
    hub        world;          /* the shared crossover world */
    fe_backend be;             /* windowing backend */
    uint32_t   fb[FB_W*FB_H];  /* RGBA frame buffer */
    uint16_t   input;          /* current button mask (FE_BTN_*) */
    int        frame;
    bool       running;

    fe_view      view;         /* which world is on screen */
    gb_machine   gb;           /* live GB world (valid when gb_loaded) */
    snes_machine snes;         /* live SNES world (valid when snes_loaded) */
    bool         gb_loaded;
    bool         snes_loaded;
} frontend;

void fe_init(frontend *fe, const fe_backend *be);
/* Run the loop for `max_frames` (0 = until quit). Returns frames rendered. */
int  fe_run(frontend *fe, int max_frames);
void fe_render(frontend *fe);

/* Load ROMs (memory must outlive the frontend). Selects that view on success. */
bool fe_load_gb(frontend *fe, const uint8_t *rom, size_t n);
bool fe_load_snes(frontend *fe, const uint8_t *rom, size_t n);

/* Set the held buttons (FE_BTN_*); mapped onto whichever machine is active. */
void fe_set_input(frontend *fe, uint16_t buttons);

/* Blit helpers (also usable for headless capture). */
void fe_blit_gb(frontend *fe, const gb_ppu *p);
void fe_blit_snes(frontend *fe, const snes_ppu *p);

/* Audio produced by the most recently stepped frame, when a SNES world is
 * loaded: `*frames` interleaved stereo samples (L,R) at 32000 Hz. Returns NULL
 * when there is no audio for this frame (hub/GB view, or nothing loaded). */
const int16_t *fe_audio(const frontend *fe, int *frames);
void fe_render_hub(frontend *fe);
/* Crossover "work together" view: both machines run each frame and their
 * displays are composited side-by-side into the frame buffer. Input set via
 * fe_set_input drives BOTH machines (shared interaction). */
void fe_render_both(frontend *fe);

#endif /* FRONTEND_H */

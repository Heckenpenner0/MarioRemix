/* main_win.c - crossover entry point.
 *
 * Two build modes:
 *   -DUSE_SDL2 : real Windows window + renderer + input (needs SDL2).
 *   (default)  : headless stub that runs the loop for N frames and prints state,
 *                used to validate the loop where SDL2/GUI are unavailable.
 *
 * Usage:  crossover [--gb <rom>] [--snes <rom>] [frames]
 *   With no ROM the shared hub world is shown; loading a ROM switches to it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frontend.h"

/* ---- small platform file loader (portable) ---- */
static uint8_t *load_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t*)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *out_size = (size_t)n;
    return buf;
}

/* ROMs persist for the program's lifetime (machines keep pointers into them). */
static uint8_t *g_rombuf[2];
static int      g_nrom;

static void load_args(frontend *fe, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "--gb") || !strcmp(a, "--snes")) && i + 1 < argc) {
            size_t n = 0;
            uint8_t *rom = load_file(argv[++i], &n);
            if (!rom) { fprintf(stderr, "cannot read %s\n", argv[i]); continue; }
            g_rombuf[g_nrom++] = rom;
            if (!strcmp(a, "--gb")) { if (!fe_load_gb(fe, rom, n)) fprintf(stderr, "bad GB ROM\n"); }
            else                    { if (!fe_load_snes(fe, rom, n)) fprintf(stderr, "bad SNES ROM\n"); }
        }
    }
    /* Both ROMs -> the crossover "work together" view; else whichever loaded. */
    if (fe->gb_loaded && fe->snes_loaded) fe->view = FE_VIEW_BOTH;
    else if (fe->snes_loaded)             fe->view = FE_VIEW_SNES;
    else if (fe->gb_loaded)               fe->view = FE_VIEW_GB;
}

#ifndef USE_SDL2
/* ---- headless backend (only needed when there is no SDL window) ---- */
typedef struct { int frames; } headless;
static bool hl_init(void *u){ (void)u; printf("headless frontend start\n"); return true; }
static bool hl_poll(void *u){ (void)u; return true; }
static void hl_present(void *u, const uint32_t *fb, int w, int h){
    (void)fb; (void)w; (void)h; ((headless*)u)->frames++;
}
static void hl_shutdown(void *u){ (void)u; printf("headless frontend stop\n"); }
#endif

static void build_world(frontend *fe){
    hub_add_entity(&fe->world, 20, 20, 0, 1);
    hub_add_entity(&fe->world, 80, 60, 1, 2);
    hub_set_tile(&fe->world, 10, 10, 1);
    hub_set_tile(&fe->world, 11, 10, 1);
}

#ifdef USE_SDL2
#include <SDL2/SDL.h>
#define PAD_MAX 4              /* up to four pads, keyed by instance id */

typedef struct {
    SDL_GameController *gc;
    SDL_JoystickID      instance;
    bool                is_edge;    /* DualSense Edge (VID $054C / PID $0DF2) */
} pad_slot;

typedef struct {
    SDL_Window      *win;
    SDL_Renderer    *ren;
    SDL_Texture     *tex;
    SDL_AudioDeviceID audio;    /* SNES DSP output: 32 kHz stereo S16 */
    const frontend  *fe;        /* for fe_audio() each frame */
    pad_slot         pads[PAD_MAX];
    bool             quit;
} sdl_state;

/* ---- game pads --------------------------------------------------------- *
 * Any SDL2 "game controller" works: the SDL mapping layer turns the raw
 * buttons/axes into the standard names below, which is what makes the
 * DualSense Edge (and the original DualSense, Xbox pads, ...) usable with no
 * device-specific code.  What this file adds is hot-plug handling, a SNES
 * button layout, stick deadzones and trigger thresholds. */

/* The DualSense Edge reports Sony vendor $054C, product $0DF2 over both USB
 * and Bluetooth (the original DualSense is $0CE6). */
#define SONY_VENDOR          0x054C
#define DUALSENSE_EDGE       0x0DF2

#define STICK_DEADZONE       8000      /* SDL's documented typical deadzone */
#define TRIGGER_THRESHOLD    0x4000    /* half-press of an analog trigger    */

static bool pad_is_edge(int index) {
    Uint16 vid = SDL_JoystickGetDeviceVendor(index);
    Uint16 pid = SDL_JoystickGetDeviceProduct(index);
    if (vid == SONY_VENDOR && pid == DUALSENSE_EDGE) return true;
    /* Some backends do not expose VID/PID, so fall back to the advertised name. */
    const char *name = SDL_JoystickNameForIndex(index);
    return name != NULL && strstr(name, "Edge") != NULL;
}

static void pad_open(sdl_state *s, int index) {
    if (index < 0 || index >= SDL_NumJoysticks()) return;
    if (!SDL_IsGameController(index)) return;
    SDL_GameController *gc = SDL_GameControllerOpen(index);
    if (!gc) return;
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    for (int i = 0; i < PAD_MAX; i++) {
        if (s->pads[i].gc != NULL) continue;
        s->pads[i].gc = gc;
        s->pads[i].instance = id;
        s->pads[i].is_edge = pad_is_edge(index);
        printf("pad %d: %s%s\n", i, SDL_GameControllerName(gc),
               s->pads[i].is_edge ? " (DualSense Edge)" : "");
        return;
    }
    SDL_GameControllerClose(gc);            /* no free slot */
}

static void pad_close_instance(sdl_state *s, SDL_JoystickID id) {
    for (int i = 0; i < PAD_MAX; i++) {
        if (s->pads[i].gc == NULL || s->pads[i].instance != id) continue;
        printf("pad %d: disconnected\n", i);
        SDL_GameControllerClose(s->pads[i].gc);
        s->pads[i].gc = NULL; s->pads[i].instance = -1; s->pads[i].is_edge = false;
        return;
    }
}

/* Radial deadzone on one stick, mapped onto the D-pad. SDL reports the stick
 * as -32768..32767 with up as negative Y. */
static uint16_t stick_to_dpad(SDL_GameController *gc, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay) {
    int x = SDL_GameControllerGetAxis(gc, ax);
    int y = SDL_GameControllerGetAxis(gc, ay);
    int64_t m2 = (int64_t)x * x + (int64_t)y * y;
    if (m2 < (int64_t)STICK_DEADZONE * STICK_DEADZONE) return 0;
    uint16_t b = 0;
    if (y < -STICK_DEADZONE) b |= FE_BTN_UP;
    if (y >  STICK_DEADZONE) b |= FE_BTN_DOWN;
    if (x < -STICK_DEADZONE) b |= FE_BTN_LEFT;
    if (x >  STICK_DEADZONE) b |= FE_BTN_RIGHT;
    return b;
}

static uint16_t map_pads(const sdl_state *s) {
    uint16_t b = 0;
    for (int i = 0; i < PAD_MAX; i++) {
        SDL_GameController *gc = s->pads[i].gc;   /* SDL2 getters are non-const */
        if (!gc) continue;
        /* The SNES diamond keeps its physical positions on a PS-style pad,
           where SDL maps Cross->A, Circle->B, Triangle->Y, Square->X:
             SNES A (right) <- Circle, SNES B (bottom) <- Cross
             SNES X (top)   <- Triangle, SNES Y (left) <- Square */
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B)) b |= FE_BTN_A;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A)) b |= FE_BTN_B;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y)) b |= FE_BTN_X;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X)) b |= FE_BTN_Y;

        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  b |= FE_BTN_L;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= FE_BTN_R;
        /* the analog triggers act as the shoulder buttons too */
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > TRIGGER_THRESHOLD) b |= FE_BTN_L;
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_THRESHOLD) b |= FE_BTN_R;

        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))  b |= FE_BTN_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START)) b |= FE_BTN_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE)) b |= FE_BTN_START;   /* PS button */
#if SDL_VERSION_ATLEAST(2,24,0)
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_MISC1)) b |= FE_BTN_SELECT;  /* mic button */
#endif
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))    b |= FE_BTN_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  b |= FE_BTN_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  b |= FE_BTN_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= FE_BTN_RIGHT;

        b |= stick_to_dpad(gc, SDL_CONTROLLER_AXIS_LEFTX,  SDL_CONTROLLER_AXIS_LEFTY);
        b |= stick_to_dpad(gc, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY);
    }
    return b;
}

static uint16_t map_keys(void){
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J]) b |= FE_BTN_A;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_K]) b |= FE_BTN_B;
    if (k[SDL_SCANCODE_A]) b |= FE_BTN_Y;
    if (k[SDL_SCANCODE_S]) b |= FE_BTN_X;
    if (k[SDL_SCANCODE_Q]) b |= FE_BTN_L;
    if (k[SDL_SCANCODE_W]) b |= FE_BTN_R;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_LSHIFT]) b |= FE_BTN_SELECT;
    if (k[SDL_SCANCODE_RETURN]) b |= FE_BTN_START;
    if (k[SDL_SCANCODE_UP])    b |= FE_BTN_UP;
    if (k[SDL_SCANCODE_DOWN])  b |= FE_BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT])  b |= FE_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= FE_BTN_RIGHT;
    return b;
}

static bool sdl_init(void *u){
    sdl_state *s = u;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0){ fprintf(stderr,"SDL: %s\n", SDL_GetError()); return false; }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 32000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    s->audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s->audio) SDL_PauseAudioDevice(s->audio, 0);   /* start playback */
    s->win = SDL_CreateWindow("crossover", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, FB_W*3, FB_H*3, SDL_WINDOW_RESIZABLE);
    s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    s->tex = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
    for (int i = 0; i < PAD_MAX; i++) { s->pads[i].gc = NULL; s->pads[i].instance = -1; }
    for (int i = 0; i < SDL_NumJoysticks(); i++) pad_open(s, i);
    s->quit = false;
    return s->win && s->ren && s->tex;
}
static bool sdl_poll(void *u){
    sdl_state *s = u; SDL_Event e;
    while (SDL_PollEvent(&e)){
        if (e.type == SDL_QUIT) s->quit = true;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) s->quit = true;
        /* hot-plug: ADDED carries a device index, REMOVED an instance id */
        if (e.type == SDL_CONTROLLERDEVICEADDED)    pad_open(s, e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED)  pad_close_instance(s, e.cdevice.which);
    }
    if (s->fe) fe_set_input((frontend*)s->fe, (uint16_t)(map_keys() | map_pads(s)));
    return !s->quit;
}
static void sdl_present(void *u, const uint32_t *fb, int w, int h){
    sdl_state *s = u;
    (void)h;
    SDL_UpdateTexture(s->tex, NULL, fb, w * 4);
    SDL_RenderClear(s->ren);
    SDL_RenderCopy(s->ren, s->tex, NULL, NULL);
    SDL_RenderPresent(s->ren);
    if (s->audio && s->fe){
        int frames = 0;
        const int16_t *a = fe_audio(s->fe, &frames);
        /* keep the queue shallow so the audio stays in step with the video */
        if (a && frames > 0 && SDL_GetQueuedAudioSize(s->audio) < (Uint32)(frames * 4 * 4))
            SDL_QueueAudio(s->audio, a, (Uint32)(frames * 2 * (int)sizeof(int16_t)));
    }
}
static void sdl_shutdown(void *u){
    sdl_state *s = u;
    for (int i = 0; i < PAD_MAX; i++) {
        if (s->pads[i].gc) { SDL_GameControllerClose(s->pads[i].gc); s->pads[i].gc = NULL; }
    }
    if (s->audio) { SDL_CloseAudioDevice(s->audio); s->audio = 0; }
    if (s->tex) SDL_DestroyTexture(s->tex);
    if (s->ren) SDL_DestroyRenderer(s->ren);
    if (s->win) SDL_DestroyWindow(s->win);
    SDL_Quit();
}

int main(int argc, char **argv){
    frontend *fe = (frontend*)calloc(1, sizeof *fe);   /* large: keep off the stack */
    if (!fe) return 1;
    sdl_state s = {0};
    s.fe = fe;
    fe_backend be = { sdl_init, sdl_present, sdl_poll, sdl_shutdown, &s };
    fe_init(fe, &be);
    load_args(fe, argc, argv);
    build_world(fe);
    fe_run(fe, 0);
    for (int i = 0; i < g_nrom; i++) free(g_rombuf[i]);
    free(fe);
    return 0;
}
#else
int main(int argc, char **argv){
    int n = 120;                                  /* default 120 frames */
    frontend *fe = (frontend*)calloc(1, sizeof *fe);   /* large: keep off the stack */
    if (!fe) return 1;
    headless hl = {0};
    fe_backend be = { hl_init, hl_present, hl_poll, hl_shutdown, &hl };
    fe_init(fe, &be);
    load_args(fe, argc, argv);
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') n = atoi(argv[i]);
    build_world(fe);
    int ran = fe_run(fe, n);
    printf("view=%s rendered %d frames; hub cycles=%llu; entities=%d\n",
           fe->view==FE_VIEW_GB?"GB":fe->view==FE_VIEW_SNES?"SNES":fe->view==FE_VIEW_BOTH?"BOTH":"HUB",
           ran, (unsigned long long)fe->world.cycles, fe->world.n_ents);
    for (int i = 0; i < g_nrom; i++) free(g_rombuf[i]);
    free(fe);
    return 0;
}
#endif

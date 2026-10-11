/* frontend.c - fixed-step game loop driving the hub or a live machine. */
#include "frontend.h"
#include <string.h>

void fe_init(frontend *fe, const fe_backend *be) {
    memset(fe, 0, sizeof *fe);
    hub_init(&fe->world);
    if (be) fe->be = *be;
    fe->view = FE_VIEW_HUB;
    fe->running = true;
}

bool fe_load_gb(frontend *fe, const uint8_t *rom, size_t n) {
    if (!gb_init(&fe->gb, rom, n)) return false;
    fe->gb_loaded = true;
    fe->view = fe->snes_loaded ? FE_VIEW_BOTH : FE_VIEW_GB;
    return true;
}

bool fe_load_snes(frontend *fe, const uint8_t *rom, size_t n) {
    if (!snes_init(&fe->snes, rom, n)) return false;
    fe->snes_loaded = true;
    fe->view = fe->gb_loaded ? FE_VIEW_BOTH : FE_VIEW_SNES;
    return true;
}

/* Map the abstract button mask onto each loaded machine. */
void fe_set_input(frontend *fe, uint16_t b) {
    fe->input = b;
    if (fe->gb_loaded) {
        uint8_t g = 0;
        if (b & FE_BTN_A) g |= GB_JOY_A;
        if (b & FE_BTN_B) g |= GB_JOY_B;
        if (b & FE_BTN_SELECT) g |= GB_JOY_SELECT;
        if (b & FE_BTN_START) g |= GB_JOY_START;
        if (b & FE_BTN_UP) g |= GB_JOY_UP;
        if (b & FE_BTN_DOWN) g |= GB_JOY_DOWN;
        if (b & FE_BTN_LEFT) g |= GB_JOY_LEFT;
        if (b & FE_BTN_RIGHT) g |= GB_JOY_RIGHT;
        gb_set_joypad(&fe->gb, g);
    }
    if (fe->snes_loaded) {
        uint16_t s = 0;
        if (b & FE_BTN_A) s |= SNES_BTN_A;
        if (b & FE_BTN_B) s |= SNES_BTN_B;
        if (b & FE_BTN_X) s |= SNES_BTN_X;
        if (b & FE_BTN_Y) s |= SNES_BTN_Y;
        if (b & FE_BTN_L) s |= SNES_BTN_L;
        if (b & FE_BTN_R) s |= SNES_BTN_R;
        if (b & FE_BTN_SELECT) s |= SNES_BTN_SELECT;
        if (b & FE_BTN_START) s |= SNES_BTN_START;
        if (b & FE_BTN_UP) s |= SNES_BTN_UP;
        if (b & FE_BTN_DOWN) s |= SNES_BTN_DOWN;
        if (b & FE_BTN_LEFT) s |= SNES_BTN_LEFT;
        if (b & FE_BTN_RIGHT) s |= SNES_BTN_RIGHT;
        snes_set_joypad(&fe->snes, 1, s);
    }
}

/* Draw the hub: a simple top-down view of the tile map + entities. */
void fe_render_hub(frontend *fe) {
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            uint8_t t = hub_get_tile(&fe->world, x & (HUB_MAP_W - 1), y & (HUB_MAP_H - 1));
            uint32_t c = ((x + y) & 1) ? 0xFF202430u : 0xFF181C28u;
            if (t) c = 0xFF3A6EA5u;
            fe->fb[y * FB_W + x] = c;
        }
    }
    for (int i = 0; i < fe->world.n_ents; i++) {
        const hub_entity *e = &fe->world.ents[i];
        if (!e->alive) continue;
        int px = (int)e->x, py = (int)e->y;
        if (px < 0 || px >= FB_W || py < 0 || py >= FB_H) continue;
        fe->fb[py * FB_W + px] = (e->world == 0) ? 0xFFE04040u : 0xFF40C040u;
    }
}

/* Game Boy frame centered inside the (larger) frame buffer. */
void fe_blit_gb(frontend *fe, const gb_ppu *p) {
    const int ox = (FB_W - GB_W) / 2, oy = (FB_H - GB_H) / 2;
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++)
            fe->fb[y * FB_W + x] = 0xFF000000u;           /* letterbox */
    for (int y = 0; y < GB_H; y++)
        for (int x = 0; x < GB_W; x++)
            fe->fb[(oy + y) * FB_W + (ox + x)] = p->fb[y * GB_W + x];
}

void fe_blit_snes(frontend *fe, const snes_ppu *p) {
    memcpy(fe->fb, p->fb, sizeof fe->fb);                 /* 256x224, 1:1 */
}

const int16_t *fe_audio(const frontend *fe, int *frames) {
    if ((fe->view != FE_VIEW_SNES && fe->view != FE_VIEW_BOTH) || !fe->snes_loaded) { if (frames) *frames = 0; return NULL; }
    return snes_audio(&fe->snes, frames);
}

/* Crossover "work together" view: GB on the left half, SNES on the right half,
 * both scaled to fit and composited into the shared frame buffer. */
void fe_render_both(frontend *fe) {
    const int half = FB_W / 2;
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            uint32_t c = 0xFF000000u;                         /* letterbox */
            if (x < half) {
                if (fe->gb_loaded) {
                    int sx = x * GB_W / half, sy = y * GB_H / FB_H;
                    c = fe->gb.ppu.fb[sy * GB_W + sx];
                }
            } else {
                if (fe->snes_loaded) {
                    int sx = (x - half) * SNES_W / half, sy = y * SNES_H / FB_H;
                    c = fe->snes.ppu.fb[sy * SNES_W + sx];
                }
            }
            fe->fb[y * FB_W + x] = c;
        }
    }
}

void fe_render(frontend *fe) {
    switch (fe->view) {
        case FE_VIEW_GB:   fe_blit_gb(fe, &fe->gb.ppu); break;
        case FE_VIEW_SNES: fe_blit_snes(fe, &fe->snes.ppu); break;
        case FE_VIEW_BOTH: fe_render_both(fe); break;
        default:           fe_render_hub(fe); break;
    }
}

int fe_run(frontend *fe, int max_frames) {
    if (fe->be.init && !fe->be.init(fe->be.user)) return 0;
    fe->running = true;
    int rendered = 0;
    while (fe->running) {
        if (fe->be.poll && !fe->be.poll(fe->be.user)) break;   /* window closed */
        switch (fe->view) {
            case FE_VIEW_GB:   if (fe->gb_loaded)   gb_frame(&fe->gb);     break;
            case FE_VIEW_SNES: if (fe->snes_loaded) snes_frame(&fe->snes); break;
            case FE_VIEW_BOTH: /* both games run together, one frame each */
                if (fe->gb_loaded)   gb_frame(&fe->gb);
                if (fe->snes_loaded) snes_frame(&fe->snes);
                break;
            default: hub_run(&fe->world, 70224 / 4 / HUB_WORLDS);          break;
        }
        fe_render(fe);
        if (fe->be.present) fe->be.present(fe->be.user, fe->fb, FB_W, FB_H);
        fe->frame++; rendered++;
        if (max_frames > 0 && rendered >= max_frames) break;
    }
    if (fe->be.shutdown) fe->be.shutdown(fe->be.user);
    return rendered;
}

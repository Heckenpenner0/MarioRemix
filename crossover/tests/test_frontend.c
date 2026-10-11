/* Integration tests: the frontend loop/load/render path for live machines. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../platform/frontend.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

static frontend *new_fe(void){ frontend *fe = calloc(1, sizeof *fe); fe_init(fe, NULL); return fe; }

static void test_gb_view_render(void) {
    printf("test_gb_view_render\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom); rom[0x147] = 0x00;
    frontend *fe = new_fe();
    CHECK(fe_load_gb(fe, rom, sizeof rom), "GB ROM loads");
    CHECK(fe->view == FE_VIEW_GB, "view switches to GB");
    for (int i = 0; i < 16; i++) gb_write(&fe->gb, 0x8000 + i, 0xFF);   /* solid tile 3 */
    fe->gb.ppu.bgp = 0xE4; fe->gb.ppu.lcdc = 0x91;
    gb_frame(&fe->gb);
    fe_render(fe);
    int ox = (FB_W - GB_W) / 2, oy = (FB_H - GB_H) / 2;
    CHECK(fe->fb[0] == 0xFF000000u, "letterbox corner is opaque black");
    CHECK(fe->fb[(oy + 8) * FB_W + (ox + 8)] == fe->gb.ppu.fb[8 * GB_W + 8],
          "GB_view pixel matches PPU frame");
    free(fe);
}

static void test_snes_view_render(void) {
    printf("test_snes_view_render\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
    frontend *fe = new_fe();
    CHECK(fe_load_snes(fe, rom, sizeof rom), "SNES ROM loads");
    CHECK(fe->view == FE_VIEW_SNES, "view switches to SNES");
    snes_frame(&fe->snes);
    fe_render(fe);
    CHECK(fe->fb[0] == fe->snes.ppu.fb[0], "SNES view pixel matches PPU frame");

    int frames = -1;
    const int16_t *a = fe_audio(fe, &frames);
    CHECK(a != NULL && frames > 500, "the SNES view exposes a frame of 32 kHz audio");
    CHECK(fe_audio(fe, NULL) == a, "fe_audio tolerates a NULL frame count");

    fe->view = FE_VIEW_HUB;
    CHECK(fe_audio(fe, &frames) == NULL && frames == 0, "no audio outside the SNES view");
    free(fe);
}

static void test_input_mapping(void) {
    printf("test_input_mapping\n");
    static uint8_t grom[0x8000]; memset(grom, 0, sizeof grom); grom[0x147] = 0x00;
    frontend *fe = new_fe();
    fe_load_gb(fe, grom, sizeof grom);
    fe_set_input(fe, FE_BTN_A | FE_BTN_RIGHT);
    CHECK((fe->gb.joypad & GB_JOY_A) != 0, "FE_BTN_A -> GB A");
    CHECK((fe->gb.joypad & GB_JOY_RIGHT) != 0, "FE_BTN_RIGHT -> GB RIGHT");

    static uint8_t srom[0x8000]; memset(srom, 0, sizeof srom);
    srom[0x7FFC] = 0x00; srom[0x7FFD] = 0x80;
    fe_load_snes(fe, srom, sizeof srom);
    fe_set_input(fe, FE_BTN_X | FE_BTN_DOWN);
    CHECK((fe->snes.joy1 & SNES_BTN_X) != 0, "FE_BTN_X -> SNES X");
    CHECK((fe->snes.joy1 & SNES_BTN_DOWN) != 0, "FE_BTN_DOWN -> SNES DOWN");
    free(fe);
}

static void test_both_machines_run_and_combine(void) {
    printf("test_both_machines_run_and_combine\n");
    static uint8_t grom[0x8000]; memset(grom, 0, sizeof grom); grom[0x147] = 0x00;
    static uint8_t srom[0x8000]; memset(srom, 0, sizeof srom);
    srom[0x7FFC] = 0x00; srom[0x7FFD] = 0x80;
    frontend *fe = new_fe();
    CHECK(fe_load_gb(fe, grom, sizeof grom), "GB ROM loads first");
    CHECK(fe_load_snes(fe, srom, sizeof srom), "SNES ROM loads second");
    CHECK(fe->gb_loaded && fe->snes_loaded, "both ROMs loaded at once");
    CHECK(fe->view == FE_VIEW_BOTH, "view auto-selects the crossover BOTH mode");

    /* Both machines must advance per frame in BOTH view. */
    uint64_t g0 = fe->gb.cycles, s0 = fe->snes.cycles;
    int ran = fe_run(fe, 8);
    CHECK(ran == 8, "fe_run renders the requested frame count");
    CHECK(fe->gb.cycles > g0, "GB machine advanced in BOTH view");
    CHECK(fe->snes.cycles > s0, "SNES machine advanced in BOTH view");

    /* Combined display: left half samples GB, right half samples SNES. */
    fe_render(fe);
    int half = FB_W / 2;
    int gsx = (half - 1) * GB_W / half, gsy = (FB_H - 1) * GB_H / FB_H;
    int ssx = (FB_W - 1 - half) * SNES_W / half, ssy = (FB_H - 1) * SNES_H / FB_H;
    CHECK(fe->fb[(FB_H - 1) * FB_W + (half - 1)] == fe->gb.ppu.fb[gsy * GB_W + gsx],
          "left half pixel comes from the GB PPU");
    CHECK(fe->fb[(FB_H - 1) * FB_W + (FB_W - 1)] == fe->snes.ppu.fb[ssy * SNES_W + ssx],
          "right half pixel comes from the SNES PPU");

    /* Shared interaction: one input drives BOTH machines. */
    fe_set_input(fe, FE_BTN_A | FE_BTN_START);
    CHECK((fe->gb.joypad & GB_JOY_A) != 0, "shared input reaches GB A");
    CHECK((fe->snes.joy1 & SNES_BTN_START) != 0, "shared input reaches SNES START");

    /* Audio remains available while the SNES runs in the crossover view. */
    int frames = -1;
    CHECK(fe_audio(fe, &frames) != NULL && frames > 500, "crossover view exposes SNES audio");
    free(fe);
}

int main(void) {
    test_gb_view_render();
    test_snes_view_render();
    test_input_mapping();
    test_both_machines_run_and_combine();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

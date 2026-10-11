/* Integration tests: the shared hub hosting BOTH CPU worlds together. */
#include <stdio.h>
#include <string.h>
#include "../core/hub.h"

static uint8_t gb_mem[0x10000];
static uint8_t gb_rd(void *c, uint16_t a){ (void)c; return gb_mem[a]; }
static void    gb_wr(void *c, uint16_t a, uint8_t v){ (void)c; gb_mem[a]=v; }

static uint8_t snes_mem[0x1000000];
static uint8_t sn_rd(void *c, uint32_t a){ (void)c; return snes_mem[a & 0xFFFFFF]; }
static void    sn_wr(void *c, uint32_t a, uint8_t v){ (void)c; snes_mem[a & 0xFFFFFF]=v; }

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

int main(void) {
    printf("test_hub_crossover\n");

    /* set up both machines */
    memset(gb_mem, 0, sizeof gb_mem);
    lr_cpu gb; lr_init(&gb, gb_rd, gb_wr, NULL); gb.pc = 0x200;
    /* GB world program: LD A,0x42 ; INC A ; HALT */
    gb_mem[0x200] = 0x3E; gb_mem[0x201] = 0x42;
    gb_mem[0x202] = 0x3C; gb_mem[0x203] = 0x76;

    memset(snes_mem, 0, sizeof snes_mem);
    snes_mem[0x00FFFC] = 0x00; snes_mem[0x00FFFD] = 0x80;
    w_cpu sn; w_init(&sn, sn_rd, sn_wr, NULL);
    sn.emulation = false; sn.p = W_FLAG_M; /* 8-bit A */
    sn.pc = 0x8000; sn.pb = 0x00;
    /* SNES world program: LDA #$10 ; INC A ; INC A */
    snes_mem[0x8000] = 0xA9; snes_mem[0x8001] = 0x10;
    snes_mem[0x8002] = 0x1A; snes_mem[0x8003] = 0x1A;

    /* build the hub with BOTH worlds running together */
    hub h; hub_init(&h);
    int m0 = hub_add_machine(&h, 0, hub_step_lr, &gb);
    int m1 = hub_add_machine(&h, 1, hub_step_w,  &sn);
    CHECK(m0 == 0 && m1 == 1, "both machines registered");

    /* entities living in each world */
    int e_gb   = hub_add_entity(&h, 5, 5, 0, 100);
    int e_snes = hub_add_entity(&h, 9, 9, 1, 200);
    CHECK(e_gb == 0 && e_snes == 1, "two entities created");
    CHECK(h.ents[e_gb].world == 0 && h.ents[e_snes].world == 1,
          "entities start in their own worlds");

    /* give the GB entity some velocity and tick the whole hub */
    h.ents[e_gb].vx = 1.5f; h.ents[e_gb].vy = -2.0f;
    hub_run(&h, 4);
    CHECK(h.cycles == 4, "hub advanced 4 cycles");
    CHECK(h.ents[e_gb].x == 11.0f, "GB entity moved by velocity");
    CHECK(gb.a == 0x43, "GB world CPU ran (A == 0x42+1)");
    CHECK((sn.a & 0xFF) == 0x12, "SNES world CPU ran (A == 0x10+2)");

    /* cross-world warp: move the SNES entity into the GB world */
    CHECK(hub_warp_entity(&h, e_snes, 0), "warp succeeded");
    CHECK(h.ents[e_snes].world == 0, "entity now shared in GB world");

    /* shared tile map across the whole hub */
    CHECK(hub_set_tile(&h, 3, 4, 77), "set tile");
    CHECK(hub_get_tile(&h, 3, 4) == 77, "get tile");
    CHECK(!hub_set_tile(&h, -1, 0, 1), "out-of-bounds set rejected");

    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

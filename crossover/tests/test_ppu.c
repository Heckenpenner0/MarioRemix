/* Headless unit tests for the Game Boy PPU. */
#include <stdio.h>
#include <string.h>
#include "../core/ppu_gb.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

/* Build a tile in VRAM at tile_id where every pixel == color (0..3). */
static void fill_tile(gb_ppu *p, int tile_id, int color, bool signed_mode) {
    int base = signed_mode ? (0x1000 + (int8_t)(uint8_t)tile_id * 16) : (tile_id * 16);
    for (int row = 0; row < 8; row++) {
        uint8_t lo = (color & 1) ? 0xFF : 0x00;
        uint8_t hi = (color & 2) ? 0xFF : 0x00;
        p->vram[(base + row*2) & 0x1FFF] = lo;
        p->vram[(base + row*2 + 1) & 0x1FFF] = hi;
    }
}

/* Write one pixel of a tile (tile_id) at (col,row) with color 0..3. */
static void set_tile_px(gb_ppu *p, int tile_id, int col, int row, int color, bool signed_mode) {
    int base = signed_mode ? (0x1000 + (int8_t)(uint8_t)tile_id * 16) : (tile_id * 16);
    int addr = base + row*2;
    uint8_t *lo = &p->vram[addr & 0x1FFF];
    uint8_t *hi = &p->vram[(addr+1) & 0x1FFF];
    int bit = 7 - col;
    *lo = (uint8_t)((*lo & ~(1<<bit)) | (((color>>0)&1)<<bit));
    *hi = (uint8_t)((*hi & ~(1<<bit)) | (((color>>1)&1)<<bit));
}

static void test_bg_flat(void) {
    printf("test_bg_flat\n");
    gb_ppu p; gb_ppu_init(&p);
    p.lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_TILE_DATA; /* unsigned 0x8000 */
    p.bgp = 0xE4; /* identity palette */
    fill_tile(&p, 0, 3, false);         /* tile 0 = solid dark */
    /* map (0,0) -> tile 0 already zero */
    gb_ppu_render_frame(&p);
    CHECK(p.shade[0] == 3, "top-left pixel == shade 3");
    CHECK(p.shade[5*GB_W + 5] == 3, "inner pixel == shade 3");
    CHECK(p.fb[0] == p.palette[3], "fb resolved to palette[3]");
}

static void test_bg_scroll(void) {
    printf("test_bg_scroll\n");
    gb_ppu p; gb_ppu_init(&p);
    p.lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_TILE_DATA;
    p.bgp = 0xE4;
    /* Two tiles across: tile1 at map col 1 is solid 2, rest 0. */
    fill_tile(&p, 1, 2, false);
    p.vram[0x1800 + 0*32 + 1] = 1;  /* map (0,0) tile at col1 = tile 1 */
    gb_ppu_render_frame(&p);
    /* screen x=8..15 shows map col1 (world x=8) when SCX=0 */
    CHECK(p.shade[8] == 2, "tile at world x=8 visible at screen x=8");
    CHECK(p.shade[7] == 0, "x=7 still tile 0");
    /* now scroll by 8 -> the tile-1 column shifts to screen x=0 */
    p.scx = 8;
    gb_ppu_render_frame(&p);
    CHECK(p.shade[0] == 2, "SCX=8 shifts tile to x=0");
    CHECK(p.shade[8] == 0, "x=8 now shows next column (tile 0)");
}

static void test_window(void) {
    printf("test_window\n");
    gb_ppu p; gb_ppu_init(&p);
    p.lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_WIN_ENABLE | LCDC_TILE_DATA;
    p.bgp = 0xE4;
    fill_tile(&p, 2, 1, false);        /* window tile = shade 1 */
    p.vram[0x1800 + 0] = 2;            /* BG map tile 0 -> tile 2 */
    /* window map at 0x1800 too; set window tile 0 -> tile 2 as well but different */
    fill_tile(&p, 3, 3, false);
    p.vram[0x1C00 + 0] = 3;            /* window map (0x9C00) tile 0 -> tile 3 */
    p.lcdc |= LCDC_WIN_MAP;            /* window uses 0x9C00 */
    p.wy = 0; p.wx = 7 + 40;           /* window starts at screen x=40 */
    gb_ppu_render_frame(&p);
    CHECK(p.shade[0] == 1, "left of window shows BG (shade 1)");
    CHECK(p.shade[40] == 3, "window region shows window tile (shade 3)");
}

static void test_sprite_overlay(void) {
    printf("test_sprite_overlay\n");
    gb_ppu p; gb_ppu_init(&p);
    p.lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_OBJ_ENABLE | LCDC_TILE_DATA;
    p.bgp = 0xE4; p.obp0 = 0xE4;
    fill_tile(&p, 0, 0, false);        /* BG all shade 0 */
    fill_tile(&p, 5, 2, false);        /* sprite tile all shade 2 */
    /* sprite 0 at screen (10,10) -> OAM y=26, x=18, tile 5 */
    p.oam[0] = 26; p.oam[1] = 18; p.oam[2] = 5; p.oam[3] = 0;
    gb_ppu_render_frame(&p);
    CHECK(p.shade[10*GB_W + 10] == 2, "sprite pixel over BG");
    CHECK(p.shade[10*GB_W + 9] == 0, "pixel left of sprite is BG");
}

static void test_sprite_xflip(void) {
    printf("test_sprite_xflip\n");
    gb_ppu p; gb_ppu_init(&p);
    p.lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_OBJ_ENABLE | LCDC_TILE_DATA;
    p.bgp = 0xE4; p.obp0 = 0xE4;
    /* sprite tile: left column color 3, rest 0 */
    fill_tile(&p, 5, 0, false);
    for (int r = 0; r < 8; r++) set_tile_px(&p, 5, 0, r, 3, false);
    /* no flip: col0 at screen x=10 */
    p.oam[0] = 16; p.oam[1] = 8; p.oam[2] = 5; p.oam[3] = 0;  /* at (0,0) */
    gb_ppu_render_frame(&p);
    CHECK(p.shade[0] == 3, "unflipped left col at x=0");
    /* x flip: col0 moves to right edge (x=7) */
    p.oam[3] = 0x20;
    gb_ppu_render_frame(&p);
    CHECK(p.shade[7] == 3, "xflip moves col0 to x=7");
    CHECK(p.shade[0] == 0, "xflip clears x=0");
}

static void test_sprite_behind_bg(void) {
    printf("test_sprite_behind_bg\n");
    gb_ppu p; gb_ppu_init(&p);
    p.lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_OBJ_ENABLE | LCDC_TILE_DATA;
    p.bgp = 0xE4; p.obp0 = 0xE4;
    fill_tile(&p, 0, 2, false);   /* BG solid shade 2 (non-zero) */
    fill_tile(&p, 5, 3, false);   /* sprite shade 3 */
    p.oam[0] = 16; p.oam[1] = 8; p.oam[2] = 5; p.oam[3] = 0x80; /* behind BG */
    gb_ppu_render_frame(&p);
    CHECK(p.shade[0] == 2, "sprite hidden behind non-zero BG");
}

static void test_sprite_8x16(void) {
    printf("test_sprite_8x16\n");
    gb_ppu p; gb_ppu_init(&p);
    p.lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_OBJ_ENABLE | LCDC_OBJ_SIZE | LCDC_TILE_DATA;
    p.bgp = 0xE4; p.obp0 = 0xE4;
    fill_tile(&p, 0, 0, false);
    /* top tile (id 4) = shade 1, bottom tile (id 5) = shade 2 */
    fill_tile(&p, 4, 1, false);
    fill_tile(&p, 5, 2, false);
    p.oam[0] = 16; p.oam[1] = 8; p.oam[2] = 4; p.oam[3] = 0; /* tile 4 -> 8x16 uses 4,5 */
    gb_ppu_render_frame(&p);
    CHECK(p.shade[0*GB_W + 0] == 1, "top half uses tile 4");
    CHECK(p.shade[8*GB_W + 0] == 2, "bottom half uses tile 5");
}

int main(void) {
    test_bg_flat();
    test_bg_scroll();
    test_window();
    test_sprite_overlay();
    test_sprite_xflip();
    test_sprite_behind_bg();
    test_sprite_8x16();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

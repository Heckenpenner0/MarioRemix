/* Headless unit tests for the SNES PPU. */
#include <stdio.h>
#include <string.h>
#include "../core/ppu_snes.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

/* solid 2bpp tile at byte offset, color 0..3 */
static void tile2bpp(snes_ppu *p, uint32_t off, int color) {
    for (int r = 0; r < 8; r++) {
        p->vram[(off + r*2) & 0xFFFF]     = (color & 1) ? 0xFF : 0x00;
        p->vram[(off + r*2 + 1) & 0xFFFF] = (color & 2) ? 0xFF : 0x00;
    }
}
/* solid 4bpp tile at byte offset, color 0..15 */
static void tile4bpp(snes_ppu *p, uint32_t off, int color) {
    for (int r = 0; r < 8; r++) {
        p->vram[(off + r*2) & 0xFFFF]        = (color & 1) ? 0xFF : 0x00;
        p->vram[(off + r*2 + 1) & 0xFFFF]    = (color & 2) ? 0xFF : 0x00;
        p->vram[(off + 16 + r*2) & 0xFFFF]   = (color & 4) ? 0xFF : 0x00;
        p->vram[(off + 16 + r*2 + 1) & 0xFFFF] = (color & 8) ? 0xFF : 0x00;
    }
}
static void set_cgram(snes_ppu *p, int idx, uint16_t bgr555) {
    p->cgram[idx*2] = (uint8_t)(bgr555 & 0xFF);
    p->cgram[idx*2+1] = (uint8_t)(bgr555 >> 8);
}
static void put_map(snes_ppu *p, uint32_t base, int tx, int ty, uint16_t entry) {
    uint32_t a = (base + (uint32_t)(ty*32 + tx)*2) & 0xFFFF;
    p->vram[a] = (uint8_t)(entry & 0xFF);
    p->vram[(a+1) & 0xFFFF] = (uint8_t)(entry >> 8);
}

static void test_palette(void) {
    printf("test_palette\n");
    snes_ppu p; snes_ppu_init(&p);
    set_cgram(&p, 0, 0x001F); /* red (BGR555 bit0=red) */
    p.fb[0] = 0; /* ignore */
    snes_ppu_render_frame(&p);
    CHECK(p.fb[0] == 0xFFFF0000u, "backdrop CGRAM[0] renders red");
    /* CGRAM register write path */
    snes_ppu_write(&p, 0x2121, 5);      /* CGADD = 5 */
    snes_ppu_write(&p, 0x2122, 0xE0);   /* low byte */
    snes_ppu_write(&p, 0x2122, 0x03);   /* high byte -> 0x03E0 = green */
    CHECK((uint16_t)(p.cgram[10] | (p.cgram[11] << 8)) == 0x03E0, "CGDATA double-write stores BGR555");
}

static void test_bg_mode0_2bpp(void) {
    printf("test_bg_mode0_2bpp\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 0;
    p.bg12nba = 0x01;   /* BG1 char base = 0x2000 */
    p.bgxsc[0] = 0x00;  /* BG1 screen base = 0x0000 */
    p.tm = 0x01;        /* BG1 on main screen */
    tile2bpp(&p, 0x2000, 3);
    put_map(&p, 0x0000, 0, 0, 0);  /* tile 0, palette 0 */
    set_cgram(&p, 3, 0x7C00);      /* index 3 = blue (BGR555) */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 3, "Mode0 BG1 pixel color == 3");
    CHECK(p.fb[0] == 0xFF0000FFu, "resolved to blue");
    CHECK(p.shade[10*SNES_W + 10] == 3, "solid tile across the map");
}

static void test_bg_mode1_4bpp(void) {
    printf("test_bg_mode1_4bpp\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 1;
    p.bg12nba = 0x01;
    p.bgxsc[0] = 0x00;
    p.tm = 0x01;
    tile4bpp(&p, 0x2000, 5);
    put_map(&p, 0x0000, 0, 0, 0);
    set_cgram(&p, 5, 0x7FFF);   /* white */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 5, "Mode1 BG1 uses 4bpp color 5");
    CHECK(p.fb[0] == 0xFFFFFFFFu, "white");
}

static void test_bg_scroll(void) {
    printf("test_bg_scroll\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 0; p.bg12nba = 0x01; p.bgxsc[0] = 0x00; p.tm = 0x01;
    tile2bpp(&p, 0x2000, 2);                 /* tile 0 = color 2 */
    tile2bpp(&p, 0x2000 + 16*3, 1);          /* tile 3 = color 1 */
    put_map(&p, 0x0000, 1, 0, 3);            /* tile (1,0) -> tile 3 */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[8] == 1, "tile col 1 at x=8");
    /* scroll right by 8 -> col1 appears at x=0 */
    snes_ppu_write(&p, 0x210D, 8);   /* BG1HOFS low */
    snes_ppu_write(&p, 0x210D, 0);   /* BG1HOFS high */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 1, "scroll shifts col1 to x=0");
}

static void test_bg_flip(void) {
    printf("test_bg_flip\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 0; p.bg12nba = 0x01; p.bgxsc[0] = 0x00; p.tm = 0x01;
    /* tile with leftmost column = color 3, rest 0 */
    for (int r = 0; r < 8; r++) {
        p.vram[(0x2000 + r*2)]     = 0x80; /* plane0 bit7 */
        p.vram[(0x2000 + r*2 + 1)] = 0x80; /* plane1 bit7 -> color 3 at col0 */
    }
    put_map(&p, 0x0000, 0, 0, 0);
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 3, "col0 set (no flip)");
    put_map(&p, 0x0000, 0, 0, (uint16_t)(1 << 14)); /* hflip */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[7] == 3, "hflip moves col0 to x=7");
    CHECK(p.shade[0] == 0, "hflip clears x=0");
}

static void test_no_layer_when_disabled(void) {
    printf("test_no_layer_when_disabled\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 0; p.bg12nba = 0x01; p.bgxsc[0] = 0x00; p.tm = 0x00; /* nothing on */
    tile2bpp(&p, 0x2000, 3);
    put_map(&p, 0x0000, 0, 0, 0);
    set_cgram(&p, 0, 0x001F); /* red */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 0, "layer off -> backdrop index 0");
    CHECK(p.fb[0] == 0xFFFF0000u, "backdrop color");
}

static void test_sprite_overlay(void) {
    printf("test_sprite_overlay\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 0; p.bg12nba = 0x01; p.bgxsc[0] = 0x00; p.tm = 0x11; /* BG1 + OBJ */
    tile2bpp(&p, 0x2000, 0);   /* BG tile 0 = color 0 (transparent-ish) */
    set_cgram(&p, 0, 0x0000);
    /* sprite: OAM 0. X=10 (upper-left), Y=10, tile 0, palette 0, priority 3 */
    p.obsel = 0x00; /* small=0 (8x8), name base 0 */
    p.oam[0] = 10; p.oam[1] = 10; p.oam[2] = 0; p.oam[3] = (3 << 4); /* prio 3 */
    tile4bpp(&p, 0x0000, 7);   /* sprite tile 0 at name base 0 = color 7 */
    set_cgram(&p, 128 + 7, 0x7C00); /* OBJ palette 0 color 7 = red */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[10*SNES_W + 10] == 128 + 7, "sprite pixel uses OBJ palette base 128");
    CHECK(p.shade[10*SNES_W + 9] == 0, "pixel left of sprite is backdrop");
}


static void test_mode7(void) {
    printf("test_mode7\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 7; p.tm = 0x01;
    p.m7a = 0x0100; p.m7b = 0; p.m7c = 0; p.m7d = 0x0100;   /* scale 1.0 */
    p.m7x = 0; p.m7y = 0; p.m7hofs = 0; p.m7vofs = 0;
    /* interleaved VRAM: word 0 low byte = map(0,0), high byte = tile0 pixel0 */
    p.vram[0] = 0x00;          /* map(0,0) -> tile 0 */
    p.vram[1] = 63;            /* tile 0, pixel (0,0) = color 63 */
    set_cgram(&p, 63, 0x7C00); /* blue */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 63, "mode7 pixel (0,0) maps to texture color 63");
    CHECK(p.fb[0] == 0xFF0000FFu, "mode7 pixel resolves to blue");

    /* horizontal scroll by 8 pixels: pixel x=0 samples plane x=8 */
    snes_ppu_init(&p);
    p.bgmode = 7; p.tm = 0x01;
    p.m7a = 0x0100; p.m7d = 0x0100;
    p.m7hofs = 8;
    p.vram[0] = 0x00; p.vram[1] = 63;         /* map(0,0) tile 0, color 63 */
    p.vram[2] = 0x01; p.vram[129] = 40;       /* map(1,0) tile 1; tile1 pixel0 = 40 */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 40, "mode7 M7HOFS shifts sampling into the next tile");

    /* screen-over: scale 8x pushes x=128 past the 1024px plane */
    snes_ppu_init(&p);
    p.bgmode = 7; p.tm = 0x01;
    p.m7a = 0x0800; p.m7d = 0x0100;           /* 8.0 horizontal scale */
    p.vram[0] = 0x00; p.vram[1] = 63;
    p.m7sel = 0x00;                           /* wrap */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[128] == 63, "mode7 wraps outside the map (M7SEL=00)");
    p.m7sel = 0x80;                           /* transparent */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[128] == 0, "mode7 transparent outside the map (M7SEL=10)");

    /* register writes: M7SEL and double-written matrix */
    snes_ppu_init(&p);
    snes_ppu_write(&p, 0x211A, 0xC3);         /* M7SEL */
    CHECK(p.m7sel == 0xC3, "M7SEL write");
    snes_ppu_write(&p, 0x211B, 0x34);         /* M7A low */
    snes_ppu_write(&p, 0x211B, 0x12);         /* M7A high */
    CHECK(p.m7a == 0x1234, "M7A double write assembles 16-bit value");
    snes_ppu_write(&p, 0x210D, 0x08);         /* M7HOFS low */
    snes_ppu_write(&p, 0x210D, 0x00);         /* M7HOFS high */
    CHECK((p.m7hofs & 0xFFFF) == 0x0008, "M7HOFS via $210D");
}


static void test_windows(void) {
    printf("test_windows\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 0; p.bg12nba = 0x01; p.bgxsc[0] = 0x00; p.tm = 0x01;
    tile2bpp(&p, 0x2000, 3);
    put_map(&p, 0x0000, 0, 0, 0);
    set_cgram(&p, 3, 0x7C00);
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 3, "BG1 visible with no window");

    /* Window 1 covers x=0..3 and masks BG1 on the main screen */
    p.w12sel = 0x04;                 /* BG1: W1 enable (bit2) */
    p.wh[0] = 0; p.wh[1] = 3;
    p.tmw = 0x01;                    /* window-mask BG1 on main */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 0 && p.shade[3] == 0, "window masks BG1 inside W1 (inclusive)");
    CHECK(p.shade[4] == 3, "BG1 unmasked outside W1");

    /* Invert W1 -> only x=0..3 shows */
    p.w12sel = 0x0C;                 /* W1 enable + W1 invert */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 3 && p.shade[3] == 3, "inverted window visible inside range");
    CHECK(p.shade[4] == 0, "inverted window masked outside range");
}

static void test_color_math(void) {
    printf("test_color_math\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 0; p.bg12nba = 0x01; p.bgxsc[0] = 0x00; p.tm = 0x01;
    tile2bpp(&p, 0x2000, 3);
    put_map(&p, 0x0000, 0, 0, 0);
    set_cgram(&p, 3, 0x000F);        /* BG color: R=15, G=0, B=0 */
    set_cgram(&p, 0, 0x0000);

    /* add fixed colour R=10 -> R = 25 */
    p.cgadsub = 0x01;                /* BG1 color math enable */
    p.cgwsel  = 0x00;                /* fixed-colour operand */
    snes_ppu_write(&p, 0x2132, 0x20 | 0x0A);
    snes_ppu_render_frame(&p);
    CHECK(p.fb[0] == 0xFFCE0000u, "color math add (15+10=25)");

    /* half add -> 25/2 = 12 */
    p.cgadsub = 0x41;                /* + half */
    snes_ppu_render_frame(&p);
    CHECK(p.fb[0] == 0xFF630000u, "color math add with halving (12)");

    /* subtract -> 15-10 = 5 */
    p.cgadsub = 0x81;                /* subtract */
    snes_ppu_render_frame(&p);
    CHECK(p.fb[0] == 0xFF290000u, "color math subtract (5)");

    /* saturation */
    set_cgram(&p, 3, 0x001F);
    p.cgadsub = 0x01;
    snes_ppu_write(&p, 0x2132, 0x20 | 0x1F);
    snes_ppu_render_frame(&p);
    CHECK(p.fb[0] == 0xFFFF0000u, "add saturates at 31 (red only)");

    /* clip to black before math */
    set_cgram(&p, 3, 0x000F);
    snes_ppu_write(&p, 0x2132, 0x20 | 0x0A);
    p.cgadsub = 0x01;
    p.cgwsel  = 0xC0;                /* always clip */
    snes_ppu_render_frame(&p);
    CHECK(p.fb[0] == 0xFF520000u, "clip-to-black then add (0+10=10)");

    /* prevent math */
    p.cgwsel = 0x30;                 /* always prevent */
    snes_ppu_render_frame(&p);
    CHECK(p.fb[0] == 0xFF7B0000u, "prevent math leaves main colour (15)");
}

static void test_color_math_subscreen(void) {
    printf("test_color_math_subscreen\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 0; p.bg12nba = 0x21;          /* BG1 chars 0x2000, BG2 0x4000 */
    p.bgxsc[0] = 0x00; p.bgxsc[1] = 0x04;    /* BG1 map 0x0000, BG2 map 0x0800 */
    p.tm = 0x01; p.ts = 0x02;                /* BG1 main, BG2 sub */
    tile2bpp(&p, 0x2000, 3);
    tile2bpp(&p, 0x4000, 3);
    put_map(&p, 0x0000, 0, 0, 0);                 /* BG1: tile 0, palette 0 -> index 3 */
    put_map(&p, 0x0800, 0, 0, (uint16_t)(1 << 10)); /* BG2: tile 0, palette 1 -> index 7 */
    set_cgram(&p, 3, 0x000F);                /* main: R=15 */
    set_cgram(&p, 7, 0x03E0);                /* sub:  G=31 */
    p.cgadsub = 0x01;                        /* math enable for BG1 */
    p.cgwsel  = 0x02;                        /* use sub screen as operand */
    snes_ppu_render_frame(&p);
    CHECK(p.fb[0] == 0xFF7BFF00u, "color math uses the sub screen (R15,G31)");
}


static void test_offset_per_tile(void) {
    printf("test_offset_per_tile\n");
    snes_ppu p; snes_ppu_init(&p);
    p.bgmode = 2;                          /* BG1/BG2 4bpp, BG3 2bpp, OPT available */
    p.bg12nba = 0x11;                      /* both BG1/BG2 chars at 0x2000 */
    p.bgxsc[0] = 0x00;                     /* BG1 map 0x0000 */
    p.bgxsc[2] = 0x08;                     /* BG3 map 0x1000 */
    p.tm = 0x01;
    tile4bpp(&p, 0x2000, 3);               /* tile 0 -> color 3 */
    tile4bpp(&p, 0x2020, 5);               /* tile 1 -> color 5 */
    put_map(&p, 0x0000, 0, 0, 0);          /* BG1 tile (0,0) -> tile 0 */
    put_map(&p, 0x0000, 1, 0, 1);          /* BG1 tile (1,0) -> tile 1 */
    set_cgram(&p, 3, 0x001F);
    set_cgram(&p, 5, 0x03E0);

    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 3, "no OPT: BG1 column 0 shows tile 0");

    /* BG3 entry (0,0): apply to BG1, horizontal offset 8 */
    put_map(&p, 0x1000, 0, 0, (uint16_t)(0x2000 | 8));
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 5, "OPT horizontal offset shifts column to tile 1");
    CHECK(p.shade[8] == 5, "second column also affected (entry column 0 covers x=0..7)");

    /* vertical offset: BG3 entry (0,1) applies to BG1 with V offset 8 */
    put_map(&p, 0x1000, 0, 0, 0);                     /* clear H */
    put_map(&p, 0x0000, 0, 1, 2);                     /* BG1 row 1 -> tile 2 */
    tile4bpp(&p, 0x2040, 7);                          /* tile 2 -> color 7 */
    set_cgram(&p, 7, 0x7C00);
    put_map(&p, 0x1000, 0, 1, (uint16_t)(0x2000 | 8));/* V offset 8 for BG1 */
    snes_ppu_render_frame(&p);
    CHECK(p.shade[0] == 7, "OPT vertical offset samples the next tile row");
}

int main(void) {
    test_palette();
    test_bg_mode0_2bpp();
    test_bg_mode1_4bpp();
    test_bg_scroll();
    test_bg_flip();
    test_no_layer_when_disabled();
    test_sprite_overlay();
    test_mode7();
    test_windows();
    test_color_math();
    test_color_math_subscreen();
    test_offset_per_tile();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

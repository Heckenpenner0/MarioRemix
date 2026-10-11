/* ppu_gb.h - Game Boy Picture Processing Unit (scanline renderer).
 * Original implementation for the crossover engine. Portable C99.
 * Produces a 160x144 frame from VRAM/OAM + PPU registers.
 */
#ifndef PPU_GB_H
#define PPU_GB_H

#include <stdint.h>
#include <stdbool.h>

#define GB_W 160
#define GB_H 144

/* LCDC bits */
enum {
    LCDC_BG_ENABLE   = 0x01,
    LCDC_OBJ_ENABLE  = 0x02,
    LCDC_OBJ_SIZE    = 0x04, /* 0 = 8x8, 1 = 8x16 */
    LCDC_BG_MAP      = 0x08, /* 0 = 0x9800, 1 = 0x9C00 */
    LCDC_TILE_DATA   = 0x10, /* 0 = 0x8800 signed, 1 = 0x8000 unsigned */
    LCDC_WIN_ENABLE  = 0x20,
    LCDC_WIN_MAP     = 0x40, /* 0 = 0x9800, 1 = 0x9C00 */
    LCDC_ENABLE      = 0x80
};

typedef struct {
    uint8_t  vram[0x2000];  /* 0x8000-0x9FFF */
    uint8_t  oam[0xA0];     /* 0xFE00-0xFE9F */
    /* PPU registers */
    uint8_t lcdc, stat, scy, scx, lyc, bgp, obp0, obp1, wy, wx;
    uint8_t ly;             /* current scanline */

    uint8_t  shade[GB_W * GB_H];  /* 0..3 raw color index per pixel */
    uint32_t fb[GB_W * GB_H];     /* resolved RGBA */

    /* Palette mapping shade(0..3) -> RGBA. Default: grayscale. */
    uint32_t palette[4];

    /* Internal window line counter (increments only while window draws). */
    uint8_t win_line;
} gb_ppu;

void gb_ppu_init(gb_ppu *p);

/* Render one scanline `y` into shade[]. */
void gb_ppu_render_line(gb_ppu *p, int y);
/* Render a full visible frame (144 lines) and resolve to fb[]. */
void gb_ppu_render_frame(gb_ppu *p);
/* Map shade[] -> fb[] via the palette. */
void gb_ppu_resolve(gb_ppu *p);

/* PPU MMIO access (register addresses 0xFF40-0xFF4B). */
void    gb_ppu_write_reg(gb_ppu *p, uint16_t addr, uint8_t val);
uint8_t gb_ppu_read_reg(const gb_ppu *p, uint16_t addr);

#endif /* PPU_GB_H */

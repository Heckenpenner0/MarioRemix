/* ppu_snes.h - Super Nintendo PPU (S-PPU1/2) core.
 * Original implementation for the crossover engine. Portable C99.
 * Renders a 256x224 frame from VRAM/CGRAM/OAM + PPU registers.
 *
 * Coverage: BG modes 0-6 tile backgrounds (2/4/8bpp), tile flips, H/V scroll,
 * offset-per-tile, CGRAM palettes (15-bit BGR555), OAM sprites, Mode 7 (affine
 * BG), mask windows, and color math.
 */
#ifndef PPU_SNES_H
#define PPU_SNES_H

#include <stdint.h>
#include <stdbool.h>

#define SNES_W 256
#define SNES_H 224

typedef struct {
    uint8_t  vram[0x10000];  /* 64 KB word-addressed, stored as bytes */
    uint8_t  cgram[0x200];   /* 256 palettes x 2 bytes (BGR555) */
    uint8_t  oam[0x220];     /* 544 bytes */
    uint8_t  hires;          /* pseudo-hires flag (rare) */

    /* Registers */
    uint8_t  inidisp;        /* 0x2100 brightness/blank */
    uint8_t  obsel;          /* 0x2101 sprite size + name base */
    uint8_t  bgmode;         /* 0x2105 mode (0-7) */
    uint8_t  bgxsc[4];       /* 0x2107-0x210A screen base + size */
    uint8_t  bg12nba, bg34nba; /* 0x210B/0x210C char base */
    uint16_t bghofs[4], bgvofs[4]; /* scroll */
    uint8_t  tm, ts, tmw, tsw;     /* 0x212C-0x212F main/sub screen enables */

    /* Mask windows (0x2123-0x212B) */
    uint8_t  w12sel, w34sel, wobsel;   /* 0x2123/0x2124/0x2125 */
    uint8_t  wh[4];                    /* 0x2126-0x2129: W1L,W1R,W2L,W2R */
    uint8_t  wbglog, wobjlog;          /* 0x212A/0x212B */

    /* Color math (0x2130-0x2132) */
    uint8_t  cgwsel, cgadsub;          /* 0x2130/0x2131 */
    uint8_t  coldata[3];               /* fixed color R,G,B (5-bit each) */
    uint8_t  cgadd;          /* 0x2121 CGRAM address */
    uint8_t  cg_latch;       /* CGRAM 2-byte latch (low byte first) */
    bool     cg_have_low;

    /* latch state for double-write scroll registers */
    bool     bghofs_low[4];   /* scroll double-write latches */
    bool     bgvofs_low[4];

    /* Mode 7 (affine background) */
    uint8_t  m7sel;           /* 0x211A */
    uint16_t m7a, m7b, m7c, m7d; /* 0x211B-0x211E, signed 8.8 matrix */
    uint16_t m7x, m7y;        /* 0x211F/0x2120, signed 13-bit centre */
    uint16_t m7hofs, m7vofs;  /* via 0x210D/0x210E, signed 13-bit scroll */
    bool     m7_low[6];       /* double-write latches for 0x211B-0x2120 */

    uint32_t fb[SNES_W * SNES_H];  /* RGBA output */
    uint8_t  shade[SNES_W * SNES_H]; /* palette index (for tests) */

    int      scanline;
} snes_ppu;

void snes_ppu_init(snes_ppu *p);

/* Render the full visible frame. */
void snes_ppu_render_frame(snes_ppu *p);

/* Render a single visible scanline `y` (0..SNES_H-1) into shade[]/fb[].
 * Used by the scanline-driven machine loop so HDMA can affect each line. */
void snes_ppu_render_line(snes_ppu *p, int y);

/* Per-channel 5-bit RGB of a CGRAM entry (for tests / color math). */
void snes_ppu_cgram_rgb(const snes_ppu *p, int idx, int *r, int *g, int *b);

/* Register access: addresses 0x2100-0x213F. */
void    snes_ppu_write(snes_ppu *p, uint16_t addr, uint8_t val);
uint8_t snes_ppu_read(const snes_ppu *p, uint16_t addr);

/* VRAM/CGRAM/OAM access helpers (word-oriented bus uses these). */

#endif /* PPU_SNES_H */

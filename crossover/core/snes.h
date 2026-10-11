/* snes.h - a complete SNES machine: 65816 CPU + PPU + memory bus.
 * This is the loadable "SNES world" of the crossover: drop in a ROM and run it.
 * Portable C99. Original implementation. LoROM mapping (covers most games).
 *
 * Includes general DMA, HDMA, and vblank NMI so games can stream data to the
 * PPU during vblank / per scanline.
 */
#ifndef SNES_MACHINE_H
#define SNES_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "w65816.h"
#include "ppu_snes.h"
#include "apu.h"

/* Auto-joypad-read button bits, as returned by $4218/$4219 (port 1). */
enum {
    SNES_BTN_B      = 1<<0,
    SNES_BTN_Y      = 1<<1,
    SNES_BTN_SELECT = 1<<2,
    SNES_BTN_START  = 1<<3,
    SNES_BTN_UP     = 1<<4,
    SNES_BTN_DOWN   = 1<<5,
    SNES_BTN_LEFT   = 1<<6,
    SNES_BTN_RIGHT  = 1<<7,
    SNES_BTN_A      = 1<<8,
    SNES_BTN_X      = 1<<9,
    SNES_BTN_L      = 1<<10,
    SNES_BTN_R      = 1<<11
};

/* One DMA/HDMA channel (registers $43x0-$43xA). */
typedef struct {
    uint8_t  dmap;       /* $43x0: direction/mode/step */
    uint8_t  bbad;       /* $43x1: B-bus register offset ($21xx) */
    uint16_t a1t;        /* $43x2/3: A-bus source / HDMA table address */
    uint8_t  a1b;        /* $43x4: A-bus bank */
    uint16_t das;        /* $43x5/6: DMA byte count / HDMA indirect addr */
    uint8_t  dasb;       /* $43x7: HDMA indirect data bank */
    uint16_t a2a;        /* $43x8/9: HDMA current address */
    uint8_t  ntram;      /* $43xA: HDMA line counter/repeat */
    /* HDMA runtime state */
    uint16_t addr;       /* active table pointer (16-bit) */
    uint16_t ind;        /* active indirect data pointer (16-bit) */
    uint8_t  raw;        /* live copy of $43xA */
    bool     do_xfer;    /* transfer this line? */
    bool     terminated; /* channel finished for the frame */
} snes_dma_ch;

/* 32000 Hz stereo out of the DSP, at 60 frames per second. */
#define SNES_AUDIO_FRAMES 534

typedef struct {
    w_cpu    cpu;
    snes_ppu ppu;
    apu      apu;

    uint8_t  wram[0x20000];  /* 128 KB work RAM */
    const uint8_t *rom;
    size_t   rom_size;
    bool     hirom;          /* cartridge mapping, detected from the header */
    uint8_t  sram[0x8000];
    size_t   sram_size;       /* battery-backed RAM, from the ROM header */

    /* PPU I/O ports */
    uint16_t vmadd;          /* $2116/$2117 VRAM word address */
    uint8_t  vram_latch;
    bool     vram_have_low;
    uint16_t oamadd;         /* $2102/$2103 OAM address */
    uint16_t cg_latch_addr;

    /* joypads */
    uint16_t joy1;
    uint16_t joy2;
    uint16_t joy1_shift;
    int      joy1_bit;

    /* DMA / HDMA */
    snes_dma_ch dma[8];
    uint8_t  mdmaen;         /* $420B (general DMA enable) */
    uint8_t  hdmaen;         /* $420C (HDMA enable) */

    /* interrupts / video timing */
    uint8_t  nmitimen;       /* $4200: bit7 NMI, bit5 H-IRQ, bit4 V-IRQ, bit0 auto-joypad */
    bool     nmi_flag;       /* $4210 bit7 */
    bool     vblank;         /* $4212 bit7 */
    bool     hblank;         /* $4212 bit6 */
    bool     timeup;         /* $4211 bit7 (IRQ) */

    /* dot-based beam position + H/V counters */
    int      h;              /* dot within the scanline, 0..340 */
    int      v;              /* scanline, 0..261 */
    uint8_t  htime;          /* $4209: H-IRQ target dot */
    uint8_t  vtime;          /* $420A: V-IRQ target scanline */
    uint16_t ophct;          /* $213C: latched H counter */
    uint16_t opvct;          /* $213D: latched V counter */
    uint8_t  hct_tog;        /* $213C low/high byte toggle */
    uint8_t  vct_tog;        /* $213D low/high byte toggle */

    /* audio produced during the last frame (interleaved stereo, 32000 Hz) */
    int16_t  audio[SNES_AUDIO_FRAMES * 2];
    int      audio_len;

    uint64_t cycles;
} snes_machine;

bool snes_init(snes_machine *m, const uint8_t *rom, size_t rom_size);
void snes_reset(snes_machine *m);

/* Run one video frame worth of CPU + PPU (scanline interleaved), firing vblank
 * NMI and running HDMA on each visible line. */
void snes_frame(snes_machine *m);

/* Audio produced by the most recent snes_frame(): `audio_len` interleaved
 * stereo samples (L,R) at 32000 Hz. */
const int16_t *snes_audio(const snes_machine *m, int *frames);

/* Set the current button state for a pad (1 or 2), SNES_BTN_* bits. */
void snes_set_joypad(snes_machine *m, int port, uint16_t buttons);

/* Perform a general DMA transfer for one channel (normally called via $420B). */
void snes_dma_run(snes_machine *m, int ch);

/* HDMA helpers (called by the frame loop; exposed for tests). */
void snes_hdma_frame_init(snes_machine *m);
void snes_hdma_line(snes_machine *m, int line);

/* Bus callbacks. */
uint8_t snes_read(void *ctx, uint32_t addr);
void    snes_write(void *ctx, uint32_t addr, uint8_t val);

#endif /* SNES_MACHINE_H */

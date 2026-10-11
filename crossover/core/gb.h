/* gb.h - a complete Game Boy machine: LR35902 CPU + PPU + memory bus + MBC.
 * This is the loadable "GB world" of the crossover: drop in a ROM and run it.
 * Portable C99. Original implementation.
 */
#ifndef GBM_H
#define GBM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lr35902.h"
#include "ppu_gb.h"

/* Interrupt flag/enable bits (IF @ 0xFF0F, IE @ 0xFFFF). */
enum {
    IF_VBLANK  = 0x01,
    IF_LCDSTAT = 0x02,
    IF_TIMER   = 0x04,
    IF_SERIAL  = 0x08,
    IF_JOYPAD  = 0x10
};

/* Joypad buttons, active-high in software (hardware reads them active-low). */
enum {
    GB_JOY_A      = 1<<0,
    GB_JOY_B      = 1<<1,
    GB_JOY_SELECT = 1<<2,
    GB_JOY_START  = 1<<3,
    GB_JOY_RIGHT  = 1<<4,
    GB_JOY_LEFT   = 1<<5,
    GB_JOY_UP     = 1<<6,
    GB_JOY_DOWN   = 1<<7
};

typedef struct {
    lr_cpu  cpu;
    gb_ppu  ppu;

    uint8_t wram[0x2000];   /* 0xC000-0xDFFF */
    uint8_t hram[0x7F];     /* 0xFF80-0xFFFE */
    uint8_t io[0x80];       /* 0xFF00-0xFF7F (non-PPU) */
    uint8_t ie;             /* 0xFFFF interrupt enable */
    uint8_t joyp;           /* 0xFF00 selection bits (0x30 = both deselected) */
    uint8_t joypad;         /* current buttons, active-high (GB_JOY_*) */

    /* Cartridge */
    const uint8_t *rom;
    size_t         rom_size;
    uint8_t        cart_ram[0x8000]; /* up to 4 banks x 8KB */
    int            mbc;              /* 0=none/ROMonly, 1=MBC1, 3=MBC3, 5=MBC5 */
    uint8_t        rom_bank;         /* current ROM bank (1..) */
    uint8_t        ram_bank;
    bool           ram_enable;

    uint64_t       cycles;
} gb_machine;

/* Set up a machine around a ROM image. `rom` is not copied (can be const
 * mmapped data). Detects MBC type from the cartridge header. */
bool gb_init(gb_machine *g, const uint8_t *rom, size_t rom_size);

/* Reset CPU to the cartridge entry point (0x0100). */
void gb_reset(gb_machine *g);

/* Run the machine for one video frame (~70224 cycles) and render the PPU. */
void gb_frame(gb_machine *g);

/* Read the JOYP register (0xFF00) from the current selection + button state. */
uint8_t gb_read_joyp(const gb_machine *g);

/* Set the current button state (active-high, GB_JOY_* bits). Requests the
 * JOYP interrupt when a newly pressed button is in a selected group. */
void gb_set_joypad(gb_machine *g, uint8_t buttons);

/* Dispatch a pending interrupt (IF & IE) if IME allows. Returns true if taken. */
bool gb_service_interrupts(gb_machine *g);

/* Bus callbacks (wired to the CPU). */
uint8_t gb_read(void *ctx, uint16_t addr);
void    gb_write(void *ctx, uint16_t addr, uint8_t val);

#endif /* GBM_H */

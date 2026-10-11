/* gb.c - complete Game Boy machine implementation. See gb.h. */
#include "gb.h"
#include <string.h>

/* ---- ROM banking (MBC) ---- */
static uint32_t rom_offset(const gb_machine *g, uint16_t addr) {
    if (g->mbc == 0) return addr;                    /* ROM-only: flat */
    /* 0x0000-0x3FFF is always the fixed bank 0; 0x4000-0x7FFF is switchable and
     * can never map bank 0 (writing 0 selects bank 1). */
    uint32_t bank = (addr < 0x4000) ? 0 : (g->rom_bank ? g->rom_bank : 1u);
    return bank * 0x4000u + (addr & 0x3FFF);
}

uint8_t gb_read(void *ctx, uint16_t addr) {
    gb_machine *g = (gb_machine*)ctx;
    if (addr < 0x8000) {                              /* ROM */
        if (!g->rom) return 0xFF;
        uint32_t off = rom_offset(g, addr);
        return (off < g->rom_size) ? g->rom[off] : 0xFF;
    }
    if (addr < 0xA000) return g->ppu.vram[addr - 0x8000];   /* VRAM */
    if (addr < 0xC000) {                              /* cart RAM */
        if (!g->ram_enable) return 0xFF;
        uint32_t off = (uint32_t)g->ram_bank * 0x2000 + (addr - 0xA000);
        return (off < sizeof g->cart_ram) ? g->cart_ram[off] : 0xFF;
    }
    if (addr < 0xE000) return g->wram[addr - 0xC000];        /* WRAM */
    if (addr < 0xFE00) return g->wram[addr - 0xE000];        /* echo WRAM */
    if (addr < 0xFEA0) return g->ppu.oam[addr - 0xFE00];     /* OAM */
    if (addr < 0xFF00) return 0x00;                          /* unusable */
    if (addr < 0xFF80) {                                     /* IO */
        if (addr == 0xFF00) return gb_read_joyp(g);          /* JOYP */
        if (addr >= 0xFF40 && addr <= 0xFF4B) return gb_ppu_read_reg(&g->ppu, addr);
        return g->io[addr - 0xFF00];
    }
    if (addr < 0xFFFF) return g->hram[addr - 0xFF80];        /* HRAM */
    return g->ie;                                            /* 0xFFFF */
}

void gb_write(void *ctx, uint16_t addr, uint8_t val) {
    gb_machine *g = (gb_machine*)ctx;
    if (addr < 0x2000) { g->ram_enable = (val & 0x0F) == 0x0A; return; }
    if (addr < 0x4000) {                                     /* ROM bank select */
        uint8_t b = val & 0x7F; if (b == 0) b = 1;
        g->rom_bank = (uint8_t)((g->rom_bank & 0x60) | b);
        return;
    }
    if (addr < 0x6000) { g->ram_bank = val & 0x03; return; } /* RAM bank */
    if (addr < 0x8000) return;                               /* banking mode */
    if (addr < 0xA000) { g->ppu.vram[addr - 0x8000] = val; return; }
    if (addr < 0xC000) {
        if (!g->ram_enable) return;
        uint32_t off = (uint32_t)g->ram_bank * 0x2000 + (addr - 0xA000);
        if (off < sizeof g->cart_ram) g->cart_ram[off] = val;
        return;
    }
    if (addr < 0xE000) { g->wram[addr - 0xC000] = val; return; }
    if (addr < 0xFE00) { g->wram[addr - 0xE000] = val; return; }
    if (addr < 0xFEA0) { g->ppu.oam[addr - 0xFE00] = val; return; }
    if (addr < 0xFF00) return;
    if (addr < 0xFF80) {
        if (addr == 0xFF00) { g->joyp = (uint8_t)(val & 0x30); return; }   /* JOYP: keep only P14/P15 */
        if (addr >= 0xFF40 && addr <= 0xFF4B) { gb_ppu_write_reg(&g->ppu, addr, val); return; }
        g->io[addr - 0xFF00] = val; return;
    }
    if (addr < 0xFFFF) { g->hram[addr - 0xFF80] = val; return; }
    g->ie = val;
}


/* ---- Joypad (0xFF00) ---- */
uint8_t gb_read_joyp(const gb_machine *g) {
    uint8_t sel = (uint8_t)(g->joyp & 0x30);
    uint8_t out = 0x0F;                                  /* idle: low nibble reads 1 */
    if (!(sel & 0x10)) {                                 /* P14=0 -> directions selected */
        if (g->joypad & GB_JOY_RIGHT) out &= (uint8_t)~0x01;
        if (g->joypad & GB_JOY_LEFT)  out &= (uint8_t)~0x02;
        if (g->joypad & GB_JOY_UP)    out &= (uint8_t)~0x04;
        if (g->joypad & GB_JOY_DOWN)  out &= (uint8_t)~0x08;
    }
    if (!(sel & 0x20)) {                                 /* P15=0 -> actions selected */
        if (g->joypad & GB_JOY_A)      out &= (uint8_t)~0x01;
        if (g->joypad & GB_JOY_B)      out &= (uint8_t)~0x02;
        if (g->joypad & GB_JOY_SELECT) out &= (uint8_t)~0x04;
        if (g->joypad & GB_JOY_START)  out &= (uint8_t)~0x08;
    }
    return (uint8_t)(0xC0 | sel | out);                  /* bits 6-7 read as 1 */
}

void gb_set_joypad(gb_machine *g, uint8_t buttons) {
    uint8_t newly = (uint8_t)(buttons & ~g->joypad);
    g->joypad = buttons;
    if (!newly) return;
    uint8_t sel = (uint8_t)(g->joyp & 0x30);
    uint8_t relevant = 0;
    if (!(sel & 0x10)) relevant |= (uint8_t)(newly & (GB_JOY_RIGHT|GB_JOY_LEFT|GB_JOY_UP|GB_JOY_DOWN));
    if (!(sel & 0x20)) relevant |= (uint8_t)(newly & (GB_JOY_A|GB_JOY_B|GB_JOY_SELECT|GB_JOY_START));
    if (relevant) g->io[0x0F] |= IF_JOYPAD;              /* request JOYP interrupt */
}

bool gb_service_interrupts(gb_machine *g) {
    uint8_t pending = (uint8_t)(g->io[0x0F] & g->ie & 0x1F);
    if (!pending) return false;
    if (g->cpu.halted) g->cpu.halted = false;            /* any enabled IRQ wakes HALT */
    if (!g->cpu.ime) return false;                       /* HALT/STOP: stay until IME */
    for (int i = 0; i < 5; i++) {
        if (pending & (1 << i)) {
            g->io[0x0F] = (uint8_t)(g->io[0x0F] & ~(1 << i));
            return lr_interrupt(&g->cpu, (uint16_t)(0x40 + i * 8));
        }
    }
    return false;
}

bool gb_init(gb_machine *g, const uint8_t *rom, size_t rom_size) {
    memset(g, 0, sizeof *g);
    g->rom = rom; g->rom_size = rom_size;
    /* detect MBC from cartridge type at 0x0147 */
    uint8_t type = (rom && rom_size > 0x0147) ? rom[0x0147] : 0x00;
    switch (type) {
        case 0x01: case 0x02: case 0x03: g->mbc = 1; break;  /* MBC1 */
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: g->mbc = 3; break; /* MBC3 */
        case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: g->mbc = 5; break; /* MBC5 */
        default: g->mbc = (rom_size > 0x8000) ? 3 : 0; break;
    }
    g->rom_bank = 1; g->ram_bank = 0; g->ram_enable = false;
    gb_ppu_init(&g->ppu);
    lr_init(&g->cpu, gb_read, gb_write, g);
    gb_reset(g);
    return rom != NULL;
}

void gb_reset(gb_machine *g) {
    lr_reset(&g->cpu);
    /* entry point is 0x0100; CPU reset state already sets PC=0x0100 */
    g->ppu.lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_TILE_DATA;
    g->joyp = 0x30;   /* both button groups deselected */
    g->joypad = 0x00;
}

void gb_frame(gb_machine *g) {
    /* ~70224 T-cycles per frame; our lr_step returns T-states. */
    const int CYCLES_PER_FRAME = 70224;
    int budget = CYCLES_PER_FRAME;
    g->io[0x0F] |= IF_VBLANK;              /* vblank at the frame boundary */
    while (budget > 0) {
        budget -= gb_service_interrupts(g) ? 12 : lr_step(&g->cpu);
        g->cycles++;
    }
    gb_ppu_render_frame(&g->ppu);
}

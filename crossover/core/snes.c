/* snes.c - complete SNES machine implementation. See snes.h. */
#include "snes.h"
#include <string.h>

/* ---- cartridge mapping ---------------------------------------------------
 * LoROM: banks $00-$3F / $80-$BF expose 32KB of ROM at $8000-$FFFF.
 * HiROM: banks $40-$7D and $C0-$FF each expose a full 64KB bank, and the upper
 *        halves of $00-$3F / $80-$BF mirror them, so $00:FFFC and $C0:FFFC are
 *        the same byte (the reset vector therefore lives at file $FFFC). */
static uint32_t lorom_offset(const snes_machine *m, uint32_t addr) {
    uint32_t bank = (addr >> 16) & 0xFF;
    uint32_t off  = addr & 0xFFFF;
    uint32_t base = bank & 0x3F;
    (void)m;
    return base * 0x8000u + (off - 0x8000u);
}

static uint32_t hirom_offset(uint32_t addr) {
    uint32_t bank = (addr >> 16) & 0xFF;
    uint32_t off  = addr & 0xFFFF;
    if (bank >= 0x40 && bank <= 0x7D) return ((bank - 0x40) << 16) | off;
    if (bank >= 0xC0)                  return ((bank - 0xC0) << 16) | off;
    return ((bank & 0x3F) << 16) | off;   /* upper half of the system banks */
}

/* Resolves a CPU address to a ROM-file offset (only valid for ROM-mapped
 * addresses; the caller decides which those are). */
static uint32_t rom_offset(const snes_machine *m, uint32_t addr) {
    return m->hirom ? hirom_offset(addr) : lorom_offset(m, addr);
}

/* True for banks that carry a full 64KB HiROM bank rather than just the upper
 * half of one. */
static bool hirom_full_bank(uint32_t bank) {
    return (bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0;
}

/* The map-mode byte is at header+$15: $20/$30 = LoROM, $21/$31 = HiROM.  Only
 * trust it when the header's checksum and its complement agree. */
static bool rom_is_hirom(const uint8_t *rom, size_t size) {
    if (!rom || size <= 0xFFDF) return false;
    if ((rom[0xFFC0 + 0x15] & 0x0F) != 0x01) return false;
    uint16_t sum = (uint16_t)(rom[0xFFDE] | (rom[0xFFDF] << 8));
    uint16_t comp = (uint16_t)(rom[0xFFDC] | (rom[0xFFDD] << 8));
    return (uint16_t)(sum ^ comp) == 0xFFFF;
}

/* VMAIN ($2115) step: 0=+1, 1=+32, 2=+128, 3=+128 words */
static void vram_inc(snes_machine *m) {
    int st = m->vram_latch & 3;
    uint16_t step = (st == 1) ? 32 : (st >= 2) ? 128 : 1;
    m->vmadd = (uint16_t)(m->vmadd + step);
}

/* ---- DMA transfer-mode tables ---- */
static const int DMA_UNIT[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };
static const uint8_t DMA_OFF[8][4] = {
    {0,0,0,0}, {0,1,0,0}, {0,0,0,0}, {0,0,1,1},
    {0,1,2,3}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1}
};

static int dma_step(uint8_t dmap) {
    switch ((dmap >> 3) & 3) { case 0: return 1; case 2: return -1; default: return 0; }
}

/* ---- general DMA ($420B) ---- */
void snes_dma_run(snes_machine *m, int ch) {
    if (ch < 0 || ch > 7) return;
    snes_dma_ch *c = &m->dma[ch];
    int mode = c->dmap & 7;
    int unit = DMA_UNIT[mode];
    int step = dma_step(c->dmap);
    uint32_t count = c->das ? c->das : 0x10000u;   /* 0 means 65536 bytes */
    uint32_t addr = ((uint32_t)c->a1b << 16) | c->a1t;
    bool b_to_a = (c->dmap & 0x80) != 0;
    for (uint32_t i = 0; i < count; i++) {
        int off = DMA_OFF[mode][i % (uint32_t)unit];
        uint16_t baddr = (uint16_t)(0x2100 + c->bbad + off);
        if (b_to_a) {
            uint8_t b = snes_read(m, baddr);
            snes_write(m, addr, b);
        } else {
            uint8_t b = snes_read(m, addr);
            snes_write(m, baddr, b);
        }
        addr = (addr & 0xFF0000u) | (((addr & 0xFFFFu) + (uint32_t)step) & 0xFFFFu);
    }
    c->das = 0;
}

/* ---- HDMA ($420C) ---- */
static uint8_t hdma_read(snes_machine *m, uint8_t bank, uint16_t off) {
    return snes_read(m, ((uint32_t)bank << 16) | off);
}

/* Load a block header: the line-count/repeat byte, plus the indirect pointer. */
static void hdma_load_header(snes_machine *m, snes_dma_ch *c) {
    c->raw = hdma_read(m, c->a1b, c->addr);
    c->addr++;
    if (c->dmap & 0x40) {                       /* indirect: 16-bit data pointer */
        uint16_t lo = hdma_read(m, c->a1b, c->addr);
        uint16_t hi = hdma_read(m, c->a1b, (uint16_t)(c->addr + 1));
        c->ind = (uint16_t)(lo | (hi << 8));
        c->addr += 2;
    }
}

void snes_hdma_frame_init(snes_machine *m) {
    for (int ch = 0; ch < 8; ch++) {
        snes_dma_ch *c = &m->dma[ch];
        c->do_xfer = false;
        c->terminated = !(m->hdmaen & (1 << ch));
        if (c->terminated) continue;
        c->addr = c->a1t;                       /* copy AAddress into Address */
        hdma_load_header(m, c);
        if (c->raw == 0) { c->terminated = true; continue; }
        c->do_xfer = true;
    }
}

void snes_hdma_line(snes_machine *m, int line) {
    (void)line;
    for (int ch = 0; ch < 8; ch++) {
        if (!(m->hdmaen & (1 << ch))) continue;
        snes_dma_ch *c = &m->dma[ch];
        if (c->terminated) continue;
        int mode = c->dmap & 7;
        int unit = DMA_UNIT[mode];
        bool indirect = (c->dmap & 0x40) != 0;
        if (c->do_xfer) {
            uint8_t bank  = indirect ? c->dasb : c->a1b;
            uint16_t *ptr = indirect ? &c->ind : &c->addr;
            for (int k = 0; k < unit; k++) {
                uint8_t b = hdma_read(m, bank, *ptr);
                (*ptr)++;
                snes_write(m, (uint16_t)(0x2100 + c->bbad + DMA_OFF[mode][k]), b);
            }
        }
        /* decrement live counter; when the 7-bit count hits zero, next entry */
        c->raw--;
        c->do_xfer = (c->raw & 0x80) != 0;
        if ((c->raw & 0x7F) == 0) {
            hdma_load_header(m, c);
            if (c->raw == 0) { c->terminated = true; continue; }
            c->do_xfer = true;
        }
    }
}

/* ---- DMA/HDMA register file ($43x0-$43xA) ---- */
static void dma_reg_write(snes_machine *m, uint32_t off, uint8_t val) {
    int ch = (int)((off - 0x4300) >> 4);
    int r  = (int)((off - 0x4300) & 0xF);
    snes_dma_ch *c = &m->dma[ch];
    switch (r) {
        case 0x0: c->dmap = val; return;
        case 0x1: c->bbad = val; return;
        case 0x2: c->a1t = (uint16_t)((c->a1t & 0xFF00) | val); return;
        case 0x3: c->a1t = (uint16_t)((c->a1t & 0x00FF) | (val << 8)); return;
        case 0x4: c->a1b = val; return;
        case 0x5: c->das = (uint16_t)((c->das & 0xFF00) | val); return;
        case 0x6: c->das = (uint16_t)((c->das & 0x00FF) | (val << 8)); return;
        case 0x7: c->dasb = val; return;
        case 0x8: c->a2a = (uint16_t)((c->a2a & 0xFF00) | val); return;
        case 0x9: c->a2a = (uint16_t)((c->a2a & 0x00FF) | (val << 8)); return;
        case 0xA: c->ntram = val; return;
        default:  return;
    }
}

/* ---- $2137 SLHV / $213C OPHCT / $213D OPVCT ------------------------------ *
 * The PPU latches the beam position into a pair of counters; each is then read
 * back as a low byte followed by a high byte (the toggle alternates). */
static void snes_latch_hv(snes_machine *m) {
    m->ophct = (uint16_t)m->h;
    m->opvct = (uint16_t)m->v;
    m->hct_tog = 0;
    m->vct_tog = 0;
}

static uint8_t snes_counter_read(snes_machine *m, bool vertical) {
    uint16_t val = vertical ? m->opvct : m->ophct;
    uint8_t *tog = vertical ? &m->vct_tog : &m->hct_tog;
    uint8_t r = (*tog == 0) ? (uint8_t)(val & 0xFF) : (uint8_t)(val >> 8);
    *tog ^= 1;
    return r;
}

/* ---- cartridge SRAM -------------------------------------------------------
 * LoROM maps the cartridge's battery RAM into $20-$3F:$6000-$7FFF and its
 * mirror at $A0-$BF.  The size comes from the internal header ($7FD8, N,
 * where the RAM is 1<<N kilobytes); a header that says "no RAM" still gets the
 * usual 8 KB, which costs nothing for ROMs that never touch the range. */
static bool sram_bank(uint32_t bank) {
    return (bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF);
}
static size_t sram_size_from_header(const uint8_t *rom, size_t rom_size, bool hirom) {
    size_t base = hirom ? 0xFFC0 : 0x7FC0;
    if (rom && rom_size > base + 0x18) {
        unsigned n = rom[base + 0x18];
        if (n >= 1 && n <= 15) {
            size_t sz = (size_t)1 << (n + 10);   /* 1<<N kilobytes */
            if (sz > sizeof(((snes_machine*)0)->sram)) sz = sizeof(((snes_machine*)0)->sram);
            return sz;
        }
    }
    return 0x2000;                               /* 8 KB fallback */
}
static size_t sram_index(uint32_t off, size_t size) {
    return (size_t)((off - 0x6000) & (size - 1));
}

uint8_t snes_read(void *ctx, uint32_t addr) {
    snes_machine *m = (snes_machine*)ctx;
    uint32_t bank = (addr >> 16) & 0xFF;
    uint32_t off  = addr & 0xFFFF;

    if (bank == 0x7E || bank == 0x7F) return m->wram[((bank - 0x7E) << 16) | off];
    if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
        if (off < 0x2000) return m->wram[off];              /* low RAM mirror */
        if (off >= 0x2140 && off <= 0x2143)                   /* APU ports $2140-$2143 */
            return apu_snes_read(&m->apu, (int)(off - 0x2140));
        if (off == 0x2137) return 0x00;                       /* SLHV is write-only */
        if (off == 0x213C) return snes_counter_read(m, false); /* OPHCT */
        if (off == 0x213D) return snes_counter_read(m, true);  /* OPVCT */
        if (off >= 0x2100 && off < 0x2140) return snes_ppu_read(&m->ppu, (uint16_t)off);
        if (off == 0x4016) {                                 /* serial joypad read */
            if (m->joy1_bit < 16) {
                uint8_t b = (uint8_t)(m->joy1_shift & 1);
                m->joy1_shift >>= 1; m->joy1_bit++;
                return b;
            }
            return 1;
        }
        if (off == 0x4210) { uint8_t v = (uint8_t)((m->nmi_flag ? 0x80 : 0) | 0x02); m->nmi_flag = false; return v; }
        if (off == 0x4211) { uint8_t v = (uint8_t)(m->timeup ? 0x80 : 0); m->timeup = false; return v; }
        if (off == 0x4212) return (uint8_t)((m->vblank ? 0x80 : 0) | (m->hblank ? 0x40 : 0));
        if (off == 0x4218) return (uint8_t)(m->joy1 & 0xFF);
        if (off == 0x4219) return (uint8_t)(m->joy1 >> 8);
        if (off == 0x421A) return (uint8_t)(m->joy2 & 0xFF);
        if (off == 0x421B) return (uint8_t)(m->joy2 >> 8);
        if (off >= 0x6000 && off < 0x8000 && m->sram_size && sram_bank(bank))
            return m->sram[sram_index(off, m->sram_size)];
        if (off >= 0x8000) {
            uint32_t ro = rom_offset(m, addr);
            return (ro < m->rom_size) ? m->rom[ro] : 0x00;
        }
    }
    /* HiROM also maps whole 64KB banks into $40-$7D and $C0-$FF. */
    if (m->hirom && hirom_full_bank(bank)) {
        uint32_t ro = rom_offset(m, addr);
        return (ro < m->rom_size) ? m->rom[ro] : 0x00;
    }
    return 0x00;
}

void snes_write(void *ctx, uint32_t addr, uint8_t val) {
    snes_machine *m = (snes_machine*)ctx;
    uint32_t bank = (addr >> 16) & 0xFF;
    uint32_t off  = addr & 0xFFFF;

    if (bank == 0x7E || bank == 0x7F) { m->wram[((bank - 0x7E) << 16) | off] = val; return; }
    if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
        if (off < 0x2000) { m->wram[off] = val; return; }
        if (off >= 0x4300 && off < 0x4380) { dma_reg_write(m, off, val); return; }
        if (off >= 0x6000 && off < 0x8000 && m->sram_size && sram_bank(bank)) {
            m->sram[sram_index(off, m->sram_size)] = val; return;
        }
        switch (off) {
            case 0x4200: m->nmitimen = val; return;                  /* NMITIMEN */
            case 0x420B: {                                            /* MDMAEN */
                m->mdmaen = val;
                for (int ch = 0; ch < 8; ch++) if (val & (1 << ch)) snes_dma_run(m, ch);
                m->mdmaen = 0;
                return;
            }
            case 0x420C: m->hdmaen = val; return;                    /* HDMAEN */
            case 0x4209: m->htime = val; return;                     /* HTIME (H-IRQ target) */
            case 0x420A: m->vtime = val; return;                     /* VTIME (V-IRQ target) */
            case 0x2137: snes_latch_hv(m); return;                   /* SLHV: latch H/V */
            case 0x2140: case 0x2141: case 0x2142: case 0x2143:      /* APU ports */
                apu_snes_write(&m->apu, (int)(off - 0x2140), val); return;
            case 0x4016:                                             /* joypad latch */
                if (val & 1) { m->joy1_shift = m->joy1; m->joy1_bit = 0; }
                return;
            case 0x2115: m->vram_latch = val; return;
            case 0x2116: m->vmadd = (uint16_t)((m->vmadd & 0xFF00) | val); return;
            case 0x2117: m->vmadd = (uint16_t)((m->vmadd & 0x00FF) | (val << 8)); return;
            case 0x2118: m->ppu.vram[((uint32_t)(m->vmadd & 0x7FFF) * 2) & 0xFFFF] = val;
                         if (m->vram_latch & 0x80) vram_inc(m);   /* inc after low-byte write */
                         return;
            case 0x2119: m->ppu.vram[((uint32_t)(m->vmadd & 0x7FFF) * 2 + 1) & 0xFFFF] = val;
                         if (!(m->vram_latch & 0x80)) vram_inc(m); /* inc after high-byte write */
                         return;
            case 0x2102: m->oamadd = (uint16_t)((m->oamadd & 0xFF00) | val); return;
            case 0x2103: m->oamadd = (uint16_t)((m->oamadd & 0x00FF) | (val << 8)); return;
            case 0x2104: m->ppu.oam[(m->oamadd & 0x21F) & 0x21F] = val; m->oamadd++; return;
            default:
                if (off >= 0x2100 && off < 0x2140) { snes_ppu_write(&m->ppu, (uint16_t)off, val); return; }
                return;
        }
    }
}

const int16_t *snes_audio(const snes_machine *m, int *frames) {
    if (frames) *frames = m->audio_len;
    return m->audio;
}

void snes_set_joypad(snes_machine *m, int port, uint16_t buttons) {
    if (port == 2) m->joy2 = buttons; else m->joy1 = buttons;
}

bool snes_init(snes_machine *m, const uint8_t *rom, size_t rom_size) {
    memset(m, 0, sizeof *m);
    /* A .smc dump may carry a 512-byte copier header; the size says so. */
    if (rom && rom_size >= 512 && (rom_size % 1024) == 512) { rom += 512; rom_size -= 512; }
    m->rom = rom; m->rom_size = rom_size;
    m->hirom = rom_is_hirom(rom, rom_size);
    m->sram_size = sram_size_from_header(rom, rom_size, m->hirom);
    snes_ppu_init(&m->ppu);
    apu_init(&m->apu, true);              /* SPC700 boots from the IPL ROM */
    w_init(&m->cpu, snes_read, snes_write, m);
    snes_reset(m);
    return rom != NULL;
}

void snes_reset(snes_machine *m) {
    w_reset(&m->cpu);
    /* The reset vector is fetched from $00:FFFC, which resolves through the
     * cartridge mapping: file $7FFC for LoROM, file $FFFC for HiROM. */
    if (m->rom) {
        uint32_t ro = rom_offset(m, 0x00FFFC);
        if (ro + 1 < m->rom_size) {
            m->cpu.pc = (uint16_t)(m->rom[ro] | (m->rom[ro + 1] << 8));
            m->cpu.pb = 0;
        }
    }
}

/* Dot-based frame loop.
 *
 * An NTSC frame is 262 scanlines of 341 dots.  Hblank begins at dot 274 and
 * vblank at scanline 225.  The CPU is stepped at a constant rate across the
 * line (slow-ROM timing, ~227 cycles per scanline); the beam position is
 * visible to the program through $2137/$213C/$213D and $4212, and the H/V IRQ
 * comparators fire at the exact dot/scanline the program asks for. */
#define SNES_DOTS      341
#define SNES_HB_START  274
#define SNES_VB_START  225
#define SNES_CPU_LINE  227
#define SNES_APU_LINE  65      /* SPC700 cycles per scanline (1.024 MHz) */

void snes_frame(snes_machine *m) {
    const int VISIBLE = SNES_H;   /* 224 visible scanlines */
    const int TOTAL   = 262;      /* NTSC total */

    m->vblank = false; m->hblank = false;
    m->audio_len = 0;
    snes_hdma_frame_init(m);

    for (int v = 0; v < TOTAL; v++) {
        m->v = v;
        m->h = 0;                               /* beam is at the start of the line */
        m->vblank = (v >= SNES_VB_START);

        if (v == SNES_VB_START) {               /* start of vblank */
            m->nmi_flag = true;
            if (m->nmitimen & 0x80) w_nmi(&m->cpu);
        }
        if (v < VISIBLE) snes_hdma_line(m, v);  /* per-scanline transfers */

        /* V-IRQ comparator: fires on the scanline matching VTIME. */
        if ((m->nmitimen & 0x10) && v == m->vtime) {
            m->timeup = true;
            snes_latch_hv(m);
            w_irq(&m->cpu);
        }

        int done = 0;
        for (int h = 0; h < SNES_DOTS; h++) {
            m->h = h;
            m->hblank = (h >= SNES_HB_START);

            if (h == 0 && v < VISIBLE) snes_ppu_render_line(&m->ppu, v);

            /* H-IRQ comparator: fires on every line at the dot matching HTIME. */
            if ((m->nmitimen & 0x20) && h == m->htime) {
                m->timeup = true;
                snes_latch_hv(m);
                w_irq(&m->cpu);
            }

            int due = (SNES_CPU_LINE * (h + 1)) / SNES_DOTS;
            while (done < due) { w_step(&m->cpu); m->cycles++; done++; }
        }

        /* The SPC700 runs at 1.024 MHz, i.e. ~65 cycles per scanline, which is
         * what produces the 32000 Hz sample stream the DSP hands back. */
        int want = SNES_AUDIO_FRAMES - m->audio_len;
        m->audio_len += apu_run(&m->apu, SNES_APU_LINE,
                                m->audio + m->audio_len * 2, want > 0 ? want : 0);
    }
    m->vblank = false;
    m->hblank = false;
    m->nmi_flag = false;
}

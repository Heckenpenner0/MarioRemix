/* Integration tests: full SNES machine (65816 + PPU + bus). */
#include <stdio.h>
#include <string.h>
#include "../core/snes.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

static void vram_word(snes_machine *m, uint16_t word, uint16_t val) {
    snes_write(m, 0x2116, (uint8_t)(word & 0xFF));
    snes_write(m, 0x2117, (uint8_t)(word >> 8));
    snes_write(m, 0x2118, (uint8_t)(val & 0xFF));
    snes_write(m, 0x2119, (uint8_t)(val >> 8));   /* increments vmadd */
}

static void test_bus_routing(void) {
    printf("test_bus_routing\n");
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;  /* reset -> $8000 */
    snes_machine m; snes_init(&m, rom, sizeof rom);

    snes_write(&m, 0x7E1234, 0xAB);
    CHECK(snes_read(&m, 0x7E1234) == 0xAB, "WRAM write/read");
    rom[0x0000] = 0x5A;
    CHECK(snes_read(&m, 0x008000) == 0x5A, "LoROM read at $00:8000");
    CHECK(snes_read(&m, 0x808000) == 0x5A, "LoROM mirror at $80:8000");
    snes_write(&m, 0x2105, 0x01);            /* BGMODE */
    CHECK(m.ppu.bgmode == 0x01, "BGMODE routed to PPU");
}

static void test_vram_port(void) {
    printf("test_vram_port\n");
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    snes_machine m; snes_init(&m, rom, sizeof rom);
    vram_word(&m, 0x0800, 0xABCD);
    CHECK(m.ppu.vram[0x1000] == 0xCD && m.ppu.vram[0x1001] == 0xAB,
          "VRAM word written through $2116-$2119");
    CHECK(m.vmadd == 0x0801, "VMADD auto-incremented");
}

static void test_rom_program_renders(void) {
    printf("test_rom_program_renders\n");
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    /* program at $00:8000:
         LDA #$01 ; STA $212C  (BG1 on)
         LDA #$01 ; STA $210B  (BG12NBA)
         BRA *                 (spin forever) */
    uint8_t prog[] = {
        0xA9, 0x01, 0x8D, 0x2C, 0x21,   /* LDA #$01 ; STA $212C (TM) */
        0xA9, 0x01, 0x8D, 0x0B, 0x21,   /* LDA #$01 ; STA $210B (bg12nba) */
        0x80, 0xFE,                     /* BRA $800A (self) */
    };
    memcpy(rom, prog, sizeof prog);
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;   /* reset -> $8000 */
    rom[0x7FFA] = 0x00; rom[0x7FFB] = 0x80;   /* NMI  -> $8000 */
    rom[0x7FFE] = 0x00; rom[0x7FFF] = 0x80;   /* IRQ/BRK -> $8000 */
    snes_machine m; snes_init(&m, rom, sizeof rom);

    /* scene via bus: char tile (solid color 3) at char base 0x2000 (word 0x1000) */
    for (int r = 0; r < 8; r++) vram_word(&m, 0x1000 + r, 0xFFFF);
    vram_word(&m, 0x0000, 0x0000);         /* tilemap (0,0) -> tile 0 */
    /* CGRAM[3] = blue */
    snes_write(&m, 0x2121, 3);
    snes_write(&m, 0x2122, 0x00);
    snes_write(&m, 0x2122, 0x7C);

    /* Frame 1 runs the program (a real game configures the PPU during vblank).
       The PPU latches layer enables at the start of each line, so the writes
       can only affect the next frame - hence a second frame renders them. */
    snes_frame(&m);
    snes_frame(&m);
    CHECK(m.ppu.tm == 0x01, "CPU program enabled BG1 (TM=1)");
    CHECK(m.ppu.bg12nba == 0x01, "CPU program set BG12NBA");
    CHECK(m.ppu.shade[0] == 3, "rendered pixel color == 3");
    CHECK(m.ppu.fb[0] == 0xFF0000FFu, "resolved to blue");
}


static void test_joypad(void) {
    printf("test_joypad\n");
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    snes_machine m; snes_init(&m, rom, sizeof rom);

    /* auto-read port 1: B + A */
    snes_set_joypad(&m, 1, SNES_BTN_B | SNES_BTN_A);
    CHECK(snes_read(&m, 0x4218) == 0x01, "auto-read low byte has B (bit0)");
    CHECK(snes_read(&m, 0x4219) == 0x01, "auto-read high byte has A (bit8)");

    /* manual serial read: latch, then 16 reads LSB-first == button pattern */
    snes_set_joypad(&m, 1, SNES_BTN_B | SNES_BTN_Y | SNES_BTN_START);
    snes_write(&m, 0x4016, 1);           /* latch */
    uint16_t got = 0;
    for (int i = 0; i < 16; i++) got |= (uint16_t)((snes_read(&m, 0x4016) & 1) << i);
    CHECK((got & 0xFF) == 0x0B, "serial read reproduces buttons (B|Y|Start)");
    CHECK((snes_read(&m, 0x4016) & 1) == 1, "line idles high after 16 bits");
}


static void test_dma(void) {
    printf("test_dma\n");
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    snes_machine m; snes_init(&m, rom, sizeof rom);

    /* mode 4: four bytes -> $2100,$2101,$2102,$2103 */
    m.wram[0x1000]=0x11; m.wram[0x1001]=0x22; m.wram[0x1002]=0x33; m.wram[0x1003]=0x44;
    snes_dma_ch *c=&m.dma[0];
    c->dmap=0x04; c->bbad=0x00; c->a1t=0x1000; c->a1b=0x7E; c->das=4;
    snes_write(&m, 0x420B, 0x01);
    CHECK(m.ppu.inidisp==0x11 && m.ppu.obsel==0x22, "DMA mode 4 -> $2100/$2101");
    CHECK(m.oamadd==0x4433, "DMA mode 4 -> $2102/$2103");
    CHECK(m.dma[0].das==0, "DMA byte count cleared after transfer");

    /* mode 2: bytes written twice to CGRAM data ($2122), auto-incrementing */
    snes_write(&m, 0x2121, 0x00);          /* CGADD = 0 */
    uint8_t d[4]={0x1F,0x00,0xE0,0x03};
    memcpy(&m.wram[0x1100], d, 4);
    c=&m.dma[0]; c->dmap=0x02; c->bbad=0x22; c->a1t=0x1100; c->a1b=0x7E; c->das=4;
    snes_write(&m, 0x420B, 0x01);
    CHECK(m.ppu.cgram[0]==0x1F && m.ppu.cgram[1]==0x00, "DMA mode 2 -> CGRAM[0] low/high");
    CHECK(m.ppu.cgram[2]==0xE0 && m.ppu.cgram[3]==0x03, "DMA mode 2 -> CGRAM[1] (auto-inc)");
}

static void test_hdma_direct(void) {
    printf("test_hdma_direct\n");
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    snes_machine m; snes_init(&m, rom, sizeof rom);

    uint16_t t=0x2000;
    m.wram[t+0]=0x82; m.wram[t+1]=0xAA; m.wram[t+2]=0xBB;  /* 2 lines repeat: AA,BB */
    m.wram[t+3]=0x01; m.wram[t+4]=0x77;                     /* 1 line non-repeat: 77 */
    m.wram[t+5]=0x00;                                       /* end */
    snes_dma_ch *c=&m.dma[0]; c->dmap=0x00; c->bbad=0x00; c->a1t=t; c->a1b=0x7E;
    m.hdmaen=0x01;
    snes_hdma_frame_init(&m);
    snes_hdma_line(&m,0); uint8_t v0=m.ppu.inidisp;
    snes_hdma_line(&m,1); uint8_t v1=m.ppu.inidisp;
    snes_hdma_line(&m,2); uint8_t v2=m.ppu.inidisp;
    CHECK(v0==0xAA && v1==0xBB, "HDMA repeat writes a fresh unit each line");
    CHECK(v2==0x77, "HDMA then loads the next (non-repeat) entry");
}

static void test_hdma_nonrepeat_hold(void) {
    printf("test_hdma_nonrepeat_hold\n");
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    snes_machine m; snes_init(&m, rom, sizeof rom);

    uint16_t t=0x2000;
    m.wram[t+0]=0x03; m.wram[t+1]=0x55; m.wram[t+2]=0x00;  /* 3 lines, write once */
    snes_dma_ch *c=&m.dma[0]; c->dmap=0x00; c->bbad=0x00; c->a1t=t; c->a1b=0x7E;
    m.hdmaen=0x01;
    snes_hdma_frame_init(&m);
    snes_hdma_line(&m,0); uint8_t v0=m.ppu.inidisp;
    snes_hdma_line(&m,1); uint8_t v1=m.ppu.inidisp;
    snes_hdma_line(&m,2); uint8_t v2=m.ppu.inidisp;
    CHECK(v0==0x55, "HDMA non-repeat writes once");
    CHECK(v1==0x55 && v2==0x55, "HDMA non-repeat holds for the remaining lines");
}

static void test_hdma_indirect(void) {
    printf("test_hdma_indirect\n");
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    snes_machine m; snes_init(&m, rom, sizeof rom);

    uint16_t t=0x2000, dat=0x3000;
    m.wram[t+0]=0x82;                        /* 2 lines repeat */
    m.wram[t+1]=0x00; m.wram[t+2]=0x30;      /* indirect pointer -> $7E3000 */
    m.wram[t+3]=0x00;                        /* end */
    m.wram[dat+0]=0xDE; m.wram[dat+1]=0xAD;
    snes_dma_ch *c=&m.dma[0];
    c->dmap=0x40; c->bbad=0x00; c->a1t=t; c->a1b=0x7E; c->dasb=0x7E;  /* indirect */
    m.hdmaen=0x01;
    snes_hdma_frame_init(&m);
    snes_hdma_line(&m,0); uint8_t v0=m.ppu.inidisp;
    snes_hdma_line(&m,1); uint8_t v1=m.ppu.inidisp;
    CHECK(v0==0xDE && v1==0xAD, "HDMA indirect streams from its data block");
}

static void test_nmi(void) {
    printf("test_nmi\n");
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    rom[0x0000]=0x80; rom[0x0001]=0xFE;                       /* $8000: BRA $8000 */
    rom[0x1000]=0xA9; rom[0x1001]=0x01;                       /* $9000: LDA #1 */
    rom[0x1002]=0x8D; rom[0x1003]=0x00; rom[0x1004]=0x00;     /* STA $0000 */
    rom[0x1005]=0x40;                                         /* RTI */
    rom[0x7FFA]=0x00; rom[0x7FFB]=0x90;                       /* NMI -> $9000 */
    rom[0x7FFC]=0x00; rom[0x7FFD]=0x80;                       /* reset -> $8000 */

    snes_machine m;
    snes_init(&m, rom, sizeof rom);
    m.nmitimen = 0x80;
    snes_frame(&m);
    CHECK(m.wram[0]==1, "NMI handler runs during vblank when enabled");

    snes_init(&m, rom, sizeof rom);
    m.nmitimen = 0x00;
    snes_frame(&m);
    CHECK(m.wram[0]==0, "no NMI when $4200 bit7 clear");
}


static void test_hdma_in_frame(void) {
    printf("test_hdma_in_frame\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    rom[0x7FFC]=0x00; rom[0x7FFD]=0x80;   /* reset -> $8000 (BRA self at 0) */
    rom[0x0000]=0x80; rom[0x0001]=0xFE;
    snes_machine m; snes_init(&m, rom, sizeof rom);

    /* HDMA: write $55 to BG1 HOFS ($210D) once, then hold (non-repeat). */
    uint16_t t=0x2000;
    m.wram[t+0]=0x64; m.wram[t+1]=0x55; m.wram[t+2]=0x00;
    snes_dma_ch *c=&m.dma[0]; c->dmap=0x00; c->bbad=0x0D; c->a1t=t; c->a1b=0x7E;
    m.hdmaen=0x01;

    snes_frame(&m);
    CHECK(m.ppu.bghofs[0]==0x55, "HDMA ran during the scanline frame loop");
}

/* $2137/$213C/$213D: latch the beam position, then read each counter back as a
 * low byte followed by a high byte. */
static void test_hv_counters(void) {
    printf("test_hv_counters\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    rom[0x0000]=0x80; rom[0x0001]=0xFE;                  /* $8000: BRA $8000 */
    rom[0x7FFC]=0x00; rom[0x7FFD]=0x80;
    snes_machine m; snes_init(&m, rom, sizeof rom);

    m.h = 123; m.v = 87;
    snes_write(&m, 0x2137, 0x00);                        /* SLHV: latch */
    CHECK(snes_read(&m, 0x213C) == 123, "$213C returns the latched H low byte");
    CHECK(snes_read(&m, 0x213C) == 0,   "$213C then returns the H high byte");
    CHECK(snes_read(&m, 0x213D) == 87,  "$213D returns the latched V low byte");
    CHECK(snes_read(&m, 0x213D) == 0,   "$213D then returns the V high byte");

    m.h = 300; m.v = 260;                                /* values that need the high byte */
    snes_write(&m, 0x2137, 0x00);
    uint8_t hl = snes_read(&m, 0x213C), hh = snes_read(&m, 0x213C);
    uint8_t vl = snes_read(&m, 0x213D), vh = snes_read(&m, 0x213D);
    CHECK((uint16_t)(hl | (hh << 8)) == 300, "H counter round-trips through low/high bytes");
    CHECK((uint16_t)(vl | (vh << 8)) == 260, "V counter round-trips through low/high bytes");

    m.vblank = true;  m.hblank = false;
    CHECK((snes_read(&m, 0x4212) & 0xC0) == 0x80, "$4212 HVBJOY reports vblank");
    m.vblank = false; m.hblank = true;
    CHECK((snes_read(&m, 0x4212) & 0xC0) == 0x40, "$4212 HVBJOY reports hblank");
}

/* The H/V IRQ comparators.  The handler must not re-latch ($2137): the hardware
 * already latched the counters at the instant the IRQ fired, so the values it
 * reads are the exact beam position the comparator matched. */
static const uint8_t irq_handler[] = {
    0xEE,0x10,0x00,                       /* INC $0010   IRQ count, low    */
    0xD0,0x03,                            /* BNE +3      (skip on no wrap) */
    0xEE,0x11,0x00,                       /* INC $0011   IRQ count, high   */
    0xAD,0x3C,0x21, 0x8D,0x12,0x00,       /* LDA $213C ; STA $0012 (H low) */
    0xAD,0x3C,0x21, 0x8D,0x13,0x00,       /* LDA $213C ; STA $0013 (H high)*/
    0xAD,0x3D,0x21, 0x8D,0x14,0x00,       /* LDA $213D ; STA $0014 (V low) */
    0xAD,0x3D,0x21, 0x8D,0x15,0x00,       /* LDA $213D ; STA $0015 (V high)*/
    0xAD,0x11,0x42,                       /* LDA $4211   acknowledge       */
    0x40                                  /* RTI                           */
};

#define IRQ_COUNT(m) ((int)(m.wram[0x10] | (m.wram[0x11] << 8)))
#define IRQ_H(m)     ((int)(m.wram[0x12] | (m.wram[0x13] << 8)))
#define IRQ_V(m)     ((int)(m.wram[0x14] | (m.wram[0x15] << 8)))

static void irq_rom(uint8_t *rom, const uint8_t *prog, size_t n) {
    memset(rom, 0, 0x8000);
    memcpy(rom, prog, n);
    memcpy(rom + 0x1000, irq_handler, sizeof irq_handler);
    rom[0x7FFC]=0x00; rom[0x7FFD]=0x80;   /* reset -> $8000 */
    rom[0x7FFE]=0x00; rom[0x7FFF]=0x90;   /* IRQ (emulation) -> $9000 */
    rom[0x7FEE]=0x00; rom[0x7FEF]=0x90;   /* IRQ (native)    -> $9000 */
}

static void test_hv_irq(void) {
    printf("test_hv_irq\n");
    static uint8_t rom[0x8000];

    /* H-IRQ every line at dot 100. */
    static const uint8_t prog_h[] = {
        0xA9,0x20, 0x8D,0x00,0x42,            /* LDA #$20 ; STA $4200 (H-IRQ) */
        0xA9,0x64, 0x8D,0x09,0x42,            /* LDA #$64 ; STA $4209 (HTIME=100) */
        0x58,                                 /* CLI */
        0x80,0xFE                             /* BRA * */
    };
    irq_rom(rom, prog_h, sizeof prog_h);
    snes_machine m; snes_init(&m, rom, sizeof rom);
    snes_frame(&m);
    CHECK(IRQ_COUNT(m) == 262, "H-IRQ fires once on every one of the 262 lines");
    CHECK(IRQ_H(m) == 100, "H-IRQ latched the H counter at dot 100");
    CHECK(IRQ_V(m) == 261, "last H-IRQ was on scanline 261");

    /* V-IRQ once, on scanline 240. */
    static const uint8_t prog_v[] = {
        0xA9,0x10, 0x8D,0x00,0x42,            /* LDA #$10 ; STA $4200 (V-IRQ) */
        0xA9,0xF0, 0x8D,0x0A,0x42,            /* LDA #$F0 ; STA $420A (VTIME=240) */
        0x58,                                 /* CLI */
        0x80,0xFE                             /* BRA * */
    };
    irq_rom(rom, prog_v, sizeof prog_v);
    snes_init(&m, rom, sizeof rom);
    snes_frame(&m);
    CHECK(IRQ_COUNT(m) == 1, "V-IRQ fires exactly once per frame");
    CHECK(IRQ_V(m) == 240, "V-IRQ latched the V counter at scanline 240");
    CHECK(IRQ_H(m) == 0, "V-IRQ latched at the start of the line (H = 0)");
}

/* The NMI fires at the start of vblank, so $4212 bit7 must already be set. */
static void test_hvbjoy_in_frame(void) {
    printf("test_hvbjoy_in_frame\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    rom[0x0000]=0x80; rom[0x0001]=0xFE;                       /* $8000: BRA $8000 */
    static const uint8_t nh[] = {
        0xAD,0x12,0x42,                       /* LDA $4212 */
        0x8D,0x00,0x00,                       /* STA $0000 */
        0x40                                  /* RTI */
    };
    memcpy(rom + 0x1000, nh, sizeof nh);
    rom[0x7FFA]=0x00; rom[0x7FFB]=0x90;                       /* NMI -> $9000 */
    rom[0x7FFC]=0x00; rom[0x7FFD]=0x80;                       /* reset -> $8000 */
    snes_machine m; snes_init(&m, rom, sizeof rom);
    m.nmitimen = 0x80;
    snes_frame(&m);
    CHECK(m.wram[0] & 0x80, "$4212 reports vblank inside the NMI handler");
}

/* The APU sits on the CPU bus at $2140-$2143. */
static void test_apu_ports(void) {
    printf("test_apu_ports\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    rom[0x0000]=0x80; rom[0x0001]=0xFE;
    rom[0x7FFC]=0x00; rom[0x7FFD]=0x80;
    snes_machine m; snes_init(&m, rom, sizeof rom);

    snes_write(&m, 0x2140, 0x12);
    snes_write(&m, 0x2143, 0xAB);
    CHECK(m.apu.in_port[0] == 0x12, "$2140 writes APU port 0");
    CHECK(m.apu.in_port[3] == 0xAB, "$2143 writes APU port 3");

    m.apu.out_port[1] = 0x34;
    CHECK(snes_read(&m, 0x2141) == 0x34, "$2141 reads APU port 1");
}

/* snes_frame() must hand back a frame of 32 kHz stereo audio from the DSP. */
static void test_frame_audio(void) {
    printf("test_frame_audio\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    rom[0x0000]=0x80; rom[0x0001]=0xFE;
    rom[0x7FFC]=0x00; rom[0x7FFD]=0x80;
    snes_machine m; snes_init(&m, rom, sizeof rom);

    /* key voice 0 onto a self-looping BRR block, straight through the DSP
       register interface ($F2/$F3) as an SPC700 driver would. */
    m.apu.dsp.regs[0x5D]=0x02;                        /* DIR -> table at $0200 */
    m.apu.aram[0x0200]=0x00; m.apu.aram[0x0201]=0x03; /* start = $0300 */
    m.apu.aram[0x0202]=0x00; m.apu.aram[0x0203]=0x03; /* loop  = $0300 */
    m.apu.aram[0x0300]=0x83;                          /* BRR: range 8, loop + end */
    for (int i = 1; i < 9; i++) m.apu.aram[0x0300 + i] = 0x77;
    apu_dsp_write(&m.apu, 0x04, 0x00);                /* SRCN */
    apu_dsp_write(&m.apu, 0x00, 0x7F);                /* VOL L */
    apu_dsp_write(&m.apu, 0x01, 0x7F);                /* VOL R */
    apu_dsp_write(&m.apu, 0x02, 0x00);                /* PITCH lo */
    apu_dsp_write(&m.apu, 0x03, 0x10);                /* PITCH hi -> $1000 */
    apu_dsp_write(&m.apu, 0x05, 0x00);                /* ADSR off -> use GAIN */
    apu_dsp_write(&m.apu, 0x07, 0x7F);                /* GAIN = $7F */
    apu_dsp_write(&m.apu, 0x0C, 0x7F);                /* MVOL L */
    apu_dsp_write(&m.apu, 0x1C, 0x7F);                /* MVOL R */
    apu_dsp_write(&m.apu, 0x4C, 0x01);                /* KON voice 0 */

    snes_frame(&m);

    int frames = 0;
    const int16_t *a = snes_audio(&m, &frames);
    CHECK(frames > 500 && frames <= SNES_AUDIO_FRAMES, "a frame yields ~534 frames of 32 kHz stereo audio");

    int nonzero = 0;
    for (int i = 0; i < frames * 2; i++) if (a[i] != 0) nonzero++;
    CHECK(nonzero > frames, "the DSP's output reaches the machine's frame audio buffer");
    CHECK(a[0] == a[1], "left and right carry the same voice");
}

/* Cartridge SRAM lives at $20-$3F:$6000-$7FFF (mirrored at $A0-$BF). */
static void test_sram(void) {
    printf("test_sram\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    rom[0x0000] = 0x80; rom[0x0001] = 0xFE;      /* $8000: BRA $8000 */
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;      /* reset -> $8000 */
    rom[0x7FD8] = 0x03;                          /* header: RAM = 1<<3 KB = 8 KB */
    snes_machine m; snes_init(&m, rom, sizeof rom);
    CHECK(m.sram_size == 0x2000, "SRAM size comes from the header byte $7FD8");

    snes_write(&m, 0x206000, 0x5A);
    snes_write(&m, 0x207FFF, 0xA5);
    CHECK(snes_read(&m, 0x206000) == 0x5A, "SRAM is writable at $20:$6000");
    CHECK(snes_read(&m, 0x207FFF) == 0xA5, "SRAM reaches the top of the window");
    CHECK(snes_read(&m, 0xA06000) == 0x5A, "$A0:$6000 mirrors the same RAM");
    CHECK(snes_read(&m, 0x3F6000) == 0x5A, "$3F:$6000 mirrors the same RAM");

    snes_write(&m, 0x006000, 0x77);
    CHECK(snes_read(&m, 0x006000) == 0x00, "$00:$6000 is not cartridge RAM");
    CHECK(snes_read(&m, 0x008000) == 0x80, "$00:$8000 still maps LoROM");

    /* header variants */
    rom[0x7FD8] = 0x00;                          /* "no RAM advertised" */
    snes_init(&m, rom, sizeof rom);
    CHECK(m.sram_size == 0x2000, "a header without a RAM size falls back to 8 KB");

    rom[0x7FD8] = 0x05;                          /* 1<<5 KB = 32 KB */
    snes_init(&m, rom, sizeof rom);
    CHECK(m.sram_size == 0x8000, "a 32 KB SRAM header is honoured");
    snes_write(&m, 0x207FFE, 0x3C);            /* outside the first 8 KB */
    CHECK(snes_read(&m, 0x207FFE) == 0x3C, "the wider SRAM is writable past 8 KB");
}

/* HiROM: $40-$7D and $C0-$FF carry full 64KB banks, and the system banks'
 * upper halves mirror them, so $00:FFFC and $C0:FFFC are the same byte. */
static void test_hirom(void) {
    printf("test_hirom\n");
    static uint8_t rom[0x10000]; memset(rom, 0, sizeof rom);
    rom[0x0000] = 0x11;                          /* $C0:0000 / $40:0000 */
    rom[0x8000] = 0x80; rom[0x8001] = 0xFE;      /* $00:8000 / $C0:8000: BRA * */
    rom[0xFFD5] = 0x21;                          /* header map mode: HiROM */
    rom[0xFFDC] = 0xFF; rom[0xFFDD] = 0xFF;      /* complement */
    rom[0xFFDE] = 0x00; rom[0xFFDF] = 0x00;      /* checksum -> XOR == $FFFF */
    rom[0xFFFC] = 0x00; rom[0xFFFD] = 0x80;      /* reset vector at file $FFFC */

    snes_machine m; snes_init(&m, rom, sizeof rom);
    CHECK(m.hirom, "a HiROM map-mode byte with a matching checksum selects HiROM");

    CHECK(snes_read(&m, 0x00FFFC) == 0x00, "$00:FFFC reads the reset vector low byte");
    CHECK(snes_read(&m, 0xC0FFFC) == 0x00, "$C0:FFFC is the same byte as $00:FFFC");
    CHECK(snes_read(&m, 0x008000) == 0x80, "$00:8000 mirrors file $8000");
    CHECK(snes_read(&m, 0xC00000) == 0x11, "$C0:0000 reads the start of the ROM");
    CHECK(snes_read(&m, 0x400000) == 0x11, "$40:0000 reads the start of the ROM");
    CHECK(m.cpu.pc == 0x8000, "the reset vector was fetched through the HiROM map");

    /* a map-mode byte whose checksum does not check out stays LoROM */
    rom[0xFFDF] = 0x01;
    snes_init(&m, rom, sizeof rom);
    CHECK(!m.hirom, "a header whose checksum disagrees is not trusted");
}

/* A .smc dump may carry a 512-byte copier header that has to be stripped. */
static void test_copier_header(void) {
    printf("test_copier_header\n");
    static uint8_t dump[512 + 0x8000];
    memset(dump, 0xFF, sizeof dump);             /* garbage prefix */
    dump[512 + 0x0000] = 0x80; dump[512 + 0x0001] = 0xFE;   /* $8000: BRA * */
    dump[512 + 0x7FFC] = 0x00; dump[512 + 0x7FFD] = 0x80;   /* reset -> $8000 */
    snes_machine m; snes_init(&m, dump, sizeof dump);
    CHECK(m.rom_size == 0x8000, "the 512-byte copier header is stripped");
    CHECK(m.cpu.pc == 0x8000, "the program still runs from $8000");
    CHECK(snes_read(&m, 0x008000) == 0x80, "LoROM mapping is unaffected");

    /* a plain 32KB image must not be truncated */
    snes_init(&m, dump + 512, 0x8000);
    CHECK(m.rom_size == 0x8000, "an ordinary ROM image is left alone");
}

int main(void) {
    test_bus_routing();
    test_vram_port();
    test_rom_program_renders();
    test_joypad();
    test_dma();
    test_hdma_direct();
    test_hdma_nonrepeat_hold();
    test_hdma_indirect();
    test_nmi();
    test_hdma_in_frame();
    test_hv_counters();
    test_hv_irq();
    test_hvbjoy_in_frame();
    test_apu_ports();
    test_sram();
    test_hirom();
    test_copier_header();
    test_frame_audio();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

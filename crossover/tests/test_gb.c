/* Integration tests: full GB machine (CPU + PPU + bus + MBC). */
#include <stdio.h>
#include <string.h>
#include "../core/gb.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

static void test_bus_routing(void) {
    printf("test_bus_routing\n");
    gb_machine g; gb_init(&g, NULL, 0);
    /* WRAM */
    gb_write(&g, 0xC000, 0xAB);
    CHECK(gb_read(&g, 0xC000) == 0xAB, "WRAM write/read");
    CHECK(gb_read(&g, 0xE000) == 0xAB, "echo RAM mirrors WRAM");
    /* HRAM + IE */
    gb_write(&g, 0xFF80, 0x55);
    CHECK(gb_read(&g, 0xFF80) == 0x55, "HRAM write/read");
    gb_write(&g, 0xFFFF, 0x0F);
    CHECK(gb_read(&g, 0xFFFF) == 0x0F, "IE register");
    /* VRAM routes into PPU */
    gb_write(&g, 0x8123, 0x77);
    CHECK(g.ppu.vram[0x123] == 0x77, "VRAM routed to PPU");
    /* OAM routes into PPU */
    gb_write(&g, 0xFE05, 0x99);
    CHECK(g.ppu.oam[5] == 0x99, "OAM routed to PPU");
    /* PPU registers route */
    gb_write(&g, 0xFF43, 0x2C);   /* SCX */
    CHECK(gb_read(&g, 0xFF43) == 0x2C, "SCX register via IO");
}

static void test_rom_program_renders(void) {
    printf("test_rom_program_renders\n");
    /* A tiny "ROM": program at 0x0100 writes a tile + map + palettes and halts. */
    uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    rom[0x0147] = 0x00; /* ROM-only cartridge */
    /* We'll just poke VRAM directly through the machine before running to set
     * up the scene, and use the CPU program to enable the screen. */

    gb_machine g; gb_init(&g, rom, sizeof rom);
    g.cpu.pc = 0x0100;

    /* build a solid tile (color 3) at tile 0 via the bus */
    for (int row = 0; row < 8; row++) {
        gb_write(&g, (uint16_t)(0x8000 + row*2), 0xFF);
        gb_write(&g, (uint16_t)(0x8000 + row*2 + 1), 0xFF);
    }
    /* map tile (0,0) -> tile 0 (already 0); palette identity */
    gb_write(&g, 0xFF47, 0xE4);
    /* CPU program: LD A,0x91 ; LDH (0x40),A  (LCDC = enable+bg+tiledata) ; HALT */
    rom[0x0100] = 0x3E; rom[0x0101] = 0x91;
    rom[0x0102] = 0xE0; rom[0x0103] = 0x40;
    rom[0x0104] = 0x76;

    /* run a frame (executes the program then renders) */
    gb_frame(&g);
    CHECK(g.ppu.shade[0] == 3, "top-left pixel is solid shade 3");
    CHECK(g.ppu.fb[0] == g.ppu.palette[3], "fb resolved");
    CHECK((gb_read(&g, 0xFF40) & 0x80) != 0, "LCDC enable set by program");
}

static void test_mbc3_banking(void) {
    printf("test_mbc3_banking\n");
    /* Build a fake MBC3 ROM with distinguishable banks. */
    static uint8_t rom[0x20000]; /* 128KB = 8 banks */
    memset(rom, 0, sizeof rom);
    rom[0x0147] = 0x01; /* MBC1 (treated as banked) */
    for (int b = 0; b < 8; b++) rom[b * 0x4000] = (uint8_t)(0x10 + b);
    gb_machine g; gb_init(&g, rom, sizeof rom);
    CHECK(gb_read(&g, 0x0000) == 0x10, "bank 0 visible at 0x0000");
    CHECK(gb_read(&g, 0x4000) == 0x11, "default bank 1 at 0x4000");
    gb_write(&g, 0x2000, 0x05);   /* select bank 5 */
    CHECK(gb_read(&g, 0x4000) == 0x15, "bank 5 mapped at 0x4000 after write");
    CHECK(gb_read(&g, 0x0000) == 0x10, "bank 0 fixed region still bank 0");
}


static void test_joypad_read(void) {
    printf("test_joypad_read\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    gb_machine g; gb_init(&g, rom, sizeof rom);

    /* directions deselected: low nibble all 1 */
    gb_write(&g, 0xFF00, 0x30);
    CHECK((gb_read(&g, 0xFF00) & 0x0F) == 0x0F, "idle JOYP low nibble == 0xF");

    /* select directions (P14=0) and press RIGHT + UP */
    gb_write(&g, 0xFF00, 0x20);
    gb_set_joypad(&g, GB_JOY_RIGHT | GB_JOY_UP);
    uint8_t v = gb_read(&g, 0xFF00);
    CHECK((v & 0x01) == 0, "RIGHT pressed -> bit0 low");
    CHECK((v & 0x04) == 0, "UP pressed -> bit2 low");
    CHECK((v & 0x02) != 0, "LEFT not pressed -> bit1 high");

    /* select actions (P15=0) and press A */
    gb_write(&g, 0xFF00, 0x10);
    gb_set_joypad(&g, GB_JOY_A);
    v = gb_read(&g, 0xFF00);
    CHECK((v & 0x01) == 0, "A pressed -> bit0 low (actions)");
    CHECK((v & 0x02) != 0, "B not pressed -> bit1 high");
}

static void test_joypad_interrupt(void) {
    printf("test_joypad_interrupt\n");
    static uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    gb_machine g; gb_init(&g, rom, sizeof rom);

    gb_write(&g, 0xFF00, 0x20);   /* directions selected */
    g.ie = IF_JOYPAD;
    g.io[0x0F] = 0x00;
    gb_set_joypad(&g, GB_JOY_LEFT);
    CHECK((g.io[0x0F] & IF_JOYPAD) != 0, "pressing LEFT raises JOYP IF bit");

    /* with IME set, servicing jumps to the JOYP vector ($60) */
    g.cpu.ime = true;
    g.cpu.pc = 0x1234; g.cpu.sp = 0xFFFE;
    bool taken = gb_service_interrupts(&g);
    CHECK(taken, "interrupt taken");
    CHECK(g.cpu.pc == 0x60, "jumped to JOYP vector 0x0060");
    CHECK((g.io[0x0F] & IF_JOYPAD) == 0, "IF bit cleared after dispatch");
}

int main(void) {
    test_bus_routing();
    test_rom_program_renders();
    test_mbc3_banking();
    test_joypad_read();
    test_joypad_interrupt();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

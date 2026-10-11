/* Headless unit tests for the SNES APU (SPC700 + ARAM + IPL boot + DSP). */
#include <stdio.h>
#include <string.h>
#include "../core/apu.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

static int16_t buf[4096];
static void spin(apu *a, int cycles) {
    while (cycles > 0) { int c = cycles > 512 ? 512 : cycles; apu_run(a, c, buf, 8); cycles -= c; }
}

static void test_brr_decode(void) {
    printf("test_brr_decode\n");
    /* range 0, filter 0 -> the nibbles pass through */
    uint8_t blk[9] = { 0x00, 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF };
    int16_t out[16], s1 = 0, s2 = 0;
    dsp_decode_brr(blk, out, &s1, &s2);
    /* range 0: RD = (nib << 0) >> 1 (the BRR decoder halves) */
    CHECK(out[0]==0 && out[2]==1 && out[6]==3, "BRR range 0 positive nibbles");
    CHECK(out[8]==-4 && out[14]==-1, "BRR range 0 negative nibbles");

    /* range 8 -> value = (nib << 8) >> 1 = nib*128 */
    uint8_t blk2[9] = { 0x80, 0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77 };
    s1 = s2 = 0;
    dsp_decode_brr(blk2, out, &s1, &s2);
    CHECK(out[0] == 7 * 128, "BRR range 8 scales the nibble");

    /* filter 2 with previous samples */
    uint8_t blk3[9] = { 0x08, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };  /* filter 2 */
    s1 = 100; s2 = 0;
    dsp_decode_brr(blk3, out, &s1, &s2);
    CHECK(out[0] == 190, "BRR filter 2 predictor (+190 for s1=100)");
}

static void test_dsp_voice_output(void) {
    printf("test_dsp_voice_output\n");
    apu a; apu_init(&a, false);

    a.dsp.regs[0x5D] = 0x02;                       /* DIR: sample table at $0200 */
    a.aram[0x0200]=0x00; a.aram[0x0201]=0x03;      /* start = $0300 */
    a.aram[0x0202]=0x00; a.aram[0x0203]=0x03;      /* loop  = $0300 */
    a.aram[0x0300]=0x83;                           /* BRR: range 8, loop + end (self-looping) */
    for (int i = 1; i < 9; i++) a.aram[0x0300 + i] = 0x77;

    apu_dsp_write(&a, 0x04, 0x00);                 /* voice 0 SRCN = 0 */
    apu_dsp_write(&a, 0x00, 0x7F);                 /* VOL L */
    apu_dsp_write(&a, 0x01, 0x7F);                 /* VOL R */
    apu_dsp_write(&a, 0x02, 0x00);                 /* PITCH low */
    apu_dsp_write(&a, 0x03, 0x10);                 /* PITCH high -> $1000 */
    apu_dsp_write(&a, 0x05, 0x00);                 /* ADSR1: ADSR off */
    apu_dsp_write(&a, 0x07, 0x7F);                 /* GAIN: direct $7F */
    apu_dsp_write(&a, 0x0C, 0x7F);                 /* MVOL L */
    apu_dsp_write(&a, 0x1C, 0x7F);                 /* MVOL R */
    apu_dsp_write(&a, 0x4C, 0x01);                 /* KON voice 0 */

    int16_t l = 0, r = 0;
    /* push a few samples through the DSP by running ~64 SPC cycles each */
    for (int i = 0; i < 200; i++) { apu_run(&a, 32, buf, 1); l = buf[0]; r = buf[1]; }
    CHECK(l != 0 || r != 0, "keyed-on voice produces audio output");
    CHECK(a.dsp.v[0].env > 0, "voice envelope is active");
}

static void test_envelope_attack(void) {
    printf("test_envelope_attack\n");
    apu a; apu_init(&a, false);
    a.dsp.regs[0x5D] = 0x02;
    a.aram[0x0200]=0x00; a.aram[0x0201]=0x03;
    a.aram[0x0202]=0x00; a.aram[0x0203]=0x03;
    a.aram[0x0300]=0x02;                            /* loop, no end */
    for (int i=1;i<9;i++) a.aram[0x0300+i]=0x11;
    apu_dsp_write(&a, 0x05, 0x8F);                  /* ADSR on, attack rate 15 */
    apu_dsp_write(&a, 0x06, 0xE0);                  /* sustain level 7 */
    apu_dsp_write(&a, 0x4C, 0x01);
    int e0 = a.dsp.v[0].env;
    spin(&a, 4000);
    CHECK(e0 == 0, "envelope starts at zero");
    CHECK(a.dsp.v[0].env > 0, "attack raises the envelope");
}

static void test_ipl_upload(void) {
    printf("test_ipl_upload\n");
    apu a; apu_init(&a, true);
    CHECK(a.cpu.pc == 0xFFC0, "SPC boots from the IPL ROM at $FFC0");

    /* wait for the $BBAA ready signature */
    for (int i = 0; i < 100000 && !(apu_snes_read(&a,0)==0xAA && apu_snes_read(&a,1)==0xBB); i++)
        spin(&a, 8);
    CHECK(apu_snes_read(&a,0)==0xAA && apu_snes_read(&a,1)==0xBB, "IPL signals $BBAA ready");

    /* upload a program to $0200: MOV A,#$42 ; MOV $0300,A */
    uint8_t prog[6] = { 0xE8, 0x42, 0xC5, 0x00, 0x03, 0x00 };
    uint16_t addr = 0x0200;
    apu_snes_write(&a, 2, (uint8_t)(addr & 0xFF));
    apu_snes_write(&a, 3, (uint8_t)(addr >> 8));
    apu_snes_write(&a, 1, 1);                       /* non-zero: transfer mode */
    apu_snes_write(&a, 0, 0xCC);
    for (int i = 0; i < 100000 && apu_snes_read(&a,0) != 0xCC; i++) spin(&a, 8);
    CHECK(apu_snes_read(&a,0) == 0xCC, "IPL accepts the $CC start");

    for (int i = 0; i < 6; i++) {
        apu_snes_write(&a, 1, prog[i]);
        apu_snes_write(&a, 0, (uint8_t)i);
        for (int w = 0; w < 100000 && apu_snes_read(&a,0) != (uint8_t)i; w++) spin(&a, 8);
    }
    CHECK(a.aram[0x0200]==0xE8 && a.aram[0x0205]==0x00, "program bytes uploaded to ARAM");

    /* tell the IPL to jump to $0200: mode 0, index+2 = 8 */
    apu_snes_write(&a, 2, 0x00);
    apu_snes_write(&a, 3, 0x02);
    apu_snes_write(&a, 1, 0);                       /* mode 0: execute */
    apu_snes_write(&a, 0, 8);                       /* next index (6+2) */
    for (int i = 0; i < 100000 && apu_snes_read(&a,0) != 8; i++) spin(&a, 8);
    spin(&a, 200);
    CHECK(a.aram[0x0300] == 0x42, "uploaded program executes on the SPC700");
}

/* $3D (NON) substitutes the noise generator for a voice's BRR samples. */
static void test_dsp_noise(void) {
    printf("test_dsp_noise\n");
    apu a; apu_init(&a, false);
    a.dsp.regs[0x5D] = 0x02;                      /* DIR -> table at $0200 */
    a.aram[0x0200]=0x00; a.aram[0x0201]=0x03;
    a.aram[0x0202]=0x00; a.aram[0x0203]=0x03;
    a.aram[0x0300]=0x83;                          /* BRR: range 8, loop + end */
    for (int i = 1; i < 9; i++) a.aram[0x0300 + i] = 0x77;
    apu_dsp_write(&a, 0x04, 0x00);
    apu_dsp_write(&a, 0x00, 0x7F);                /* VOL L */
    apu_dsp_write(&a, 0x01, 0x7F);                /* VOL R */
    apu_dsp_write(&a, 0x02, 0x00);                /* PITCH lo */
    apu_dsp_write(&a, 0x03, 0x10);                /* PITCH hi */
    apu_dsp_write(&a, 0x05, 0x00);                /* ADSR off -> GAIN */
    apu_dsp_write(&a, 0x07, 0x7F);                /* GAIN = $7F */
    apu_dsp_write(&a, 0x0C, 0x7F);                /* MVOL L */
    apu_dsp_write(&a, 0x1C, 0x7F);                /* MVOL R */
    apu_dsp_write(&a, 0x3D, 0x01);                /* NON: voice 0 = noise */
    apu_dsp_write(&a, 0x4C, 0x01);                /* KON voice 0 */

    uint16_t lfsr0 = a.dsp.noise_lfsr;
    for (int i = 0; i < 64; i++) { int16_t l, r; apu_run(&a, 32, buf, 4); (void)l; (void)r; }
    CHECK(a.dsp.noise_lfsr != lfsr0, "the noise LFSR advances while a voice plays noise");
    CHECK(a.dsp.v[0].env > 0, "the noise voice is keyed on");

    /* without NON the same block plays its BRR samples, not noise */
    apu b; apu_init(&b, false);
    b.dsp.regs[0x5D] = 0x02;
    b.aram[0x0200]=0x00; b.aram[0x0201]=0x03;
    b.aram[0x0202]=0x00; b.aram[0x0203]=0x03;
    b.aram[0x0300]=0x83;
    for (int i = 1; i < 9; i++) b.aram[0x0300 + i] = 0x77;
    uint16_t lfsr_before = b.dsp.noise_lfsr;
    for (int i = 0; i < 64; i++) apu_run(&b, 32, buf, 4);
    CHECK(b.dsp.noise_lfsr == lfsr_before, "noise stays idle when NON is clear");
}

int main(void) {
    test_brr_decode();
    test_dsp_voice_output();
    test_envelope_attack();
    test_ipl_upload();
    test_dsp_noise();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

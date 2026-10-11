/* Headless unit tests for the WDC 65816 CPU core. */
#include <stdio.h>
#include <string.h>
#include "../core/w65816.h"

static uint8_t mem[0x1000000];
static uint8_t t_read(void *ctx, uint32_t a) { (void)ctx; return mem[a & 0xFFFFFF]; }
static void    t_write(void *ctx, uint32_t a, uint8_t v) { (void)ctx; mem[a & 0xFFFFFF] = v; }

static w_cpu cpu;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

static void setup_native(void) {
    memset(mem, 0, sizeof mem);
    mem[0x00FFFC] = 0x00; mem[0x00FFFD] = 0x80; /* reset -> 0x8000 */
    w_init(&cpu, t_read, t_write, NULL);
    /* leave emulation mode -> 16-bit M/X */
    cpu.emulation = false;
    cpu.p = 0x00;           /* M=0,X=0 -> 16-bit A/X/Y */
    cpu.pc = 0x8000; cpu.pb = 0x00; cpu.db = 0x00;
    cpu.s = 0x01FF; cpu.d = 0x0000;
}

static void test_lda_imm16(void) {
    printf("test_lda_imm16\n");
    setup_native();
    /* LDA #$1234  -> A9 34 12 */
    mem[0x8000] = 0xA9; mem[0x8001] = 0x34; mem[0x8002] = 0x12;
    w_step(&cpu);
    CHECK(cpu.a == 0x1234, "A == 0x1234");
    CHECK((cpu.p & W_FLAG_N) == 0, "N clear");
    CHECK((cpu.p & W_FLAG_Z) == 0, "Z clear");
}

static void test_lda_imm8_sep(void) {
    printf("test_lda_imm8_sep\n");
    setup_native();
    cpu.p = W_FLAG_M;              /* M=1 -> 8-bit accumulator */
    /* LDA #$80  -> A9 80 */
    mem[0x8000] = 0xA9; mem[0x8001] = 0x80;
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0x80, "A low == 0x80");
    CHECK((cpu.p & W_FLAG_N) != 0, "N set on 0x80");
}

static void test_adc_carry(void) {
    printf("test_adc_carry\n");
    setup_native();
    cpu.p = W_FLAG_M;              /* 8-bit A */
    /* CLC; LDA #$FF; ADC #$01 -> 0x00, C=1, Z=1 */
    mem[0x8000] = 0x18;
    mem[0x8001] = 0xA9; mem[0x8002] = 0xFF;
    mem[0x8003] = 0x69; mem[0x8004] = 0x01;
    w_step(&cpu); w_step(&cpu); w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0x00, "A == 0x00");
    CHECK((cpu.p & W_FLAG_C) != 0, "C set");
    CHECK((cpu.p & W_FLAG_Z) != 0, "Z set");
}

static void test_sta_abs_lda(void) {
    printf("test_sta_abs_lda\n");
    setup_native();
    cpu.p = W_FLAG_M;   /* 8-bit A for simple STA/LDA */
    /* LDA #$5A ; STA $1234 ; LDA #$00 ; LDA $1234 */
    mem[0x8000] = 0xA9; mem[0x8001] = 0x5A;
    mem[0x8002] = 0x8D; mem[0x8003] = 0x34; mem[0x8004] = 0x12;
    mem[0x8005] = 0xA9; mem[0x8006] = 0x00;
    mem[0x8007] = 0xAD; mem[0x8008] = 0x34; mem[0x8009] = 0x12;
    w_step(&cpu); w_step(&cpu); w_step(&cpu); w_step(&cpu);
    CHECK(mem[0x1234] == 0x5A, "stored 0x5A at $1234");
    CHECK((cpu.a & 0xFF) == 0x5A, "A reloaded from $1234");
}

static void test_inx_dex(void) {
    printf("test_inx_dex\n");
    setup_native();
    cpu.p = W_FLAG_X;   /* 8-bit X */
    cpu.x = 0x00;
    mem[0x8000] = 0xE8; /* INX */
    w_step(&cpu);
    CHECK((cpu.x & 0xFF) == 0x01, "INX 0 -> 1");
    cpu.x = 0x00;
    mem[0x8001] = 0xCA; /* DEX */
    cpu.pc = 0x8001;
    w_step(&cpu);
    CHECK((cpu.x & 0xFF) == 0xFF, "DEX 0 -> 0xFF");
    CHECK((cpu.p & W_FLAG_N) != 0, "N set after DEX to 0xFF");
}

static void test_jsr_rts(void) {
    printf("test_jsr_rts\n");
    setup_native();
    cpu.p = W_FLAG_M;
    /* at $8000: JSR $8100 ; then $8003 is return point */
    mem[0x8000] = 0x20; mem[0x8001] = 0x00; mem[0x8002] = 0x81;
    /* at $8100: LDA #$77 ; RTS */
    mem[0x8100] = 0xA9; mem[0x8101] = 0x77; mem[0x8102] = 0x60;
    w_step(&cpu);            /* JSR */
    w_step(&cpu);            /* LDA */
    w_step(&cpu);            /* RTS */
    CHECK((cpu.a & 0xFF) == 0x77, "subroutine ran");
    CHECK(cpu.pc == 0x8003, "returned to 0x8003 (after JSR)");
}

static void test_branch_bne(void) {
    printf("test_branch_bne\n");
    setup_native();
    cpu.p = W_FLAG_M;
    /* LDA #$00 ; BNE +2 (not taken since Z=1) */
    mem[0x8000] = 0xA9; mem[0x8001] = 0x00;
    mem[0x8002] = 0xD0; mem[0x8003] = 0x02;
    w_step(&cpu); w_step(&cpu);
    CHECK(cpu.pc == 0x8004, "BNE not taken (Z=1)");
}

static void test_sep_rep_widths(void) {
    printf("test_sep_rep_widths\n");
    setup_native();
    cpu.p = 0x00;
    /* SEP #$20 -> set M (8-bit A) */
    mem[0x8000] = 0xE2; mem[0x8001] = 0x20;
    w_step(&cpu);
    CHECK((cpu.p & W_FLAG_M) != 0, "SEP #$20 sets M");
    /* REP #$20 -> clear M (16-bit A) */
    mem[0x8002] = 0xC2; mem[0x8003] = 0x20;
    cpu.pc = 0x8002;
    w_step(&cpu);
    CHECK((cpu.p & W_FLAG_M) == 0, "REP #$20 clears M");
}

static void test_push_pull(void) {
    printf("test_push_pull\n");
    setup_native();
    cpu.p = W_FLAG_M | W_FLAG_X;  /* 8-bit A/X */
    cpu.a = 0x00AB;
    /* PHA ; LDA #$00 ; PLA */
    mem[0x8000] = 0x48;
    mem[0x8001] = 0xA9; mem[0x8002] = 0x00;
    mem[0x8003] = 0x68;
    w_step(&cpu); w_step(&cpu); w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0xAB, "PHA/PLA preserved 0xAB");
}


static void test_mem_shift(void) {
    printf("test_mem_shift\n");
    setup_native();
    cpu.p = W_FLAG_M;   /* 8-bit */
    mem[0x1234] = 0x81;
    /* ASL $1234 -> 0x02, C=1 */
    mem[0x8000] = 0x0E; mem[0x8001] = 0x34; mem[0x8002] = 0x12;
    w_step(&cpu);
    CHECK(mem[0x1234] == 0x02, "ASL mem 0x81 -> 0x02");
    CHECK((cpu.p & W_FLAG_C) != 0, "ASL carry out set");
    /* ROR $1234 with C=1 -> 0x81, C=0 */
    mem[0x8003] = 0x6E; mem[0x8004] = 0x34; mem[0x8005] = 0x12;
    cpu.pc = 0x8003;
    w_step(&cpu);
    CHECK(mem[0x1234] == 0x81, "ROR mem 0x02 (C=1) -> 0x81");
    CHECK((cpu.p & W_FLAG_C) == 0, "ROR carry out clear");
}

static void test_tsb_trb(void) {
    printf("test_tsb_trb\n");
    setup_native();
    cpu.p = W_FLAG_M;
    cpu.a = 0x0F; mem[0x1234] = 0xF0;
    /* TSB $1234 -> mem = 0xFF, Z=1 (A & mem == 0 before) */
    mem[0x8000] = 0x0C; mem[0x8001] = 0x34; mem[0x8002] = 0x12;
    w_step(&cpu);
    CHECK(mem[0x1234] == 0xFF, "TSB set bits");
    CHECK((cpu.p & W_FLAG_Z) != 0, "TSB Z set (no overlap)");
    /* TRB $1234 -> mem = 0xF0 */
    cpu.pc = 0x8003;
    mem[0x8003] = 0x1C; mem[0x8004] = 0x34; mem[0x8005] = 0x12;
    w_step(&cpu);
    CHECK(mem[0x1234] == 0xF0, "TRB cleared A bits");
}

static void test_bit_imm(void) {
    printf("test_bit_imm\n");
    setup_native();
    cpu.p = W_FLAG_M;
    cpu.a = 0x80;
    /* BIT #$00 -> Z=1 (A & 0 == 0); N/V unaffected by imm */
    mem[0x8000] = 0x89; mem[0x8001] = 0x00;
    w_step(&cpu);
    CHECK((cpu.p & W_FLAG_Z) != 0, "BIT # Z set when no overlap");
}

static void test_lda_dp_ind(void) {
    printf("test_lda_dp_ind\n");
    setup_native();
    cpu.p = W_FLAG_M;
    cpu.d = 0x0000; cpu.db = 0x00;
    /* pointer at $20 = 0x1234 ; LDA ($20) loads [$1234] */
    mem[0x0020] = 0x34; mem[0x0021] = 0x12;
    mem[0x1234] = 0x99;
    mem[0x8000] = 0xB2; mem[0x8001] = 0x20;
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0x99, "LDA (dp) loaded through 16-bit pointer");
}

static void test_lda_long_ind(void) {
    printf("test_lda_long_ind\n");
    setup_native();
    cpu.p = W_FLAG_M;
    cpu.d = 0x0000;
    /* 24-bit pointer at $30 = 0x021234 ; LDA [$30] loads [$021234] */
    mem[0x0030] = 0x34; mem[0x0031] = 0x12; mem[0x0032] = 0x02;
    mem[0x021234] = 0x77;
    mem[0x8000] = 0xA7; mem[0x8001] = 0x30;
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0x77, "LDA [dp] loaded through 24-bit pointer");
}

static void test_shift_16bit(void) {
    printf("test_shift_16bit\n");
    setup_native();
    cpu.p = 0x00;   /* 16-bit M */
    mem[0x1234] = 0x01; mem[0x1235] = 0x80; /* 0x8001 */
    /* ASL $1234 -> 0x0002, C=1 */
    mem[0x8000] = 0x0E; mem[0x8001] = 0x34; mem[0x8002] = 0x12;
    w_step(&cpu);
    CHECK(mem[0x1234] == 0x02 && mem[0x1235] == 0x00, "ASL 16-bit 0x8001 -> 0x0002");
    CHECK((cpu.p & W_FLAG_C) != 0, "ASL 16-bit carry out");
}


static void test_nmi_irq_vectors(void) {
    printf("test_nmi_irq_vectors\n");
    /* emulation mode uses the $FFFx vectors */
    memset(mem, 0, sizeof mem);
    w_init(&cpu, t_read, t_write, NULL);
    cpu.emulation = true; cpu.pc = 0x8000; cpu.pb = 0x00; cpu.s = 0x01FF;
    mem[0x00FFFA]=0x00; mem[0x00FFFB]=0x90;   /* NMI -> $9000 */
    mem[0x00FFFE]=0x00; mem[0x00FFFF]=0xA0;   /* IRQ -> $A000 */
    w_nmi(&cpu);
    CHECK(cpu.pc == 0x9000, "emulation NMI uses the $FFFA vector");
    cpu.p = 0; cpu.pc = 0x8000; cpu.s = 0x01FF;
    w_irq(&cpu);
    CHECK(cpu.pc == 0xA000, "emulation IRQ uses the $FFFE vector");

    /* native mode uses the $FFEx vectors */
    memset(mem, 0, sizeof mem);
    w_init(&cpu, t_read, t_write, NULL);
    cpu.emulation = false; cpu.p = 0x00; cpu.pc = 0x8000; cpu.pb = 0x00; cpu.s = 0x01FF;
    mem[0x00FFEA]=0x00; mem[0x00FFEB]=0xB0;   /* NMI -> $B000 */
    mem[0x00FFEE]=0x00; mem[0x00FFEF]=0xC0;   /* IRQ -> $C000 */
    w_nmi(&cpu);
    CHECK(cpu.pc == 0xB000, "native NMI uses the $FFEA vector");
    cpu.p = 0; cpu.pc = 0x8000; cpu.s = 0x01FF;
    w_irq(&cpu);
    CHECK(cpu.pc == 0xC000, "native IRQ uses the $FFEE vector");

    /* IRQ is maskable */
    cpu.p = W_FLAG_I; cpu.pc = 0x8000;
    w_irq(&cpu);
    CHECK(cpu.pc == 0x8000, "IRQ suppressed while the I flag is set");
}

/* ---- operand byte count per opcode, for 8-bit A and X/Y ---------------- *
 * From the WDC 65816 opcode matrix (oxyron.de/opcodes816), one row of 16
 * digits per opcode page: op_len[op] = operand bytes that follow the opcode. */
static const char *const op_len[16] = {
    "1111111101002223",  /* $0x  BRK, ORA, COP, TSB, PHP, PHD */
    "1111111102002223",  /* $1x  BPL, TRB, INC, ASL */
    "2131111101002223",  /* $2x  JSR, AND, JSL, BIT, ROL, PLD */
    "1111111102002223",  /* $3x  BMI, BIT, DEC, ROL */
    "0111211101002223",  /* $4x  RTI, WDM, MVP, LSR, PHA, PHK, JMP */
    "1111211102003223",  /* $5x  BVC, MVN, LSR, CLI, PHY, TCD, JML */
    "0121111101002223",  /* $6x  RTS, PER, STZ, ROR, PLA, RTL, JMP */
    "1111111102002223",  /* $7x  BVS, STZ, ROR, SEI, PLY, TDC, JMP */
    "1121111101002223",  /* $8x  BRA, BRL, STY/STX, PHB, STZ */
    "1111111102002223",  /* $9x  BCC, STZ, STY/STX, TYA, TXS, STZ */
    "1111111101002223",  /* $Ax  LDY, LDX, PLB */
    "1111111102002223",  /* $Bx  BCS, LDY, LDX, TSX, TYX */
    "1111111101002223",  /* $Cx  CPY, REP, DEC, INY, WAI */
    "1111111102002223",  /* $Dx  BNE, PEI, DEC, PHX, STP, JMP */
    "1111111101002223",  /* $Ex  CPX, SEP, INC, XBA, NOP */
    "1111211102002223",  /* $Fx  BEQ, PEA, INC, SED, PLX, XCE, JSR */
};

/* These redirect PC to a computed or vector target, so their "length" is a
 * destination rather than an operand count; they are covered by the jump,
 * branch and interrupt tests instead. */
static bool redirects_pc(int op) {
    switch (op) {
        case 0x00: case 0x02: case 0x20: case 0x22: case 0x40:
        case 0x4C: case 0x5C: case 0x60: case 0x6B: case 0x6C:
        case 0x7C: case 0xDC: case 0xFC: return true;
        default: return false;
    }
}

/* Every opcode must exist and consume exactly its documented operand count.
 * An unhandled opcode falls through to the default NOP, which shows up here
 * as a wrong PC delta. */
static void test_all_256_decode(void) {
    printf("test_all_256_decode\n");
    int shape_ok = 1;
    for (int i = 0; i < 16; i++) if (strlen(op_len[i]) != 16) shape_ok = 0;
    CHECK(shape_ok, "the decode-length table covers all 256 opcodes");

    int bad = 0, first = -1;
    for (int op = 0; op < 256; op++) {
        if (redirects_pc(op)) continue;
        memset(mem, 0, sizeof mem);
        mem[0x00FFFC] = 0x00; mem[0x00FFFD] = 0x80;
        w_init(&cpu, t_read, t_write, NULL);
        cpu.emulation = true;                    /* forces 8-bit A and X/Y */
        cpu.p = W_FLAG_M | W_FLAG_X | W_FLAG_I;
        cpu.pc = 0x8000; cpu.pb = 0x00; cpu.db = 0x00;
        cpu.s = 0x01FF; cpu.d = 0x0000;
        cpu.stopped = false; cpu.waiting = false;
        mem[0x8000] = (uint8_t)op;               /* opcode, operands all zero */
        w_step(&cpu);
        /* delta = opcode byte + operand bytes */
        if ((int)cpu.pc - 0x8000 != 1 + (op_len[op >> 4][op & 15] - '0')) {
            bad++;
            if (first < 0) first = op;
            if (bad <= 6) printf("  dbg op=%02X pc=%04X delta=%d want=%d\n",
                                 op, cpu.pc, (int)cpu.pc - 0x8000, 1 + (op_len[op >> 4][op & 15] - '0'));
        }
    }
    if (bad) printf("  (first mismatch: opcode $%02X)\n", first);
    CHECK(bad == 0, "all opcodes decode with the documented operand count");
}

/* Stack-relative ($00,S) and ($00,S),Y for the ALU operations. */
static void test_alu_stack_relative(void) {
    printf("test_alu_stack_relative\n");
    /* ORA $01,S -> 03 01 : operand at S+1 */
    setup_native(); cpu.p = W_FLAG_M; cpu.s = 0x01FF; cpu.a = 0x000F;
    mem[0x8000] = 0x03; mem[0x8001] = 0x01;
    mem[0x0200] = 0xF0;
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0xFF, "ORA sr reads at S + offset");

    /* ORA ($01,S),Y -> 13 01 : pointer at S+1, indexed by Y */
    setup_native(); cpu.p = W_FLAG_M; cpu.s = 0x01FF; cpu.a = 0x000F; cpu.y = 0x0010;
    mem[0x8000] = 0x13; mem[0x8001] = 0x01;
    mem[0x0200] = 0x34; mem[0x0201] = 0x12;      /* pointer = $1234 */
    mem[0x001244] = 0xF0;                         /* $1234 + Y */
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0xFF, "ORA (sr,S),Y indexes the pointer with Y");
}

/* Long-indirect [dp] and [dp],Y for the ALU operations. */
static void test_alu_long_indirect(void) {
    printf("test_alu_long_indirect\n");
    /* ORA [$05] -> 07 05 : 24-bit pointer at $0005 */
    setup_native(); cpu.p = W_FLAG_M; cpu.a = 0x000F;
    mem[0x8000] = 0x07; mem[0x8001] = 0x05;
    mem[0x0005] = 0x34; mem[0x0006] = 0x12; mem[0x0007] = 0x00;
    mem[0x001234] = 0xF0;
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0xFF, "ORA [dp] follows a 24-bit pointer");

    /* ORA [$05],Y -> 17 05 */
    setup_native(); cpu.p = W_FLAG_M; cpu.a = 0x000F; cpu.y = 0x0010;
    mem[0x8000] = 0x17; mem[0x8001] = 0x05;
    mem[0x0005] = 0x34; mem[0x0006] = 0x12; mem[0x0007] = 0x00;
    mem[0x001244] = 0xF0;
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0xFF, "ORA [dp],Y adds Y to the 24-bit pointer");
}

/* (dp) must NOT apply Y - it is a distinct mode from (dp),Y. */
static void test_idp_vs_idy(void) {
    printf("test_idp_vs_idy\n");
    setup_native(); cpu.p = W_FLAG_M; cpu.a = 0x0000; cpu.y = 0x0100;
    mem[0x8000] = 0x72; mem[0x8001] = 0x10;            /* ADC ($10) */
    mem[0x0010] = 0x34; mem[0x0011] = 0x12;            /* pointer = $1234 */
    mem[0x001234] = 0x05;
    mem[0x001334] = 0x50;                              /* where Y would land */
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0x05, "ADC (dp) ignores Y");

    setup_native(); cpu.p = W_FLAG_M; cpu.a = 0x0000; cpu.y = 0x0100;
    mem[0x8000] = 0x71; mem[0x8001] = 0x10;            /* ADC ($10),Y */
    mem[0x0010] = 0x34; mem[0x0011] = 0x12;
    mem[0x001234] = 0x05;
    mem[0x001334] = 0x50;
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0x50, "ADC (dp),Y adds Y");

    /* abs,Y must fetch two operand bytes (it used to read one). */
    setup_native(); cpu.p = W_FLAG_M; cpu.a = 0x000F; cpu.y = 0x0000;
    mem[0x8000] = 0x19; mem[0x8001] = 0x34; mem[0x8002] = 0x12;  /* ORA $1234,Y */
    mem[0x001234] = 0xF0;
    w_step(&cpu);
    CHECK((cpu.a & 0xFF) == 0xFF, "ORA abs,Y fetches a 16-bit address");
}

/* PHK / PEA / PEI / PER */
static void test_push_variants(void) {
    printf("test_push_variants\n");
    /* PHK pushes PBR, and the program counter fetches through PBR - so the
     * instruction itself has to live in that bank. */
    setup_native(); cpu.pb = 0x7E; cpu.pc = 0x8000;
    mem[0x007E8000] = 0x4B;
    w_step(&cpu);
    CHECK(mem[0x01FF] == 0x7E && cpu.s == 0x01FE && cpu.pc == 0x8001,
          "PHK pushes the program bank");

    setup_native();
    mem[0x8000] = 0xF4; mem[0x8001] = 0x34; mem[0x8002] = 0x12;  /* PEA #$1234 */
    w_step(&cpu);
    CHECK(mem[0x01FF] == 0x12 && mem[0x01FE] == 0x34 && cpu.s == 0x01FD,
          "PEA pushes the literal operand, high byte first");

    setup_native(); cpu.d = 0x0100;
    mem[0x8000] = 0xD4; mem[0x8001] = 0x20;                       /* PEI ($20) */
    w_step(&cpu);
    CHECK(mem[0x01FF] == 0x01 && mem[0x01FE] == 0x20, "PEI pushes the (dp) address");

    setup_native();
    mem[0x8000] = 0x62; mem[0x8001] = 0x10; mem[0x8002] = 0x00;   /* PER $0010 */
    w_step(&cpu);
    CHECK(mem[0x01FF] == 0x80 && mem[0x01FE] == 0x13, "PER pushes PC-after-instruction + offset");
}

/* STZ indexed forms */
static void test_stz_variants(void) {
    printf("test_stz_variants\n");
    setup_native(); cpu.p = W_FLAG_M; cpu.x = 0x0004;
    mem[0x8000] = 0x74; mem[0x8001] = 0x10;                       /* STZ $10,X */
    mem[0x0014] = 0xAA;
    w_step(&cpu);
    CHECK(mem[0x0014] == 0x00, "STZ dp,X clears dp + X");

    setup_native(); cpu.p = W_FLAG_M; cpu.x = 0x0002;
    mem[0x8000] = 0x9E; mem[0x8001] = 0x34; mem[0x8002] = 0x12;   /* STZ $1234,X */
    mem[0x001236] = 0x55;
    w_step(&cpu);
    CHECK(mem[0x001236] == 0x00, "STZ abs,X clears abs + X");
}

/* The indexed and long indirect jumps */
static void test_indirect_jumps(void) {
    printf("test_indirect_jumps\n");
    setup_native(); cpu.x = 0x0004;
    mem[0x8000] = 0x7C; mem[0x8001] = 0x04; mem[0x8002] = 0x80;   /* JMP ($8004,X) */
    mem[0x008008] = 0x00; mem[0x008009] = 0x90;
    w_step(&cpu);
    CHECK(cpu.pc == 0x9000 && cpu.pb == 0x00, "JMP (abs,X) jumps through the indexed vector");

    setup_native();
    mem[0x8000] = 0xDC; mem[0x8001] = 0x10; mem[0x8002] = 0x80;   /* JML [$8010] */
    mem[0x008010] = 0x00; mem[0x008011] = 0x90; mem[0x008012] = 0x7E;
    w_step(&cpu);
    CHECK(cpu.pc == 0x9000 && cpu.pb == 0x7E, "JML [abs] loads PC and PB");
}

int main(void) {
    test_lda_imm16();
    test_lda_imm8_sep();
    test_adc_carry();
    test_sta_abs_lda();
    test_inx_dex();
    test_jsr_rts();
    test_branch_bne();
    test_sep_rep_widths();
    test_push_pull();
    test_mem_shift();
    test_tsb_trb();
    test_bit_imm();
    test_lda_dp_ind();
    test_lda_long_ind();
    test_shift_16bit();
    test_nmi_irq_vectors();
    test_all_256_decode();
    test_alu_stack_relative();
    test_alu_long_indirect();
    test_idp_vs_idy();
    test_push_variants();
    test_stz_variants();
    test_indirect_jumps();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

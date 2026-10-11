/* Headless unit tests for the SPC700 CPU core. */
#include <stdio.h>
#include <string.h>
#include "../core/spc700.h"

static uint8_t mem[0x10000];
static uint8_t  r_(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void     w_(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }

static spc_cpu cpu;
static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

static void setup(void) {
    memset(mem, 0, sizeof mem);
    spc_init(&cpu, r_, w_, NULL);
    cpu.pc = 0x0200; cpu.sp = 0xEF; cpu.psw = 0x02;
    cpu.a = cpu.x = cpu.y = 0;
}

static void test_load_alu(void) {
    printf("test_load_alu\n");
    setup();
    mem[0x0200]=0xE8; mem[0x0201]=0x42;   /* MOV A,#$42 */
    mem[0x0202]=0x88; mem[0x0203]=0x10;   /* ADC A,#$10 */
    spc_step(&cpu);
    CHECK(cpu.a == 0x42, "MOV A,#imm");
    spc_step(&cpu);
    CHECK(cpu.a == 0x52 && !(cpu.psw & SPC_FLAG_C), "ADC A,#imm");

    /* carry */
    cpu.a = 0xFF; cpu.psw |= SPC_FLAG_C;
    mem[0x0204]=0x88; mem[0x0205]=0x00;   /* ADC A,#0 with carry */
    cpu.pc = 0x0204;
    spc_step(&cpu);
    CHECK(cpu.a == 0x00 && (cpu.psw & SPC_FLAG_C) && (cpu.psw & SPC_FLAG_Z), "ADC carry -> 0, C, Z");

    /* SBC borrow */
    cpu.a = 0x10; cpu.psw |= SPC_FLAG_C;  /* no borrow */
    mem[0x0206]=0xA8; mem[0x0207]=0x20;   /* SBC A,#$20 */
    cpu.pc = 0x0206;
    spc_step(&cpu);
    CHECK(cpu.a == 0xF0 && !(cpu.psw & SPC_FLAG_C), "SBC borrow sets result/C");
}

static void test_direct_page(void) {
    printf("test_direct_page\n");
    setup();
    mem[0x0200]=0x20;                     /* CLRP */
    mem[0x0201]=0x8F; mem[0x0202]=0x55; mem[0x0203]=0x10;  /* MOV $10,#$55 (imm, dp) */
    mem[0x0204]=0xE4; mem[0x0205]=0x10;   /* MOV A,$10 */
    spc_step(&cpu); spc_step(&cpu);
    CHECK(mem[0x0010] == 0x55, "MOV dp,#imm writes page 0");
    spc_step(&cpu);
    CHECK(cpu.a == 0x55, "MOV A,dp reads page 0");

    /* with P set, direct page is page 1 */
    cpu.psw |= SPC_FLAG_P;
    mem[0x0206]=0xE4; mem[0x0207]=0x10;
    cpu.pc = 0x0206;
    mem[0x0110] = 0x77;
    spc_step(&cpu);
    CHECK(cpu.a == 0x77, "direct page follows the P flag");
}

static void test_stack_call(void) {
    printf("test_stack_call\n");
    setup();
    mem[0x0200]=0xE8; mem[0x0201]=0x11;   /* MOV A,#$11 */
    mem[0x0202]=0x2D;                     /* PUSH A */
    mem[0x0203]=0xE8; mem[0x0204]=0x00;   /* MOV A,#0 */
    mem[0x0205]=0xAE;                     /* POP A */
    spc_step(&cpu); spc_step(&cpu);
    CHECK(cpu.sp == 0xEE && mem[0x01EF] == 0x11, "PUSH A to stack");
    spc_step(&cpu); spc_step(&cpu);
    CHECK(cpu.a == 0x11 && cpu.sp == 0xEF, "POP A restores A and SP");

    /* CALL / RET */
    setup();
    mem[0x0200]=0x3F; mem[0x0201]=0x10; mem[0x0202]=0x02;   /* CALL $0210 */
    mem[0x0210]=0xE8; mem[0x0211]=0x77;                     /* MOV A,#$77 */
    mem[0x0212]=0x6F;                                       /* RET */
    spc_step(&cpu);
    CHECK(cpu.pc == 0x0210, "CALL jumps to target");
    spc_step(&cpu);
    CHECK(cpu.a == 0x77, "subroutine body runs");
    spc_step(&cpu);
    CHECK(cpu.pc == 0x0203 && cpu.sp == 0xEF, "RET returns after CALL");
}

static void test_branches(void) {
    printf("test_branches\n");
    setup();
    mem[0x0200]=0xE8; mem[0x0201]=0x05;   /* MOV A,#5 */
    mem[0x0202]=0x68; mem[0x0203]=0x05;   /* CMP A,#5 -> Z */
    mem[0x0204]=0xF0; mem[0x0205]=0x02;   /* BEQ +2 */
    mem[0x0206]=0xE8; mem[0x0207]=0xFF;   /* (skipped) */
    mem[0x0208]=0xE8; mem[0x0209]=0x42;
    spc_step(&cpu); spc_step(&cpu);
    CHECK(cpu.psw & SPC_FLAG_Z, "CMP sets Z on equal");
    spc_step(&cpu);                        /* BEQ */
    CHECK(cpu.pc == 0x0208, "BEQ branches when Z set");
    spc_step(&cpu);
    CHECK(cpu.a == 0x42, "branch target executed");
}

static void test_movw_arith(void) {
    printf("test_movw_arith\n");
    setup();
    mem[0x0010]=0x34; mem[0x0011]=0x12;
    mem[0x0200]=0x20;                     /* CLRP */
    mem[0x0201]=0xBA; mem[0x0202]=0x10;   /* MOVW YA,$10 */
    spc_step(&cpu); spc_step(&cpu);
    CHECK(cpu.a == 0x34 && cpu.y == 0x12, "MOVW YA,dp");

    cpu.a = 0x34; cpu.y = 0x12;           /* YA = $1234 */
    mem[0x0203]=0x7A; mem[0x0204]=0x10;   /* ADDW YA,$10 -> $2468 */
    spc_step(&cpu);
    CHECK(cpu.a == 0x68 && cpu.y == 0x24, "ADDW YA,dp");

    setup();
    mem[0x0200]=0x8D; mem[0x0201]=0x10;   /* MOV Y,#$10 */
    mem[0x0202]=0xE8; mem[0x0203]=0x20;   /* MOV A,#$20 */
    mem[0x0204]=0xCF;                     /* MUL YA */
    spc_step(&cpu); spc_step(&cpu); spc_step(&cpu);
    CHECK(cpu.a == 0x00 && cpu.y == 0x02, "MUL YA (0x10*0x20=0x0200)");

    setup();
    mem[0x0200]=0xCD; mem[0x0201]=0x10;   /* MOV X,#$10 */
    mem[0x0202]=0x8D; mem[0x0203]=0x01;   /* MOV Y,#$01 */
    mem[0x0204]=0xE8; mem[0x0205]=0x00;   /* MOV A,#$00 */
    mem[0x0206]=0x9E;                     /* DIV YA,X */
    spc_step(&cpu); spc_step(&cpu); spc_step(&cpu); spc_step(&cpu);
    CHECK(cpu.a == 0x10 && cpu.y == 0x00, "DIV YA,X (0x0100/0x10=0x10)");
}

static void test_incdec_shift(void) {
    printf("test_incdec_shift\n");
    setup();
    mem[0x0200]=0x1C;                     /* ASL A */
    cpu.a = 0x81;
    spc_step(&cpu);
    CHECK(cpu.a == 0x02 && (cpu.psw & SPC_FLAG_C), "ASL A sets carry");
    mem[0x0201]=0x5C;                     /* LSR A */
    spc_step(&cpu);
    CHECK(cpu.a == 0x01 && !(cpu.psw & SPC_FLAG_C), "LSR A");
    mem[0x0202]=0x9F;                     /* XCN A */
    cpu.a = 0x12;
    spc_step(&cpu);
    CHECK(cpu.a == 0x21, "XCN A nibble swap");
}

static void test_bits(void) {
    printf("test_bits\n");
    setup();
    mem[0x0200]=0x20;                     /* CLRP */
    mem[0x0201]=0xE2; mem[0x0202]=0x10;   /* SET1 $10.7 */
    spc_step(&cpu); spc_step(&cpu);
    CHECK(mem[0x0010] == 0x80, "SET1 d.7");
    mem[0x0203]=0xF2; mem[0x0204]=0x10;   /* CLR1 $10.7 */
    spc_step(&cpu);
    CHECK(mem[0x0010] == 0x00, "CLR1 d.7");
    /* MOV1 C, m.b : bit 3 of $0010 */
    mem[0x0010] = 0x08;
    mem[0x0205]=0xAA; mem[0x0206]=0x10; mem[0x0207]=0x60;  /* bit=3, addr=$0010 */
    spc_step(&cpu);
    CHECK(cpu.psw & SPC_FLAG_C, "MOV1 C,m.b reads the addressed bit");
}

static void test_daa(void) {
    printf("test_daa\n");
    setup();
    mem[0x0200]=0xDF;                     /* DAA */
    cpu.a = 0x1A; cpu.psw = 0x02;
    spc_step(&cpu);
    CHECK(cpu.a == 0x20, "DAA of $1A -> $20");
    cpu.a = 0x99; cpu.psw = 0x02;
    cpu.pc = 0x0200;
    spc_step(&cpu);
    CHECK(cpu.a == 0x99, "DAA leaves a valid BCD $99 unchanged");

    cpu.a = 0x9A; cpu.psw = 0x02;
    cpu.pc = 0x0200;
    spc_step(&cpu);
    CHECK(cpu.a == 0x00 && (cpu.psw & SPC_FLAG_C), "DAA of $9A -> $00 + carry");
}

static void test_tcall_pcall(void) {
    printf("test_tcall_pcall\n");
    setup();
    /* vector for TCALL 0 is at $FFDE */
    mem[0xFFDE]=0x00; mem[0xFFDF]=0x02;
    mem[0x0200]=0x01;                     /* TCALL 0 */
    spc_step(&cpu);
    CHECK(cpu.pc == 0x0200, "TCALL 0 vectors via $FFDE");
    /* PCALL */
    setup();
    mem[0x0200]=0x4F; mem[0x0201]=0x34;   /* PCALL $34 -> $FF34 */
    spc_step(&cpu);
    CHECK(cpu.pc == 0xFF34, "PCALL targets $FF00+u");
}

int main(void) {
    test_load_alu();
    test_direct_page();
    test_stack_call();
    test_branches();
    test_movw_arith();
    test_incdec_shift();
    test_bits();
    test_daa();
    test_tcall_pcall();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

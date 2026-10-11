/* Headless unit tests for the LR35902 CPU core. Build & run with gcc. */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../core/lr35902.h"

static uint8_t mem[0x10000];
static uint8_t t_read(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void    t_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }

static lr_cpu cpu;
static int failures = 0;

static void load(uint16_t addr, const uint8_t *code, size_t n) {
    memcpy(&mem[addr], code, n);
}
static void setup(void) {
    memset(mem, 0, sizeof mem);
    lr_init(&cpu, t_read, t_write, NULL);
    cpu.pc = 0x200;
    cpu.sp = 0xFFFE;
}
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } } while (0)

static void test_ld_and_add(void) {
    printf("test_ld_and_add\n");
    setup();
    /* LD A,0x3E ; LD B,0x21 ; ADD A,B ; HALT */
    const uint8_t code[] = { 0x3E, 0x3E, 0x06, 0x21, 0x80, 0x76 };
    load(0x200, code, sizeof code);
    for (int i = 0; i < 4; i++) lr_step(&cpu);
    CHECK(cpu.a == 0x5F, "A == 0x3E + 0x21 == 0x5F");
    CHECK((cpu.f & LR_FLAG_Z) == 0, "Z clear");
    CHECK((cpu.f & LR_FLAG_N) == 0, "N clear");
    CHECK((cpu.f & LR_FLAG_C) == 0, "C clear");
}

static void test_carry_halfcarry(void) {
    printf("test_carry_halfcarry\n");
    setup();
    /* LD A,0x0F ; LD B,0x01 ; ADD A,B  -> 0x10, H set */
    const uint8_t code[] = { 0x3E, 0x0F, 0x06, 0x01, 0x80 };
    load(0x200, code, sizeof code);
    for (int i = 0; i < 3; i++) lr_step(&cpu);
    CHECK(cpu.a == 0x10, "A == 0x10");
    CHECK((cpu.f & LR_FLAG_H) != 0, "H set");

    setup();
    /* LD A,0xFF ; LD B,0x01 ; ADD A,B -> 0x00, C and Z set */
    const uint8_t code2[] = { 0x3E, 0xFF, 0x06, 0x01, 0x80 };
    load(0x200, code2, sizeof code2);
    for (int i = 0; i < 3; i++) lr_step(&cpu);
    CHECK(cpu.a == 0x00, "A wraps to 0");
    CHECK((cpu.f & LR_FLAG_C) != 0, "C set");
    CHECK((cpu.f & LR_FLAG_Z) != 0, "Z set");
}

static void test_sub_cp(void) {
    printf("test_sub_cp\n");
    setup();
    /* LD A,0x10 ; LD B,0x10 ; CP B  (should set Z, N) */
    const uint8_t code[] = { 0x3E, 0x10, 0x06, 0x10, 0xB8 };
    load(0x200, code, sizeof code);
    for (int i = 0; i < 3; i++) lr_step(&cpu);
    CHECK(cpu.a == 0x10, "CP does not store (A unchanged)");
    CHECK((cpu.f & LR_FLAG_Z) != 0, "Z set on equal");
    CHECK((cpu.f & LR_FLAG_N) != 0, "N set for subtract");
}

static void test_ld_hl_mem(void) {
    printf("test_ld_hl_mem\n");
    setup();
    /* LD HL,0x300 ; LD (HL),0xAB ; LD A,(HL) */
    const uint8_t code[] = { 0x21, 0x00, 0x03, 0x36, 0xAB, 0x7E };
    load(0x200, code, sizeof code);
    for (int i = 0; i < 4; i++) lr_step(&cpu);
    CHECK(mem[0x300] == 0xAB, "memory at HL == 0xAB");
    CHECK(cpu.a == 0xAB, "A loaded from (HL)");
}

static void test_call_ret(void) {
    printf("test_call_ret\n");
    setup();
    /* at 0x200: CALL 0x300 ; HALT */
    const uint8_t code[] = { 0xCD, 0x00, 0x03, 0x76 };
    load(0x200, code, sizeof code);
    /* at 0x300: LD A,0x77 ; RET */
    const uint8_t sub[] = { 0x3E, 0x77, 0xC9 };
    load(0x300, sub, sizeof sub);
    lr_step(&cpu);            /* CALL */
    lr_step(&cpu);            /* LD A */
    lr_step(&cpu);            /* RET */
    CHECK(cpu.a == 0x77, "subroutine ran");
    CHECK(cpu.pc == 0x203, "returned to instruction after CALL");
}

static void test_push_pop(void) {
    printf("test_push_pop\n");
    setup();
    /* LD BC,0x1234 ; PUSH BC ; POP DE */
    const uint8_t code[] = { 0x01, 0x34, 0x12, 0xC5, 0xD1 };
    load(0x200, code, sizeof code);
    for (int i = 0; i < 3; i++) lr_step(&cpu);
    CHECK(cpu.de == 0x1234, "DE == pushed BC == 0x1234");
    CHECK(cpu.sp == 0xFFFE, "SP restored after push/pop");
}

static void test_cb_bit_res_set(void) {
    printf("test_cb_bit_res_set\n");
    setup();
    /* LD A,0x00 ; SET 3,A ; BIT 3,A ; RES 3,A ; BIT 3,A */
    const uint8_t code[] = { 0x3E, 0x00, 0xCB, 0xDF, 0xCB, 0x5F, 0xCB, 0x9F, 0xCB, 0x5F };
    load(0x200, code, sizeof code);
    lr_step(&cpu); lr_step(&cpu);        /* LD, SET */
    CHECK(cpu.a == 0x08, "SET 3 -> bit3 set");
    lr_step(&cpu);                        /* BIT 3 (Z=0 means bit set) */
    CHECK((cpu.f & LR_FLAG_Z) == 0, "BIT 3 sees set bit (Z clear)");
    lr_step(&cpu); lr_step(&cpu);        /* RES, BIT */
    CHECK((cpu.f & LR_FLAG_Z) != 0, "after RES 3, BIT reports clear (Z set)");
}

static void test_inc_dec_flags(void) {
    printf("test_inc_dec_flags\n");
    setup();
    /* LD A,0x7F ; INC A -> 0x80, H set */
    const uint8_t code[] = { 0x3E, 0x7F, 0x3C };
    load(0x200, code, sizeof code);
    for (int i = 0; i < 2; i++) lr_step(&cpu);
    CHECK(cpu.a == 0x80, "INC A 0x7F -> 0x80");
    CHECK((cpu.f & LR_FLAG_H) != 0, "H set on 0x7F->0x80 inc");
}

int main(void) {
    test_ld_and_add();
    test_carry_halfcarry();
    test_sub_cp();
    test_ld_hl_mem();
    test_call_ret();
    test_push_pop();
    test_cb_bit_res_set();
    test_inc_dec_flags();
    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

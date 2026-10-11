/* spc700.c - SPC700 CPU core implementation. See spc700.h.
 * Instruction set per Anomie's SPC700 documentation.
 */
#include "spc700.h"
#include <stdlib.h>
#include <stdio.h>

static uint8_t rd(spc_cpu *c, uint16_t a) { return c->read(c->ctx, a); }
static void    wr(spc_cpu *c, uint16_t a, uint8_t v) { c->write(c->ctx, a, v); }
static uint8_t f8(spc_cpu *c)  { return rd(c, c->pc++); }
static uint16_t f16(spc_cpu *c) { uint8_t lo = f8(c), hi = f8(c); return (uint16_t)(lo | (hi << 8)); }

static void setf(spc_cpu *c, uint8_t m, bool v) { if (v) c->psw |= m; else c->psw = (uint8_t)(c->psw & ~m); }
static void set_nz(spc_cpu *c, uint8_t v) { setf(c, SPC_FLAG_N, (v & 0x80) != 0); setf(c, SPC_FLAG_Z, v == 0); }

/* Direct-page base follows the P flag; the stack is always page 1. */
static uint16_t dpbase(spc_cpu *c) { return (c->psw & SPC_FLAG_P) ? 0x0100 : 0x0000; }
static uint16_t dp (spc_cpu *c, uint8_t d) { return (uint16_t)(dpbase(c) | d); }
static uint16_t dpx(spc_cpu *c, uint8_t d) { return (uint16_t)(dpbase(c) | ((d + c->x) & 0xFF)); }
static uint16_t dpy(spc_cpu *c, uint8_t d) { return (uint16_t)(dpbase(c) | ((d + c->y) & 0xFF)); }
static uint16_t inx(spc_cpu *c) { return (uint16_t)(dpbase(c) | c->x); }
static uint16_t iny(spc_cpu *c) { return (uint16_t)(dpbase(c) | c->y); }
static uint16_t membit(spc_cpu *c, int *bit) { uint16_t raw = f16(c); *bit = (raw >> 13) & 7; return (uint16_t)(raw & 0x1FFF); }

static void push(spc_cpu *c, uint8_t v) { wr(c, (uint16_t)(0x0100 | c->sp), v); c->sp--; }
static uint8_t pop(spc_cpu *c) { c->sp++; return rd(c, (uint16_t)(0x0100 | c->sp)); }

/* ---- ALU ---- */
static void alu_or (spc_cpu *c, uint8_t v) { c->a |= v; set_nz(c, c->a); }
static void alu_and(spc_cpu *c, uint8_t v) { c->a &= v; set_nz(c, c->a); }
static void alu_eor(spc_cpu *c, uint8_t v) { c->a ^= v; set_nz(c, c->a); }
static void alu_cmp(spc_cpu *c, uint8_t v) { int r = c->a - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); }
static void alu_adc(spc_cpu *c, uint8_t v) {
    int ci = (c->psw & SPC_FLAG_C) ? 1 : 0;
    unsigned r = c->a + v + ci;
    setf(c, SPC_FLAG_H, ((c->a & 0xF) + (v & 0xF) + ci) > 0xF);
    setf(c, SPC_FLAG_V, (~(c->a ^ v) & (c->a ^ r) & 0x80) != 0);
    setf(c, SPC_FLAG_C, r > 0xFF);
    c->a = (uint8_t)r;
    set_nz(c, c->a);
}
static void alu_sbc(spc_cpu *c, uint8_t v) {
    int ci = (c->psw & SPC_FLAG_C) ? 0 : 1;   /* borrow = !C */
    int r = c->a - v - ci;
    setf(c, SPC_FLAG_H, ((c->a & 0xF) - (v & 0xF) - ci) >= 0);
    setf(c, SPC_FLAG_V, ((c->a ^ v) & (c->a ^ r) & 0x80) != 0);
    setf(c, SPC_FLAG_C, r >= 0);
    c->a = (uint8_t)r;
    set_nz(c, c->a);
}
/* shift helpers return the new value and set C/N/Z */
static uint8_t sh_asl(spc_cpu *c, uint8_t v) { setf(c, SPC_FLAG_C, (v & 0x80) != 0); v = (uint8_t)(v << 1); set_nz(c, v); return v; }
static uint8_t sh_lsr(spc_cpu *c, uint8_t v) { setf(c, SPC_FLAG_C, (v & 1) != 0); v >>= 1; set_nz(c, v); return v; }
static uint8_t sh_rol(spc_cpu *c, uint8_t v) { int cy = (c->psw & SPC_FLAG_C) ? 1 : 0; setf(c, SPC_FLAG_C, (v & 0x80) != 0); v = (uint8_t)((v << 1) | cy); set_nz(c, v); return v; }
static uint8_t sh_ror(spc_cpu *c, uint8_t v) { int cy = (c->psw & SPC_FLAG_C) ? 1 : 0; setf(c, SPC_FLAG_C, (v & 1) != 0); v = (uint8_t)((v >> 1) | (cy << 7)); set_nz(c, v); return v; }
static uint8_t op_inc(spc_cpu *c, uint8_t v) { v++; set_nz(c, v); return v; }
static uint8_t op_dec(spc_cpu *c, uint8_t v) { v--; set_nz(c, v); return v; }

void spc_init(spc_cpu *c, spc_read_fn read, spc_write_fn write, void *ctx) {
    c->read = read; c->write = write; c->ctx = ctx;
    spc_reset(c);
}
void spc_reset(spc_cpu *c) {
    c->pc = (uint16_t)(rd(c, 0xFFFE) | (rd(c, 0xFFFF) << 8));
    c->sp = 0xEF;
    c->psw = 0x02;
    c->a = c->x = c->y = 0;
    c->stopped = false;
}

int spc_step(spc_cpu *c) {
    if (c->stopped) return 2;
    uint8_t op = f8(c);
    switch (op) {
    /* ---- 0x00 row ---- */
    case 0x00: return 2;                                    /* NOP */
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41:
    case 0x51: case 0x61: case 0x71: case 0x81: case 0x91:
    case 0xA1: case 0xB1: case 0xC1: case 0xD1: case 0xE1: case 0xF1: { /* TCALL n */
        int n = (op >> 4) & 0xF;
        uint16_t vec = (uint16_t)(0xFFDE - n * 2);
        uint16_t target = (uint16_t)(rd(c, vec) | (rd(c, (uint16_t)(vec + 1)) << 8));
        push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)(c->pc & 0xFF));
        c->pc = target; return 8;
    }
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82:
    case 0xA2: case 0xC2: case 0xE2: {                       /* SET1 d.b */
        uint8_t d = f8(c); int b = (op >> 5) & 7;
        uint16_t a = dp(c, d); wr(c, a, (uint8_t)(rd(c, a) | (1 << b))); return 4;
    }
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92:
    case 0xB2: case 0xD2: case 0xF2: {                       /* CLR1 d.b */
        uint8_t d = f8(c); int b = (op >> 5) & 7;
        uint16_t a = dp(c, d); wr(c, a, (uint8_t)(rd(c, a) & ~(1 << b))); return 4;
    }
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83:
    case 0xA3: case 0xC3: case 0xE3: {                       /* BBS d.b, r */
        uint8_t d = f8(c); int8_t r = (int8_t)f8(c); int b = (op >> 5) & 7;
        if (rd(c, dp(c, d)) & (1 << b)) c->pc = (uint16_t)(c->pc + r);
        return 5;
    }
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93:
    case 0xB3: case 0xD3: case 0xF3: {                       /* BBC d.b, r */
        uint8_t d = f8(c); int8_t r = (int8_t)f8(c); int b = (op >> 5) & 7;
        if (!(rd(c, dp(c, d)) & (1 << b))) c->pc = (uint16_t)(c->pc + r);
        return 5;
    }
    case 0x08: { uint8_t v = f8(c); alu_or(c, v); return 2; }
    case 0x04: { uint8_t d = f8(c); alu_or(c, rd(c, dp(c, d))); return 3; }
    case 0x14: { uint8_t d = f8(c); alu_or(c, rd(c, dpx(c, d))); return 4; }
    case 0x05: { uint16_t a = f16(c); alu_or(c, rd(c, a)); return 4; }
    case 0x15: { uint16_t a = (uint16_t)(f16(c) + c->x); alu_or(c, rd(c, a)); return 5; }
    case 0x16: { uint16_t a = (uint16_t)(f16(c) + c->y); alu_or(c, rd(c, a)); return 5; }
    case 0x06: { alu_or(c, rd(c, inx(c))); return 3; }
    case 0x07: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dpx(c, d)) | (rd(c, (uint16_t)(dpx(c, d) + 1)) << 8)); alu_or(c, rd(c, p)); return 6; }
    case 0x17: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dp(c, d)) | (rd(c, (uint16_t)(dp(c, d) + 1)) << 8)); alu_or(c, rd(c, (uint16_t)(p + c->y))); return 6; }
    case 0x09: { uint8_t s = f8(c), d = f8(c); uint16_t a = dp(c, d); wr(c, a, (uint8_t)(rd(c, a) | rd(c, dp(c, s)))); set_nz(c, rd(c, a)); return 6; }
    case 0x18: { uint8_t i = f8(c), d = f8(c); uint16_t a = dp(c, d); uint8_t v = (uint8_t)(rd(c, a) | i); wr(c, a, v); set_nz(c, v); return 5; }
    case 0x19: { uint8_t v = (uint8_t)(rd(c, inx(c)) | rd(c, iny(c))); wr(c, inx(c), v); set_nz(c, v); return 5; }
    case 0x0A: { int b; uint16_t a = membit(c, &b); setf(c, SPC_FLAG_C, ((c->psw & SPC_FLAG_C) != 0) && ((rd(c, a) >> b) & 1)); return 4; }
    case 0x2A: { int b; uint16_t a = membit(c, &b); setf(c, SPC_FLAG_C, ((c->psw & SPC_FLAG_C) != 0) && !((rd(c, a) >> b) & 1)); return 5; }
    case 0x0B: { uint8_t d = f8(c); uint16_t a = dp(c, d); wr(c, a, sh_asl(c, rd(c, a))); return 4; }
    case 0x1B: { uint8_t d = f8(c); uint16_t a = dpx(c, d); wr(c, a, sh_asl(c, rd(c, a))); return 5; }
    case 0x0C: { uint16_t a = f16(c); wr(c, a, sh_asl(c, rd(c, a))); return 5; }
    case 0x0D: push(c, c->psw); return 4;                    /* PUSH PSW */
    case 0x0E: { uint16_t a = f16(c); uint8_t v = rd(c, a); int r = c->a - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); wr(c, a, (uint8_t)(v | c->a)); return 6; } /* TSET1 */
    case 0x0F: {                                             /* BRK */
        push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)(c->pc & 0xFF)); push(c, c->psw);
        setf(c, SPC_FLAG_B, true); setf(c, SPC_FLAG_I, false);
        c->pc = (uint16_t)(rd(c, 0xFFDE) | (rd(c, 0xFFDF) << 8)); return 8;
    }
    case 0x10: { int8_t r = (int8_t)f8(c); if (!(c->psw & SPC_FLAG_N)) c->pc = (uint16_t)(c->pc + r); return 2; } /* BPL */
    case 0x30: { int8_t r = (int8_t)f8(c); if (c->psw & SPC_FLAG_N) c->pc = (uint16_t)(c->pc + r); return 2; }   /* BMI */
    case 0x50: { int8_t r = (int8_t)f8(c); if (!(c->psw & SPC_FLAG_V)) c->pc = (uint16_t)(c->pc + r); return 2; } /* BVC */
    case 0x70: { int8_t r = (int8_t)f8(c); if (c->psw & SPC_FLAG_V) c->pc = (uint16_t)(c->pc + r); return 2; }   /* BVS */
    case 0x90: { int8_t r = (int8_t)f8(c); if (!(c->psw & SPC_FLAG_C)) c->pc = (uint16_t)(c->pc + r); return 2; } /* BCC */
    case 0xB0: { int8_t r = (int8_t)f8(c); if (c->psw & SPC_FLAG_C) c->pc = (uint16_t)(c->pc + r); return 2; }   /* BCS */
    case 0xD0: { int8_t r = (int8_t)f8(c); if (!(c->psw & SPC_FLAG_Z)) c->pc = (uint16_t)(c->pc + r); return 2; } /* BNE */
    case 0xF0: { int8_t r = (int8_t)f8(c); if (c->psw & SPC_FLAG_Z) c->pc = (uint16_t)(c->pc + r); return 2; }   /* BEQ */
    case 0x2F: { int8_t r = (int8_t)f8(c); c->pc = (uint16_t)(c->pc + r); return 4; }                           /* BRA */
    case 0x1A: { uint8_t d = f8(c); uint16_t a = dp(c, d); uint16_t v = (uint16_t)((rd(c, a) | (rd(c, (uint16_t)(a + 1)) << 8)) - 1); wr(c, a, (uint8_t)v); wr(c, (uint16_t)(a + 1), (uint8_t)(v >> 8)); set_nz(c, (uint8_t)(v >> 8)); return 6; } /* DECW */
    case 0x1C: c->a = sh_asl(c, c->a); return 2;
    case 0x1D: c->x = op_dec(c, c->x); return 2;
    case 0x1E: { uint16_t a = f16(c); alu_cmp(c, rd(c, a)); return 4; }        /* CMP X,!a -- uses A? no: X-(a) */
    case 0x1F: { uint16_t a = (uint16_t)(f16(c) + c->x); c->pc = (uint16_t)(rd(c, a) | (rd(c, (uint16_t)(a + 1)) << 8)); return 6; } /* JMP [!a+X] */

    /* ---- 0x20 row ---- */
    case 0x20: setf(c, SPC_FLAG_P, false); return 2;          /* CLRP */
    case 0x40: setf(c, SPC_FLAG_P, true); return 2;           /* SETP */
    case 0x2B: { uint8_t d = f8(c); uint16_t a = dp(c, d); wr(c, a, sh_rol(c, rd(c, a))); return 4; }
    case 0x3B: { uint8_t d = f8(c); uint16_t a = dpx(c, d); wr(c, a, sh_rol(c, rd(c, a))); return 5; }
    case 0x2C: { uint16_t a = f16(c); wr(c, a, sh_rol(c, rd(c, a))); return 5; }
    case 0x2D: push(c, c->a); return 4;                      /* PUSH A */
    case 0x2E: { uint8_t d = f8(c); int8_t r = (int8_t)f8(c); if (c->a != rd(c, dp(c, d))) c->pc = (uint16_t)(c->pc + r); return 5; }
    case 0x3A: { uint8_t d = f8(c); uint16_t a = dp(c, d); uint16_t v = (uint16_t)((rd(c, a) | (rd(c, (uint16_t)(a + 1)) << 8)) + 1); wr(c, a, (uint8_t)v); wr(c, (uint16_t)(a + 1), (uint8_t)(v >> 8)); set_nz(c, (uint8_t)(v >> 8)); return 6; } /* INCW */
    case 0x3C: c->a = sh_rol(c, c->a); return 2;
    case 0x3D: c->x = op_inc(c, c->x); return 2;
    case 0x3E: { uint8_t d = f8(c); uint8_t v = rd(c, dp(c, d)); int r = c->x - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); return 3; }
    case 0x3F: { uint16_t a = f16(c); push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)(c->pc & 0xFF)); c->pc = a; return 8; } /* CALL */
    case 0x28: { uint8_t v = f8(c); alu_and(c, v); return 2; }
    case 0x24: { uint8_t d = f8(c); alu_and(c, rd(c, dp(c, d))); return 3; }
    case 0x34: { uint8_t d = f8(c); alu_and(c, rd(c, dpx(c, d))); return 4; }
    case 0x25: { uint16_t a = f16(c); alu_and(c, rd(c, a)); return 4; }
    case 0x35: { uint16_t a = (uint16_t)(f16(c) + c->x); alu_and(c, rd(c, a)); return 5; }
    case 0x36: { uint16_t a = (uint16_t)(f16(c) + c->y); alu_and(c, rd(c, a)); return 5; }
    case 0x26: { alu_and(c, rd(c, inx(c))); return 3; }
    case 0x27: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dpx(c, d)) | (rd(c, (uint16_t)(dpx(c, d) + 1)) << 8)); alu_and(c, rd(c, p)); return 6; }
    case 0x37: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dp(c, d)) | (rd(c, (uint16_t)(dp(c, d) + 1)) << 8)); alu_and(c, rd(c, (uint16_t)(p + c->y))); return 6; }
    case 0x29: { uint8_t s = f8(c), d = f8(c); uint16_t a = dp(c, d); uint8_t v = (uint8_t)(rd(c, a) & rd(c, dp(c, s))); wr(c, a, v); set_nz(c, v); return 6; }
    case 0x38: { uint8_t i = f8(c), d = f8(c); uint16_t a = dp(c, d); uint8_t v = (uint8_t)(rd(c, a) & i); wr(c, a, v); set_nz(c, v); return 5; }
    case 0x39: { uint8_t v = (uint8_t)(rd(c, inx(c)) & rd(c, iny(c))); wr(c, inx(c), v); set_nz(c, v); return 5; }
    case 0x4A: { int b; uint16_t a = membit(c, &b); setf(c, SPC_FLAG_C, ((c->psw & SPC_FLAG_C) != 0) && ((rd(c, a) >> b) & 1)); return 4; }
    case 0x6A: { int b; uint16_t a = membit(c, &b); setf(c, SPC_FLAG_C, ((c->psw & SPC_FLAG_C) != 0) && !((rd(c, a) >> b) & 1)); return 4; }

    /* ---- 0x40 row ---- */
    case 0x4B: { uint8_t d = f8(c); uint16_t a = dp(c, d); wr(c, a, sh_lsr(c, rd(c, a))); return 4; }
    case 0x5B: { uint8_t d = f8(c); uint16_t a = dpx(c, d); wr(c, a, sh_lsr(c, rd(c, a))); return 5; }
    case 0x4C: { uint16_t a = f16(c); wr(c, a, sh_lsr(c, rd(c, a))); return 5; }
    case 0x4D: push(c, c->x); return 4;                      /* PUSH X */
    case 0x4E: { uint16_t a = f16(c); uint8_t v = rd(c, a); int r = c->a - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); wr(c, a, (uint8_t)(v & ~c->a)); return 6; } /* TCLR1 */
    case 0x4F: { uint8_t u = f8(c); uint16_t a = (uint16_t)(0xFF00 + u); push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)(c->pc & 0xFF)); c->pc = a; return 6; } /* PCALL */
    case 0x5A: { uint8_t d = f8(c); uint16_t a = dp(c, d); uint16_t ya = (uint16_t)((c->y << 8) | c->a); uint16_t v = (uint16_t)(rd(c, a) | (rd(c, (uint16_t)(a + 1)) << 8)); int r = ya - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)(r >> 8)); return 4; } /* CMPW */
    case 0x5C: c->a = sh_lsr(c, c->a); return 2;
    case 0x5D: c->x = c->a; set_nz(c, c->x); return 2;
    case 0x5E: { uint16_t a = f16(c); uint8_t v = rd(c, a); int r = c->y - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); return 4; }
    case 0x5F: c->pc = f16(c); return 3;                     /* JMP !a */
    case 0x48: { uint8_t v = f8(c); alu_eor(c, v); return 2; }
    case 0x44: { uint8_t d = f8(c); alu_eor(c, rd(c, dp(c, d))); return 3; }
    case 0x54: { uint8_t d = f8(c); alu_eor(c, rd(c, dpx(c, d))); return 4; }
    case 0x45: { uint16_t a = f16(c); alu_eor(c, rd(c, a)); return 4; }
    case 0x55: { uint16_t a = (uint16_t)(f16(c) + c->x); alu_eor(c, rd(c, a)); return 5; }
    case 0x56: { uint16_t a = (uint16_t)(f16(c) + c->y); alu_eor(c, rd(c, a)); return 5; }
    case 0x46: { alu_eor(c, rd(c, inx(c))); return 3; }
    case 0x47: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dpx(c, d)) | (rd(c, (uint16_t)(dpx(c, d) + 1)) << 8)); alu_eor(c, rd(c, p)); return 6; }
    case 0x57: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dp(c, d)) | (rd(c, (uint16_t)(dp(c, d) + 1)) << 8)); alu_eor(c, rd(c, (uint16_t)(p + c->y))); return 6; }
    case 0x49: { uint8_t s = f8(c), d = f8(c); uint16_t a = dp(c, d); uint8_t v = (uint8_t)(rd(c, a) ^ rd(c, dp(c, s))); wr(c, a, v); set_nz(c, v); return 6; }
    case 0x58: { uint8_t i = f8(c), d = f8(c); uint16_t a = dp(c, d); uint8_t v = (uint8_t)(rd(c, a) ^ i); wr(c, a, v); set_nz(c, v); return 5; }
    case 0x59: { uint8_t v = (uint8_t)(rd(c, inx(c)) ^ rd(c, iny(c))); wr(c, inx(c), v); set_nz(c, v); return 5; }

    /* ---- 0x60 row ---- */
    case 0x60: setf(c, SPC_FLAG_C, false); return 2;          /* CLRC */
    case 0x80: setf(c, SPC_FLAG_C, true); return 2;           /* SETC */
    case 0x6B: { uint8_t d = f8(c); uint16_t a = dp(c, d); wr(c, a, sh_ror(c, rd(c, a))); return 4; }
    case 0x7B: { uint8_t d = f8(c); uint16_t a = dpx(c, d); wr(c, a, sh_ror(c, rd(c, a))); return 5; }
    case 0x6C: { uint16_t a = f16(c); wr(c, a, sh_ror(c, rd(c, a))); return 5; }
    case 0x6D: push(c, c->y); return 4;                      /* PUSH Y */
    case 0x6E: { uint8_t d = f8(c); int8_t r = (int8_t)f8(c); uint16_t a = dp(c, d); uint8_t v = op_dec(c, rd(c, a)); wr(c, a, v); if (v != 0) c->pc = (uint16_t)(c->pc + r); return 5; }
    case 0x6F: { uint8_t lo = pop(c), hi = pop(c); c->pc = (uint16_t)(lo | (hi << 8)); return 5; }  /* RET */
    case 0x7A: { uint8_t d = f8(c); uint16_t a = dp(c, d); uint16_t ya = (uint16_t)((c->y << 8) | c->a); uint16_t v = (uint16_t)(rd(c, a) | (rd(c, (uint16_t)(a + 1)) << 8)); unsigned r = ya + v; setf(c, SPC_FLAG_H, ((ya & 0xFFF) + (v & 0xFFF)) > 0xFFF); setf(c, SPC_FLAG_C, r > 0xFFFF); c->a = (uint8_t)r; c->y = (uint8_t)(r >> 8); set_nz(c, c->y); return 5; } /* ADDW */
    case 0x7C: c->a = sh_ror(c, c->a); return 2;
    case 0x7D: c->a = c->x; set_nz(c, c->a); return 2;
    case 0x7E: { uint8_t d = f8(c); uint8_t v = rd(c, dp(c, d)); int r = c->y - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); return 3; }
    case 0x7F: { c->psw = pop(c); uint8_t lo = pop(c), hi = pop(c); c->pc = (uint16_t)(lo | (hi << 8)); return 6; } /* RET1 */
    case 0x68: { uint8_t v = f8(c); alu_cmp(c, v); return 2; }
    case 0x64: { uint8_t d = f8(c); alu_cmp(c, rd(c, dp(c, d))); return 3; }
    case 0x74: { uint8_t d = f8(c); alu_cmp(c, rd(c, dpx(c, d))); return 4; }
    case 0x65: { uint16_t a = f16(c); alu_cmp(c, rd(c, a)); return 4; }
    case 0x75: { uint16_t a = (uint16_t)(f16(c) + c->x); alu_cmp(c, rd(c, a)); return 5; }
    case 0x76: { uint16_t a = (uint16_t)(f16(c) + c->y); alu_cmp(c, rd(c, a)); return 5; }
    case 0x66: { alu_cmp(c, rd(c, inx(c))); return 3; }
    case 0x67: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dpx(c, d)) | (rd(c, (uint16_t)(dpx(c, d) + 1)) << 8)); alu_cmp(c, rd(c, p)); return 6; }
    case 0x77: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dp(c, d)) | (rd(c, (uint16_t)(dp(c, d) + 1)) << 8)); alu_cmp(c, rd(c, (uint16_t)(p + c->y))); return 6; }
    case 0x69: { uint8_t s = f8(c), d = f8(c); uint8_t a = rd(c, dp(c, d)), b = rd(c, dp(c, s)); int r = a - b; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); return 6; }
    case 0x78: { uint8_t i = f8(c), d = f8(c); int r = rd(c, dp(c, d)) - i; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); return 5; }
    case 0x79: { uint8_t a = rd(c, inx(c)), b = rd(c, iny(c)); int r = a - b; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); return 5; }
    case 0x8A: { int b; uint16_t a = membit(c, &b); setf(c, SPC_FLAG_C, ((c->psw & SPC_FLAG_C) != 0) ^ ((rd(c, a) >> b) & 1)); return 5; }

    /* ---- 0x80 row ---- */
    case 0x8B: { uint8_t d = f8(c); uint16_t a = dp(c, d); wr(c, a, op_dec(c, rd(c, a))); return 4; }
    case 0x9B: { uint8_t d = f8(c); uint16_t a = dpx(c, d); wr(c, a, op_dec(c, rd(c, a))); return 5; }
    case 0x8C: { uint16_t a = f16(c); wr(c, a, op_dec(c, rd(c, a))); return 5; }
    case 0x8D: { uint8_t v = f8(c); c->y = v; set_nz(c, v); return 2; }
    case 0x8E: c->psw = pop(c); return 4;                    /* POP PSW */
    case 0x8F: { uint8_t i = f8(c), d = f8(c); wr(c, dp(c, d), i); return 5; }
    case 0x9A: { uint8_t d = f8(c); uint16_t a = dp(c, d); uint16_t ya = (uint16_t)((c->y << 8) | c->a); uint16_t v = (uint16_t)(rd(c, a) | (rd(c, (uint16_t)(a + 1)) << 8)); unsigned r = ya - v; setf(c, SPC_FLAG_H, (ya & 0xFFF) >= (v & 0xFFF)); setf(c, SPC_FLAG_C, r <= 0xFFFF); c->a = (uint8_t)r; c->y = (uint8_t)(r >> 8); set_nz(c, c->y); return 5; } /* SUBW */
    case 0x9C: c->a = op_dec(c, c->a); return 2;
    case 0x9D: c->x = c->sp; set_nz(c, c->x); return 2;
    case 0x9E: {                                             /* DIV YA,X */
        if (c->x == 0) { c->a = 0xFF; c->y = 0; setf(c, SPC_FLAG_V, true); set_nz(c, c->a); return 12; }
        unsigned ya = ((unsigned)c->y << 8) | c->a;
        unsigned q = ya / c->x, r = ya % c->x;
        c->a = (uint8_t)q; c->y = (uint8_t)r;
        setf(c, SPC_FLAG_V, q > 0xFF);
        setf(c, SPC_FLAG_H, (c->x & 0xF) <= (c->y & 0xF));
        set_nz(c, c->a); return 12;
    }
    case 0x9F: c->a = (uint8_t)((c->a >> 4) | (c->a << 4)); set_nz(c, c->a); return 5;   /* XCN */
    case 0x88: { uint8_t v = f8(c); alu_adc(c, v); return 2; }
    case 0x84: { uint8_t d = f8(c); alu_adc(c, rd(c, dp(c, d))); return 3; }
    case 0x94: { uint8_t d = f8(c); alu_adc(c, rd(c, dpx(c, d))); return 4; }
    case 0x85: { uint16_t a = f16(c); alu_adc(c, rd(c, a)); return 4; }
    case 0x95: { uint16_t a = (uint16_t)(f16(c) + c->x); alu_adc(c, rd(c, a)); return 5; }
    case 0x96: { uint16_t a = (uint16_t)(f16(c) + c->y); alu_adc(c, rd(c, a)); return 5; }
    case 0x86: { alu_adc(c, rd(c, inx(c))); return 3; }
    case 0x87: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dpx(c, d)) | (rd(c, (uint16_t)(dpx(c, d) + 1)) << 8)); alu_adc(c, rd(c, p)); return 6; }
    case 0x97: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dp(c, d)) | (rd(c, (uint16_t)(dp(c, d) + 1)) << 8)); alu_adc(c, rd(c, (uint16_t)(p + c->y))); return 6; }
    case 0x89: { uint8_t s = f8(c), d = f8(c); uint16_t a = dp(c, d); alu_adc(c, rd(c, dp(c, s))); wr(c, a, c->a); return 6; }
    case 0x98: { uint8_t i = f8(c), d = f8(c); uint16_t a = dp(c, d); alu_adc(c, i); wr(c, a, c->a); return 5; }
    case 0x99: { alu_adc(c, rd(c, iny(c))); wr(c, inx(c), c->a); return 5; }
    case 0xAA: { int b; uint16_t a = membit(c, &b); setf(c, SPC_FLAG_C, ((rd(c, a) >> b) & 1) != 0); return 4; }

    /* ---- 0xA0 row ---- */
    case 0xA0: setf(c, SPC_FLAG_I, true); return 3;          /* EI */
    case 0xC0: setf(c, SPC_FLAG_I, false); return 3;         /* DI */
    case 0xAB: { uint8_t d = f8(c); uint16_t a = dp(c, d); wr(c, a, op_inc(c, rd(c, a))); return 4; }
    case 0xBB: { uint8_t d = f8(c); uint16_t a = dpx(c, d); wr(c, a, op_inc(c, rd(c, a))); return 5; }
    case 0xAC: { uint16_t a = f16(c); wr(c, a, op_inc(c, rd(c, a))); return 5; }
    case 0xAD: { uint8_t v = f8(c); int r = c->y - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); return 2; }
    case 0xAE: c->a = pop(c); return 4;                      /* POP A */
    case 0xAF: { wr(c, inx(c), c->a); c->x++; return 4; }    /* MOV (X)+,A */
    case 0xBA: { uint8_t d = f8(c); uint16_t a = dp(c, d); c->a = rd(c, a); c->y = rd(c, (uint16_t)(a + 1)); set_nz(c, c->y); return 5; } /* MOVW YA,d */
    case 0xBC: c->a = op_inc(c, c->a); return 2;
    case 0xBD: c->sp = c->x; return 2;
    case 0xBE: {                                             /* DAS */
        if (c->a > 0x99 || (c->psw & SPC_FLAG_C)) { c->a = (uint8_t)(c->a - 0x60); setf(c, SPC_FLAG_C, true); }
        if ((c->a & 0x0F) > 9 || (c->psw & SPC_FLAG_H)) { c->a = (uint8_t)(c->a - 0x06); }
        set_nz(c, c->a); return 3;
    }
    case 0xBF: { c->a = rd(c, inx(c)); c->x++; set_nz(c, c->a); return 4; }  /* MOV A,(X)+ */
    case 0xA8: { uint8_t v = f8(c); alu_sbc(c, v); return 2; }
    case 0xA4: { uint8_t d = f8(c); alu_sbc(c, rd(c, dp(c, d))); return 3; }
    case 0xB4: { uint8_t d = f8(c); alu_sbc(c, rd(c, dpx(c, d))); return 4; }
    case 0xA5: { uint16_t a = f16(c); alu_sbc(c, rd(c, a)); return 4; }
    case 0xB5: { uint16_t a = (uint16_t)(f16(c) + c->x); alu_sbc(c, rd(c, a)); return 5; }
    case 0xB6: { uint16_t a = (uint16_t)(f16(c) + c->y); alu_sbc(c, rd(c, a)); return 5; }
    case 0xA6: { alu_sbc(c, rd(c, inx(c))); return 3; }
    case 0xA7: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dpx(c, d)) | (rd(c, (uint16_t)(dpx(c, d) + 1)) << 8)); alu_sbc(c, rd(c, p)); return 6; }
    case 0xB7: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dp(c, d)) | (rd(c, (uint16_t)(dp(c, d) + 1)) << 8)); alu_sbc(c, rd(c, (uint16_t)(p + c->y))); return 6; }
    case 0xA9: { uint8_t s = f8(c), d = f8(c); uint16_t a = dp(c, d); alu_sbc(c, rd(c, dp(c, s))); wr(c, a, c->a); return 6; }
    case 0xB8: { uint8_t i = f8(c), d = f8(c); uint16_t a = dp(c, d); alu_sbc(c, i); wr(c, a, c->a); return 5; }
    case 0xB9: { alu_sbc(c, rd(c, iny(c))); wr(c, inx(c), c->a); return 5; }
    case 0xCA: { int b; uint16_t a = membit(c, &b); wr(c, a, (uint8_t)((rd(c, a) & ~(1 << b)) | ((c->psw & SPC_FLAG_C) ? (1 << b) : 0))); return 6; }
    case 0xEA: { int b; uint16_t a = membit(c, &b); wr(c, a, (uint8_t)(rd(c, a) ^ (1 << b))); return 5; }

    /* ---- 0xC0 row ---- */
    case 0xC4: { uint8_t d = f8(c); wr(c, dp(c, d), c->a); return 4; }
    case 0xD4: { uint8_t d = f8(c); wr(c, dpx(c, d), c->a); return 5; }
    case 0xC5: { uint16_t a = f16(c); wr(c, a, c->a); return 5; }
    case 0xD5: { uint16_t a = (uint16_t)(f16(c) + c->x); wr(c, a, c->a); return 6; }
    case 0xD6: { uint16_t a = (uint16_t)(f16(c) + c->y); wr(c, a, c->a); return 6; }
    case 0xC6: { wr(c, inx(c), c->a); return 4; }            /* MOV (X),A */
    case 0xC7: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dpx(c, d)) | (rd(c, (uint16_t)(dpx(c, d) + 1)) << 8)); wr(c, p, c->a); return 7; }
    case 0xD7: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dp(c, d)) | (rd(c, (uint16_t)(dp(c, d) + 1)) << 8)); wr(c, (uint16_t)(p + c->y), c->a); return 7; }
    case 0xC8: { uint8_t v = f8(c); int r = c->x - v; setf(c, SPC_FLAG_C, r >= 0); set_nz(c, (uint8_t)r); return 2; }
    case 0xC9: { uint16_t a = f16(c); wr(c, a, c->x); return 5; }
    case 0xCB: { uint8_t d = f8(c); wr(c, dp(c, d), c->y); return 4; }
    case 0xCC: { uint16_t a = f16(c); wr(c, a, c->y); return 5; }
    case 0xCD: { uint8_t v = f8(c); c->x = v; set_nz(c, v); return 2; }
    case 0xCE: c->x = pop(c); return 4;                      /* POP X */
    case 0xCF: {                                             /* MUL YA */
        unsigned r = (unsigned)c->y * c->a;
        c->a = (uint8_t)r; c->y = (uint8_t)(r >> 8);
        setf(c, SPC_FLAG_N, (c->y & 0x80) != 0); setf(c, SPC_FLAG_Z, c->y == 0); return 9;
    }
    case 0xD8: { uint8_t d = f8(c); wr(c, dp(c, d), c->x); return 4; }
    case 0xD9: { uint8_t d = f8(c); wr(c, dpy(c, d), c->x); return 5; }
    case 0xDA: { uint8_t d = f8(c); uint16_t a = dp(c, d); wr(c, a, c->a); wr(c, (uint16_t)(a + 1), c->y); return 5; } /* MOVW d,YA */
    case 0xDB: { uint8_t d = f8(c); wr(c, dpx(c, d), c->y); return 5; }
    case 0xDC: c->y = op_dec(c, c->y); return 2;
    case 0xDD: c->a = c->y; set_nz(c, c->a); return 2;
    case 0xDE: { uint8_t d = f8(c); int8_t r = (int8_t)f8(c); uint16_t ad=dpx(c,d); uint8_t mv=rd(c,ad); if(getenv("CB_TRACE"))fprintf(stderr,"[CB d=%02X X=%02X addr=%04X A=%02X mem=%02X %s]",d,c->x,ad,c->a,mv,(c->a!=mv)?"BRANCH":"fall"); if (c->a != mv) c->pc = (uint16_t)(c->pc + r); return 6; }
    case 0xDF: {                                             /* DAA */
        if (c->a > 0x99 || (c->psw & SPC_FLAG_C)) { c->a = (uint8_t)(c->a + 0x60); setf(c, SPC_FLAG_C, true); }
        if ((c->a & 0x0F) > 9 || (c->psw & SPC_FLAG_H)) { c->a = (uint8_t)(c->a + 0x06); }
        set_nz(c, c->a); return 3;
    }

    /* ---- 0xE0 row ---- */
    case 0xE0: setf(c, SPC_FLAG_V, false); setf(c, SPC_FLAG_H, false); return 2;   /* CLRV */
    case 0xE4: { uint8_t d = f8(c); c->a = rd(c, dp(c, d)); set_nz(c, c->a); return 3; }
    case 0xF4: { uint8_t d = f8(c); c->a = rd(c, dpx(c, d)); set_nz(c, c->a); return 4; }
    case 0xE5: { uint16_t a = f16(c); c->a = rd(c, a); set_nz(c, c->a); return 4; }
    case 0xF5: { uint16_t a = (uint16_t)(f16(c) + c->x); c->a = rd(c, a); set_nz(c, c->a); if(getenv("R5_TRACE")&&c->pc>=0x05A5&&c->pc<=0x05C1)fprintf(stderr,"[MvA absX=%04X ->A=%02X spc=%04X X=%02X]",a,c->a,c->pc,c->x); return 5; }
    case 0xF6: { uint16_t a = (uint16_t)(f16(c) + c->y); c->a = rd(c, a); set_nz(c, c->a); return 5; }
    case 0xE6: { c->a = rd(c, inx(c)); set_nz(c, c->a); return 3; }
    case 0xE7: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dpx(c, d)) | (rd(c, (uint16_t)(dpx(c, d) + 1)) << 8)); c->a = rd(c, p); set_nz(c, c->a); return 6; }
    case 0xF7: { uint8_t d = f8(c); uint16_t p = (uint16_t)(rd(c, dp(c, d)) | (rd(c, (uint16_t)(dp(c, d) + 1)) << 8)); c->a = rd(c, (uint16_t)(p + c->y)); set_nz(c, c->a); return 6; }
    case 0xE8: { uint8_t v = f8(c); c->a = v; set_nz(c, v); return 2; }
    case 0xE9: { uint16_t a = f16(c); c->x = rd(c, a); set_nz(c, c->x); return 4; }
    case 0xEB: { uint8_t d = f8(c); c->y = rd(c, dp(c, d)); set_nz(c, c->y); return 3; }
    case 0xEC: { uint16_t a = f16(c); uint8_t vv=rd(c, a); if(getenv("EC_TRACE"))fprintf(stderr,"[EC a=%04X v=%02X]",a,vv); c->y = vv; set_nz(c, c->y); return 4; }
    case 0xED: setf(c, SPC_FLAG_C, !(c->psw & SPC_FLAG_C)); return 3;   /* NOTC */
    case 0xEE: c->y = pop(c); return 4;                      /* POP Y */
    case 0xEF: c->stopped = true; return 2;                  /* SLEEP */
    case 0xF8: { uint8_t d = f8(c); c->x = rd(c, dp(c, d)); set_nz(c, c->x); return 3; }
    case 0xF9: { uint8_t d = f8(c); c->x = rd(c, dpy(c, d)); set_nz(c, c->x); return 4; }
    case 0xFA: { uint8_t s = f8(c), d = f8(c); wr(c, dp(c, d), rd(c, dp(c, s))); return 5; }  /* MOV dd,ds */
    case 0xFB: { uint8_t d = f8(c); c->y = rd(c, dpx(c, d)); set_nz(c, c->y); return 4; }
    case 0xFC: c->y = op_inc(c, c->y); return 2;
    case 0xFD: c->y = c->a; set_nz(c, c->y); return 2; /* MOV Y,A */
    case 0xFE: { int8_t r = (int8_t)f8(c); c->y = op_dec(c, c->y); if (c->y != 0) c->pc = (uint16_t)(c->pc + r); return 4; }
    case 0xFF: c->stopped = true; return 2;                  /* STOP */

    default: return 2;  /* unreachable: all 256 opcodes are handled */
    }
}

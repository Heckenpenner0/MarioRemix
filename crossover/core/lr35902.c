/* lr35902.c - Game Boy CPU core implementation. See lr35902.h. */
#include "lr35902.h"

/* ---- helpers on the memory bus ---- */
static inline uint8_t rd(lr_cpu *c, uint16_t a) { return c->read(c->ctx, a); }
static inline void    wr(lr_cpu *c, uint16_t a, uint8_t v) { c->write(c->ctx, a, v); }
static inline void wr16(lr_cpu *c, uint16_t a, uint16_t v) {
    wr(c, a, (uint8_t)(v & 0xFF));
    wr(c, (uint16_t)(a + 1), (uint8_t)(v >> 8));
}

/* stack push/pop (SP grows downward) */
static inline void push16(lr_cpu *c, uint16_t v) {
    wr(c, --c->sp, (uint8_t)(v >> 8));
    wr(c, --c->sp, (uint8_t)(v & 0xFF));
}
static inline uint16_t pop16(lr_cpu *c) {
    uint8_t lo = rd(c, c->sp++);
    uint8_t hi = rd(c, c->sp++);
    return (uint16_t)(lo | (hi << 8));
}

static inline uint8_t fetch8(lr_cpu *c) { return rd(c, c->pc++); }
static inline uint16_t fetch16(lr_cpu *c) {
    uint8_t lo = fetch8(c);
    uint8_t hi = fetch8(c);
    return (uint16_t)(lo | (hi << 8));
}

/* ---- flag helpers ---- */
static inline void setf(lr_cpu *c, uint8_t mask, bool on) {
    if (on) c->f |= mask; else c->f &= (uint8_t)~mask;
}
static inline bool getf(lr_cpu *c, uint8_t mask) { return (c->f & mask) != 0; }

/* register file access by opcode index: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A */
static uint8_t get_r(lr_cpu *c, int i) {
    switch (i) {
        case 0: return c->b; case 1: return c->c; case 2: return c->d; case 3: return c->e;
        case 4: return c->h; case 5: return c->l; case 6: return rd(c, c->hl); case 7: return c->a;
    }
    return 0;
}
static void set_r(lr_cpu *c, int i, uint8_t v) {
    switch (i) {
        case 0: c->b = v; break; case 1: c->c = v; break; case 2: c->d = v; break; case 3: c->e = v; break;
        case 4: c->h = v; break; case 5: c->l = v; break; case 6: wr(c, c->hl, v); break; case 7: c->a = v; break;
    }
}
/* 16-bit register pair by index: 0=BC 1=DE 2=HL 3=SP (or AF for push/pop) */
static uint16_t get_rr(lr_cpu *c, int i) {
    switch (i) { case 0: return c->bc; case 1: return c->de; case 2: return c->hl; case 3: return c->sp; }
    return 0;
}
static void set_rr(lr_cpu *c, int i, uint16_t v) {
    switch (i) { case 0: c->bc = v; break; case 1: c->de = v; break; case 2: c->hl = v; break; case 3: c->sp = v; break; }
}

/* ---- ALU primitives (all set flags: Z N H C) ---- */
static void alu_add(lr_cpu *c, uint8_t v, bool carry) {
    unsigned ci = (carry && getf(c, LR_FLAG_C)) ? 1u : 0u;
    unsigned r = (unsigned)c->a + v + ci;
    setf(c, LR_FLAG_H, ((c->a & 0xF) + (v & 0xF) + ci) > 0xF);
    setf(c, LR_FLAG_C, r > 0xFF);
    setf(c, LR_FLAG_N, false);
    c->a = (uint8_t)r;
    setf(c, LR_FLAG_Z, c->a == 0);
}
static void alu_sub(lr_cpu *c, uint8_t v, bool carry, bool store) {
    unsigned ci = (carry && getf(c, LR_FLAG_C)) ? 1u : 0u;
    unsigned r = (unsigned)c->a - v - ci;
    setf(c, LR_FLAG_H, (c->a & 0xF) < ((v & 0xF) + ci));
    setf(c, LR_FLAG_C, r > 0xFF);
    setf(c, LR_FLAG_N, true);
    setf(c, LR_FLAG_Z, (uint8_t)r == 0);
    if (store) c->a = (uint8_t)r;
}
static void alu_and(lr_cpu *c, uint8_t v) {
    c->a &= v;
    setf(c, LR_FLAG_Z, c->a == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, true);       setf(c, LR_FLAG_C, false);
}
static void alu_xor(lr_cpu *c, uint8_t v) {
    c->a ^= v;
    setf(c, LR_FLAG_Z, c->a == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);      setf(c, LR_FLAG_C, false);
}
static void alu_or(lr_cpu *c, uint8_t v) {
    c->a |= v;
    setf(c, LR_FLAG_Z, c->a == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);      setf(c, LR_FLAG_C, false);
}

static void inc8(lr_cpu *c, int i) {
    uint8_t v = (uint8_t)(get_r(c, i) + 1);
    set_r(c, i, v);
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, (v & 0xF) == 0);
}
static void dec8(lr_cpu *c, int i) {
    uint8_t v = (uint8_t)(get_r(c, i) - 1);
    set_r(c, i, v);
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, true);
    setf(c, LR_FLAG_H, (v & 0xF) == 0xF);
}

static void add16_hl(lr_cpu *c, uint16_t v) {
    unsigned r = (unsigned)c->hl + v;
    setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, ((c->hl & 0xFFF) + (v & 0xFFF)) > 0xFFF);
    setf(c, LR_FLAG_C, r > 0xFFFF);
    c->hl = (uint16_t)r;
}

/* add signed imm to SP -> HL (or SP), classic ADD SP,e8 with odd flags */
static uint16_t add_sp_e8(lr_cpu *c) {
    int8_t e = (int8_t)fetch8(c);
    unsigned r = (unsigned)c->sp + (unsigned)(int)e;
    setf(c, LR_FLAG_Z, false); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, ((c->sp & 0xF) + ((unsigned)(uint8_t)e & 0xF)) > 0xF);
    setf(c, LR_FLAG_C, ((c->sp & 0xFF) + ((unsigned)(uint8_t)e & 0xFF)) > 0xFF);
    return (uint16_t)r;
}

/* rotate/shift group used by CB prefix and accumulator rotates */
static uint8_t rot_rlc(lr_cpu *c, uint8_t v) {
    uint8_t cy = (uint8_t)(v >> 7);
    v = (uint8_t)((v << 1) | cy);
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);  setf(c, LR_FLAG_C, cy != 0);
    return v;
}
static uint8_t rot_rrc(lr_cpu *c, uint8_t v) {
    uint8_t cy = (uint8_t)(v & 1);
    v = (uint8_t)((v >> 1) | (cy << 7));
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);  setf(c, LR_FLAG_C, cy != 0);
    return v;
}
static uint8_t rot_rl(lr_cpu *c, uint8_t v) {
    uint8_t cy = (uint8_t)(v >> 7);
    v = (uint8_t)((v << 1) | (getf(c, LR_FLAG_C) ? 1 : 0));
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);  setf(c, LR_FLAG_C, cy != 0);
    return v;
}
static uint8_t rot_rr(lr_cpu *c, uint8_t v) {
    uint8_t cy = (uint8_t)(v & 1);
    v = (uint8_t)((v >> 1) | (getf(c, LR_FLAG_C) ? 0x80 : 0));
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);  setf(c, LR_FLAG_C, cy != 0);
    return v;
}
static uint8_t rot_sla(lr_cpu *c, uint8_t v) {
    uint8_t cy = (uint8_t)(v >> 7);
    v = (uint8_t)(v << 1);
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);  setf(c, LR_FLAG_C, cy != 0);
    return v;
}
static uint8_t rot_sra(lr_cpu *c, uint8_t v) {
    uint8_t cy = (uint8_t)(v & 1);
    v = (uint8_t)((v >> 1) | (v & 0x80));
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);  setf(c, LR_FLAG_C, cy != 0);
    return v;
}
static uint8_t rot_swap(lr_cpu *c, uint8_t v) {
    v = (uint8_t)((v << 4) | (v >> 4));
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);  setf(c, LR_FLAG_C, false);
    return v;
}
static uint8_t rot_srl(lr_cpu *c, uint8_t v) {
    uint8_t cy = (uint8_t)(v & 1);
    v = (uint8_t)(v >> 1);
    setf(c, LR_FLAG_Z, v == 0); setf(c, LR_FLAG_N, false);
    setf(c, LR_FLAG_H, false);  setf(c, LR_FLAG_C, cy != 0);
    return v;
}

/* ---- public API ---- */
void lr_init(lr_cpu *c, lr_read_fn read, lr_write_fn write, void *ctx) {
    c->read = read; c->write = write; c->ctx = ctx;
    lr_reset(c);
}

void lr_reset(lr_cpu *c) {
    c->af = 0x01B0; c->bc = 0x0013; c->de = 0x00D8; c->hl = 0x014D;
    c->sp = 0xFFFE; c->pc = 0x0100;
    c->ime = false; c->ime_pending = false; c->halted = false; c->stopped = false;
}

bool lr_interrupt(lr_cpu *c, uint16_t vec) {
    if (!c->ime) return false;
    c->ime = false;
    c->halted = false;
    push16(c, c->pc);
    c->pc = vec;
    return true;
}

/* condition codes for JP/JR/RET/CALL: 0=NZ 1=Z 2=NC 3=C */
static bool cond(lr_cpu *c, int cc) {
    switch (cc) {
        case 0: return !getf(c, LR_FLAG_Z);
        case 1: return  getf(c, LR_FLAG_Z);
        case 2: return !getf(c, LR_FLAG_C);
        case 3: return  getf(c, LR_FLAG_C);
    }
    return false;
}

static void daa(lr_cpu *c) {
    uint8_t a = c->a;
    uint8_t adj = 0;
    bool carry = getf(c, LR_FLAG_C);
    if (!getf(c, LR_FLAG_N)) {
        if (getf(c, LR_FLAG_H) || (a & 0xF) > 9) adj |= 0x06;
        if (carry || a > 0x99) { adj |= 0x60; carry = true; }
        a = (uint8_t)(a + adj);
    } else {
        if (getf(c, LR_FLAG_H)) adj |= 0x06;
        if (carry) adj |= 0x60;
        a = (uint8_t)(a - adj);
    }
    c->a = a;
    setf(c, LR_FLAG_Z, a == 0); setf(c, LR_FLAG_H, false); setf(c, LR_FLAG_C, carry);
}

/* execute the CB-prefixed opcode `op` */
static int exec_cb(lr_cpu *c) {
    uint8_t op = fetch8(c);
    int r = op & 7;
    uint8_t v = get_r(c, r);
    int cy = op >> 6;
    int n = (op >> 3) & 7;
    if (cy == 0) {           /* rotates / shifts / swap */
        switch (n) {
            case 0: v = rot_rlc(c, v); break;
            case 1: v = rot_rrc(c, v); break;
            case 2: v = rot_rl(c, v);  break;
            case 3: v = rot_rr(c, v);  break;
            case 4: v = rot_sla(c, v); break;
            case 5: v = rot_sra(c, v); break;
            case 6: v = rot_swap(c, v);break;
            case 7: v = rot_srl(c, v); break;
        }
        set_r(c, r, v);
    } else if (cy == 1) {    /* BIT n,r */
        setf(c, LR_FLAG_Z, (v & (1u << n)) == 0);
        setf(c, LR_FLAG_N, false);
        setf(c, LR_FLAG_H, true);
    } else if (cy == 2) {    /* RES n,r */
        set_r(c, r, (uint8_t)(v & ~(1u << n)));
    } else {                 /* SET n,r */
        set_r(c, r, (uint8_t)(v | (1u << n)));
    }
    return (r == 6) ? 16 : 8;
}

int lr_step(lr_cpu *c) {
    if (c->stopped) return 4;

    /* EI defers enabling interrupts by one instruction. */
    bool defer_ime = c->ime_pending;

    if (c->halted) {
        c->ime_pending = defer_ime ? false : c->ime_pending;
        c->ime = defer_ime ? true : c->ime;
        return 4;
    }

    uint8_t op = fetch8(c);
    int r = op & 7;          /* register index */
    int rd_ = (op >> 3) & 7; /* operation / destination group */

    switch (op) {
    case 0x00: /* NOP */ break;

    /* 16-bit loads: LD rr,nn */
    case 0x01: case 0x11: case 0x21: case 0x31:
        set_rr(c, (op >> 4) & 3, fetch16(c)); break;

    case 0x02: wr(c, c->bc, c->a); break;               /* LD (BC),A */
    case 0x12: wr(c, c->de, c->a); break;               /* LD (DE),A */
    case 0x22: wr(c, c->hl, c->a); c->hl++; break;      /* LD (HL+),A */
    case 0x32: wr(c, c->hl, c->a); c->hl--; break;      /* LD (HL-),A */

    case 0x0A: c->a = rd(c, c->bc); break;              /* LD A,(BC) */
    case 0x1A: c->a = rd(c, c->de); break;              /* LD A,(DE) */
    case 0x2A: c->a = rd(c, c->hl); c->hl++; break;     /* LD A,(HL+) */
    case 0x3A: c->a = rd(c, c->hl); c->hl--; break;     /* LD A,(HL-) */

    /* INC/DEC rr */
    case 0x03: case 0x13: case 0x23: case 0x33:
        set_rr(c, (op >> 4) & 3, (uint16_t)(get_rr(c, (op >> 4) & 3) + 1)); break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B:
        set_rr(c, (op >> 4) & 3, (uint16_t)(get_rr(c, (op >> 4) & 3) - 1)); break;

    /* INC/DEC r  (0x04/0x05 pattern) */
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C: inc8(c, rd_); break;
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D: dec8(c, rd_); break;

    /* LD r,n (immediate) */
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: case 0x36: case 0x3E: set_r(c, rd_, fetch8(c)); break;

    /* accumulator rotates (8-bit, clear Z) */
    case 0x07: c->a = rot_rlc(c, c->a); setf(c, LR_FLAG_Z, false); break; /* RLCA */
    case 0x0F: c->a = rot_rrc(c, c->a); setf(c, LR_FLAG_Z, false); break; /* RRCA */
    case 0x17: c->a = rot_rl(c, c->a);  setf(c, LR_FLAG_Z, false); break; /* RLA  */
    case 0x1F: c->a = rot_rr(c, c->a);  setf(c, LR_FLAG_Z, false); break; /* RRA  */

    case 0x08: { uint16_t a = fetch16(c); wr16(c, a, c->sp); } break;      /* LD (nn),SP */
    case 0x09: case 0x19: case 0x29: case 0x39: add16_hl(c, get_rr(c, (op >> 4) & 3)); break;

    case 0x10: fetch8(c); c->stopped = true; break;      /* STOP */
    case 0x18: { int8_t e = (int8_t)fetch8(c); c->pc = (uint16_t)(c->pc + e); } break; /* JR */
    case 0x20: case 0x28: case 0x30: case 0x38:          /* JR cc,e */
        { int8_t e = (int8_t)fetch8(c); if (cond(c, (op >> 3) & 3)) c->pc = (uint16_t)(c->pc + e); }
        break;

    case 0x27: daa(c); break;                            /* DAA */
    case 0x2F: c->a = (uint8_t)~c->a; setf(c, LR_FLAG_N, true); setf(c, LR_FLAG_H, true); break; /* CPL */
    case 0x37: setf(c, LR_FLAG_N, false); setf(c, LR_FLAG_H, false); setf(c, LR_FLAG_C, true); break; /* SCF */
    case 0x3F: setf(c, LR_FLAG_N, false); setf(c, LR_FLAG_H, false); setf(c, LR_FLAG_C, !getf(c, LR_FLAG_C)); break; /* CCF */

    /* LD r,r'  (block 0x40..0x7F except 0x76=HALT) */
    case 0x76: c->halted = true; break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:        case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        set_r(c, rd_, get_r(c, r)); break;

    /* ALU A,r  (block 0x80..0xBF) */
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        alu_add(c, get_r(c, r), false); break;
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        alu_add(c, get_r(c, r), true); break;
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        alu_sub(c, get_r(c, r), false, true); break;
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        alu_sub(c, get_r(c, r), true, true); break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        alu_and(c, get_r(c, r)); break;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        alu_xor(c, get_r(c, r)); break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        alu_or(c, get_r(c, r)); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        alu_sub(c, get_r(c, r), false, false); break;   /* CP */

    /* RET / RETI / conditional RET */
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        if (cond(c, (op >> 3) & 3)) { c->pc = pop16(c); return 20; } return 8;
    case 0xC9: c->pc = pop16(c); return 16;
    case 0xD9: c->pc = pop16(c); c->ime = true; return 16;

    /* POP rr (0xC1..AF) */
    case 0xC1: c->bc = pop16(c); break;
    case 0xD1: c->de = pop16(c); break;
    case 0xE1: c->hl = pop16(c); break;
    case 0xF1: c->af = (uint16_t)(pop16(c) & 0xFFF0); break;

    /* JP nn / conditional / JP HL */
    case 0xC3: c->pc = fetch16(c); return 16;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
        uint16_t a = fetch16(c);
        if (cond(c, (op >> 3) & 3)) { c->pc = a; return 16; }
        return 12;
    }
    case 0xE9: c->pc = c->hl; return 4;

    /* CALL / conditional */
    case 0xCD: { uint16_t a = fetch16(c); push16(c, c->pc); c->pc = a; return 24; }
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
        uint16_t a = fetch16(c);
        if (cond(c, (op >> 3) & 3)) { push16(c, c->pc); c->pc = a; return 24; }
        return 12;
    }

    /* PUSH rr */
    case 0xC5: push16(c, c->bc); break;
    case 0xD5: push16(c, c->de); break;
    case 0xE5: push16(c, c->hl); break;
    case 0xF5: push16(c, c->af); break;

    /* ALU A,n (immediate) */
    case 0xC6: alu_add(c, fetch8(c), false); break;
    case 0xCE: alu_add(c, fetch8(c), true);  break;
    case 0xD6: alu_sub(c, fetch8(c), false, true); break;
    case 0xDE: alu_sub(c, fetch8(c), true,  true); break;
    case 0xE6: alu_and(c, fetch8(c)); break;
    case 0xEE: alu_xor(c, fetch8(c)); break;
    case 0xF6: alu_or (c, fetch8(c)); break;
    case 0xFE: alu_sub(c, fetch8(c), false, false); break;

    /* RST vec */
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        push16(c, c->pc); c->pc = (uint16_t)(op & 0x38); return 16;

    /* LDH (n),A  /  LDH A,(n)  /  LD (C),A / LD A,(C) */
    case 0xE0: wr(c, (uint16_t)(0xFF00 + fetch8(c)), c->a); break;
    case 0xF0: c->a = rd(c, (uint16_t)(0xFF00 + fetch8(c))); break;
    case 0xE2: wr(c, (uint16_t)(0xFF00 + c->c), c->a); break;
    case 0xF2: c->a = rd(c, (uint16_t)(0xFF00 + c->c)); break;

    case 0xE8: c->sp = add_sp_e8(c); break;             /* ADD SP,e8 */
    case 0xF8: c->hl = add_sp_e8(c); break;             /* LD HL,SP+e8 */
    case 0xF9: c->sp = c->hl; break;                    /* LD SP,HL */

    case 0xEA: wr(c, fetch16(c), c->a); break;          /* LD (nn),A */
    case 0xFA: c->a = rd(c, fetch16(c)); break;         /* LD A,(nn) */

    case 0xCB: return exec_cb(c);

    case 0xF3: c->ime = false; c->ime_pending = false; break; /* DI */
    case 0xFB: c->ime_pending = true; break;                  /* EI */

    /* unused / illegal opcodes behave as NOP */
    default: break;
    }

    /* apply a deferred EI now that the following instruction has run */
    if (c->ime_pending && op != 0xFB) { c->ime = true; c->ime_pending = false; }
    else if (defer_ime) { c->ime = true; c->ime_pending = false; }

    return (r == 6 || rd_ == 6) ? 8 : 4; /* rough cycle for (HL) memory forms */
}

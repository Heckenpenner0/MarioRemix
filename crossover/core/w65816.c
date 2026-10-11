/* w65816.c - WDC 65C816 CPU core implementation. See w65816.h. */
#include <stdio.h>
#include <stdlib.h>
#include "w65816.h"

/* ---- bus ---- */
static inline uint8_t rdb(w_cpu *c, uint32_t a) { return c->read(c->ctx, a); }
static inline void    wrb(w_cpu *c, uint32_t a, uint8_t v) { c->write(c->ctx, a, v); }
static inline uint16_t rd16(w_cpu *c, uint32_t a) {
    return (uint16_t)(rdb(c, a) | (rdb(c, (a + 1) & 0xFFFFFF) << 8));
}
static inline void wr16(w_cpu *c, uint32_t a, uint16_t v) {
    wrb(c, a, (uint8_t)(v & 0xFF));
    wrb(c, (a + 1) & 0xFFFFFF, (uint8_t)(v >> 8));
}

/* program fetch at PB:PC */
static inline uint8_t f8(w_cpu *c) {
    uint8_t v = rdb(c, ((uint32_t)c->pb << 16) | c->pc);
    c->pc++;
    return v;
}
static inline uint16_t f16(w_cpu *c) { uint8_t lo = f8(c); return (uint16_t)(lo | (f8(c) << 8)); }
static inline uint32_t f24(w_cpu *c) {
    uint32_t lo = f16(c);
    return (lo | ((uint32_t)f8(c) << 16));
}

/* ---- widths ---- */
static inline bool m8(w_cpu *c) { return c->emulation || (c->p & W_FLAG_M); }
static inline bool x8(w_cpu *c) { return c->emulation || (c->p & W_FLAG_X); }

/* ---- flags ---- */
static inline void setf(w_cpu *c, uint8_t mask, bool on) {
    if (on) c->p |= mask; else c->p &= (uint8_t)~mask;
}
static inline bool getf(w_cpu *c, uint8_t mask) { return (c->p & mask) != 0; }
static void set_nz(w_cpu *c, uint32_t v, bool is8) {
    if (is8) { setf(c, W_FLAG_Z, (v & 0xFF) == 0); setf(c, W_FLAG_N, (v & 0x80) != 0); }
    else     { setf(c, W_FLAG_Z, (v & 0xFFFF) == 0); setf(c, W_FLAG_N, (v & 0x8000) != 0); }
}

/* ---- stack (bank 0, S) ---- */
static inline void push8(w_cpu *c, uint8_t v) { wrb(c, (uint32_t)c->s, v); c->s--; }
static inline uint8_t pull8(w_cpu *c) { c->s++; return rdb(c, (uint32_t)c->s); }
static void pushN(w_cpu *c, uint32_t v, bool is8) {
    if (is8) push8(c, (uint8_t)v);
    else { push8(c, (uint8_t)(v >> 8)); push8(c, (uint8_t)(v & 0xFF)); }
}
static uint32_t pullN(w_cpu *c, bool is8) {
    if (is8) return pull8(c);
    { uint8_t lo = pull8(c); return (uint32_t)(lo | (pull8(c) << 8)); }
}

/* ---- addressing-mode effective addresses (24-bit) ---- */
static inline uint32_t ea_abs(w_cpu *c)   { return ((uint32_t)c->db << 16) | f16(c); }
static inline uint32_t ea_absx(w_cpu *c)  { return ((uint32_t)c->db << 16) | (uint16_t)(f16(c) + c->x); }
static inline uint32_t ea_absy(w_cpu *c)  { return ((uint32_t)c->db << 16) | (uint16_t)(f16(c) + c->y); }
static inline uint32_t ea_long(w_cpu *c)  { return f24(c); }
static inline uint32_t ea_longx(w_cpu *c) { return (f24(c) + c->x) & 0xFFFFFF; }
static inline uint32_t ea_dp(w_cpu *c)    { return (uint32_t)(uint16_t)(c->d + f8(c)); }
static inline uint32_t ea_dpx(w_cpu *c)   { return (uint32_t)(uint16_t)(c->d + f8(c) + c->x); }
static inline uint32_t ea_dpy(w_cpu *c)   { return (uint32_t)(uint16_t)(c->d + f8(c) + c->y); }
static uint32_t ea_dpix(w_cpu *c) { /* (dp,X) */
    uint16_t zp = (uint16_t)(c->d + f8(c) + c->x);
    uint16_t off = rd16(c, zp);
    return ((uint32_t)c->db << 16) | off;
}
static uint32_t ea_dpiy(w_cpu *c) { /* (dp),Y */
    uint16_t zp = (uint16_t)(c->d + f8(c));
    uint16_t off = rd16(c, zp);
    return ((uint32_t)c->db << 16) | (uint16_t)(off + c->y);
}
static uint32_t ea_sr(w_cpu *c) { /* sr,S */
    return (uint32_t)(uint16_t)(c->s + f8(c));
}
static uint32_t ea_dpi(w_cpu *c) { /* (dp) 16-bit indirect, no index */
    uint16_t zp = (uint16_t)(c->d + f8(c));
    return ((uint32_t)c->db << 16) | rd16(c, zp);
}
static uint32_t ea_dplong(w_cpu *c) { /* [dp] 24-bit indirect */
    uint16_t zp = (uint16_t)(c->d + f8(c));
    uint32_t lo = rd16(c, zp);
    return lo | ((uint32_t)rdb(c, (uint32_t)(uint16_t)(zp + 2)) << 16);
}
static uint32_t ea_dplongy(w_cpu *c) { /* [dp],Y */
    uint32_t base = ea_dplong(c);
    uint32_t ea = (base + c->y) & 0xFFFFFF;
    if (getenv("ILD_TRACE")) { static int n=0; if(n++<30) fprintf(stderr,"[ILD base=%06X Y=%04X ea=%06X D=%04X]\n", base, c->y, ea, c->d); }
    return ea;
}
static uint32_t ea_sry(w_cpu *c) { /* (sr,S),Y */
    uint16_t zp = (uint16_t)(c->s + f8(c));
    uint16_t off = rd16(c, zp);
    return ((uint32_t)c->db << 16) | (uint16_t)(off + c->y);
}

/* ---- operand read/write at M or X width ---- */
static uint32_t read_op(w_cpu *c, uint32_t a, bool is8) {
    return is8 ? rdb(c, a) : rd16(c, a);
}
static void write_op(w_cpu *c, uint32_t a, uint32_t v, bool is8) {
    if (is8) wrb(c, a, (uint8_t)v); else wr16(c, a, (uint16_t)v);
}

/* ---- ALU ---- */
static void op_adc(w_cpu *c, uint32_t v, bool is8) {
    if (getf(c, W_FLAG_D)) { /* decimal */
        if (is8) {
            unsigned ci = getf(c, W_FLAG_C) ? 1u : 0u;
            unsigned al = (c->a & 0x0F) + (v & 0x0F) + ci;
            unsigned ah = (c->a >> 4) + (v >> 4);
            if (al > 9) { al = (al + 6) & 0x0F; ah++; }
            bool z = (uint8_t)((c->a + v + ci) & 0xFF) == 0;
            bool n = (ah & 0x08) != 0;
            bool vv = (~(c->a ^ v) & (c->a ^ (ah << 4)) & 0x80) != 0;
            if (ah > 9) ah += 6;
            setf(c, W_FLAG_C, ah > 0x0F);
            c->a = (uint16_t)(((ah << 4) | (al & 0x0F)) & 0xFF);
            setf(c, W_FLAG_Z, z); setf(c, W_FLAG_N, n); setf(c, W_FLAG_V, vv);
        } else { /* 16-bit decimal */
            unsigned ci = getf(c, W_FLAG_C) ? 1u : 0u;
            uint32_t r = 0; unsigned carry = 0; unsigned nibble;
            for (int i = 0; i < 4; i++) {
                nibble = ((c->a >> (i*4)) & 0xF) + ((v >> (i*4)) & 0xF) + carry;
                if (nibble > 9) { nibble += 6; carry = 1; nibble &= 0xF; } else carry = 0;
                r |= (nibble & 0xF) << (i*4);
            }
            r += ci; /* fold initial carry roughly */
            setf(c, W_FLAG_C, carry != 0 || r > 0xFFFF);
            set_nz(c, r & 0xFFFF, false);
            setf(c, W_FLAG_V, false);
            c->a = (uint16_t)(r & 0xFFFF);
        }
        return;
    }
    /* binary */
    if (is8) {
        unsigned ci = getf(c, W_FLAG_C) ? 1u : 0u;
        unsigned r = (c->a & 0xFF) + (v & 0xFF) + ci;
        setf(c, W_FLAG_C, r > 0xFF);
        setf(c, W_FLAG_V, (~( (c->a ^ v) ) & (c->a ^ r) & 0x80) != 0);
        c->a = (uint16_t)((c->a & 0xFF00) | (r & 0xFF));
        set_nz(c, r & 0xFF, true);
    } else {
        unsigned ci = getf(c, W_FLAG_C) ? 1u : 0u;
        uint32_t r = (uint32_t)c->a + v + ci;
        setf(c, W_FLAG_C, r > 0xFFFF);
        setf(c, W_FLAG_V, ((~(c->a ^ v)) & (c->a ^ r) & 0x8000) != 0);
        c->a = (uint16_t)r;
        set_nz(c, r & 0xFFFF, false);
    }
}
static void op_sbc(w_cpu *c, uint32_t v, bool is8) {
    /* SBC = ADC with inverted operand in binary; decimal handled as ADC on ~v */
    op_adc(c, is8 ? (~v) & 0xFF : (~v) & 0xFFFF, is8);
}
static void op_cmp(w_cpu *c, uint32_t reg, uint32_t v, bool is8) {
    if (is8) {
        unsigned r = (reg & 0xFF) - (v & 0xFF);
        setf(c, W_FLAG_C, (reg & 0xFF) >= (v & 0xFF));
        set_nz(c, r & 0xFF, true);
    } else {
        uint32_t r = (uint32_t)reg - v;
        setf(c, W_FLAG_C, reg >= v);
        set_nz(c, r & 0xFFFF, false);
    }
}

/* ---- reset ---- */
void w_init(w_cpu *c, w_read_fn read, w_write_fn write, void *ctx) {
    c->read = read; c->write = write; c->ctx = ctx;
    w_reset(c);
}
void w_reset(w_cpu *c) {
    c->emulation = true;
    c->p = W_FLAG_M | W_FLAG_X | W_FLAG_I; /* M=1,X=1,I=1 */
    c->a = c->x = c->y = 0;
    c->d = 0; c->db = 0; c->pb = 0;
    c->s = 0x01FF;
    c->pc = (uint16_t)(rdb(c, 0x00FFFC) | (rdb(c, 0x00FFFD) << 8)); /* reset vector */
    c->stopped = c->waiting = false;
}

static void set_x_width_side_effects(w_cpu *c) {
    if (x8(c)) { c->x &= 0xFF; c->y &= 0xFF; } /* high bytes cleared when 8-bit */
}

/* branch helper */
static void branch(w_cpu *c, bool take) {
    int8_t off = (int8_t)f8(c);
    if (take) c->pc = (uint16_t)(c->pc + off);
}

int w_step(w_cpu *c) {
    if (c->stopped) return 1;
    if (c->waiting) return 1;

    /* ring buffer of recent (PB:PC) to catch crash divergence */
    if (getenv("CPU_TRACE")) {
        static uint32_t ring[64]; static int rp=0; static int dumped=0;
        uint32_t cur=((uint32_t)c->pb<<16)|c->pc;
        ring[rp++ & 63]=cur;
        if (!dumped && c->pb < 0x40 && c->pc < 0x8000) {
            dumped=1;
            fprintf(stderr, "\n[CPU crash at PB:PC=%02X:%04X A=%04X X=%04X Y=%04X S=%04X P=%02X DB=%02X]\n  ring (oldest->newest):\n", c->pb,c->pc,c->a,c->x,c->y,c->s,c->p,c->db);
            for(int i=0;i<64;i++){ uint32_t e=ring[(rp+i)&63]; if(e) fprintf(stderr,"    %02X:%04X\n",(e>>16)&0xFF,e&0xFFFF); }
        }
    }
    uint8_t op = f8(c);
    bool is8m = m8(c), is8x = x8(c);
    uint32_t ea = 0, v = 0;
    if (getenv("XP_TRACE") && c->pb==0x00 && c->pc>=0x86D0 && c->pc<=0x86FA) {
        fprintf(stderr, "[XP pc=%04X op=%02X A=%04X X=%04X Y=%04X S=%04X D=%04X P=%02X 8m=%d 8x=%d DP00=%02X%02X%02X]\n",
            (unsigned)(c->pc-1), op, c->a, c->x, c->y, c->s, c->d, c->p, is8m, is8x,
            rdb(c,0x0002), rdb(c,0x0001), rdb(c,0x0000));
    }

    switch (op) {
    /* ---- loads / stores ---- */
    case 0xA9: /* LDA # */
        v = is8m ? f8(c) : f16(c); c->a = is8m ? (uint16_t)((c->a & 0xFF00) | (v & 0xFF)) : (uint16_t)v;
        set_nz(c, v, is8m); return 3;
    case 0xA5: case 0xAD: case 0xB5: case 0xBD: case 0xB9: case 0xA1: case 0xB1:
    case 0xAF: case 0xBF: case 0xB2: case 0xA3: case 0xB3:
    case 0xA7: case 0xB7: {
        switch (op) {
            case 0xA5: ea = ea_dp(c); break;   case 0xAD: ea = ea_abs(c); break;
            case 0xB5: ea = ea_dpx(c); break;  case 0xBD: ea = ea_absx(c); break;
            case 0xB9: ea = ea_absy(c); break; case 0xA1: ea = ea_dpix(c); break;
            case 0xB1: ea = ea_dpiy(c); break; case 0xAF: ea = ea_long(c); break;
            case 0xBF: ea = ea_longx(c); break;case 0xB2: ea = ea_dpi(c); break;
            case 0xA7: ea = ea_dplong(c); break; case 0xB7: ea = ea_dplongy(c); break;
            case 0xA3: ea = ea_sr(c); break;   case 0xB3: ea = ea_sry(c); break;
        }
        v = read_op(c, ea, is8m);
        if (op==0xB7 && getenv("ILD_TRACE")) { static int n=0; if(n++<30) fprintf(stderr,"  LDA[dp],Y ea=%06X -> v=%04X (m8=%d)\n", ea, v, is8m); }
        c->a = is8m ? (uint16_t)((c->a & 0xFF00) | (v & 0xFF)) : (uint16_t)v;
        set_nz(c, v, is8m); return 4;
    }
    case 0x85: case 0x8D: case 0x95: case 0x9D: case 0x99: case 0x81: case 0x91:
    case 0x8F: case 0x9F: case 0x92: case 0x83: case 0x93:
    case 0x87: case 0x97: {
        switch (op) {
            case 0x85: ea = ea_dp(c); break;   case 0x8D: ea = ea_abs(c); break;
            case 0x95: ea = ea_dpx(c); break;  case 0x9D: ea = ea_absx(c); break;
            case 0x99: ea = ea_absy(c); break; case 0x81: ea = ea_dpix(c); break;
            case 0x91: ea = ea_dpiy(c); break; case 0x8F: ea = ea_long(c); break;
            case 0x9F: ea = ea_longx(c); break;case 0x92: ea = ea_dpi(c); break;
            case 0x87: ea = ea_dplong(c); break; case 0x97: ea = ea_dplongy(c); break;
            case 0x83: ea = ea_sr(c); break;   case 0x93: ea = ea_sry(c); break;
        }
        write_op(c, ea, c->a, is8m); return 4;
    }
    case 0xA2: v = is8x ? f8(c) : f16(c); c->x = (uint16_t)v; set_x_width_side_effects(c); set_nz(c, v, is8x); return 3; /* LDX # */
    case 0xA6: case 0xAE: case 0xB6: case 0xBE: {
        switch (op) { case 0xA6: ea = ea_dp(c); break; case 0xAE: ea = ea_abs(c); break;
                      case 0xB6: ea = ea_dpy(c); break; case 0xBE: ea = ea_absy(c); break; }
        v = read_op(c, ea, is8x); c->x = (uint16_t)v; set_x_width_side_effects(c); set_nz(c, v, is8x); return 4;
    }
    case 0x86: case 0x8E: case 0x96: {
        switch (op) { case 0x86: ea = ea_dp(c); break; case 0x8E: ea = ea_abs(c); break; case 0x96: ea = ea_dpy(c); break; }
        write_op(c, ea, c->x, is8x); return 4;
    }
    case 0xA0: v = is8x ? f8(c) : f16(c); c->y = (uint16_t)v; set_x_width_side_effects(c); set_nz(c, v, is8x); return 3; /* LDY # */
    case 0xA4: case 0xAC: case 0xB4: case 0xBC: {
        switch (op) { case 0xA4: ea = ea_dp(c); break; case 0xAC: ea = ea_abs(c); break;
                      case 0xB4: ea = ea_dpx(c); break; case 0xBC: ea = ea_absx(c); break; }
        v = read_op(c, ea, is8x); c->y = (uint16_t)v; set_x_width_side_effects(c); set_nz(c, v, is8x); return 4;
    }
    case 0x84: case 0x8C: case 0x94: {
        switch (op) { case 0x84: ea = ea_dp(c); break; case 0x8C: ea = ea_abs(c); break; case 0x94: ea = ea_dpx(c); break; }
        write_op(c, ea, c->y, is8x); return 4;
    }
    case 0x9C: ea = ea_abs(c); write_op(c, ea, 0, is8m); return 4; /* STZ abs */
    case 0x64: ea = ea_dp(c);  write_op(c, ea, 0, is8m); return 4; /* STZ dp */

    /* ---- transfers ---- */
    case 0xAA: c->x = is8x ? (c->a & 0xFF) : c->a; set_x_width_side_effects(c); set_nz(c, c->x, is8x); return 2; /* TAX */
    case 0xA8: c->y = is8x ? (c->a & 0xFF) : c->a; set_x_width_side_effects(c); set_nz(c, c->y, is8x); return 2; /* TAY */
    case 0x8A: c->a = is8m ? (uint16_t)((c->a & 0xFF00) | (c->x & 0xFF)) : c->x; set_nz(c, c->x, is8m); return 2; /* TXA */
    case 0x98: c->a = is8m ? (uint16_t)((c->a & 0xFF00) | (c->y & 0xFF)) : c->y; set_nz(c, c->y, is8m); return 2; /* TYA */
    case 0x9A: c->s = c->x; return 2; /* TXS */
    case 0xBA: c->x = c->s; set_x_width_side_effects(c); set_nz(c, c->x, is8x); return 2; /* TSX */
    case 0x9B: c->y = c->x; set_x_width_side_effects(c); set_nz(c, c->y, is8x); return 2; /* TXY */
    case 0xBB: c->x = c->y; set_x_width_side_effects(c); set_nz(c, c->x, is8x); return 2; /* TYX */
    case 0x5B: c->d = c->a; set_nz(c, c->d, false); return 2; /* TCD */
    case 0x7B: c->a = c->d; set_nz(c, c->a, is8m); return 2; /* TDC */
    case 0x1B: c->s = c->a; return 2; /* TCS */
    case 0x3B: c->a = c->s; set_nz(c, c->a, is8m); return 2; /* TSC */

    /* ---- ALU (M width) ---- */
    case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71: case 0x6F: case 0x7F: case 0x72:
    case 0x63: case 0x67: case 0x73: case 0x77:
    case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1: case 0xEF: case 0xFF: case 0xF2:
    case 0xE3: case 0xE7: case 0xF3: case 0xF7:
    case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x21: case 0x31: case 0x2F: case 0x3F: case 0x32:
    case 0x23: case 0x27: case 0x33: case 0x37:
    case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x01: case 0x11: case 0x0F: case 0x1F: case 0x12:
    case 0x03: case 0x07: case 0x13: case 0x17:
    case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x41: case 0x51: case 0x4F: case 0x5F: case 0x52:
    case 0x43: case 0x47: case 0x53: case 0x57:
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1: case 0xCF: case 0xDF: case 0xD2:
    case 0xC3: case 0xC7: case 0xD3: case 0xD7: {
        int grp = op & 0xE0; /* 0x60=ADC 0xE0=SBC 0x20=AND 0x00=ORA 0x40=EOR 0xC0=CMP */
        bool imm = (op & 0x1F) == 0x09;
        if (imm) v = is8m ? f8(c) : f16(c);
        else {
            switch (op & 0x1F) {
                case 0x05: ea = ea_dp(c); break;   case 0x0D: ea = ea_abs(c); break;
                case 0x15: ea = ea_dpx(c); break;  case 0x1D: ea = ea_absx(c); break;
                case 0x19: ea = ea_absy(c); break; case 0x01: ea = ea_dpix(c); break;
                case 0x11: ea = ea_dpiy(c); break;   case 0x12: ea = ea_dpi(c); break;
                case 0x03: ea = ea_sr(c); break;     case 0x13: ea = ea_sry(c); break;
                case 0x07: ea = ea_dplong(c); break; case 0x17: ea = ea_dplongy(c); break;
                case 0x0F: ea = ea_long(c); break; case 0x1F: ea = ea_longx(c); break;
                default: ea = ea_abs(c); break;
            }
            v = read_op(c, ea, is8m);
        }
        switch (grp) {
            case 0x60: op_adc(c, v, is8m); break;
            case 0xE0: op_sbc(c, v, is8m); break;
            case 0x20: c->a = is8m ? (uint16_t)((c->a & 0xFF00) | ((c->a & v) & 0xFF)) : (uint16_t)(c->a & v); set_nz(c, is8m ? (c->a & 0xFF) : c->a, is8m); break;
            case 0x00: c->a = is8m ? (uint16_t)((c->a & 0xFF00) | ((c->a | v) & 0xFF)) : (uint16_t)(c->a | v); set_nz(c, is8m ? (c->a & 0xFF) : c->a, is8m); break;
            case 0x40: c->a = is8m ? (uint16_t)((c->a & 0xFF00) | ((c->a ^ v) & 0xFF)) : (uint16_t)(c->a ^ v); set_nz(c, is8m ? (c->a & 0xFF) : c->a, is8m); break;
            case 0xC0: op_cmp(c, c->a, v, is8m); break;
        }
        return 4;
    }
    /* ---- stack-relative / long-indirect push-pulls and jumps ---- */
    case 0x4B: push8(c, c->pb); return 3;                          /* PHK */
    case 0xF4: pushN(c, f16(c), false); return 5;                  /* PEA abs */
    case 0xD4: pushN(c, ea_dp(c), false); return 6;                /* PEI (dp) */
    case 0x62: {                                                   /* PER rell */
        int16_t off = (int16_t)f16(c);      /* fetched first: pc now past the instruction */
        pushN(c, (uint16_t)(c->pc + off), false); return 6;
    }
    case 0x42: f8(c); return 2;                                    /* WDM (reserved 2-byte NOP) */
    case 0x74: write_op(c, ea_dpx(c), 0, is8m); return 4;          /* STZ dp,X */
    case 0x9E: write_op(c, ea_absx(c), 0, is8m); return 5;         /* STZ abs,X */
    case 0x7C: {                                                   /* JMP (abs,X) */
        uint32_t a = (uint32_t)(((uint32_t)c->pb << 16) | (uint16_t)(f16(c) + c->x));
        c->pc = rd16(c, a); return 6;
    }
    case 0xDC: {                                                   /* JML [abs] */
        uint16_t a = f16(c);
        uint32_t target = rd16(c, a) | ((uint32_t)rdb(c, (uint32_t)(uint16_t)(a + 2)) << 16);
        if (getenv("JML_TRACE")) { static int n=0; if(n++<40) fprintf(stderr,"[JML[abs]=%04X ptr=%02X%02X%02X -> %06X (A=%04X X=%04X Y=%04X P=%02X)]",
            a, rdb(c,(uint32_t)(uint16_t)(a+2)), rd16(c,a)>>8, rd16(c,a)&0xFF, target, c->a,c->x,c->y,c->p); }
        c->pb = (uint8_t)(target >> 16); c->pc = (uint16_t)target; return 6;
    }

    case 0xE0: case 0xE4: case 0xEC: { /* CPX */
        if ((op & 0x0F) == 0x00) v = is8x ? f8(c) : f16(c);
        else v = read_op(c, (op == 0xEC) ? ea_abs(c) : ea_dp(c), is8x);
        op_cmp(c, c->x, v, is8x); return 3;
    }
    case 0xC0: case 0xC4: case 0xCC: { /* CPY */
        if ((op & 0x0F) == 0x00) v = is8x ? f8(c) : f16(c);
        else v = read_op(c, (op == 0xCC) ? ea_abs(c) : ea_dp(c), is8x);
        op_cmp(c, c->y, v, is8x); return 3;
    }
    case 0x24: case 0x2C: case 0x3C: case 0x34: { /* BIT */
        ea = (op == 0x2C) ? ea_abs(c) : (op == 0x3C) ? ea_absx(c) : (op == 0x34) ? ea_dpx(c) : ea_dp(c);
        v = read_op(c, ea, is8m);
        setf(c, W_FLAG_Z, (is8m ? (c->a & v) & 0xFF : (c->a & v)) == 0);
        if (is8m) { setf(c, W_FLAG_N, (v & 0x80) != 0); setf(c, W_FLAG_V, (v & 0x40) != 0); }
        else      { setf(c, W_FLAG_N, (v & 0x8000) != 0); setf(c, W_FLAG_V, (v & 0x4000) != 0); }
        return 4;
    }

    /* ---- inc / dec ---- */
    case 0x1A: c->a = is8m ? (uint16_t)((c->a & 0xFF00) | ((c->a + 1) & 0xFF)) : (uint16_t)(c->a + 1); set_nz(c, is8m ? (c->a & 0xFF) : c->a, is8m); return 2; /* INC A */
    case 0x3A: c->a = is8m ? (uint16_t)((c->a & 0xFF00) | ((c->a - 1) & 0xFF)) : (uint16_t)(c->a - 1); set_nz(c, is8m ? (c->a & 0xFF) : c->a, is8m); return 2; /* DEC A */
    case 0xE8: c->x = (uint16_t)(c->x + 1); set_x_width_side_effects(c); set_nz(c, c->x, is8x); return 2; /* INX */
    case 0xC8: c->y = (uint16_t)(c->y + 1); set_x_width_side_effects(c); set_nz(c, c->y, is8x); return 2; /* INY */
    case 0xCA: c->x = (uint16_t)(c->x - 1); set_x_width_side_effects(c); set_nz(c, c->x, is8x); return 2; /* DEX */
    case 0x88: c->y = (uint16_t)(c->y - 1); set_x_width_side_effects(c); set_nz(c, c->y, is8x); return 2; /* DEY */
    case 0xE6: case 0xEE: case 0xF6: case 0xFE: { /* INC mem */
        switch (op) { case 0xE6: ea = ea_dp(c); break; case 0xEE: ea = ea_abs(c); break;
                      case 0xF6: ea = ea_dpx(c); break; case 0xFE: ea = ea_absx(c); break; }
        v = read_op(c, ea, is8m) + 1; write_op(c, ea, v, is8m); set_nz(c, v, is8m); return 6;
    }
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: { /* DEC mem */
        switch (op) { case 0xC6: ea = ea_dp(c); break; case 0xCE: ea = ea_abs(c); break;
                      case 0xD6: ea = ea_dpx(c); break; case 0xDE: ea = ea_absx(c); break; }
        v = read_op(c, ea, is8m) - 1; write_op(c, ea, v, is8m); set_nz(c, v, is8m); return 6;
    }

    /* ---- shifts (A + mem) ---- */
    case 0x0A: case 0x4A: case 0x2A: case 0x6A: { /* ASL/LSR/ROL/ROR A */
        if (is8m) {
            uint8_t a = (uint8_t)c->a; bool cy;
            switch (op) {
                case 0x0A: cy = a >> 7; a = (uint8_t)(a << 1); break;
                case 0x4A: cy = a & 1;  a = (uint8_t)(a >> 1); break;
                case 0x2A: cy = a >> 7; a = (uint8_t)((a << 1) | (getf(c, W_FLAG_C) ? 1 : 0)); break;
                default:   cy = a & 1;  a = (uint8_t)((a >> 1) | (getf(c, W_FLAG_C) ? 0x80 : 0)); break;
            }
            c->a = (uint16_t)((c->a & 0xFF00) | a); setf(c, W_FLAG_C, cy); set_nz(c, a, true);
        } else {
            uint16_t a = c->a; bool cy;
            switch (op) {
                case 0x0A: cy = a >> 15; a = (uint16_t)(a << 1); break;
                case 0x4A: cy = a & 1;   a = (uint16_t)(a >> 1); break;
                case 0x2A: cy = a >> 15; a = (uint16_t)((a << 1) | (getf(c, W_FLAG_C) ? 1 : 0)); break;
                default:   cy = a & 1;   a = (uint16_t)((a >> 1) | (getf(c, W_FLAG_C) ? 0x8000 : 0)); break;
            }
            c->a = a; setf(c, W_FLAG_C, cy); set_nz(c, a, false);
        }
        return 2;
    }

    case 0x06: case 0x0E: case 0x16: case 0x1E:   /* ASL mem */
    case 0x26: case 0x2E: case 0x36: case 0x3E:   /* ROL mem */
    case 0x46: case 0x4E: case 0x56: case 0x5E:   /* LSR mem */
    case 0x66: case 0x6E: case 0x76: case 0x7E: { /* ROR mem */
        int kind = op >> 5; /* 0=ASL 1=ROL 2=LSR 3=ROR */
        switch (op & 0x1F) {
            case 0x06: ea = ea_dp(c); break;   case 0x0E: ea = ea_abs(c); break;
            case 0x16: ea = ea_dpx(c); break;  case 0x1E: ea = ea_absx(c); break;
        }
        v = read_op(c, ea, is8m);
        if (is8m) {
            uint8_t r = (uint8_t)v; bool cy;
            switch (kind) {
                case 0: cy = r >> 7; r = (uint8_t)(r << 1); break;
                case 1: cy = r >> 7; r = (uint8_t)((r << 1) | (getf(c, W_FLAG_C) ? 1 : 0)); break;
                case 2: cy = r & 1;  r = (uint8_t)(r >> 1); break;
                default: cy = r & 1; r = (uint8_t)((r >> 1) | (getf(c, W_FLAG_C) ? 0x80 : 0)); break;
            }
            setf(c, W_FLAG_C, cy); write_op(c, ea, r, true); set_nz(c, r, true);
        } else {
            uint16_t r = (uint16_t)v; bool cy;
            switch (kind) {
                case 0: cy = r >> 15; r = (uint16_t)(r << 1); break;
                case 1: cy = r >> 15; r = (uint16_t)((r << 1) | (getf(c, W_FLAG_C) ? 1 : 0)); break;
                case 2: cy = r & 1;   r = (uint16_t)(r >> 1); break;
                default: cy = r & 1;  r = (uint16_t)((r >> 1) | (getf(c, W_FLAG_C) ? 0x8000 : 0)); break;
            }
            setf(c, W_FLAG_C, cy); write_op(c, ea, r, false); set_nz(c, r, false);
        }
        return 6;
    }
    case 0x04: case 0x0C: { /* TSB mem */
        ea = (op == 0x0C) ? ea_abs(c) : ea_dp(c);
        v = read_op(c, ea, is8m);
        setf(c, W_FLAG_Z, (is8m ? (c->a & v) & 0xFF : (c->a & v)) == 0);
        v |= is8m ? (c->a & 0xFF) : c->a;
        write_op(c, ea, v, is8m); return 6;
    }
    case 0x14: case 0x1C: { /* TRB mem */
        ea = (op == 0x1C) ? ea_abs(c) : ea_dp(c);
        v = read_op(c, ea, is8m);
        setf(c, W_FLAG_Z, (is8m ? (c->a & v) & 0xFF : (c->a & v)) == 0);
        v &= is8m ? ~(c->a & 0xFF) : ~c->a;
        write_op(c, ea, v, is8m); return 6;
    }
    case 0x89: { /* BIT # (only Z affected) */
        v = is8m ? f8(c) : f16(c);
        setf(c, W_FLAG_Z, (is8m ? (c->a & v) & 0xFF : (c->a & v)) == 0);
        return 2;
    }

    /* ---- control flow ---- */
    case 0x4C: c->pc = f16(c); return 3;                        /* JMP abs */
    case 0x5C: { uint32_t l = f24(c); c->pb = (uint8_t)(l >> 16); c->pc = (uint16_t)l; } return 4; /* JML long */
    case 0x6C: { uint16_t ind = f16(c); c->pc = rd16(c, ind); } return 5; /* JMP (abs) */
    case 0x20: { /* JSR abs */
        uint8_t lo = f8(c); uint16_t ret = c->pc; uint8_t hi = f8(c);
        pushN(c, (uint32_t)(uint16_t)(ret), false);
        c->pc = (uint16_t)(lo | (hi << 8)); return 6;
    }
    case 0x22: { /* JSL long */
        uint8_t lo = f8(c); uint8_t hi = f8(c); uint8_t bk = f8(c);
        push8(c, c->pb);
        /* 65816: push address of LAST byte of instruction (PC-1); RTL adds 1 */
        pushN(c, (uint32_t)(uint16_t)(c->pc - 1), false);
        c->pb = bk; c->pc = (uint16_t)(lo | (hi << 8)); return 8;
    }
    case 0xFC: { /* JSR (abs,X) */
        uint16_t ind = f16(c); uint16_t ret = (uint16_t)(c->pc - 1);
        pushN(c, ret, false);
        uint16_t target = rd16(c, (uint32_t)(uint16_t)(ind + c->x));
        c->pc = target; return 8;
    }
    case 0x60: c->pc = (uint16_t)(pullN(c, false) + 1); return 6; /* RTS */
    case 0x6B: c->pc = (uint16_t)(pullN(c, false) + 1); c->pb = pull8(c); return 6; /* RTL */
    case 0x40: { /* RTI */
        c->p = pull8(c);
        set_x_width_side_effects(c);
        if (c->emulation) { c->p |= W_FLAG_M | W_FLAG_X; c->p &= (uint8_t)~(W_FLAG_M & 0); }
        c->pc = (uint16_t)pullN(c, false);
        if (!c->emulation) c->pb = pull8(c);
        return 6;
    }
    case 0x80: branch(c, true); return 3;                       /* BRA */
    case 0xF0: branch(c, getf(c, W_FLAG_Z)); return 3;          /* BEQ */
    case 0xD0: branch(c, !getf(c, W_FLAG_Z)); return 3;         /* BNE */
    case 0x90: branch(c, !getf(c, W_FLAG_C)); return 3;         /* BCC */
    case 0xB0: branch(c, getf(c, W_FLAG_C)); return 3;          /* BCS */
    case 0x30: branch(c, getf(c, W_FLAG_N)); return 3;          /* BMI */
    case 0x10: branch(c, !getf(c, W_FLAG_N)); return 3;         /* BPL */
    case 0x50: branch(c, !getf(c, W_FLAG_V)); return 3;         /* BVC */
    case 0x70: branch(c, getf(c, W_FLAG_V)); return 3;          /* BVS */
    case 0x82: { int8_t off = (int8_t)f16(c); c->pc = (uint16_t)(c->pc + off); } return 4; /* BRL */
    case 0x00: case 0x02: { /* BRK / COP */
        f8(c); /* signature byte */
        if (!c->emulation) push8(c, c->pb);
        pushN(c, c->pc, false); push8(c, c->p);
        setf(c, W_FLAG_I, true); setf(c, W_FLAG_D, false);
        c->pb = 0;
        /* emulation mode uses the $FFFx vectors; native mode the $FFEx ones */
        uint16_t vec;
        if (op == 0x00)
            vec = c->emulation ? 0x00FFFE : 0x00FFE6;   /* BRK / IRQ */
        else
            vec = c->emulation ? 0x00FFF4 : 0x00FFE4;   /* COP */
        c->pc = (uint16_t)(rdb(c, vec) | (rdb(c, (uint16_t)(vec + 1)) << 8));
        return 8;
    }

    /* ---- stack ---- */
    case 0x48: pushN(c, c->a, is8m); return 3;  /* PHA */
    case 0x68: c->a = is8m ? (uint16_t)((c->a & 0xFF00) | (pullN(c, true) & 0xFF)) : (uint16_t)pullN(c, false); set_nz(c, is8m ? (c->a & 0xFF) : c->a, is8m); return 4; /* PLA */
    case 0x08: push8(c, c->p); return 3;        /* PHP */
    case 0x28: c->p = pull8(c); set_x_width_side_effects(c); return 4; /* PLP */
    case 0xDA: pushN(c, c->x, is8x); return 3;  /* PHX */
    case 0xFA: c->x = (uint16_t)pullN(c, is8x); set_x_width_side_effects(c); set_nz(c, c->x, is8x); return 4; /* PLX */
    case 0x5A: pushN(c, c->y, is8x); return 3;  /* PHY */
    case 0x7A: c->y = (uint16_t)pullN(c, is8x); set_x_width_side_effects(c); set_nz(c, c->y, is8x); return 4; /* PLY */
    case 0x0B: pushN(c, c->d, false); return 4; /* PHD */
    case 0x2B: c->d = (uint16_t)pullN(c, false); set_nz(c, c->d, false); return 5; /* PLD */
    case 0x8B: push8(c, c->db); return 3;       /* PHB */
    case 0xAB: c->db = pull8(c); set_nz(c, c->db, true); return 4; /* PLB */

    /* ---- P / flags ---- */
    case 0xE2: c->p |= f8(c); set_x_width_side_effects(c); return 3;  /* SEP */
    case 0xC2: c->p &= (uint8_t)~f8(c); set_x_width_side_effects(c); return 3; /* REP */
    case 0x18: setf(c, W_FLAG_C, false); return 2; /* CLC */
    case 0x38: setf(c, W_FLAG_C, true);  return 2; /* SEC */
    case 0x58: setf(c, W_FLAG_I, false); return 2; /* CLI */
    case 0x78: setf(c, W_FLAG_I, true);  return 2; /* SEI */
    case 0xD8: setf(c, W_FLAG_D, false); return 2; /* CLD */
    case 0xF8: setf(c, W_FLAG_D, true);  return 2; /* SED */
    case 0xB8: setf(c, W_FLAG_V, false); return 2; /* CLV */

    /* ---- misc ---- */
    case 0xEB: { uint16_t lo = c->a & 0xFF, hi = c->a >> 8; c->a = (uint16_t)((lo << 8) | hi); set_nz(c, c->a >> 8, true); } return 3; /* XBA */
    case 0xFB: { bool e = c->emulation; c->emulation = getf(c, W_FLAG_C); setf(c, W_FLAG_C, e ? 1 : 0);
                 if (c->emulation) { setf(c, W_FLAG_M, true); setf(c, W_FLAG_X, true); c->s = (uint16_t)(0x0100 | (c->s & 0xFF)); set_x_width_side_effects(c); }
               } return 3; /* XCE */
    case 0xCB: c->waiting = true; return 3; /* WAI */
    case 0xDB: c->stopped = true; return 3; /* STP */
    case 0xEA: return 2; /* NOP */
    case 0x54: case 0x44: { /* MVN / MVP block move */
        uint8_t dstBank = f8(c), srcBank = f8(c);
        c->db = dstBank;
        if (op == 0x54) { wrb(c, ((uint32_t)dstBank << 16) | c->y, rdb(c, ((uint32_t)srcBank << 16) | c->x));
                          c->x = (uint16_t)(c->x + 1); c->y = (uint16_t)(c->y + 1); }
        else            { wrb(c, ((uint32_t)dstBank << 16) | c->y, rdb(c, ((uint32_t)srcBank << 16) | c->x));
                          c->x = (uint16_t)(c->x - 1); c->y = (uint16_t)(c->y - 1); }
        c->a = (uint16_t)(c->a - 1);
        if (c->a != 0xFFFF) c->pc = (uint16_t)(c->pc - 3); /* repeat */
        return 7;
    }

    default: return 2; /* unimplemented opcode -> NOP */
    }
}

void w_nmi(w_cpu *c) {
    /* NMI is non-maskable. Vector: $FFFA (emulation) / $FFEA (native). */
    if (c->stopped) return;
    c->waiting = false;
    if (!c->emulation) push8(c, c->pb);
    pushN(c, c->pc, false); push8(c, c->p);
    setf(c, W_FLAG_I, true); setf(c, W_FLAG_D, false);
    c->pb = 0;
    uint16_t v = c->emulation ? 0x00FFFA : 0x00FFEA;
    c->pc = (uint16_t)(rdb(c, v) | (rdb(c, (uint16_t)(v + 1)) << 8));
}
void w_irq(w_cpu *c) {
    if (getf(c, W_FLAG_I)) return;              /* maskable via I */
    if (c->stopped) return;
    c->waiting = false;                          /* IRQ wakes WAI */
    if (!c->emulation) push8(c, c->pb);
    pushN(c, c->pc, false); push8(c, c->p);
    setf(c, W_FLAG_I, true); setf(c, W_FLAG_D, false);
    c->pb = 0;
    uint16_t v = c->emulation ? 0x00FFFE : 0x00FFEE;
    c->pc = (uint16_t)(rdb(c, v) | (rdb(c, (uint16_t)(v + 1)) << 8));
}

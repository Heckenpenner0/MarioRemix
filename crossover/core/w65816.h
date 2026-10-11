/* w65816.h - WDC 65C816 CPU core (the SNES application CPU).
 * Part of the crossover engine. Portable C99, no external deps.
 * Drives the Super Mario World side of the crossover.
 *
 * Coverage note: this implements the instruction/addressing-mode framework and
 * the core opcode set (loads/stores, ALU, shifts, control flow, stack, P ops,
 * 8/16-bit M/X widths, emulation/native modes). It is structured so remaining
 * opcodes slot in cleanly. See tests for what is verified today.
 */
#ifndef W65816_H
#define W65816_H

#include <stdint.h>
#include <stdbool.h>

/* 24-bit address bus callbacks (addr = PB/DB-bank <<16 | offset). */
typedef uint8_t (*w_read_fn)(void *ctx, uint32_t addr24);
typedef void    (*w_write_fn)(void *ctx, uint32_t addr24, uint8_t val);

/* Status register (P) bits. */
enum {
    W_FLAG_C = 0x01, /* carry / not borrow */
    W_FLAG_Z = 0x02, /* zero */
    W_FLAG_I = 0x04, /* IRQ disable */
    W_FLAG_D = 0x08, /* decimal mode */
    W_FLAG_X = 0x10, /* index register width (1 = 8-bit) */
    W_FLAG_M = 0x20, /* accumulator width  (1 = 8-bit) */
    W_FLAG_V = 0x40, /* overflow */
    W_FLAG_N = 0x80  /* negative */
};

typedef struct {
    /* A is the 16-bit accumulator (only low 8 used when M=1).
     * X, Y are 16-bit index regs (only low 8 used when X=1). */
    uint16_t a, x, y;
    uint16_t s;   /* stack pointer */
    uint16_t d;   /* direct page */
    uint8_t  db;  /* data bank */
    uint8_t  pb;  /* program bank */
    uint16_t pc;  /* program counter (offset within pb) */
    uint8_t  p;   /* status */
    bool     emulation; /* 6502 emulation mode (E=1) */
    bool     stopped;   /* WAI/STP awaiting */
    bool     waiting;   /* WAI */

    w_read_fn  read;
    w_write_fn write;
    void      *ctx;
} w_cpu;

void w_init(w_cpu *c, w_read_fn read, w_write_fn write, void *ctx);
void w_reset(w_cpu *c);

/* Execute one instruction. Returns cycles consumed (approximate). */
int w_step(w_cpu *c);

/* Hardware interrupt entry. */
void w_nmi(w_cpu *c);
void w_irq(w_cpu *c);

#endif /* W65816_H */

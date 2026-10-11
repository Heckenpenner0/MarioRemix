/* lr35902.h - Game Boy CPU (Sharp LR35902, a modified Z80) core.
 * Part of the crossover engine. Portable C99, no external deps.
 * This drives the Pokemon Red side of the crossover.
 */
#ifndef LR35902_H
#define LR35902_H

#include <stdint.h>
#include <stdbool.h>

/* Memory access callbacks so the core is testable and bus-agnostic.
 * `ctx` is an opaque pointer you provide (a flat RAM array in tests,
 * the full Game Boy bus in the real machine). */
typedef uint8_t (*lr_read_fn)(void *ctx, uint16_t addr);
typedef void    (*lr_write_fn)(void *ctx, uint16_t addr, uint8_t val);

typedef struct {
    /* 8-bit registers. AF/BC/DE/HL are overlaid via unions. */
    union { struct { uint8_t f, a; }; uint16_t af; };
    union { struct { uint8_t c, b; }; uint16_t bc; };
    union { struct { uint8_t e, d; }; uint16_t de; };
    union { struct { uint8_t l, h; }; uint16_t hl; };
    uint16_t sp;
    uint16_t pc;

    bool ime;         /* interrupt master enable */
    bool ime_pending; /* EI takes effect after the next instruction */
    bool halted;      /* CPU halted (HALT) */
    bool stopped;     /* CPU stopped (STOP) */

    lr_read_fn  read;
    lr_write_fn write;
    void       *ctx;
} lr_cpu;

/* Flag bits in the F register (low nibble is always 0). */
enum {
    LR_FLAG_Z = 0x80, /* zero */
    LR_FLAG_N = 0x40, /* subtract */
    LR_FLAG_H = 0x20, /* half-carry */
    LR_FLAG_C = 0x10  /* carry */
};

/* Initialise a CPU with a memory bus. Does not reset; call lr_reset. */
void lr_init(lr_cpu *cpu, lr_read_fn read, lr_write_fn write, void *ctx);

/* Power-on / reset state. */
void lr_reset(lr_cpu *cpu);

/* Execute one instruction. Returns the number of clock cycles (T-states)
 * consumed, in the 4..24 range. Returns 4 when halted with no work. */
int lr_step(lr_cpu *cpu);

/* Cause an interrupt dispatch (jump to `vec`), honouring IME. Returns true
 * if the interrupt was actually taken. */
bool lr_interrupt(lr_cpu *cpu, uint16_t vec);

#endif /* LR35902_H */

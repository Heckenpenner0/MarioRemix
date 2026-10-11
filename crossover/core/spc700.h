/* spc700.h - SPC700 audio CPU core (the SNES S-SMP).
 * Part of the crossover engine. Portable C99, no external deps.
 * 8-bit CPU, 64KB address space, flags N V P B H I Z C.
 */
#ifndef SPC700_H
#define SPC700_H

#include <stdint.h>
#include <stdbool.h>

/* PSW flag bits. */
enum {
    SPC_FLAG_C = 0x01,  /* carry */
    SPC_FLAG_Z = 0x02,  /* zero */
    SPC_FLAG_I = 0x04,  /* interrupt disable */
    SPC_FLAG_H = 0x08,  /* half carry */
    SPC_FLAG_B = 0x10,  /* break */
    SPC_FLAG_P = 0x20,  /* direct page select (0 = page 0, 1 = page 1) */
    SPC_FLAG_V = 0x40,  /* overflow */
    SPC_FLAG_N = 0x80   /* negative */
};

typedef uint8_t (*spc_read_fn)(void *ctx, uint16_t addr);
typedef void    (*spc_write_fn)(void *ctx, uint16_t addr, uint8_t val);

typedef struct {
    uint8_t  a, x, y, sp;
    uint16_t pc;
    uint8_t  psw;
    bool     stopped;     /* STOP/SLEEP until reset */
    spc_read_fn  read;
    spc_write_fn write;
    void    *ctx;
} spc_cpu;

void spc_init(spc_cpu *c, spc_read_fn read, spc_write_fn write, void *ctx);
void spc_reset(spc_cpu *c);

/* Execute one instruction. Returns SPC700 CPU cycles consumed. */
int  spc_step(spc_cpu *c);

#endif /* SPC700_H */

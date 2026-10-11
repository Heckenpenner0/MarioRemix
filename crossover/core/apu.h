/* apu.h - the SNES audio subsystem: SPC700 + 64KB ARAM + IPL boot ROM +
 * DSP (8 voices, BRR samples, ADSR, echo).
 *
 * Part of the crossover engine. Portable C99.
 */
#ifndef APU_H
#define APU_H

#include <stdint.h>
#include <stdbool.h>
#include "spc700.h"

#define DSP_VOICES 8

typedef struct {
    bool     active;
    int8_t   vol_l, vol_r;
    uint16_t pitch;
    uint8_t  srcn;
    uint8_t  adsr1, adsr2, gain;
    uint8_t  envx_out;
    int16_t  outx;

    /* BRR playback */
    int16_t  decoded[16];
    int      buf_pos;
    uint32_t phase;
    uint8_t  brr_header;
    uint16_t sample_ptr;
    uint16_t loop_ptr;
    int16_t  s1, s2;

    /* envelope (11-bit, 0..0x7FF) */
    int      env;
    int      env_state;      /* 0 attack, 1 decay, 2 sustain, 3 release, 4 off */
    int      env_counter;
    int      env_rate;
    int      sustain_level;

    bool     noise;
    bool     echo;
    bool     keyed_on;
} dsp_voice;

typedef struct {
    uint8_t   regs[128];
    dsp_voice v[DSP_VOICES];
    uint16_t  noise_lfsr;
    int16_t   fir_l[8], fir_r[8];   /* echo FIR history */
    int       fir_pos;
    uint16_t  echo_ptr;             /* ring buffer word address */
} dsp;

typedef struct {
    uint8_t  aram[0x10000];
    spc_cpu  cpu;
    dsp      dsp;
    uint8_t  control;               /* $F1 */
    uint8_t  dsp_addr;              /* $F2 */
    uint8_t  in_port[4];            /* written by SNES, read by SPC at $F4-$F7 */
    uint8_t  out_port[4];           /* written by SPC, read by SNES at $2140-$2143 */
    int      sample_phase;          /* SPC cycles toward the next 32kHz sample */
    uint8_t  timer_target[3];       /* $FA/$FB/$FC: timer divider target */
    uint8_t  timer_stage1[3];       /* internal divider counter */
    uint8_t  timer_out[3];          /* $FD/$FE/$FF: 4-bit output counter (read clears) */
    uint16_t timer_div[3];          /* cycles toward the next timer tick */
    uint8_t  timer_en;              /* previous $F1 enable bits, for 0->1 edge detect */
    bool     ipl_enabled;
    uint64_t cycles;
} apu;

/* Initialise. `ipl` enables the boot ROM overlay at $FFC0-$FFFF. */
void apu_init(apu *a, bool ipl);

/* SNES side: $2140-$2143 (port 0-3). */
uint8_t apu_snes_read(const apu *a, int port);
void    apu_snes_write(apu *a, int port, uint8_t val);

/* Run the APU for `cycles` SPC700 cycles, appending up to `max_frames`
 * interleaved-stereo int16 samples into `out`. Returns frames written. */
int apu_run(apu *a, int cycles, int16_t *out, int max_frames);

/* DSP register access (via SPC $F2/$F3). Exposed for tests. */
uint8_t apu_dsp_read(const apu *a, int reg);
void    apu_dsp_write(apu *a, int reg, uint8_t val);

/* Decode one 9-byte BRR block into 16 samples, applying the predictor filter.
 * `s1`/`s2` are the previous two decoded samples (updated in place). */
void dsp_decode_brr(const uint8_t *brr, int16_t *out, int16_t *s1, int16_t *s2);

/* Boot ROM image (64 bytes at $FFC0-$FFFF). */
extern const uint8_t spc_ipl_rom[64];

#endif /* APU_H */

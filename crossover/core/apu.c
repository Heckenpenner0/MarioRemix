/* apu.c - SNES APU implementation (SPC700 + ARAM + IPL ROM + DSP). See apu.h. */
#include "apu.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- IPL boot ROM ($FFC0-$FFFF) ---- */
const uint8_t spc_ipl_rom[64] = {
    0xCD,0xEF,0xBD,0xE8,0x00,0xC6,0x1D,0xD0,0xFC,0x8F,0xAA,0xF4,0x8F,0xBB,0xF5,0x78,
    0xCC,0xF4,0xD0,0xFB,0x2F,0x19,0xEB,0xF4,0xD0,0xFC,0x7E,0xF4,0xD0,0x0B,0xE4,0xF5,
    0xCB,0xF4,0xD7,0x00,0xFC,0xD0,0xF3,0xAB,0x01,0x10,0xEF,0x7E,0xF4,0x10,0xEB,0xBA,
    0xF6,0xDA,0x00,0xBA,0xF4,0xC4,0xF4,0xDD,0x5D,0xD0,0xDB,0x1F,0x00,0x00,0xC0,0xFF
};

/* ---- BRR decoding ---- */
void dsp_decode_brr(const uint8_t *brr, int16_t *out, int16_t *s1, int16_t *s2) {
    uint8_t header = brr[0];
    int range  = header >> 4;
    int filter = (header >> 2) & 3;
    for (int i = 0; i < 16; i++) {
        uint8_t byte = brr[1 + (i >> 1)];
        int nib = (i & 1) ? (byte & 0x0F) : (byte >> 4);
        if (nib >= 8) nib -= 16;
        int32_t s;
        if (range <= 12) s = (nib << range) >> 1;
        else             s = (nib >> 3) << 11;
        int32_t p1 = *s1, p2 = *s2;
        switch (filter) {
            case 0: break;
            case 1: s += p1 + ((-p1) >> 4); break;
            case 2: s += (p1 << 1) + (-((p1 << 1) + p1) >> 5) - p2 + (p2 >> 4); break;
            default: s += (p1 << 1) + (-(p1 + (p1 << 2) + (p1 << 3)) >> 6) - p2 + (((p2 << 1) + p2) >> 4); break;
        }
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
        *s2 = *s1;
        *s1 = (int16_t)(s & ~1);   /* the DSP keeps a 15-bit sample for history */
    }
}

/* ---- envelope rate table (samples per envelope step), indexed 0..31 ---- */
static const uint16_t env_period[32] = {
    30720, 2048, 1536, 1280, 1024, 768, 640, 512,
     384,  320,  256,  192,  160,  128,  96,  80,
      64,   48,   40,   32,   24,   20,  16,  12,
      10,    8,    6,    5,    4,    3,   2,   1
};

/* ---- DSP register write ---- */
void apu_dsp_write(apu *a, int reg, uint8_t val) {
    reg &= 0x7F;
    a->dsp.regs[reg] = val;
    int ch = reg >> 4, idx = reg & 0x0F;
    dsp_voice *v = &a->dsp.v[ch];
    switch (idx) {
        case 0x0: v->vol_l = (int8_t)val; break;
        case 0x1: v->vol_r = (int8_t)val; break;
        case 0x2: v->pitch = (uint16_t)((v->pitch & 0x3F00) | val); break;
        case 0x3: v->pitch = (uint16_t)((v->pitch & 0x00FF) | ((val & 0x3F) << 8)); break;
        case 0x4: v->srcn = val; break;
        case 0x5: v->adsr1 = val; break;
        case 0x6: v->adsr2 = val; break;
        case 0x7: v->gain = val; break;
        default: break;
    }
    switch (reg) {
        case 0x4C: {                                   /* KON */
            for (int i = 0; i < 8; i++) if (val & (1 << i)) {
                dsp_voice *kv = &a->dsp.v[i];
                uint16_t dir = (uint16_t)(a->dsp.regs[0x5D] << 8);
                uint16_t entry = (uint16_t)(dir + kv->srcn * 4);
                uint16_t start = (uint16_t)(a->aram[entry] | (a->aram[(entry + 1) & 0xFFFF] << 8));
                kv->loop_ptr  = (uint16_t)(a->aram[(entry + 2) & 0xFFFF] | (a->aram[(entry + 3) & 0xFFFF] << 8));
                kv->sample_ptr = start;
                kv->s1 = kv->s2 = 0;
                kv->phase = 0;
                kv->buf_pos = 16;              /* force a block fetch */
                kv->env = 0; kv->env_state = 0; kv->env_counter = 0;
                kv->active = true; kv->keyed_on = true;
            }
            break;
        }
        case 0x5C: {                                   /* KOFF */
            for (int i = 0; i < 8; i++) if (val & (1 << i)) a->dsp.v[i].env_state = 3;
            break;
        }
        case 0x7C:                                     /* ENDX: writing clears */
            break;
        default: break;
    }
}

uint8_t apu_dsp_read(const apu *a, int reg) {
    reg &= 0x7F;
    int ch = reg >> 4, idx = reg & 0x0F;
    if (ch < 8) {
        const dsp_voice *v = &a->dsp.v[ch];
        switch (idx) {
            case 0x8: return (uint8_t)((v->env >> 4) & 0xFF);      /* ENVX */
            case 0x9: return (uint8_t)(v->outx >> 8);              /* OUTX */
            default: break;
        }
    }
    return a->dsp.regs[reg];
}

/* ---- SPC700 bus ---- */
static uint8_t apu_read(void *ctx, uint16_t addr) {
    apu *a = (apu*)ctx;
    if (addr >= 0x00F0 && addr <= 0x00FF) {
        switch (addr) {
            case 0x00F2: return a->dsp_addr;
            case 0x00F3: return apu_dsp_read(a, a->dsp_addr);
            case 0x00F4: return a->in_port[0];
            case 0x00F5: if (getenv("PORT_TRACE")) fprintf(stderr, "[R F5 v=%02X spc=%04X]", a->in_port[1], a->cpu.pc); return a->in_port[1];
            case 0x00F6: return a->in_port[2];
            case 0x00F7: return a->in_port[3];
            case 0x00FD: case 0x00FE: case 0x00FF: {
                int i = (int)(addr - 0x00FD);
                uint8_t tv = a->timer_out[i];
                if (getenv("FD_TRACE")) fprintf(stderr, "[FD read i=%d tv=%02X spc=%04X]", i, tv, a->cpu.pc);
                a->timer_out[i] = 0;
                return tv;
            }
            default: return a->aram[addr];
        }
    }
    if (addr >= 0xFFC0 && (a->control & 0x80)) return spc_ipl_rom[addr - 0xFFC0];
    return a->aram[addr];
}

static void apu_write(void *ctx, uint16_t addr, uint8_t val) {
    apu *a = (apu*)ctx;
    if (addr >= 0x00F0 && addr <= 0x00FF) {
        a->aram[addr] = val;                      /* writes also land in RAM */
        switch (addr) {
            case 0x00F1:
                a->control = val;
                for (int ti = 0; ti < 3; ti++) {
                    if ((val & (1 << ti)) && !(a->timer_en & (1 << ti))) {
                        a->timer_stage1[ti] = 0; a->timer_out[ti] = 0; a->timer_div[ti] = 0;
                    }
                }
                a->timer_en = val & 0x07;
                if (val & 0x10) { a->in_port[0] = 0; a->in_port[1] = 0; }
                if (val & 0x20) { a->in_port[2] = 0; a->in_port[3] = 0; }
                return;
            case 0x00F2: a->dsp_addr = val; return;
            case 0x00F3: apu_dsp_write(a, a->dsp_addr, val); return;
            case 0x00FA: a->timer_target[0] = val; return;
            case 0x00FB: a->timer_target[1] = val; return;
            case 0x00FC: a->timer_target[2] = val; return;
            case 0x00F4: if (getenv("APU_TRACE")) fprintf(stderr,"  S->out0=%02X spc=%04X (in0=%02X)\n",val,a->cpu.pc,a->in_port[0]); a->out_port[0] = val; return;
            case 0x00F5: a->out_port[1] = val; return;
            case 0x00F6: a->out_port[2] = val; return;
            case 0x00F7: a->out_port[3] = val; return;
            default: return;
        }
    }
    a->aram[addr] = val;
}

/* ---- SNES side ---- */
uint8_t apu_snes_read(const apu *a, int port) {
    return a->out_port[port & 3];
}
void apu_snes_write(apu *a, int port, uint8_t val) {
    if (getenv("APU_TRACE")) fprintf(stderr,"C->p%d=%02X (in0=%02X out0=%02X spc=%04X)\n",port,val,a->in_port[0],a->out_port[0],a->cpu.pc);
    a->in_port[port & 3] = val;
}

void apu_init(apu *a, bool ipl) {
    memset(a, 0, sizeof *a);
    a->ipl_enabled = ipl;
    a->control = ipl ? 0x80 : 0x00;
    a->dsp.noise_lfsr = 0x4000;
    a->dsp.regs[0x6C] = 0x20;              /* FLG: echo disabled until enabled by the program */
    for (int i = 0; i < 8; i++) a->dsp.v[i].buf_pos = 16;
    spc_init(&a->cpu, apu_read, apu_write, a);
}

/* ---- per-voice sample fetch ---- */
static void voice_fetch_block(apu *a, dsp_voice *v) {
    uint8_t blk[9];
    for (int i = 0; i < 9; i++) blk[i] = a->aram[(v->sample_ptr + i) & 0xFFFF];
    v->brr_header = blk[0];
    dsp_decode_brr(blk, v->decoded, &v->s1, &v->s2);
    v->buf_pos = 0;
    v->sample_ptr = (uint16_t)((v->sample_ptr + 9) & 0xFFFF);
    bool end  = (blk[0] & 0x01) != 0;
    bool loop = (blk[0] & 0x02) != 0;
    if (end) {
        a->dsp.regs[0x7C] |= (uint8_t)(1 << v->srcn);   /* ENDX is per-sample; use voice-ish flag */
        if (loop) v->sample_ptr = v->loop_ptr;
        else      v->env_state = 3;                     /* end without loop -> release */
    }
}

/* ---- envelope ---- */
static void voice_envelope(dsp_voice *v) {
    if (!v->active) return;
    if (v->env_state == 4) return;
    if (!(v->adsr1 & 0x80)) {                            /* GAIN mode */
        if (!(v->gain & 0x80)) { v->env = (v->gain & 0x7F) << 4; return; }
        int period = env_period[v->gain & 0x1F];
        if (++v->env_counter < period) return;
        v->env_counter = 0;
        int mode = (v->gain >> 5) & 3;
        if (mode == 0) v->env -= (v->env > 0x1F) ? 0x20 : 0;         /* linear decrease */
        else if (mode == 1) v->env -= ((v->env >> 8) + 1);           /* exponential decrease */
        else if (mode == 2) v->env += 0x20;                          /* linear increase */
        else v->env += ((v->env < 0x600) ? 0x20 : 0x08);             /* bent increase */
        if (v->env > 0x7FF) v->env = 0x7FF;
        if (v->env < 0) v->env = 0;
        return;
    }
    switch (v->env_state) {
        case 0: v->env_rate = v->adsr1 & 0x0F; break;                     /* attack  */
        case 1: v->env_rate = 0x10 + (((v->adsr1 >> 4) & 7) << 1); break; /* decay   */
        case 2: v->env_rate = v->adsr2 & 0x1F; break;                     /* sustain */
        default: v->env_rate = 0x1F; break;                               /* release */
    }
    int period = env_period[v->env_rate & 0x1F];
    if (++v->env_counter < period) return;
    v->env_counter = 0;
    switch (v->env_state) {
        case 0:
            v->env += 0x20;
            if (v->env >= 0x7FF) { v->env = 0x7FF; v->env_state = 1; }
            break;
        case 1: {
            int sl = ((v->adsr2 >> 5) & 7) << 8;
            v->env -= ((v->env >> 8) + 1);
            if (v->env <= sl) { v->env = sl; v->env_state = 2; }
            break;
        }
        case 2:
            if (v->adsr2 & 0x20) v->env += 0x20;
            else v->env -= ((v->env >> 8) + 1);
            if (v->env > 0x7FF) v->env = 0x7FF;
            if (v->env < 0) v->env = 0;
            break;
        default:
            v->env -= 8;
            if (v->env <= 0) { v->env = 0; v->env_state = 4; v->active = false; }
            break;
    }
}

/* ---- one stereo sample (32000 Hz) ---- */
static void dsp_sample(apu *a, int16_t *out_l, int16_t *out_r) {
    dsp *d = &a->dsp;
    int32_t mix_l = 0, mix_r = 0;
    int32_t echo_in_l = 0, echo_in_r = 0;

    for (int i = 0; i < 8; i++) {
        dsp_voice *v = &d->v[i];
        if (!v->active) { continue; }
        voice_envelope(v);
        /* advance the BRR position by the pitch amount */
        v->phase += v->pitch;
        int steps = (int)(v->phase >> 12);
        v->phase &= 0xFFF;
        while (steps-- > 0) {
            if (v->buf_pos >= 16) voice_fetch_block(a, v);
            v->buf_pos++;
        }
        int sample;
        if (v->buf_pos == 0) sample = v->decoded[0];
        else sample = v->decoded[(v->buf_pos - 1) & 15];
        if (d->regs[0x3D] & (1 << i)) {                /* noise substitute */
            d->noise_lfsr = (uint16_t)((d->noise_lfsr >> 1) ^ ((d->noise_lfsr & 1) ? 0xB400 : 0));
            sample = (int16_t)(d->noise_lfsr << 1);
        }
        int32_t s = sample;
        int32_t senv = (int32_t)((s * v->env) >> 11);
        v->outx = (int16_t)senv;
        v->envx_out = (uint8_t)(v->env >> 4);
        int32_t sl = (senv * v->vol_l) >> 7;
        int32_t sr = (senv * v->vol_r) >> 7;
        mix_l += sl; mix_r += sr;
        if (d->regs[0x4D] & (1 << i)) { echo_in_l += sl; echo_in_r += sr; }
    }

    /* FIR filter over the echo history */
    int32_t fir_l = 0, fir_r = 0;
    for (int k = 0; k < 8; k++) {
        int idx = (d->fir_pos + k) & 7;
        int8_t coef = (int8_t)d->regs[0x0F + k * 0x10];
        fir_l += d->fir_l[idx] * coef;
        fir_r += d->fir_r[idx] * coef;
    }
    fir_l >>= 7; fir_r >>= 7;
    if (fir_l > 32767) fir_l = 32767;
    if (fir_l < -32768) fir_l = -32768;
    if (fir_r > 32767) fir_r = 32767;
    if (fir_r < -32768) fir_r = -32768;

    /* echo buffer (page ESA, EDL controls length) */
    bool echo_on = (d->regs[0x6C] & 0x20) == 0;
    int efb = (int8_t)d->regs[0x0D];
    d->fir_l[d->fir_pos] = (int16_t)(echo_in_l + ((fir_l * efb) >> 7));
    d->fir_r[d->fir_pos] = (int16_t)(echo_in_r + ((fir_r * efb) >> 7));
    /* The echo ring buffer is only touched while echo is enabled (FLG bit5 = 0).
       Writing it unconditionally would clobber ARAM page 0 at ESA = 0 and
       corrupt the CPU's indirect pointer before any program has configured it. */
    if (echo_on) {
        int edl = (d->regs[0x7D] & 0x0F);
        uint16_t esa = (uint16_t)(d->regs[0x6D] << 8);
        int echo_len = (edl == 0 ? 4 : edl * 2048);   /* one 4-byte entry minimum */
        d->echo_ptr = (uint16_t)((d->echo_ptr + 4) & (echo_len - 1));
        if (d->fir_pos == 0) {
            uint16_t base = (uint16_t)((esa + d->echo_ptr) & 0xFFFF);
            a->aram[(base + 0) & 0xFFFF] = (uint8_t)(d->fir_l[0] & 0xFF);
            a->aram[(base + 1) & 0xFFFF] = (uint8_t)((d->fir_l[0] >> 8) & 0xFF);
            a->aram[(base + 2) & 0xFFFF] = (uint8_t)(d->fir_r[0] & 0xFF);
            a->aram[(base + 3) & 0xFFFF] = (uint8_t)((d->fir_r[0] >> 8) & 0xFF);
        }
    }
    d->fir_pos = (d->fir_pos + 1) & 7;

    int evol_l = (int8_t)d->regs[0x2C], evol_r = (int8_t)d->regs[0x3C];
    mix_l += (fir_l * evol_l) >> 7;
    mix_r += (fir_r * evol_r) >> 7;

    if (d->regs[0x6C] & 0x40) { mix_l = mix_r = 0; }   /* mute */
    int mvol_l = (int8_t)d->regs[0x0C], mvol_r = (int8_t)d->regs[0x1C];
    mix_l = (mix_l * mvol_l) >> 7;
    mix_r = (mix_r * mvol_r) >> 7;
    if (mix_l > 32767) mix_l = 32767;
    if (mix_l < -32768) mix_l = -32768;
    if (mix_r > 32767) mix_r = 32767;
    if (mix_r < -32768) mix_r = -32768;
    *out_l = (int16_t)mix_l;
    *out_r = (int16_t)mix_r;
}

int apu_run(apu *a, int cycles, int16_t *out, int max_frames) {
    int done = 0, frames = 0;
    while (done < cycles) {
        if (getenv("SPC_TRACE")) fprintf(stderr, "%04X ", a->cpu.pc);
        int c = spc_step(&a->cpu);
        done += c;
        for (int ti = 0; ti < 3; ti++) {
            if (!(a->control & (1 << ti))) continue;
            int period = (ti == 2) ? 16 : 128;
            a->timer_div[ti] += c;
            while (a->timer_div[ti] >= period) {
                a->timer_div[ti] -= period;
                a->timer_stage1[ti]++;
                if (a->timer_stage1[ti] == a->timer_target[ti]) {
                    a->timer_stage1[ti] = 0;
                    a->timer_out[ti] = (uint8_t)((a->timer_out[ti] + 1) & 0x0F);
                }
            }
        }
        a->cycles++;
        a->sample_phase += c;                 /* SPC: 1.024 MHz / 32 = 32000 Hz */
        while (a->sample_phase >= 32) {
            a->sample_phase -= 32;
            int16_t l = 0, r = 0;
            dsp_sample(a, &l, &r);
            if (frames < max_frames) { out[frames * 2] = l; out[frames * 2 + 1] = r; frames++; }
        }
    }
    return frames;
}

/* ppu_snes.c - Super Nintendo PPU implementation. See ppu_snes.h. */
#include "ppu_snes.h"
#include <string.h>

/* ---- palette ---- */
void snes_ppu_cgram_rgb(const snes_ppu *p, int index, int *r, int *g, int *b) {
    index &= 0xFF;
    uint16_t c = (uint16_t)(p->cgram[index*2] | (p->cgram[index*2+1] << 8));
    *r = c & 0x1F; *g = (c >> 5) & 0x1F; *b = (c >> 10) & 0x1F;
}

static uint32_t pack_rgb(int r, int g, int b) {
    uint32_t R = (uint32_t)((r & 0x1F) << 3); R |= R >> 5;
    uint32_t G = (uint32_t)((g & 0x1F) << 3); G |= G >> 5;
    uint32_t B = (uint32_t)((b & 0x1F) << 3); B |= B >> 5;
    return 0xFF000000u | (R << 16) | (G << 8) | B;
}

/* ---- bpp per (mode, layer) : layer 0=BG1 .. 3=BG4 ---- */
static int bg_bpp(int mode, int layer) {
    switch (mode) {
        case 0: return 2;
        case 1: return (layer < 2) ? 4 : 2;
        case 2: return (layer < 2) ? 4 : 2;
        case 3: return (layer == 0) ? 8 : (layer == 1 ? 4 : 0);
        case 4: return (layer == 0) ? 8 : (layer == 1 ? 2 : 0);
        default: return 0; /* modes 5-7 not yet rendered as tile BG */
    }
}

/* char base (in bytes) for a BG layer */
static uint32_t bg_char_base(const snes_ppu *p, int layer) {
    int nba = (layer < 2) ? p->bg12nba : p->bg34nba;
    int nib = (layer & 1) ? (nba >> 4) : (nba & 0x0F);
    return (uint32_t)nib * 0x2000;   /* CHR base in 4 KiW (0x2000-byte) steps */
}

/* fetch one pixel color (0..2^bpp-1) from a tile */
static unsigned tile_pixel(const snes_ppu *p, uint32_t charbase, int bpp, int row, int col) {
    unsigned c = 0;
    int blocks = bpp / 2;
    for (int b = 0; b < blocks; b++) {
        uint32_t addr = charbase + (uint32_t)b * 16 + (uint32_t)row * 2;
        uint8_t lo = p->vram[(addr) & 0xFFFF];
        uint8_t hi = p->vram[(addr + 1) & 0xFFFF];
        int bit = 7 - col;
        c |= (unsigned)((lo >> bit) & 1) << (b * 2);
        c |= (unsigned)((hi >> bit) & 1) << (b * 2 + 1);
    }
    return c;
}

/* tilemap entry (2 bytes) for BG layer at tile (tx,ty) */
static uint16_t map_entry(const snes_ppu *p, int layer, int tx, int ty) {
    uint32_t base = (uint32_t)(p->bgxsc[layer] >> 2) * 0x800;   /* tilemap base in 1 KiW (0x800-byte) steps */
    int size = p->bgxsc[layer] & 3;
    int w = (size & 1) ? 64 : 32;
    int h = (size & 2) ? 64 : 32;
    tx &= (w - 1); ty &= (h - 1);
    /* screen is built from 32x32 blocks; layout depends on size */
    int block = 0;
    if (w == 64 && h == 64) block = ((ty >= 32) ? 2 : 0) + ((tx >= 32) ? 1 : 0);
    else if (w == 64)       block = (tx >= 32) ? 1 : 0;
    else if (h == 64)       block = (ty >= 32) ? 1 : 0;
    int off = block * 32 * 32 + (ty & 31) * 32 + (tx & 31);
    uint32_t a = (base + (uint32_t)off * 2) & 0xFFFF;
    return (uint16_t)(p->vram[a] | (p->vram[(a + 1) & 0xFFFF] << 8));
}


/* ---- Mode 7 (affine background) ---- */
/* 13-bit signed scroll/centre values */
static int sign13(uint16_t v){ v &= 0x1FFF; return (v & 0x1000) ? (int)v - 0x2000 : (int)v; }

/* Fetch a mode-7 pixel: tilemap is the LOW byte of VRAM words, pixel data the
 * HIGH byte. Tile `t`, pixel (col,row): byte = 2*(t*64 + row*8 + col) + 1. */
static int m7_fetch(const snes_ppu *p, int tile, int col, int row) {
    int pidx = tile * 64 + row * 8 + col;
    return p->vram[(uint32_t)(pidx * 2 + 1) & 0xFFFF];
}

/* Affine-map one screen pixel back to the 1024x1024 mode-7 plane. */
static int mode7_pixel(const snes_ppu *p, int sx, int sy) {
    int32_t A = (int16_t)p->m7a, B = (int16_t)p->m7b, C = (int16_t)p->m7c, D = (int16_t)p->m7d;
    int X = sign13(p->m7x), Y = sign13(p->m7y);
    int H = sign13(p->m7hofs), V = sign13(p->m7vofs);
    if (p->m7sel & 1) sx = 255 - sx;      /* horizontal screen flip */
    if (p->m7sel & 2) sy = 255 - sy;      /* vertical screen flip */
    int dx = sx + H - X;
    int dy = sy + V - Y;
    int32_t u = ((int32_t)X << 8) + A * dx + B * dy;   /* 8 fractional bits */
    int32_t v = ((int32_t)Y << 8) + C * dx + D * dy;
    int px = u >> 8, py = v >> 8;
    if (px < 0 || px >= 1024 || py < 0 || py >= 1024) {
        int sel = (p->m7sel >> 6) & 3;
        if (sel == 2) return 0;                          /* transparent */
        if (sel == 3) return m7_fetch(p, 0, sx & 7, sy & 7);   /* fill w/ tile 0 */
        px &= 1023; py &= 1023;                          /* wrap */
    }
    int mx = (px >> 3) & 127, my = (py >> 3) & 127;
    int tile = p->vram[(uint32_t)((my * 128 + mx) * 2) & 0xFFFF];  /* low byte */
    return m7_fetch(p, tile, px & 7, py & 7);
}

/* ---- offset-per-tile (BG modes 2, 4, 6) ---- */
/* Look up the per-column H/V offset for BG1/BG2 from the BG3 tilemap.
 * Word bits: 15 = H/V select (mode 4 only), 14 = apply to BG2, 13 = apply to BG1,
 * 9-0 = the 10-bit offset. Modes 2/6 take H from the BG3 entry at this tile and
 * V from the entry one tile row lower. */
static void opt_lookup(const snes_ppu *p, int layer, int x, int y,
                       bool *h_en, int *h_val, bool *v_en, int *v_val) {
    *h_en = false; *v_en = false; *h_val = 0; *v_val = 0;
    int mode = p->bgmode & 7;
    if (mode != 2 && mode != 4 && mode != 6) return;
    if (layer > 1) return;                       /* only BG1/BG2 */
    int bx = ((x + p->bghofs[2]) & 0x3FF) >> 3;
    int by = ((y + p->bgvofs[2]) & 0x3FF) >> 3;
    uint16_t e = map_entry(p, 2, bx, by);
    int bit = (layer == 0) ? 0x2000 : 0x4000;
    if (mode == 4) {
        if (!(e & bit)) return;
        if (e & 0x8000) { *v_en = true; *v_val = e & 0x3FF; }
        else            { *h_en = true; *h_val = e & 0x3FF; }
    } else {
        if (e & bit) { *h_en = true; *h_val = e & 0x3FF; }
        uint16_t ev = map_entry(p, 2, bx, by + 1);
        if (ev & bit) { *v_en = true; *v_val = ev & 0x3FF; }
    }
}

/* composite one BG layer pixel; returns 0 if transparent, else CGRAM index.
 * Also outputs priority rank via *prio. */
static int bg_pixel(const snes_ppu *p, int layer, int x, int y, int *prio) {
    int mode = p->bgmode & 7;
    int bpp = bg_bpp(mode, layer);
    if (bpp == 0) return 0;
    uint32_t charbase = bg_char_base(p, layer);
    int px = (x + p->bghofs[layer]) & 0x3FF;
    int py = (y + p->bgvofs[layer]) & 0x3FF;
    /* offset-per-tile replaces the coarse scroll for this column */
    bool h_en, v_en; int h_val, v_val;
    opt_lookup(p, layer, x, y, &h_en, &h_val, &v_en, &v_val);
    if (h_en) px = (h_val & ~7) | (x & 7);
    if (v_en) py = (y + v_val) & 0x3FF;
    int tx = px >> 3, ty = py >> 3;
    uint16_t e = map_entry(p, layer, tx, ty);
    int tile = e & 0x3FF;
    int pal = (e >> 10) & 7;
    int hflip = (e >> 14) & 1, vflip = (e >> 15) & 1;
    int col = px & 7, row = py & 7;
    if (hflip) col = 7 - col;
    if (vflip) row = 7 - row;
    unsigned c = tile_pixel(p, charbase + (uint32_t)tile * (bpp * 8), bpp, row, col);
    if (c == 0) return 0; /* transparent */
    *prio = ((e >> 13) & 1) ? 1 : 0;
    if (bpp == 2) return pal * 4 + (int)c;
    if (bpp == 4) return pal * 16 + (int)c;
    return (int)c; /* 8bpp uses full CGRAM */
}

/* ---- sprites ---- */
static const int sprite_sizes[8][2] = {
    {8,8},{16,16},{32,32},{64,64},{16,32},{32,64},{64,32},{32,16}
};

/* Top-most sprite pixel at (x,y): outputs its CGRAM index and priority rank. */
static void sprite_pixel(const snes_ppu *p, int x, int y, int *out_idx, int *out_rank) {
    int obsel = p->obsel;
    int small = obsel & 7;
    int large = (obsel >> 5) & 7;
    uint32_t namebase = (uint32_t)((obsel >> 3) & 3) * 0x2000;
    int best_idx = 0, best_rank = -1;
    for (int i = 0; i < 128; i++) {
        int ox = p->oam[i*4+0] | ((p->oam[0x200 + (i/4)] >> ((i%4)*2)) & 1) << 8;
        int oy = p->oam[i*4+1];
        int tile = p->oam[i*4+2];
        int attr = p->oam[i*4+3];
        int size_sel = (p->oam[0x200 + (i/4)] >> ((i%4)*2 + 1)) & 1;
        int sz = size_sel ? large : small;
        int w = sprite_sizes[sz][0], h = sprite_sizes[sz][1];
        int sx = ox, sy = oy;
        if (x < sx || x >= sx + w || y < sy || y >= sy + h) continue;
        int pal = (attr >> 1) & 7;
        int sprio = (attr >> 4) & 3;
        int hflip = (attr >> 6) & 1, vflip = (attr >> 7) & 1;
        int col = x - sx, row = y - sy;
        if (hflip) col = w - 1 - col;
        if (vflip) row = h - 1 - row;
        int tcol = col & 7, trow = row & 7;
        int t = tile + (row / 8) * 16 + (col / 8);
        unsigned c = tile_pixel(p, namebase + (uint32_t)(t & 0xFF) * 32, 4, trow, tcol);
        if (c == 0) continue; /* transparent */
        int rank = 16 + (3 - sprio);
        if (rank > best_rank) { best_rank = rank; best_idx = 128 + pal * 16 + (int)c; }
    }
    *out_idx = best_idx; *out_rank = best_rank;
}

/* ---- mask windows ---- */
static bool window_combine(bool in1, bool in2, bool en1, bool en2, int log) {
    if (en1 && en2) {
        switch (log) {
            case 0: return in1 || in2;   /* OR   */
            case 1: return in1 && in2;   /* AND  */
            case 2: return in1 != in2;   /* XOR  */
            default: return in1 == in2;  /* XNOR */
        }
    }
    if (en1) return in1;
    if (en2) return in2;
    return false;                        /* neither window enabled: no mask */
}

/* W1 = [wh0,wh1], W2 = [wh2,wh3], inclusive at both ends. */
static void window_bits(const snes_ppu *p, uint8_t sel, int x,
                        bool *in1, bool *in2, bool *en1, bool *en2) {
    *en2 = (sel & 0x1) != 0; bool inv2 = (sel & 0x2) != 0;
    *en1 = (sel & 0x4) != 0; bool inv1 = (sel & 0x8) != 0;
    bool w1 = (x >= p->wh[0] && x <= p->wh[1]);
    bool w2 = (x >= p->wh[2] && x <= p->wh[3]);
    *in1 = inv1 ? !w1 : w1;
    *in2 = inv2 ? !w2 : w2;
}

/* Window predicate for a graphics layer (0-3 BG1-4, 4 OBJ). */
static bool window_pixel(const snes_ppu *p, int layer, int x) {
    uint8_t sel = (layer < 2) ? (uint8_t)(p->w12sel >> (layer * 4))
                : (layer < 4) ? (uint8_t)(p->w34sel >> ((layer - 2) * 4))
                              : (uint8_t)(p->wobsel & 0x0F);
    bool in1, in2, en1, en2;
    window_bits(p, sel, x, &in1, &in2, &en1, &en2);
    int log = (layer < 4) ? ((p->wbglog >> (layer * 2)) & 3) : (p->wobjlog & 3);
    return window_combine(in1, in2, en1, en2, log);
}

/* Window predicate for the color/math window. */
static bool color_window(const snes_ppu *p, int x) {
    bool in1, in2, en1, en2;
    window_bits(p, (uint8_t)(p->wobsel >> 4), x, &in1, &in2, &en1, &en2);
    return window_combine(in1, in2, en1, en2, (p->wobjlog >> 4) & 3);
}

/* Whether a layer is masked out on the given screen by its window. */
static bool layer_masked(const snes_ppu *p, int layer, int x, bool sub) {
    uint8_t ten = sub ? p->tsw : p->tmw;
    if (!(ten & (1 << layer))) return false;
    return window_pixel(p, layer, x);
}

/* Composite one screen's top pixel: outputs CGRAM index and source layer
 * (0-3 = BG1-4, 4 = OBJ, 5 = backdrop). */
static void eval_screen(const snes_ppu *p, uint8_t en, bool sub, int x, int y,
                        int *out_idx, int *out_layer) {
    int best_idx = 0, best_rank = -1, best_layer = 5;
    int mode = p->bgmode & 7;
    if (mode == 7) {
        if ((en & 0x01) && !layer_masked(p, 0, x, sub)) {
            int idx = mode7_pixel(p, x, y);
            if (idx) { best_idx = idx; best_layer = 0; }
        }
    } else {
        for (int layer = 3; layer >= 0; layer--) {
            if (!(en & (1 << layer))) continue;
            if (layer_masked(p, layer, x, sub)) continue;
            int prio = 0;
            int idx = bg_pixel(p, layer, x, y, &prio);
            if (idx == 0) continue;
            int rank = (prio << 3) | (3 - layer);
            if (rank >= best_rank) { best_rank = rank; best_idx = idx; best_layer = layer; }
        }
    }
    if ((en & 0x10) && !layer_masked(p, 4, x, sub)) {
        int sidx, srank;
        sprite_pixel(p, x, y, &sidx, &srank);
        if (sidx && srank >= best_rank) { best_idx = sidx; best_layer = 4; }
    }
    *out_idx = best_idx; *out_layer = best_layer;
}

void snes_ppu_init(snes_ppu *p) {
    memset(p, 0, sizeof *p);
}

void snes_ppu_render_line(snes_ppu *p, int y) {
    for (int x = 0; x < SNES_W; x++) {
        int m_idx, m_layer, s_idx, s_layer;
        eval_screen(p, p->tm, false, x, y, &m_idx, &m_layer);
        eval_screen(p, p->ts, true,  x, y, &s_idx, &s_layer);

        int r, g, b;
        snes_ppu_cgram_rgb(p, m_idx, &r, &g, &b);

        /* Color math is enabled by the MAIN layer's CGADSUB bit. */
        bool math_en;
        if (m_layer == 5)      math_en = (p->cgadsub & 0x20) != 0;                     /* backdrop */
        else if (m_layer == 4) math_en = (p->cgadsub & 0x10) != 0 && (m_idx & 0x0C) == 0x04; /* OBJ pal 4-7 */
        else                   math_en = (p->cgadsub & (1 << m_layer)) != 0;

        int clip = (p->cgwsel >> 6) & 3;   /* 00 never, 01 outside cw, 10 inside cw, 11 always */
        int prev = (p->cgwsel >> 4) & 3;
        bool cw = color_window(p, x);
        bool clip_black = (clip == 3) || (clip == 1 && !cw) || (clip == 2 && cw);
        bool prevent    = (prev == 3) || (prev == 1 && !cw) || (prev == 2 && cw);
        if (clip_black) { r = g = b = 0; }

        if (math_en && !prevent) {
            bool use_sub  = (p->cgwsel & 0x02) != 0;
            bool half     = (p->cgadsub & 0x40) != 0;
            bool subtract = (p->cgadsub & 0x80) != 0;
            int orr, og, ob;
            if (use_sub && s_layer != 5) {
                snes_ppu_cgram_rgb(p, s_idx, &orr, &og, &ob);
            } else {
                orr = p->coldata[0]; og = p->coldata[1]; ob = p->coldata[2];
                if (use_sub) half = false;  /* transparent sub-screen fallback is not halved */
            }
            if (subtract) { r -= orr; g -= og; b -= ob; }
            else          { r += orr; g += og; b += ob; }
            if (half) { r >>= 1; g >>= 1; b >>= 1; }
            if (r < 0) r = 0;
            if (r > 31) r = 31;
            if (g < 0) g = 0;
            if (g > 31) g = 31;
            if (b < 0) b = 0;
            if (b > 31) b = 31;
        }

        p->shade[y * SNES_W + x] = (uint8_t)m_idx;
        p->fb[y * SNES_W + x] = pack_rgb(r, g, b);
    }
}

void snes_ppu_render_frame(snes_ppu *p) {
    for (int y = 0; y < SNES_H; y++) snes_ppu_render_line(p, y);
}

/* ---- registers ---- */
void snes_ppu_write(snes_ppu *p, uint16_t addr, uint8_t val) {
    switch (addr) {
        case 0x2100: p->inidisp = val; break;
        case 0x2101: p->obsel = val; break;
        case 0x2105: p->bgmode = val; break;
        case 0x2107: case 0x2108: case 0x2109: case 0x210A:
            p->bgxsc[addr - 0x2107] = val; break;
        case 0x210B: p->bg12nba = val; break;
        case 0x210C: p->bg34nba = val; break;
        case 0x210D: case 0x210F: case 0x2111: case 0x2113: { /* BG1-4 HOFS (BG1 also = M7HOFS) */
            int l = (addr - 0x210D) / 2;
            if (!p->bghofs_low[l]) {
                p->bghofs[l] = (uint16_t)((p->bghofs[l] & 0x300) | val);
                if (l == 0) p->m7hofs = (uint16_t)((p->m7hofs & 0xFF00) | val);
                p->bghofs_low[l] = true;
            } else {
                p->bghofs[l] = (uint16_t)((p->bghofs[l] & 0xFF) | ((val & 3) << 8));
                if (l == 0) p->m7hofs = (uint16_t)((p->m7hofs & 0x00FF) | (val << 8));
                p->bghofs_low[l] = false;
            }
        } break;
        case 0x210E: case 0x2110: case 0x2112: case 0x2114: { /* BG1-4 VOFS (BG1 also = M7VOFS) */
            int l = (addr - 0x210E) / 2;
            if (!p->bgvofs_low[l]) {
                p->bgvofs[l] = (uint16_t)((p->bgvofs[l] & 0x300) | val);
                if (l == 0) p->m7vofs = (uint16_t)((p->m7vofs & 0xFF00) | val);
                p->bgvofs_low[l] = true;
            } else {
                p->bgvofs[l] = (uint16_t)((p->bgvofs[l] & 0xFF) | ((val & 3) << 8));
                if (l == 0) p->m7vofs = (uint16_t)((p->m7vofs & 0x00FF) | (val << 8));
                p->bgvofs_low[l] = false;
            }
        } break;
        case 0x211A: p->m7sel = val; break;                       /* M7SEL */
        case 0x211B: case 0x211C: case 0x211D:
        case 0x211E: case 0x211F: case 0x2120: {                  /* M7A..M7D, M7X, M7Y */
            int i = addr - 0x211B;
            uint16_t *r = (i == 0) ? &p->m7a : (i == 1) ? &p->m7b : (i == 2) ? &p->m7c :
                          (i == 3) ? &p->m7d : (i == 4) ? &p->m7x : &p->m7y;
            if (!p->m7_low[i]) { *r = (uint16_t)((*r & 0xFF00) | val); p->m7_low[i] = true; }
            else               { *r = (uint16_t)((*r & 0x00FF) | (val << 8)); p->m7_low[i] = false; }
        } break;
        case 0x2121: p->cgadd = val; p->cg_have_low = false; break;
        case 0x2122: /* CGDATA double write: low then high */
            if (!p->cg_have_low) { p->cgram[(p->cgadd & 0xFF)*2] = val; p->cg_have_low = true; }
            else { p->cgram[(p->cgadd & 0xFF)*2 + 1] = val & 0x7F; p->cg_have_low = false; p->cgadd++; }
            break;
        case 0x2123: p->w12sel = val; break;
        case 0x2124: p->w34sel = val; break;
        case 0x2125: p->wobsel = val; break;
        case 0x2126: p->wh[0] = val; break;
        case 0x2127: p->wh[1] = val; break;
        case 0x2128: p->wh[2] = val; break;
        case 0x2129: p->wh[3] = val; break;
        case 0x212A: p->wbglog = val; break;
        case 0x212B: p->wobjlog = val; break;
        case 0x212C: p->tm = val; break;
        case 0x212D: p->ts = val; break;
        case 0x212E: p->tmw = val; break;
        case 0x212F: p->tsw = val; break;
        case 0x2130: p->cgwsel = val; break;
        case 0x2131: p->cgadsub = val; break;
        case 0x2132:                                   /* COLDATA: bit5/6/7 = R/G/B */
            if (val & 0x20) p->coldata[0] = (uint8_t)(val & 0x1F);
            if (val & 0x40) p->coldata[1] = (uint8_t)(val & 0x1F);
            if (val & 0x80) p->coldata[2] = (uint8_t)(val & 0x1F);
            break;
        default: break;
    }
}

uint8_t snes_ppu_read(const snes_ppu *p, uint16_t addr) {
    switch (addr) {
        case 0x2100: return p->inidisp;
        case 0x2101: return p->obsel;
        case 0x2105: return p->bgmode;
        case 0x211A: return p->m7sel;
        case 0x2121: return p->cgadd;
        case 0x212C: return p->tm;
        case 0x2130: return p->cgwsel;
        case 0x2131: return p->cgadsub;
        default: return 0;
    }
}

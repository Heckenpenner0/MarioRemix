/* ppu_gb.c - Game Boy PPU implementation. See ppu_gb.h. */
#include "ppu_gb.h"
#include <string.h>

/* Resolve one tile pixel color index (0..3) from VRAM.
 * tile_id: 8-bit tile index; signed_mode: LCDC bit4 clear -> 0x8800 base.
 * row/col: within-tile coordinates (row 0..7 top->bottom, col 0..7 left->right),
 * already flip-adjusted. */
static int tile_pixel(const gb_ppu *p, int tile_id, int row, int col,
                      bool signed_mode) {
    int base;
    if (signed_mode) base = 0x1000 + (int8_t)(uint8_t)tile_id * 16; /* 0x8800 */
    else             base = tile_id * 16;                            /* 0x8000 */
    int addr = base + row * 2;
    uint8_t lo = p->vram[addr & 0x1FFF];
    uint8_t hi = p->vram[(addr + 1) & 0x1FFF];
    int bit = 7 - col;
    return (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);
}

/* Map a 2-bit color through a BGP/OBP palette register. */
static int pal_lookup(uint8_t palreg, int color) {
    return (palreg >> (color * 2)) & 3;
}

/* Fetch a background/window color index for the given tile map + coords. */
static int bg_color_at(const gb_ppu *p, int map_base, bool signed_mode,
                       int px, int py, int *raw_color) {
    int tile_col = (px >> 3) & 31;
    int tile_row = (py >> 3) & 31;
    int map = map_base + tile_row * 32 + tile_col;
    int tile_id = p->vram[map & 0x1FFF];
    int col = px & 7, row = py & 7;
    int c = tile_pixel(p, tile_id, row, col, signed_mode);
    *raw_color = c;
    return pal_lookup(p->bgp, c);
}

void gb_ppu_init(gb_ppu *p) {
    memset(p, 0, sizeof *p);
    /* grayscale palette (shade 0 = lightest .. 3 = darkest) */
    p->palette[0] = 0xFFE0F8D0u;
    p->palette[1] = 0xFF88C070u;
    p->palette[2] = 0xFF346856u;
    p->palette[3] = 0xFF081820u;
    p->lcdc = LCDC_ENABLE | LCDC_BG_ENABLE | LCDC_TILE_DATA;
    p->bgp = 0xE4; /* 11 10 01 00 -> identity */
}

/* Background + window into shade[] for one line. Also fills bg_raw[] (pre-BGP
 * color 0..3) and bg_prio[] (whether that pixel is OBJ-behind-eligible). */
static void render_bg_window(gb_ppu *p, int y, int *bg_raw) {
    bool signed_mode = !(p->lcdc & LCDC_TILE_DATA);
    int bg_map = (p->lcdc & LCDC_BG_MAP) ? 0x1C00 : 0x1800;   /* 0x9C00/0x9800 */
    int win_map = (p->lcdc & LCDC_WIN_MAP) ? 0x1C00 : 0x1800;
    bool bg_on = (p->lcdc & LCDC_BG_ENABLE) != 0;
    bool win_on = (p->lcdc & LCDC_WIN_ENABLE) != 0 && y >= p->wy;

    for (int x = 0; x < GB_W; x++) {
        int raw = 0, color = 0;

        /* window takes priority where active */
        int wx = p->wx - 7;
        bool use_win = win_on && x >= wx;
        if (use_win) {
            color = bg_color_at(p, win_map, signed_mode, x - wx, p->win_line, &raw);
        } else if (bg_on) {
            int px = (p->scx + x) & 0xFF;
            int py = (p->scy + y) & 0xFF;
            color = bg_color_at(p, bg_map, signed_mode, px, py, &raw);
        } else {
            raw = 0; color = pal_lookup(p->bgp, 0);
        }
        p->shade[y * GB_W + x] = (uint8_t)color;
        bg_raw[x] = raw;
    }
    if (win_on) p->win_line++;
}

/* Render up to 10 sprites over one line. bg_raw[] used for priority. */
static void render_sprites(gb_ppu *p, int y, const int *bg_raw) {
    if (!(p->lcdc & LCDC_OBJ_ENABLE)) return;
    int height = (p->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    int drawn = 0;

    /* DMG priority: lower X first; ties -> lower OAM index first. Collect then
     * paint back-to-front so the highest-priority sprite wins on overlaps. */
    int sel[10], selx[10], n = 0;
    for (int i = 0; i < 40 && n < 10; i++) {
        int sy = p->oam[i*4+0] - 16;
        if (y < sy || y >= sy + height) continue;
        int sx = p->oam[i*4+1] - 8;
        /* insert ordered by (x, index) so sel[0] is highest priority */
        int j = n;
        while (j > 0 && selx[j-1] > sx) { sel[j] = sel[j-1]; selx[j] = selx[j-1]; j--; }
        sel[j] = i; selx[j] = sx; n++;
    }
    /* paint lowest priority first, highest last (priority index n-1 -> 0) */
    for (int k = n - 1; k >= 0; k--) {
        int i = sel[k];
        int sy = p->oam[i*4+0] - 16;
        int sx = p->oam[i*4+1] - 8;
        int tile = p->oam[i*4+2];
        int attr = p->oam[i*4+3];
        bool yflip = attr & 0x40, xflip = attr & 0x20;
        bool behind_bg = attr & 0x80;
        uint8_t pal = (attr & 0x10) ? p->obp1 : p->obp0;

        int row = y - sy;
        if (yflip) row = height - 1 - row;
        int tile_id = (height == 16) ? (tile & 0xFE) : tile;
        if (height == 16 && row >= 8) { tile_id++; row -= 8; }

        for (int col = 0; col < 8; col++) {
            int x = sx + col;
            if (x < 0 || x >= GB_W) continue;
            int tcol = xflip ? (7 - col) : col;
            int c = tile_pixel(p, tile_id, row, tcol, false);
            if (c == 0) continue; /* transparent */
            if (behind_bg && bg_raw[x] != 0) continue; /* behind non-zero BG */
            p->shade[y * GB_W + x] = (uint8_t)pal_lookup(pal, c);
        }
        drawn++;
    }
    (void)drawn;
}

void gb_ppu_render_line(gb_ppu *p, int y) {
    if (y < 0 || y >= GB_H) return;
    int bg_raw[GB_W];
    render_bg_window(p, y, bg_raw);
    render_sprites(p, y, bg_raw);
}

void gb_ppu_resolve(gb_ppu *p) {
    for (int i = 0; i < GB_W * GB_H; i++)
        p->fb[i] = p->palette[p->shade[i] & 3];
}

void gb_ppu_render_frame(gb_ppu *p) {
    p->win_line = 0;
    if (!(p->lcdc & LCDC_ENABLE)) {
        memset(p->shade, 0, sizeof p->shade); /* blank screen = shade 0 */
        gb_ppu_resolve(p);
        return;
    }
    for (int y = 0; y < GB_H; y++) { p->ly = (uint8_t)y; gb_ppu_render_line(p, y); }
    gb_ppu_resolve(p);
}

void gb_ppu_write_reg(gb_ppu *p, uint16_t addr, uint8_t val) {
    switch (addr) {
        case 0xFF40: p->lcdc = val; break;
        case 0xFF41: p->stat = (uint8_t)((p->stat & 0x07) | (val & 0x78)); break;
        case 0xFF42: p->scy = val; break;
        case 0xFF43: p->scx = val; break;
        case 0xFF45: p->lyc = val; break;
        case 0xFF47: p->bgp = val; break;
        case 0xFF48: p->obp0 = val; break;
        case 0xFF49: p->obp1 = val; break;
        case 0xFF4A: p->wy = val; break;
        case 0xFF4B: p->wx = val; break;
        default: break; /* LY (0xFF44) is read-only */
    }
}

uint8_t gb_ppu_read_reg(const gb_ppu *p, uint16_t addr) {
    switch (addr) {
        case 0xFF40: return p->lcdc;
        case 0xFF41: return (uint8_t)(0x80 | (p->stat & 0x78) | 0x02); /* mode 2 stub */
        case 0xFF42: return p->scy;
        case 0xFF43: return p->scx;
        case 0xFF44: return p->ly;
        case 0xFF45: return p->lyc;
        case 0xFF47: return p->bgp;
        case 0xFF48: return p->obp0;
        case 0xFF49: return p->obp1;
        case 0xFF4A: return p->wy;
        case 0xFF4B: return p->wx;
        default: return 0xFF;
    }
}

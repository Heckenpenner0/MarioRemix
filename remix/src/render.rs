//! Software renderer: framebuffer, zone backgrounds, sprites, HUD, screens.
use crate::enemy::FoeKind;
use crate::font;
use crate::game::{Game, Progress, VIEW_H, VIEW_W};
use crate::level::{Tile, TILE};
use crate::player::{Air, Form};
use crate::sprites::{self, Spr};

pub type RGB = [u8; 3];
pub struct Frame { pub w: usize, pub h: usize, pub buf: Vec<u8> }
impl Frame {
    pub fn new(w: usize, h: usize) -> Self { Frame { w, h, buf: vec![0; w * h * 3] } }
    pub fn set(&mut self, x: i32, y: i32, c: RGB) {
        if x < 0 || y < 0 || x >= self.w as i32 || y >= self.h as i32 { return; }
        let i = (y as usize * self.w + x as usize) * 3;
        self.buf[i] = c[0]; self.buf[i + 1] = c[1]; self.buf[i + 2] = c[2];
    }
    pub fn rect(&mut self, x: i32, y: i32, w: i32, h: i32, c: RGB) {
        for dy in 0..h { for dx in 0..w { self.set(x + dx, y + dy, c); } }
    }
    pub fn spr(&mut self, s: &Spr, x: i32, y: i32, flip: bool, mode: u8, tick: u64) {
        for (ry, row) in s.px.iter().enumerate() {
            for (rx, ch) in row.chars().enumerate() {
                if ch == '.' { continue; }
                let sx = if flip { s.w as i32 - 1 - rx as i32 } else { rx as i32 };
                let mut c = pal(ch, mode);
                if mode == 9 {
                    // star rainbow flash
                    let k = ((tick / 4) % 3) as u8;
                    c = match ch { 'R' | 'Y' | 'W' => [[232,32,32],[248,200,32],[255,255,255]][k as usize], _ => c };
                }
                self.set(x + sx, y + ry as i32, c);
            }
        }
    }
    pub fn spr_squash(&mut self, s: &Spr, x: i32, y: i32, h: i32, flip: bool, mode: u8, tick: u64) {
        // draw sprite vertically squashed into h pixels (duck/pound)
        for dy in 0..h {
            let ry = (dy as usize * s.h) / h as usize;
            if let Some(row) = s.px.get(ry) {
                for (rx, ch) in row.chars().enumerate() {
                    if ch == '.' { continue; }
                    let sx = if flip { s.w as i32 - 1 - rx as i32 } else { rx as i32 };
                    self.set(x + sx, y + dy, pal(ch, mode));
                }
            }
        }
        let _ = tick;
    }
    pub fn text(&mut self, s: &str, x: i32, y: i32, c: RGB) {
        let mut cx = x;
        for ch in s.chars() {
            let g = font::glyph(ch.to_ascii_uppercase());
            for (ry, row) in g.iter().enumerate() {
                for (rx, px) in row.chars().enumerate() {
                    if px == '#' { self.set(cx + rx as i32, y + ry as i32, c); }
                }
            }
            cx += 4;
        }
    }
    pub fn text_c(&mut self, s: &str, y: i32, c: RGB) {
        let w = font::text_w(s) as i32;
        self.text(s, (self.w as i32 - w) / 2, y, c);
    }
}
// pal modes: 0 normal, 1 fire (white/red), 2 ball (yellow/black), 10 red-coin tint
pub fn pal(c: char, mode: u8) -> RGB {
    let (mut r, mut g, mut b) = match c {
        'K' => (16, 16, 24), 'W' => (248, 248, 248),
        'R' => (232, 48, 32), 'B' => (48, 96, 232),
        'S' => (255, 205, 160), 'N' => (120, 62, 20),
        'G' => (56, 176, 64), 'Y' => (248, 200, 32),
        'O' => (248, 140, 32), 'P' => (248, 150, 200),
        'U' => (168, 104, 52), 'V' => (150, 150, 165),
        'C' => (80, 170, 250), _ => (255, 0, 255),
    };
    if mode == 1 {
        if c == 'R' { r = 248; g = 248; b = 248; }
        if c == 'B' { r = 232; g = 48; b = 32; }
    } else if mode == 2 {
        if c == 'R' { r = 248; g = 200; b = 32; }
        if c == 'B' { r = 16; g = 16; b = 24; }
    } else if mode == 10 && c == 'Y' {
        r = 240; g = 60; b = 60;
    }
    [r, g, b]
}
pub fn pcol(i: u8) -> RGB {
    match i {
        4 => [168, 104, 52], 6 => [150, 150, 165], 7 => [255, 255, 255],
        9 => [248, 200, 32], 10 => [240, 60, 60], 11 => [200, 200, 200],
        _ => [255, 255, 255],
    }
}
fn hash(x: i32, y: i32) -> u32 {
    let mut h = (x as u32).wrapping_mul(374761393).wrapping_add((y as u32).wrapping_mul(668265263));
    h ^= h >> 13; h = h.wrapping_mul(1274126177); h ^= h >> 16; h
}

fn bg(f: &mut Frame, zone: usize, tick: u64, cam: f32) {
    let w = f.w as i32; let h = f.h as i32;
    match zone {
        1 => { // space night
            f.rect(0, 0, w, h, [8, 8, 24]);
            for i in 0..70 {
                let sx = (hash(i, 7) % w as u32) as i32;
                let sy = (hash(i, 13) % h as u32) as i32;
                if (tick / 20 + i as u64) % 7 != 0 { f.set(sx, sy, [255, 255, 255]); }
            }
            let px = (w - 40 - (cam * 0.1) as i32 % (w + 80) + (w + 80)) % (w + 80) - 40;
            for dy in 0..16 { for dx in 0..16 {
                if (dx - 8) * (dx - 8) + (dy - 8) * (dy - 8) < 56 { f.set(px + dx, 20 + dy, [120, 140, 200]); }
            }}
        }
        3 => { // pumpkin night
            for y in 0..h { let t = y as f32 / h as f32;
                f.rect(0, y, w, 1, [(40.0 + 30.0*t) as u8, (16.0+20.0*t) as u8, (60.0+30.0*t) as u8]); }
            for dy in 0..12 { for dx in 0..12 {
                if (dx - 6) * (dx - 6) + (dy - 6) * (dy - 6) < 30 { f.set(w - 30 + dx, 8 + dy, [240, 230, 180]); }
            }}
            for i in 0..30 {
                let sx = (hash(i, 31) % w as u32) as i32;
                let sy = (hash(i, 37) % h as u32) as i32;
                f.set(sx, sy, [200, 200, 220]);
            }
        }
        6 => { // castle interior
            f.rect(0, 0, w, h, [24, 20, 32]);
            for y in (0..h).step_by(16) { for x in (0..w).step_by(16) {
                f.rect(x, y, 16, 1, [50, 44, 66]);
            }}
            for i in 0..6 {
                let tx = (i * 47 + 10) as i32;
                f.rect(tx, h - 40, 4, 12, [120, 70, 30]);
                if (tick / 15 + i as u64) % 2 == 0 { f.set(tx + 1, h - 42, [255, 200, 80]); f.set(tx + 2, h - 41, [255, 120, 40]); }
            }
        }
        _ => { // day zones with tint per zone
            let top = match zone { 0 => [110, 180, 250], 2 => [150, 230, 160], 4 => [140, 200, 250], 5 => [120, 210, 245], _ => [110, 180, 250] };
            let bot = match zone { 0 => [200, 235, 255], 2 => [220, 250, 200], 4 => [230, 220, 250], 5 => [210, 245, 250], _ => [200, 235, 255] };
            for y in 0..h { let t = y as f32 / h as f32;
                f.rect(0, y, w, 1, [
                    (top[0] as f32 + (bot[0] as f32 - top[0] as f32) * t) as u8,
                    (top[1] as f32 + (bot[1] as f32 - top[1] as f32) * t) as u8,
                    (top[2] as f32 + (bot[2] as f32 - top[2] as f32) * t) as u8]); }
            // clouds
            for i in 0..5 {
                let cx = ((hash(i, 3) % 220) as i32 - (cam * 0.3) as i32 % 220 + 220 + 192) % (192 + 40) - 20;
                let cy = (hash(i, 5) % 40) as i32 + 6;
                f.rect(cx, cy, 22, 6, [255, 255, 255]);
                f.rect(cx + 4, cy - 3, 12, 3, [255, 255, 255]);
            }
            // hills
            for i in 0..4 {
                let hx = ((hash(i, 11) % 240) as i32 - (cam * 0.5) as i32 % 240 + 240 + 192) % (192 + 60) - 30;
                for dy in 0..14 { let hw = dy * 2;
                    f.rect(hx - hw / 2, h - 14 - 40 + dy, hw.max(1), 1, if zone == 2 { [60, 200, 90] } else { [70, 190, 90] });
                }
            }
            if zone == 5 { // sea sparkles
                for i in 0..20 {
                    let sx = (hash(i, 51) % w as u32) as i32;
                    let sy = h - 8 - (hash(i, 53) % 20) as i32;
                    if (tick / 25 + i as u64) % 3 == 0 { f.set(sx, sy, [255, 255, 255]); }
                }
            }
        }
    }
}

fn draw_tile(f: &mut Frame, t: Tile, x: i32, y: i32, above_solid: bool, tick: u64) {
    use sprites::*;
    match t {
        Tile::Empty | Tile::Coin | Tile::Red | Tile::Dragon | Tile::Gold
        | Tile::Goal | Tile::Check | Tile::Spike => {}
        Tile::Ground => f.spr(if above_solid { &T_GROUND } else { &T_GROUND_TOP }, x, y, false, 0, tick),
        Tile::Hard => f.spr(&T_HARD, x, y, false, 0, tick),
        Tile::Brick => f.spr(&T_BRICK, x, y, false, 0, tick),
        Tile::Q(_) => {
            f.spr(&T_Q, x, y, false, 0, tick);
            if tick % 30 < 15 { f.set(x + 2, y + 2, [255, 255, 255]); }
        }
        Tile::Used => f.spr(&T_USED, x, y, false, 0, tick),
        Tile::Note => f.spr(&T_NOTE, x, y, false, 0, tick),
        Tile::Stem => f.spr(&T_STEM, x, y, false, 0, tick),
        Tile::Water => f.spr(if tick % 40 < 20 { &T_WATER0 } else { &T_WATER1 }, x, y, false, 0, tick),
    }
}
fn draw_pickup(f: &mut Frame, t: Tile, x: i32, y: i32, tick: u64) {
    use sprites::*;
    match t {
        Tile::Coin => f.spr(if tick % 30 < 15 { &T_COIN } else { &T_COIN2 }, x, y, false, 0, tick),
        Tile::Red => f.spr(if tick % 30 < 15 { &T_COIN } else { &T_COIN2 }, x, y, false, 10, tick),
        Tile::Dragon => f.spr(&T_DRAGON, x, y, false, 0, tick),
        Tile::Gold => f.spr(&T_GOLD, x, y, false, 0, tick),
        Tile::Goal => {
            f.spr(&T_GOAL0, x, y - 16, false, 0, tick);
            f.spr(&T_GOAL0, x, y - 8, false, 0, tick);
            f.spr(&T_GOAL0, x, y, false, 0, tick);
        }
        Tile::Check => {
            f.spr(&T_CHECK, x, y - 8, false, 0, tick);
            f.spr(&T_CHECK, x, y, false, 0, tick);
        }
        Tile::Spike => f.spr(&T_SPIKE, x, y, false, 0, tick),
        _ => {}
    }
}

pub fn render_world(g: &Game) -> Frame {
    let mut f = Frame::new(VIEW_W as usize, VIEW_H as usize);
    let tick = g.tick;
    let shx = if g.shake > 0 { ((tick % 5) as i32) - 2 } else { 0 };
    let shy = if g.shake > 0 { ((tick % 7) as i32) - 3 } else { 0 };
    let cam = g.cam_x + shx as f32;
    bg(&mut f, g.meta.zone, tick, g.cam_x);
    let x0 = (cam / TILE) as i32 - 1;
    let x1 = ((cam + VIEW_W) / TILE) as i32 + 1;
    for ty in 0..g.lv.h as i32 {
        for tx in x0..=x1 {
            let t = g.lv.at(tx, ty);
            let above = g.lv.at(tx, ty - 1).solid();
            draw_tile(&mut f, t, (tx as f32 * TILE - cam) as i32, (ty as f32 * TILE + shy as f32) as i32, above, tick);
        }
    }
    for ty in 0..g.lv.h as i32 {
        for tx in x0..=x1 {
            let t = g.lv.at(tx, ty);
            draw_pickup(&mut f, t, (tx as f32 * TILE - cam) as i32, (ty as f32 * TILE + shy as f32) as i32, tick);
        }
    }
    let ox = |x: f32| (x - cam) as i32;
    let oy = |y: f32| (y + shy as f32) as i32;
    // items
    for it in &g.items {
        use sprites::*;
        let s = match it.kind { 0 => &I_MUSH, 1 => &I_FLOWER, 2 => &I_CARROT, 3 => &I_BALLFLOWER, 4 => &I_STAR, _ => &I_HEART };
        f.spr(s, ox(it.x), oy(it.y), false, 0, tick);
    }
    // shots
    for s in &g.shots {
        let c = if s.ball { [255, 255, 255] } else { if tick % 10 < 5 { [255, 120, 40] } else { [255, 255, 255] } };
        f.rect(ox(s.x) - 2, oy(s.y) - 2, 5, 5, c);
    }
    // foes
    for foe in &g.foes {
        if !foe.alive { continue; }
        use sprites::*;
        let fx = ox(foe.b.x); let fy = oy(foe.b.y);
        let flip = foe.dir > 0.0;
        let alt = (tick / 20 + foe.t as u64) % 2 == 0;
        match foe.kind {
            FoeKind::Goomba => f.spr(if alt { &E_GOOMBA1 } else { &E_GOOMBA2 }, fx - 1, fy, flip, 0, tick),
            FoeKind::Koopa => f.spr(if alt { &E_KOOPA1 } else { &E_KOOPA2 }, fx - 1, fy, flip, 0, tick),
            FoeKind::Shell => f.spr(&E_SHELL, fx - 1, fy, false, 0, tick),
            FoeKind::Piranha => {
                if foe.b.h >= 4.0 {
                    f.spr(if (tick / 25) % 2 == 0 { &E_PIR_OPEN } else { &E_PIR_SHUT },
                        fx - 1, oy(foe.ay - foe.b.h), false, 0, tick);
                }
            }
            FoeKind::Spiny => f.spr(&E_SPINY, fx - 1, fy, flip, 0, tick),
            FoeKind::Bat => f.spr(if alt { &E_BAT1 } else { &E_BAT2 }, fx - 1, fy, flip, 0, tick),
            FoeKind::Ant => f.spr(&E_ANT, fx - 1, fy, flip, 0, tick),
            FoeKind::Pig => f.spr(&E_PIG, fx - 1, fy, flip, 0, tick),
            FoeKind::Octopus => f.spr(if alt { &E_OCTO1 } else { &E_OCTO2 }, fx - 1, fy, flip, 0, tick),
            FoeKind::Bird => f.spr(if alt { &E_BIRD1 } else { &E_BIRD2 }, fx - 1, fy, flip, 0, tick),
            FoeKind::Fish => f.spr(&E_FISH, fx - 1, fy, flip, 0, tick),
            FoeKind::Trooper => f.spr(&E_TROOP, fx - 1, fy, flip, 0, tick),
            FoeKind::Thwomp => f.spr(if foe.state >= 1 { &E_THWOMP2 } else { &E_THWOMP1 },
                ox(foe.b.cx() - 10.0), oy(foe.b.y), false, 0, tick),
            FoeKind::Wario => {
                let s = if foe.state == 2 || foe.state == 3 { &E_WARIOCH } else if alt { &E_WARIO1 } else { &E_WARIO2 };
                f.spr(s, fx - 1, fy, flip, 0, tick);
            }
        }
    }
    // player
    if !g.pl.dead && !(g.pl.iframes > 0 && tick % 4 < 2) {
        use sprites::*;
        let p = &g.pl;
        let small = p.form == Form::Small;
        let mode = match p.form { Form::Fire => 1, Form::Ball => 2, _ => 0 };
        let fx = ox(p.b.x); let fy = oy(p.b.y);
        let flip = p.face < 0.0;
        let star = if p.star_t > 0 { 9 } else { mode };
        if p.air == Air::Dive {
            f.spr(&M_DIVE, fx - 3, fy, flip, star, tick);
        } else if p.air == Air::Jump3 && (tick / 6) % 2 == 0 {
            // triple-jump backflip: draw upside down
            let s = if small { &M_SMALL_JUMP } else { &M_BIG_JUMP };
            f.spr(s, fx, fy, !flip, star, tick);
        } else if p.duck || p.air == Air::Pound {
            let s = if small { &M_SMALL_STAND } else { &M_BIG_STAND };
            f.spr_squash(s, fx, fy + (p.b.h as i32) - (p.b.h as i32).min(12), p.b.h as i32, flip, star, tick);
        } else {
            let moving = p.b.vx.abs() > 8.0 && p.b.on_ground;
            let cyc = ((p.b.x.abs() / 8.0) as u64) % 4;
            let s = if small {
                if !p.b.on_ground { &M_SMALL_JUMP }
                else if moving && cyc < 2 { &M_SMALL_WALK1 } else if moving { &M_SMALL_WALK2 } else { &M_SMALL_STAND }
            } else {
                if !p.b.on_ground { &M_BIG_JUMP }
                else if moving && cyc < 2 { &M_BIG_WALK1 } else if moving { &M_BIG_WALK2 } else { &M_BIG_STAND }
            };
            let dy = fy + p.b.h as i32 - s.h as i32;
            f.spr(s, fx + (p.b.w as i32 - s.w as i32) / 2, dy, flip, star, tick);
            if p.form == Form::Bunny {
                // ears
                let ex = fx + (p.b.w as i32) / 2 - 3;
                let ey = dy - 4;
                let ec: RGB = [248, 248, 248];
                f.rect(ex, ey, 2, 4, ec); f.rect(ex + 4, ey, 2, 4, ec);
                f.rect(ex, ey + 2, 2, 2, [248, 150, 200]); f.rect(ex + 4, ey + 2, 2, 2, [248, 150, 200]);
            }
        }
    }
    // particles
    for pt in &g.parts {
        if pt.ring {
            let r = pt.t as i32 * 2 + 2;
            let cx = ox(pt.x); let cy = oy(pt.y);
            for dx in -r..=r {
                f.set(cx + dx, cy - r, pcol(pt.col)); f.set(cx + dx, cy + r, pcol(pt.col));
                f.set(cx - r, cy + dx, pcol(pt.col)); f.set(cx + r, cy + dx, pcol(pt.col));
            }
        } else {
            f.rect(ox(pt.x) as i32 - 1, oy(pt.y) as i32 - 1, 2, 2, pcol(pt.col));
        }
    }
    hud(&mut f, g);
    // intro card
    if g.tick < 150 && g.outcome == crate::game::Outcome::None {
        f.text_c(&format!("{}", g.meta.name.to_uppercase()), 40, [255, 255, 255]);
        f.text_c(&format!("{} - TIME {}", crate::level::ZONES[g.meta.zone].name.to_uppercase(), g.time_left), 50, [248, 200, 32]);
    }
    if !g.msg.is_empty() && g.msg_t > 0 {
        f.text_c(&g.msg, 88, [255, 255, 255]);
    }
    f
}
fn hud(f: &mut Frame, g: &Game) {
    f.rect(0, 0, VIEW_W as i32, 9, [0, 0, 0]);
    let wht = [255, 255, 255]; let yel = [248, 200, 32];
    f.text(&format!("SC {:06} C{:02} T{:03} L{}", g.score, g.coin_total % 100, g.time_left, g.lives.max(0)), 2, 2, wht);
    let _ = yel;
    f.text(&format!("RED {}/8{} DRAG {}/5", g.reds.min(8), if g.prog.stars[g.lv_idx] { "*" } else { "" }, g.dragons.min(5)), 2, 11, [255, 255, 255]);
}
pub fn render_title(prog: &Progress) -> Frame {
    let mut f = Frame::new(VIEW_W as usize, VIEW_H as usize);
    bg(&mut f, 0, 60, 0.0);
    f.text_c("SUPER MARIO", 18, [255, 255, 255]);
    f.text_c("WORLDS UNITED", 28, [248, 200, 32]);
    f.text_c("SMW X SML2 X SM64", 40, [255, 255, 255]);
    f.text_c(&format!("GOLD {}/6  STARS {}", prog.gold_count(), prog.star_count()), 54, [248, 200, 32]);
    f.text_c("PRESS ENTER", 72, [255, 255, 255]);
    f.text_c("ARROWS MOVE  X JUMP  Z RUN", 84, [200, 200, 200]);
    f.text_c("DOWN+JUMP: FLIP  DOWN AIR: POUND", 92, [200, 200, 200]);
    f.text_c("WALL+JUMP: KICK  RUN TAP AIR: DIVE", 100, [200, 200, 200]);
    f
}
pub fn render_map(prog: &Progress, cursor: usize, metas: &[crate::level::LevelMeta]) -> Frame {
    let mut f = Frame::new(VIEW_W as usize, VIEW_H as usize);
    bg(&mut f, 0, 90, 0.0);
    f.text_c("COURSE SELECT", 4, [248, 200, 32]);
    let mut y = 14;
    for (i, m) in metas.iter().enumerate() {
        let locked = i > prog.unlocked || (m.zone == 6 && !prog.castle_open());
        let zn = crate::level::ZONES[m.zone].name;
        let mark = if locked { "X" } else if prog.stars[i] { "*" } else { " " };
        let cur = if i == cursor { ">" } else { " " };
        let line = format!("{}{} {}-{} {}", cur, mark, zn, m.name, if m.zone < 6 && prog.gold[m.zone] { "+" } else { "" });
        let col = if locked { [120, 120, 120] } else if i == cursor { [255, 255, 255] } else { [210, 210, 210] };
        // trim to fit
        let mut s = line;
        while font::text_w(&s) as i32 > VIEW_W as i32 - 8 { s.pop(); }
        f.text(&s, 4, y, col);
        y += 7;
        if y > 104 { break; }
    }
    f
}
pub fn render_ending(score: u32, stars: usize) -> Frame {
    let mut f = Frame::new(VIEW_W as usize, VIEW_H as usize);
    bg(&mut f, 6, 30, 0.0);
    f.text_c("WARIO DEFEATED!", 24, [248, 200, 32]);
    f.text_c("THE 6 GOLD COINS", 38, [255, 255, 255]);
    f.text_c("ARE MARIO'S AGAIN!", 48, [255, 255, 255]);
    f.text_c(&format!("SCORE {}  STARS {}/13", score, stars), 64, [255, 255, 255]);
    f.text_c("THANK YOU FOR PLAYING!", 80, [248, 200, 32]);
    f.text_c("PRESS ENTER", 94, [200, 200, 200]);
    f
}
pub fn render_gameover() -> Frame {
    let mut f = Frame::new(VIEW_W as usize, VIEW_H as usize);
    f.rect(0, 0, VIEW_W as i32, VIEW_H as i32, [0, 0, 0]);
    f.text_c("GAME OVER", 48, [240, 60, 60]);
    f.text_c("PRESS ENTER", 64, [255, 255, 255]);
    f
}

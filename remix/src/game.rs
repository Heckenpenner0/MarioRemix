//! World simulation: blocks, items, projectiles, foes, pickups, boss, HUD state.
//! Borrow discipline: index loops + owned locals; in-loop mutation only touches
//! disjoint fields directly (no &mut self calls while element borrows are live).
use crate::enemy::{self, FoeKind};
use crate::level::{self, LevelMeta, Parsed, Tile, TILE};
use crate::physics::overlap;
use crate::player::{Air, Form, Input, Player};

pub const VIEW_W: f32 = 192.0;
pub const VIEW_H: f32 = 112.0;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Outcome { None, Clear, CastleClear }

pub struct Progress {
    pub unlocked: usize,
    pub gold: [bool; 6],
    pub stars: Vec<bool>,
    pub lives: i32,
    pub coins: u32,
    pub score: u32,
}
impl Progress {
    pub fn new(nlevels: usize) -> Self {
        Progress { unlocked: 0, gold: [false; 6], stars: vec![false; nlevels], lives: 3, coins: 0, score: 0 }
    }
    pub fn gold_count(&self) -> usize { self.gold.iter().filter(|&&g| g).count() }
    pub fn star_count(&self) -> usize { self.stars.iter().filter(|&&s| s).count() }
    pub fn castle_open(&self) -> bool { self.gold_count() >= 6 }
}

pub struct Item {
    pub x: f32, pub y: f32, pub w: f32, pub h: f32,
    pub vx: f32, pub vy: f32,
    pub kind: u8,
    pub rise: u8,
    pub alive: bool,
}
pub struct Shot {
    pub x: f32, pub y: f32, pub vx: f32, pub vy: f32,
    pub ball: bool, pub life: u16, pub alive: bool,
}
pub struct Particle {
    pub x: f32, pub y: f32, pub vx: f32, pub vy: f32,
    pub t: u16, pub life: u16, pub col: u8, pub ring: bool,
}
#[derive(Clone, Default)]
pub struct ProgressSnapshot {
    pub gold: [bool; 6],
    pub stars: Vec<bool>,
}

pub struct Game {
    pub lv_idx: usize,
    pub meta: LevelMeta,
    pub lv: Parsed,
    pub pl: Player,
    pub foes: Vec<enemy::Foe>,
    pub items: Vec<Item>,
    pub shots: Vec<Shot>,
    pub parts: Vec<Particle>,
    pub cam_x: f32,
    pub time_left: u32,
    pub tick: u64,
    pub coins: u32,
    pub reds: u32,
    pub dragons: u32,
    pub outcome: Outcome,
    pub clear_t: u16,
    pub dead_t: u16,
    pub shake: u8,
    pub respawn: (f32, f32),
    pub lowgrav: bool,
    pub msg: String,
    pub msg_t: u8,
    pub prog: ProgressSnapshot,
    pub score: u32,
    pub coin_total: u32,
    pub lives: i32,
    pub game_over: bool,
}

impl Game {
    pub fn new(lv_idx: usize, prog: &Progress) -> Self {
        let metas = level::levels();
        let m = &metas[lv_idx];
        let meta = LevelMeta { zone: m.zone, name: m.name, map: m.map, time: m.time, castle: m.castle };
        let lowgrav = level::ZONES[m.zone].lowgrav;
        let mut g = Game {
            lv_idx, meta, lv: level::parse(m.map),
            pl: Player::new(0.0, 0.0),
            foes: Vec::new(), items: Vec::new(), shots: Vec::new(), parts: Vec::new(),
            cam_x: 0.0, time_left: m.time, tick: 0, coins: 0, reds: 0, dragons: 0,
            outcome: Outcome::None, clear_t: 0, dead_t: 0, shake: 0,
            respawn: (0.0, 0.0), lowgrav, msg: String::new(), msg_t: 0,
            prog: ProgressSnapshot { gold: prog.gold, stars: prog.stars.clone() },
            score: prog.score, coin_total: prog.coins, lives: prog.lives, game_over: false,
        };
        g.reset_positions();
        g
    }
    fn reset_positions(&mut self) {
        let (sx, sy) = self.lv.spawn;
        self.pl = Player::new(sx, sy);
        self.respawn = (sx, sy);
        self.foes.clear();
        let spawns = self.lv.foes.clone();
        for (c, cx, cy) in spawns {
            if let Some(f) = enemy::foe_of(c, cx, cy) { self.foes.push(f); }
        }
        self.items.clear(); self.shots.clear(); self.parts.clear();
        self.reds = 0; self.dragons = 0; self.coins = 0;
        self.clear_t = 0; self.dead_t = 0; self.outcome = Outcome::None;
        self.shake = 0; self.msg.clear(); self.msg_t = 0;
    }
    fn respawn_level(&mut self) {
        let metas = level::levels();
        self.lv = level::parse(metas[self.lv_idx].map);
        self.time_left = metas[self.lv_idx].time;
        self.reset_positions();
    }
    fn say(&mut self, s: &str) { self.msg = s.to_string(); self.msg_t = 150; }
    fn burst(&mut self, x: f32, y: f32, col: u8) {
        for i in 0..8 {
            let a = i as f32 * 0.785;
            self.parts.push(Particle { x, y, vx: a.cos() * 70.0, vy: a.sin() * 70.0 - 40.0,
                t: 0, life: 30, col, ring: false });
        }
    }

    pub fn update(&mut self, inp: &Input, prog: &mut Progress) {
        let dt = 1.0 / 60.0;
        self.tick += 1;
        self.score = prog.score; self.coin_total = prog.coins; self.lives = prog.lives;
        if self.msg_t > 0 { self.msg_t -= 1; }
        if self.shake > 0 { self.shake -= 1; }

        if self.outcome != Outcome::None {
            self.clear_t += 1;
            for p in &mut self.parts { p.t += 1; p.x += p.vx * dt; p.y += p.vy * dt; }
            self.parts.retain(|p| p.t < p.life);
            return;
        }
        if self.pl.dead {
            self.dead_t += 1;
            if self.dead_t > 150 {
                prog.lives -= 1;
                if prog.lives < 0 { self.game_over = true; }
                else { self.respawn_level(); }
            }
            return;
        }
        if self.tick % 60 == 0 {
            if self.time_left > 0 { self.time_left -= 1; }
            if self.time_left == 0 { self.kill_player(); return; }
        }

        let grav = if self.lowgrav { 495.0 } else { 900.0 };
        self.pl.update(inp, dt, &self.lv, self.lowgrav);

        if self.pl.b.hit_head {
            let tx = (self.pl.b.cx() / TILE) as i32;
            let ty = ((self.pl.b.y - 1.0) / TILE) as i32;
            self.bump(tx, ty);
        }
        if self.pl.pound_impact {
            self.shake = 12;
            let cx = self.pl.b.cx();
            let feet = self.pl.b.feet();
            for dx in [-8.0f32, 0.0, 8.0] {
                let tx = ((cx + dx) / TILE) as i32;
                let ty = ((feet + 1.0) / TILE) as i32;
                if self.lv.at(tx, ty) == Tile::Brick {
                    self.lv.set(tx, ty, Tile::Empty);
                    self.burst(tx as f32 * TILE + 4.0, ty as f32 * TILE + 4.0, 4);
                }
            }
            self.parts.push(Particle { x: cx, y: feet, vx: 0.0, vy: 0.0, t: 0, life: 12, col: 7, ring: true });
            let mut victims = Vec::new();
            for (i, f) in self.foes.iter().enumerate() {
                if f.alive && f.kind != FoeKind::Thwomp
                    && (f.b.cx() - cx).abs() < 30.0 && (f.b.y - feet).abs() < 20.0 {
                    victims.push(i);
                }
            }
            for i in victims {
                if self.foes[i].kind == FoeKind::Wario { self.hit_wario_idx(i, prog); }
                else {
                    let (bx, by) = (self.foes[i].b.cx(), self.foes[i].b.y);
                    self.foes[i].alive = false;
                    self.burst(bx, by + 4.0, 11);
                    prog.score += 100;
                }
            }
        }
        {
            let tx = (self.pl.b.cx() / TILE) as i32;
            let ty = ((self.pl.b.feet() + 1.0) / TILE) as i32;
            if self.lv.at(tx, ty) == Tile::Note && self.pl.b.vy >= 0.0 {
                self.pl.b.vy = -330.0;
                self.pl.b.y = ty as f32 * TILE - self.pl.b.h - 0.5;
                self.pl.b.on_ground = false;
                self.burst(tx as f32 * TILE + 4.0, ty as f32 * TILE, 9);
            }
        }

        self.update_items(dt, grav, prog);
        self.update_shots(dt, grav, prog);
        self.update_foes(dt, grav, prog);
        self.pickups(prog);
        // crossing the goal line counts (flying/jumping over the gate still wins)
        if self.outcome == Outcome::None && self.meta.zone != 6 && self.pl.b.cx() >= self.lv.goal_x {
            self.outcome = Outcome::Clear;
            prog.score += self.time_left * 10 + 1000;
            self.say("COURSE CLEAR!");
        }
        self.hazards(prog);

        let target = (self.pl.b.cx() - VIEW_W * 0.42).clamp(0.0, (self.lv.w as f32 * TILE - VIEW_W).max(0.0));
        self.cam_x += (target - self.cam_x) * 0.15;
        for p in &mut self.parts { p.t += 1; p.x += p.vx * dt; p.y += p.vy * dt; if !p.ring { p.vy += 500.0 * dt; } }
        self.parts.retain(|p| p.t < p.life);

        if self.meta.zone == 6 && self.outcome == Outcome::None
            && self.foes.iter().any(|f| f.kind == FoeKind::Wario && !f.alive) {
            self.outcome = Outcome::CastleClear;
            self.say("WARIO DEFEATED!");
            prog.score += 5000;
        }
    }

    fn bump(&mut self, tx: i32, ty: i32) {
        match self.lv.at(tx, ty) {
            Tile::Brick => {
                if self.pl.form == Form::Small {
                    self.burst(tx as f32 * TILE + 4.0, ty as f32 * TILE + 8.0, 6);
                } else {
                    self.lv.set(tx, ty, Tile::Empty);
                    self.burst(tx as f32 * TILE + 4.0, ty as f32 * TILE + 4.0, 4);
                }
            }
            Tile::Q(c) => {
                self.lv.set(tx, ty, Tile::Used);
                let kind = match c { 0 => 0, 1 => 1, 2 => 2, 3 => 3, 4 => 4, _ => 5 };
                self.items.push(Item { x: tx as f32 * TILE - 1.0, y: ty as f32 * TILE - 10.0,
                    w: 10.0, h: 10.0, vx: 0.0, vy: 0.0, kind, rise: 20, alive: true });
                self.burst(tx as f32 * TILE + 4.0, ty as f32 * TILE - 2.0, 9);
            }
            Tile::Note => {
                self.pl.b.vy = -330.0;
                self.burst(tx as f32 * TILE + 4.0, ty as f32 * TILE, 9);
            }
            _ => {}
        }
    }

    fn update_items(&mut self, dt: f32, grav: f32, prog: &mut Progress) {
        for i in 0..self.items.len() {
            // movement (disjoint field borrows only)
            let (kind, rise) = (self.items[i].kind, self.items[i].rise);
            if rise > 0 {
                self.items[i].rise -= 1;
                self.items[i].y -= 48.0 * dt;
            } else if kind == 0 || kind == 4 {
                if self.items[i].vx == 0.0 { self.items[i].vx = 30.0; }
                let (mut x, mut y, vx0, vy0, w, h) = {
                    let it = &self.items[i];
                    (it.x, it.y, it.vx, it.vy, it.w, it.h)
                };
                let mut vy = (vy0 + grav * dt).min(200.0);
                if kind == 4 && vy0 == 0.0 { vy = -150.0; }
                let mut b = crate::physics::Body::new(x, y, w, h);
                b.vx = vx0; b.vy = vy;
                crate::physics::move_body(&mut b, dt, TILE, &|tx, ty| self.lv.solid_at(tx, ty));
                let mut vx = b.vx;
                if b.hit_wall != 0 { vx = -vx; }
                if b.on_ground && kind == 4 { b.vy = -150.0; }
                x = b.x; y = b.y; vy = b.vy;
                self.items[i].x = x; self.items[i].y = y;
                self.items[i].vx = vx; self.items[i].vy = vy;
            }
            // pickup
            let (ix, iy, iw, ih, alive) = {
                let it = &self.items[i];
                (it.x, it.y, it.w, it.h, it.alive)
            };
            if alive && overlap((ix, iy, iw, ih),
                    (self.pl.b.x, self.pl.b.y, self.pl.b.w, self.pl.b.h)) {
                self.items[i].alive = false;
                match kind {
                    0 => {
                        if self.pl.form == Form::Small {
                            self.pl.form = Form::Super; self.pl.apply_form();
                            self.msg = "SUPER!".to_string(); self.msg_t = 150;
                        } else { prog.score += 1000; }
                    }
                    1 => { self.pl.form = Form::Fire; self.pl.apply_form(); self.msg = "FIRE FLOWER!".to_string(); self.msg_t = 150; }
                    2 => { self.pl.form = Form::Bunny; self.pl.apply_form(); self.msg = "BUNNY EARS! HOLD JUMP TO FLOAT".to_string(); self.msg_t = 150; }
                    3 => { self.pl.form = Form::Ball; self.pl.apply_form(); self.msg = "SUPERBALL FLOWER!".to_string(); self.msg_t = 150; }
                    4 => { self.pl.star_t = 8 * 60; self.msg = "STAR POWER!".to_string(); self.msg_t = 150; }
                    _ => { prog.lives += 1; self.msg = "1UP!".to_string(); self.msg_t = 150; }
                }
            }
            if self.items[i].y > self.lv.h as f32 * TILE + 40.0 { self.items[i].alive = false; }
        }
        self.items.retain(|i| i.alive);
    }

    fn update_shots(&mut self, dt: f32, grav: f32, prog: &mut Progress) {
        let _ = grav;
        for si in 0..self.shots.len() {
            if !self.shots[si].alive { continue; }
            self.shots[si].life -= 1;
            if self.shots[si].life == 0 { self.shots[si].alive = false; continue; }
            if self.shots[si].ball {
                let (mut x, mut y, mut vx, mut vy) = {
                    let s = &self.shots[si];
                    (s.x, s.y, s.vx, s.vy)
                };
                x += vx * dt; y += vy * dt;
                let tx = (x / TILE) as i32; let ty = (y / TILE) as i32;
                let tx2 = ((x + 4.0) / TILE) as i32;
                if self.lv.solid_at(tx.min(tx2), ty) || self.lv.solid_at(tx.max(tx2), ty) {
                    x -= vx * dt; vx = -vx;
                }
                let ty2 = ((y + 4.0) / TILE) as i32;
                if self.lv.solid_at(tx, ty.min(ty2)) || self.lv.solid_at(tx, ty.max(ty2)) {
                    y -= vy * dt; vy = -vy;
                }
                self.shots[si].x = x; self.shots[si].y = y;
                self.shots[si].vx = vx; self.shots[si].vy = vy;
            } else {
                let (x0, y0, vx0, vy0) = {
                    let s = &self.shots[si];
                    (s.x, s.y, s.vx, s.vy)
                };
                let vy = (vy0 + 700.0 * dt).min(200.0);
                let mut b = crate::physics::Body::new(x0, y0, 4.0, 4.0);
                b.vx = vx0; b.vy = vy;
                crate::physics::move_body(&mut b, dt, TILE, &|tx, ty| self.lv.solid_at(tx, ty));
                if b.hit_wall != 0 { self.shots[si].alive = false; continue; }
                if b.on_ground { b.vy = -110.0; }
                self.shots[si].x = b.x; self.shots[si].y = b.y;
                self.shots[si].vx = b.vx; self.shots[si].vy = b.vy;
            }
            // foe hit? find first, then apply
            let (sx, sy) = (self.shots[si].x, self.shots[si].y);
            let mut hit: Option<usize> = None;
            for (fi, f) in self.foes.iter().enumerate() {
                if f.alive && f.kind != FoeKind::Thwomp
                    && overlap((sx - 2.0, sy - 2.0, 8.0, 8.0), (f.b.x, f.b.y, f.b.w, f.b.h)) {
                    hit = Some(fi); break;
                }
            }
            if let Some(fi) = hit {
                self.shots[si].alive = false;
                if self.foes[fi].kind == FoeKind::Wario { self.hit_wario_idx(fi, prog); }
                else {
                    let (bx, by) = (self.foes[fi].b.cx(), self.foes[fi].b.y);
                    self.foes[fi].alive = false;
                    self.burst(bx, by + 4.0, 10);
                    prog.score += 100;
                }
            }
            if self.shots[si].x < self.cam_x - 32.0 || self.shots[si].x > self.cam_x + VIEW_W + 32.0 {
                self.shots[si].alive = false;
            }
        }
        self.shots.retain(|s| s.alive);
    }

    fn hit_wario_idx(&mut self, i: usize, prog: &mut Progress) {
        if self.foes[i].hp > 0 { self.foes[i].hp -= 1; }
        let hp = self.foes[i].hp;
        self.foes[i].state = 0; self.foes[i].ph = 0;
        let (bx, by) = (self.foes[i].b.cx(), self.foes[i].b.y);
        self.burst(bx, by, 9);
        self.msg = format!("WARIO HIT! {} LEFT", hp);
        self.msg_t = 150;
        prog.score += 500;
        if hp == 0 { self.foes[i].alive = false; }
    }

    fn update_foes(&mut self, dt: f32, grav: f32, prog: &mut Progress) {
        let px = self.pl.b.cx(); let py = self.pl.b.y;
        let cam_mid = self.cam_x + VIEW_W * 0.5;
        for i in 0..self.foes.len() {
            let near = (self.foes[i].b.cx() - cam_mid).abs() <= VIEW_W * 0.75 + 64.0
                || self.foes[i].kind == FoeKind::Wario;
            if !near { continue; }
            // enemy::update_foe needs &mut foe + &self.lv: split via raw parts
            let (lv_w, lv_h) = (self.lv.w, self.lv.h);
            let grid = &self.lv.grid;
            let solid = &|tx: i32, ty: i32| -> bool {
                if tx < 0 || ty < 0 || tx >= lv_w as i32 || ty >= lv_h as i32 { return true; }
                grid[ty as usize * lv_w + tx as usize].solid()
            };
            enemy::update_foe(&mut self.foes[i], dt, solid, px, py, grav);
        }
        // sliding shells kill other foes
        let mut kills = Vec::new();
        for (i, f) in self.foes.iter().enumerate() {
            if !enemy::shell_kills(f) { continue; }
            let hb = (f.b.x, f.b.y, f.b.w, f.b.h);
            for (j, o) in self.foes.iter().enumerate() {
                if i == j || !o.alive { continue; }
                if o.kind == FoeKind::Thwomp || o.kind == FoeKind::Wario { continue; }
                if overlap(hb, (o.b.x, o.b.y, o.b.w, o.b.h)) { kills.push(j); }
            }
        }
        for j in kills {
            if self.foes[j].alive {
                let (bx, by) = (self.foes[j].b.cx(), self.foes[j].b.y);
                self.foes[j].alive = false;
                self.burst(bx, by + 4.0, 11);
                prog.score += 200;
            }
        }
        // player vs foes: decide with immutable reads, then apply
        enum Act { Kill(usize), Hurt, WarioStomp(usize), KoopaShell(usize), ShellStop(usize), ShellKick(usize, f32), Bounce(f32), KickOnly(usize, f32) }
        let star = self.pl.star_t > 0;
        let pounding = self.pl.air == Air::Pound && self.pl.pound_t >= 12;
        let diving = self.pl.air == Air::Dive;
        let (pbx, pby, pbw, pbh, pvy, pfeet, phold) = {
            let b = &self.pl.b;
            (b.x, b.y, b.w, b.h, b.vy, b.y + b.h, self.pl.jump_holding)
        };
        let mut acts = Vec::new();
        for (i, f) in self.foes.iter().enumerate() {
            if !f.alive { continue; }
            let hb = match enemy::foe_hitbox(f) { Some(h) => h, None => continue };
            if !overlap(hb, (pbx, pby, pbw, pbh)) { continue; }
            if star || pounding || diving {
                if f.kind == FoeKind::Thwomp {
                    if !star { acts.push(Act::Hurt); }
                } else if f.kind == FoeKind::Wario {
                    acts.push(Act::WarioStomp(i));
                } else {
                    acts.push(Act::Kill(i));
                }
                if diving { acts.push(Act::Bounce(-160.0)); }
                continue;
            }
            let falling = pvy > 0.0;
            let above = pfeet - f.b.y < 12.0;
            if falling && above {
                match f.kind {
                    FoeKind::Thwomp | FoeKind::Spiny => acts.push(Act::Hurt),
                    FoeKind::Wario => acts.push(Act::WarioStomp(i)),
                    FoeKind::Koopa => acts.push(Act::KoopaShell(i)),
                    FoeKind::Shell => {
                        if f.b.vx.abs() > 10.0 { acts.push(Act::ShellStop(i)); }
                        else {
                            let dir = if pbx + pbw * 0.5 < f.b.cx() { 1.0 } else { -1.0 };
                            acts.push(Act::ShellKick(i, dir));
                        }
                    }
                    _ => {
                        acts.push(Act::Kill(i));
                        acts.push(Act::Bounce(if phold { -280.0 } else { -160.0 }));
                    }
                }
            } else if f.kind == FoeKind::Shell && f.b.vx.abs() <= 10.0 {
                let dir = if pbx + pbw * 0.5 < f.b.cx() { 1.0 } else { -1.0 };
                acts.push(Act::KickOnly(i, dir));
            } else {
                acts.push(Act::Hurt);
            }
        }
        for a in acts {
            match a {
                Act::Kill(i) => {
                    if self.foes[i].alive {
                        let (bx, by) = (self.foes[i].b.cx(), self.foes[i].b.y);
                        self.foes[i].alive = false;
                        self.burst(bx, by + 4.0, 11);
                        prog.score += 100;
                    }
                }
                Act::Hurt => self.hurt_now(prog),
                Act::WarioStomp(i) => {
                    self.hit_wario_idx(i, prog);
                    self.pl.b.vy = -300.0;
                    self.pl.b.y = self.foes[i].b.y - self.pl.b.h - 1.0;
                }
                Act::KoopaShell(i) => {
                    let (cx, by) = (self.foes[i].b.cx(), self.foes[i].b.feet());
                    self.foes[i] = enemy::spawn(FoeKind::Shell, cx, by - 4.0);
                    self.pl.b.vy = if self.pl.jump_holding { -280.0 } else { -160.0 };
                    prog.score += 100;
                }
                Act::ShellStop(i) => {
                    self.foes[i].b.vx = 0.0;
                    self.pl.b.vy = if self.pl.jump_holding { -280.0 } else { -160.0 };
                    prog.score += 100;
                }
                Act::ShellKick(i, dir) | Act::KickOnly(i, dir) => {
                    self.foes[i].b.vx = dir * 140.0;
                    self.foes[i].dir = dir;
                    self.pl.b.vy = -140.0;
                    let (bx, by) = (self.foes[i].b.cx(), self.foes[i].b.y);
                    self.burst(bx, by, 9);
                    prog.score += 100;
                }
                Act::Bounce(vy) => { self.pl.b.vy = vy; }
            }
            if self.pl.dead { break; }
        }
        let lim = self.lv.h as f32 * TILE + 60.0;
        for f in &mut self.foes {
            if f.b.y > lim && !matches!(f.kind, FoeKind::Bird | FoeKind::Fish | FoeKind::Bat | FoeKind::Wario) {
                f.alive = false;
            }
        }
        self.foes.retain(|f| f.alive || f.kind == FoeKind::Wario);
    }

    fn hurt_now(&mut self, prog: &mut Progress) {
        if self.pl.iframes > 0 || self.pl.star_t > 0 || self.pl.dead { return; }
        if self.pl.form != Form::Small {
            self.pl.form = Form::Small;
            self.pl.apply_form();
            self.pl.iframes = 120;
            self.msg = "OUCH!".to_string(); self.msg_t = 150;
        } else {
            self.kill_player();
            let _ = prog;
        }
    }
    fn kill_player(&mut self) {
        if self.pl.dead { return; }
        self.pl.dead = true;
        self.dead_t = 0;
        let (bx, by) = (self.pl.b.cx(), self.pl.b.y);
        self.burst(bx, by + 6.0, 10);
    }

    fn pickups(&mut self, prog: &mut Progress) {
        let pb = (self.pl.b.x - 1.0, self.pl.b.y - 1.0, self.pl.b.w + 2.0, self.pl.b.h + 2.0);
        let x0 = (pb.0 / TILE) as i32; let x1 = ((pb.0 + pb.2) / TILE) as i32;
        let y0 = (pb.1 / TILE) as i32; let y1 = ((pb.1 + pb.3) / TILE) as i32;
        enum Pk { Coin(i32, i32), Red(i32, i32), Dragon(i32, i32), Gold(i32, i32), Check(f32, f32), Goal }
        let mut found = Vec::new();
        for ty in y0..=y1 {
            for tx in x0..=x1 {
                match self.lv.at(tx, ty) {
                    Tile::Coin => found.push(Pk::Coin(tx, ty)),
                    Tile::Red => found.push(Pk::Red(tx, ty)),
                    Tile::Dragon => found.push(Pk::Dragon(tx, ty)),
                    Tile::Gold => found.push(Pk::Gold(tx, ty)),
                    Tile::Check => found.push(Pk::Check(tx as f32 * TILE, ty as f32 * TILE - self.pl.b.h)),
                    Tile::Goal => found.push(Pk::Goal),
                    _ => {}
                }
            }
        }
        for pk in found {
            match pk {
                Pk::Coin(tx, ty) => {
                    self.lv.set(tx, ty, Tile::Empty);
                    self.coins += 1; prog.coins += 1; prog.score += 10;
                    self.burst(tx as f32 * TILE + 4.0, ty as f32 * TILE + 4.0, 9);
                    if prog.coins % 100 == 0 { prog.lives += 1; self.say("100 COINS! 1UP!"); }
                }
                Pk::Red(tx, ty) => {
                    self.lv.set(tx, ty, Tile::Empty);
                    self.reds += 1; prog.coins += 2; prog.score += 50;
                    self.burst(tx as f32 * TILE + 4.0, ty as f32 * TILE + 4.0, 10);
                    if self.reds >= 8 && !self.prog.stars[self.lv_idx] {
                        self.prog.stars[self.lv_idx] = true;
                        prog.stars[self.lv_idx] = true;
                        prog.lives += 1;
                        self.say("8 RED COINS! POWER STAR! 1UP!");
                        prog.score += 2000;
                    } else {
                        let n = self.reds.min(8);
                        self.msg = format!("RED COIN {}/8", n); self.msg_t = 150;
                    }
                }
                Pk::Dragon(tx, ty) => {
                    self.lv.set(tx, ty, Tile::Empty);
                    self.dragons += 1; prog.score += 200;
                    if self.dragons >= 5 { prog.lives += 1; self.say("5 DRAGON COINS! 1UP!"); }
                    else { let n = self.dragons; self.msg = format!("DRAGON COIN {}/5", n); self.msg_t = 150; }
                }
                Pk::Gold(tx, ty) => {
                    self.lv.set(tx, ty, Tile::Empty);
                    let z = self.meta.zone;
                    if z < 6 {
                        self.prog.gold[z] = true;
                        prog.gold[z] = true;
                        self.msg = format!("{}!", level::ZONES[z].coin.to_uppercase());
                        self.msg_t = 150;
                        prog.score += 2000;
                    }
                }
                Pk::Check(rx, ry) => {
                    if (self.respawn.0 - rx).abs() > 1.0 {
                        self.respawn = (rx, ry);
                        self.say("CHECKPOINT!");
                    }
                }
                Pk::Goal => {
                    if self.outcome == Outcome::None {
                        self.outcome = Outcome::Clear;
                        prog.score += self.time_left * 10 + 1000;
                        self.say("COURSE CLEAR!");
                    }
                }
            }
        }
    }

    fn hazards(&mut self, prog: &mut Progress) {
        let pb = (self.pl.b.x + 1.0, self.pl.b.y + 1.0, self.pl.b.w - 2.0, self.pl.b.h - 1.0);
        let x0 = (pb.0 / TILE) as i32; let x1 = ((pb.0 + pb.2) / TILE) as i32;
        let y0 = (pb.1 / TILE) as i32; let y1 = ((pb.1 + pb.3) / TILE) as i32;
        let mut spike = false; let mut water = false;
        for ty in y0..=y1 {
            for tx in x0..=x1 {
                match self.lv.at(tx, ty) {
                    Tile::Spike => spike = true,
                    Tile::Water => water = true,
                    _ => {}
                }
            }
        }
        if water { self.kill_player(); return; }
        if spike { self.hurt_now(prog); }
        if self.pl.b.y > self.lv.h as f32 * TILE + 24.0 { self.kill_player(); }
    }

    pub fn update_shoot_edge(&mut self, inp: &Input, last_run: bool) {
        if inp.run && !last_run && self.outcome == Outcome::None && !self.pl.dead {
            let dir = self.pl.face;
            match self.pl.form {
                Form::Fire if self.shots.len() < 2 => {
                    self.shots.push(Shot { x: self.pl.b.cx(), y: self.pl.b.y + 6.0,
                        vx: dir * 130.0, vy: -60.0, ball: false, life: 180, alive: true });
                }
                Form::Ball if self.shots.len() < 3 => {
                    self.shots.push(Shot { x: self.pl.b.cx(), y: self.pl.b.y + 6.0,
                        vx: dir * 106.0, vy: -106.0, ball: true, life: 240, alive: true });
                }
                _ => {}
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn prog0() -> Progress { Progress::new(level::levels().len()) }
    #[test]
    fn bump_q_spawns_mushroom() {
        let pr = prog0();
        let mut g = Game::new(0, &pr);
        let mut found = None;
        for ty in 0..g.lv.h { for tx in 0..g.lv.w {
            if matches!(g.lv.at(tx as i32, ty as i32), Tile::Q(_)) { found = Some((tx as i32, ty as i32)); break; }
        } if found.is_some() { break; } }
        let (tx, ty) = found.expect("level 0 has a Q block");
        g.bump(tx, ty);
        assert_eq!(g.lv.at(tx, ty), Tile::Used);
        assert_eq!(g.items.len(), 1);
    }
    #[test]
    fn red_coins_award_star_at_8() {
        let mut pr = prog0();
        let mut g = Game::new(0, &pr);
        g.reds = 7;
        let tx = (g.pl.b.cx() / TILE) as i32; let ty = (g.pl.b.y / TILE) as i32;
        g.lv.set(tx, ty, Tile::Red);
        g.pickups(&mut pr);
        assert!(pr.stars[0], "8th red coin earns the power star");
        assert_eq!(g.reds, 8);
    }
    #[test]
    fn goomba_stomp_kills_and_bounces() {
        let mut pr = prog0();
        let mut g = Game::new(0, &pr);
        g.foes.clear();
        let mut f = enemy::spawn(FoeKind::Goomba, g.pl.b.cx(), 68.0);
        f.dir = 0.0;
        g.foes.push(f);
        // drop the player from above the foe
        g.pl.b.x = g.pl.b.cx() - g.pl.b.w * 0.5;
        g.pl.b.y = 20.0;
        g.pl.b.vx = 0.0; g.pl.b.vy = 0.0;
        let idle = Input::default();
        for _ in 0..120 {
            g.update(&idle, &mut pr);
            if g.foes.is_empty() { break; }
        }
        assert!(g.foes.is_empty(), "stomp kills goomba");
        assert!(!g.pl.dead);
    }
}
